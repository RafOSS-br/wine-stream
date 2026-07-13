#!/usr/bin/env python3
"""In-container unit test for uinput-shim.so.

Run under the shim with an X display available:
  LD_PRELOAD=/usr/local/lib/uinput-shim.so python3 test-shim.py

Creates fake uinput devices exactly like inputtino would (new-style API),
emits events, and asserts the Xwayland pointer actually moved (via xdotool).
"""
import fcntl, os, struct, subprocess, sys, time

# ---- uinput protocol constants (linux/uinput.h) ----
def _IOC(d, t, nr, size): return (d << 30) | (size << 16) | (ord(t) << 8) | nr
def _IO(t, nr):           return _IOC(0, t, nr, 0)
def _IOW(t, nr, size):    return _IOC(1, t, nr, size)

UI_DEV_CREATE  = _IO('U', 1)
UI_DEV_DESTROY = _IO('U', 2)
UI_DEV_SETUP   = _IOW('U', 3, 92)    # struct uinput_setup
UI_ABS_SETUP   = _IOW('U', 4, 28)    # struct uinput_abs_setup
UI_SET_EVBIT   = _IOW('U', 100, 4)
UI_SET_KEYBIT  = _IOW('U', 101, 4)
UI_SET_RELBIT  = _IOW('U', 102, 4)
UI_SET_ABSBIT  = _IOW('U', 103, 4)

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
SYN_REPORT = 0
REL_X, REL_Y = 0x00, 0x01
ABS_X, ABS_Y = 0x00, 0x01
BTN_LEFT, KEY_A = 0x110, 30

def ev(t, c, v):
    return struct.pack("qqHHi", 0, 0, t, c, v)   # struct input_event (x86_64)

def dev_setup(name):
    # struct uinput_setup: input_id (4x u16), name[80], ff_effects_max (u32)
    return struct.pack("HHHH80sI", 3, 0xbeef, 0xcafe, 1, name.encode(), 0)

def abs_setup(code, mn, mx):
    # struct uinput_abs_setup: u16 code (+2 pad), input_absinfo (6x s32)
    return struct.pack("HHiiiiii", code, 0, 0, mn, mx, 0, 0, 0)

def pointer_pos():
    out = subprocess.check_output(["xdotool", "getmouselocation"]).decode()
    p = dict(kv.split(":") for kv in out.split())
    return int(p["x"]), int(p["y"])

def make_abs_mouse():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_LEFT)
    fcntl.ioctl(fd, UI_SET_ABSBIT, ABS_X)
    fcntl.ioctl(fd, UI_SET_ABSBIT, ABS_Y)
    fcntl.ioctl(fd, UI_ABS_SETUP, abs_setup(ABS_X, 0, 65535))
    fcntl.ioctl(fd, UI_ABS_SETUP, abs_setup(ABS_Y, 0, 65535))
    fcntl.ioctl(fd, UI_DEV_SETUP, dev_setup("shim-test abs mouse"))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd

def make_rel_mouse():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_REL)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_LEFT)
    fcntl.ioctl(fd, UI_SET_RELBIT, REL_X)
    fcntl.ioctl(fd, UI_SET_RELBIT, REL_Y)
    fcntl.ioctl(fd, UI_DEV_SETUP, dev_setup("shim-test rel mouse"))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd

def make_keyboard():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
    fcntl.ioctl(fd, UI_SET_KEYBIT, KEY_A)
    fcntl.ioctl(fd, UI_DEV_SETUP, dev_setup("shim-test keyboard"))
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd

failures = []
def check(label, cond, detail=""):
    print(f"{'PASS' if cond else 'FAIL'}: {label} {detail}")
    if not cond:
        failures.append(label)

# screen size for expected abs coords
out = subprocess.check_output(["xdotool", "getdisplaygeometry"]).decode().split()
SW, SH = int(out[0]), int(out[1])

# 1) absolute motion: center of the 0..65535 range -> center of the screen
m = make_abs_mouse()
os.write(m, ev(EV_ABS, ABS_X, 32768) + ev(EV_ABS, ABS_Y, 32768) + ev(EV_SYN, SYN_REPORT, 0))
time.sleep(0.3)
x, y = pointer_pos()
ex, ey = 32768 * (SW - 1) // 65535, 32768 * (SH - 1) // 65535
check("abs motion to center", abs(x - ex) <= 2 and abs(y - ey) <= 2,
      f"got ({x},{y}) expected (~{ex},~{ey})")

# 2) relative motion from a mixed batch on another device
r = make_rel_mouse()
os.write(r, ev(EV_REL, REL_X, 100) + ev(EV_REL, REL_Y, 50) + ev(EV_SYN, SYN_REPORT, 0))
time.sleep(0.3)
x2, y2 = pointer_pos()
check("rel motion +100+50", abs(x2 - (x + 100)) <= 2 and abs(y2 - (y + 50)) <= 2,
      f"got ({x2},{y2}) expected (~{x+100},~{y+50})")

# 3) click and keyboard: must not raise and must not disturb the pointer
os.write(r, ev(EV_KEY, BTN_LEFT, 1) + ev(EV_SYN, SYN_REPORT, 0))
os.write(r, ev(EV_KEY, BTN_LEFT, 0) + ev(EV_SYN, SYN_REPORT, 0))
k = make_keyboard()
os.write(k, ev(EV_KEY, KEY_A, 1) + ev(EV_SYN, SYN_REPORT, 0))
os.write(k, ev(EV_KEY, KEY_A, 0) + ev(EV_SYN, SYN_REPORT, 0))
time.sleep(0.2)
x3, y3 = pointer_pos()
check("click+key do not move pointer", (x3, y3) == (x2, y2), f"got ({x3},{y3})")

for fd in (m, r, k):
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)

print("=" * 40)
print("ALL PASS" if not failures else f"FAILED: {failures}")
sys.exit(1 if failures else 0)
