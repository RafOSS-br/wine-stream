/*
 * uinput-shim: userspace /dev/uinput emulation over XTest.
 *
 * LD_PRELOADed into Sunshine (rootless container: no /dev/uinput allowed).
 * Intercepts the uinput protocol at the libc layer and re-emits input events
 * as XTest calls into Xwayland — the same injection mechanism proven by the
 * old Sunshine "legacy input" build, but decoupled from Sunshine's source.
 *
 * Scope (v1): mouse (relative, absolute, buttons, wheel) and keyboard.
 * Gamepad-looking devices are accepted but their events are dropped.
 *
 * Build: gcc -O2 -Wall -Wextra -shared -fPIC -o uinput-shim.so uinput-shim.c -lX11 -lXtst
 * Debug: UINPUT_SHIM_DEBUG=1
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#define MAX_DEVS 64
#define UINPUT_PATH "/dev/uinput"

typedef struct {
    bool used;
    bool created;          /* UI_DEV_CREATE seen */
    int fd;                /* pipe read end, handed to the caller */
    int wfd;               /* pipe write end, kept open so reads just block */
    char name[UINPUT_MAX_NAME_SIZE];
    bool gamepad;          /* config looked like a gamepad -> drop events */
    /* absolute axis ranges (from UI_ABS_SETUP or legacy uinput_user_dev) */
    int abs_min[2], abs_max[2];          /* [0]=X [1]=Y */
    /* pending state, flushed on SYN_REPORT or before a key/button event */
    int rel_x, rel_y, wheel, hwheel;
    int abs_x, abs_y;
    bool abs_x_set, abs_y_set;
} shim_dev;

static shim_dev g_devs[MAX_DEVS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Display *g_dpy;
static bool g_x_failed;
static int g_debug = -1;

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_openat64)(int, const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_close)(int);

__attribute__((constructor)) static void shim_init(void) {
    real_open    = dlsym(RTLD_NEXT, "open");
    real_open64  = dlsym(RTLD_NEXT, "open64");
    real_openat  = dlsym(RTLD_NEXT, "openat");
    real_openat64 = dlsym(RTLD_NEXT, "openat64");
    real_ioctl   = dlsym(RTLD_NEXT, "ioctl");
    real_write   = dlsym(RTLD_NEXT, "write");
    real_close   = dlsym(RTLD_NEXT, "close");
}

static bool dbg(void) {
    if (g_debug < 0) {
        const char *e = getenv("UINPUT_SHIM_DEBUG");
        g_debug = (e && *e && *e != '0') ? 1 : 0;
    }
    return g_debug;
}

#define LOG(...) do { fprintf(stderr, "uinput-shim: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DBG(...) do { if (dbg()) LOG(__VA_ARGS__); } while (0)

/* ---- X connection (lazy, single, mutex-guarded by g_lock) ---- */

static Display *xdpy(void) {
    if (g_dpy) return g_dpy;
    if (g_x_failed) return NULL;
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        g_x_failed = true;
        LOG("cannot open DISPLAY=%s — input events will be dropped",
            getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return NULL;
    }
    int ev, err, maj, min;
    if (!XTestQueryExtension(g_dpy, &ev, &err, &maj, &min)) {
        LOG("XTest extension missing — input events will be dropped");
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        g_x_failed = true;
        return NULL;
    }
    LOG("connected to X display, XTest %d.%d", maj, min);
    return g_dpy;
}

/* ---- fd table ---- */

static shim_dev *dev_by_fd(int fd) {
    for (int i = 0; i < MAX_DEVS; i++)
        if (g_devs[i].used && g_devs[i].fd == fd) return &g_devs[i];
    return NULL;
}

static int dev_new(void) {
    int p[2];
    if (pipe(p) != 0) return -1;
    pthread_mutex_lock(&g_lock);
    shim_dev *d = NULL;
    for (int i = 0; i < MAX_DEVS; i++)
        if (!g_devs[i].used) { d = &g_devs[i]; break; }
    if (!d) {
        pthread_mutex_unlock(&g_lock);
        real_close(p[0]); real_close(p[1]);
        errno = ENOMEM;
        return -1;
    }
    memset(d, 0, sizeof *d);
    d->used = true;
    d->fd = p[0];
    d->wfd = p[1];
    d->abs_max[0] = d->abs_max[1] = 0;
    snprintf(d->name, sizeof d->name, "unnamed");
    pthread_mutex_unlock(&g_lock);
    DBG("fake uinput fd %d opened", d->fd);
    return d->fd;
}

/* ---- event translation ---- */

static void press_release(Display *dpy, unsigned button, int times) {
    for (int i = 0; i < times; i++) {
        XTestFakeButtonEvent(dpy, button, True, CurrentTime);
        XTestFakeButtonEvent(dpy, button, False, CurrentTime);
    }
}

/* flush pending motion/wheel; call with g_lock held */
static void flush_motion(shim_dev *d) {
    Display *dpy = xdpy();
    if (!dpy) { d->rel_x = d->rel_y = d->wheel = d->hwheel = 0;
                d->abs_x_set = d->abs_y_set = false; return; }
    if (d->abs_x_set || d->abs_y_set) {
        int scr = DefaultScreen(dpy);
        int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
        /* unset axis: keep current position on that axis */
        int cx = 0, cy = 0;
        if (!d->abs_x_set || !d->abs_y_set) {
            Window r, c; int wx, wy; unsigned m;
            XQueryPointer(dpy, DefaultRootWindow(dpy), &r, &c, &cx, &cy, &wx, &wy, &m);
        }
        long rx = d->abs_max[0] > d->abs_min[0] ? d->abs_max[0] - d->abs_min[0] : 1;
        long ry = d->abs_max[1] > d->abs_min[1] ? d->abs_max[1] - d->abs_min[1] : 1;
        int x = d->abs_x_set ? (int)(((long)(d->abs_x - d->abs_min[0]) * (sw - 1)) / rx) : cx;
        int y = d->abs_y_set ? (int)(((long)(d->abs_y - d->abs_min[1]) * (sh - 1)) / ry) : cy;
        XTestFakeMotionEvent(dpy, scr, x, y, CurrentTime);
        DBG("abs motion -> (%d,%d)", x, y);
        d->abs_x_set = d->abs_y_set = false;
    }
    if (d->rel_x || d->rel_y) {
        XTestFakeRelativeMotionEvent(dpy, d->rel_x, d->rel_y, CurrentTime);
        DBG("rel motion (%d,%d)", d->rel_x, d->rel_y);
        d->rel_x = d->rel_y = 0;
    }
    if (d->wheel)  { press_release(dpy, d->wheel  > 0 ? 4 : 5, abs(d->wheel));  d->wheel = 0; }
    if (d->hwheel) { press_release(dpy, d->hwheel > 0 ? 7 : 6, abs(d->hwheel)); d->hwheel = 0; }
    XFlush(dpy);
}

/* call with g_lock held */
static void handle_key(shim_dev *d, unsigned short code, int value) {
    Display *dpy = xdpy();
    if (!dpy) return;
    flush_motion(d);            /* clicks must land after pending motion */
    Bool down = value != 0;     /* value 2 (autorepeat) counts as press */
    unsigned btn = 0;
    switch (code) {
        case BTN_LEFT:   btn = 1; break;
        case BTN_RIGHT:  btn = 3; break;
        case BTN_MIDDLE: btn = 2; break;
        case BTN_SIDE:   btn = 8; break;
        case BTN_EXTRA:  btn = 9; break;
        case BTN_TOUCH:  btn = 1; break;
        default: break;
    }
    if (btn) {
        XTestFakeButtonEvent(dpy, btn, down, CurrentTime);
        DBG("button %u %s", btn, down ? "down" : "up");
    } else if (code < 248) {    /* evdev keyboard range; X keycode = evdev + 8 */
        XTestFakeKeyEvent(dpy, code + 8, down, CurrentTime);
        DBG("key %u %s", code + 8, down ? "down" : "up");
    }
    XFlush(dpy);
}

/* call with g_lock held */
static void handle_events(shim_dev *d, const struct input_event *ev, size_t n) {
    if (d->gamepad) return;
    for (size_t i = 0; i < n; i++) {
        switch (ev[i].type) {
        case EV_REL:
            switch (ev[i].code) {
                case REL_X:      d->rel_x  += ev[i].value; break;
                case REL_Y:      d->rel_y  += ev[i].value; break;
                case REL_WHEEL:  d->wheel  += ev[i].value; break;
                case REL_HWHEEL: d->hwheel += ev[i].value; break;
                default: break;  /* *_HI_RES etc: detent events already cover it */
            }
            break;
        case EV_ABS:
            if (ev[i].code == ABS_X) { d->abs_x = ev[i].value; d->abs_x_set = true; }
            if (ev[i].code == ABS_Y) { d->abs_y = ev[i].value; d->abs_y_set = true; }
            break;
        case EV_KEY:
            handle_key(d, ev[i].code, ev[i].value);
            break;
        case EV_SYN:
            if (ev[i].code == SYN_REPORT) flush_motion(d);
            break;
        default:
            break;
        }
    }
}

/* ---- intercepted libc entry points ---- */

static bool is_uinput_path(const char *path) {
    return path && strcmp(path, UINPUT_PATH) == 0;
}

int open(const char *path, int flags, ...) {
    if (is_uinput_path(path)) return dev_new();
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    if (is_uinput_path(path)) return dev_new();
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);
    return real_open64(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...) {
    if (is_uinput_path(path)) return dev_new();
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    if (is_uinput_path(path)) return dev_new();
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);
    return real_openat64(dirfd, path, flags, mode);
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap; va_start(ap, request);
    void *arg = va_arg(ap, void *); va_end(ap);

    pthread_mutex_lock(&g_lock);
    shim_dev *d = dev_by_fd(fd);
    if (!d) {
        pthread_mutex_unlock(&g_lock);
        return real_ioctl(fd, request, arg);
    }

    int ret = 0;
    if (_IOC_TYPE(request) == UINPUT_IOCTL_BASE) {
        switch (_IOC_NR(request)) {
        case _IOC_NR(UI_DEV_CREATE):
            d->created = true;
            LOG("device \"%s\" created%s", d->name, d->gamepad ? " (gamepad: events dropped)" : "");
            break;
        case _IOC_NR(UI_DEV_DESTROY):
            d->created = false;
            break;
        case _IOC_NR(UI_DEV_SETUP): {
            const struct uinput_setup *s = arg;
            if (s) snprintf(d->name, sizeof d->name, "%s", s->name);
            break;
        }
        case _IOC_NR(UI_ABS_SETUP): {
            const struct uinput_abs_setup *s = arg;
            if (s) {
                if (s->code == ABS_X) { d->abs_min[0] = s->absinfo.minimum; d->abs_max[0] = s->absinfo.maximum; }
                if (s->code == ABS_Y) { d->abs_min[1] = s->absinfo.minimum; d->abs_max[1] = s->absinfo.maximum; }
                /* gamepad sticks/triggers/hats */
                if (s->code >= ABS_RX && s->code <= ABS_HAT3Y) d->gamepad = true;
            }
            break;
        }
        case _IOC_NR(UI_SET_KEYBIT): {
            long code = (long)arg;
            if (code >= BTN_JOYSTICK && code <= BTN_THUMBR && code != BTN_TOUCH)
                d->gamepad = true;
            break;
        }
        case _IOC_NR(UI_SET_ABSBIT): {
            long code = (long)arg;
            if (code >= ABS_RX && code <= ABS_HAT3Y) d->gamepad = true;
            break;
        }
        case _IOC_NR(UI_GET_SYSNAME(0)): {
            /* fabricate a sysname; nothing under /sys will match it */
            if (arg) snprintf(arg, _IOC_SIZE(request), "uinput-shim-%d", fd);
            ret = (int)strlen(arg ? arg : "") + 1;
            break;
        }
        case _IOC_NR(UI_GET_VERSION):
            if (arg) *(unsigned int *)arg = 5;
            break;
        default:
            break;      /* UI_SET_EVBIT/RELBIT/FFBIT/...: accept silently */
        }
    }
    /* non-'U' ioctls on a fake fd (EVIOC*, FIONREAD...): pretend success */
    pthread_mutex_unlock(&g_lock);
    return ret;
}

ssize_t write(int fd, const void *buf, size_t count) {
    pthread_mutex_lock(&g_lock);
    shim_dev *d = dev_by_fd(fd);
    if (!d) {
        pthread_mutex_unlock(&g_lock);
        return real_write(fd, buf, count);
    }
    if (!d->created) {
        /* legacy uinput API: a struct uinput_user_dev is written before create */
        if (buf && count == sizeof(struct uinput_user_dev)) {
            const struct uinput_user_dev *u = buf;
            snprintf(d->name, sizeof d->name, "%s", u->name);
            d->abs_min[0] = u->absmin[ABS_X]; d->abs_max[0] = u->absmax[ABS_X];
            d->abs_min[1] = u->absmin[ABS_Y]; d->abs_max[1] = u->absmax[ABS_Y];
        }
    } else if (buf && count % sizeof(struct input_event) == 0) {
        handle_events(d, buf, count / sizeof(struct input_event));
    }
    pthread_mutex_unlock(&g_lock);
    return (ssize_t)count;
}

int close(int fd) {
    pthread_mutex_lock(&g_lock);
    shim_dev *d = dev_by_fd(fd);
    if (d) {
        int wfd = d->wfd;
        DBG("device \"%s\" closed", d->name);
        memset(d, 0, sizeof *d);
        pthread_mutex_unlock(&g_lock);
        real_close(wfd);
        return real_close(fd);
    }
    pthread_mutex_unlock(&g_lock);
    return real_close(fd);
}
