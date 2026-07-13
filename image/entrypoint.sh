#!/bin/bash
# Boot order: dbus/pipewire -> Sway (headless Wayland) -> Wine app (Xwayland)
#             -> Sunshine (wlr capture + XTest input).
# Input is XTest injected into Xwayland over the X socket: no uinput, no udev,
# no extra capabilities. Rootless, cap-drop=ALL.
set -uxo pipefail
LOG=/tmp/wine-stream.log; exec > >(tee -a "$LOG") 2>&1
echo "=== wine-stream entrypoint ==="

export XDG_RUNTIME_DIR=/run/user/1000
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"

# 1) session dbus + in-container pipewire audio
export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
dbus-daemon --session --address="$DBUS_SESSION_BUS_ADDRESS" --fork --nopidfile 2>/dev/null || \
  echo "WARN: session dbus failed to start (audio may be missing; video continues)"
pipewire &            sleep 0.3
wireplumber &         sleep 0.3
pipewire-pulse &      sleep 0.5

# 2) Headless Sway. No libinput: input arrives via XTest on Xwayland instead.
export WLR_BACKENDS=headless
export WLR_RENDERER=gles2          # buffers importable by Sunshine's wlr capture
export LIBSEAT_BACKEND=noop
sway -c /etc/sway/config &
SWAY_PID=$!

for i in $(seq 1 30); do
  export SWAYSOCK=$(ls /run/user/1000/sway-ipc.*.sock 2>/dev/null | head -1)
  [ -n "${SWAYSOCK:-}" ] && swaymsg -t get_version >/dev/null 2>&1 && break
  sleep 0.5
done
echo "SWAYSOCK=$SWAYSOCK"

# WAYLAND_DISPLAY (wlr capture)
for i in $(seq 1 20); do
  WD=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock' | head -1)
  [ -n "$WD" ] && break; sleep 0.5
done
export WAYLAND_DISPLAY="$(basename "${WD:-wayland-1}")"
echo "WAYLAND_DISPLAY=$WAYLAND_DISPLAY"

for i in $(seq 1 30); do
  swaymsg -t get_outputs 2>/dev/null | grep -q '"active": true' && break
  sleep 0.5
done
swaymsg output HEADLESS-1 resolution "${RESOLUTION:-1920x1080}" || true

# 3) The app: Wine on Xwayland (this spawns Xwayland). DXVK -> Vulkan on renderD128.
APP="${APP_EXE:-}"
if [ -n "$APP" ] && [ -f "$APP" ]; then
  APP_CWD=$(dirname "$APP")
  swaymsg exec "cd '$APP_CWD' && env WINEPREFIX=$WINEPREFIX WINEARCH=$WINEARCH \
        WINEESYNC=1 WINEFSYNC=1 WINE_LARGE_ADDRESS_AWARE=1 WINEDEBUG=-all \
        DXVK_STATE_CACHE_PATH=$DXVK_STATE_CACHE_PATH \
        wine '$APP'"
else
  echo "WARN: APP_EXE '$APP' not found — container stays up for debugging"
fi

# 4) wait for Xwayland (the app connects and creates the X socket), export DISPLAY
for i in $(seq 1 60); do
  XSOCK=$(ls /tmp/.X11-unix/X* 2>/dev/null | head -1)
  [ -n "${XSOCK:-}" ] && break; sleep 0.5
done
export DISPLAY=":$(basename "${XSOCK:-X0}" | tr -d 'X')"
echo "DISPLAY=$DISPLAY (Xwayland, XTest input target)"

# 5) Sunshine: wlr capture (WAYLAND_DISPLAY) + input via the uinput->XTest shim
#    (DISPLAY). The shim emulates /dev/uinput in userspace, so stock Sunshine
#    works rootless with no real input devices.
start_sunshine() {
  while true; do
    LD_PRELOAD=/usr/local/lib/uinput-shim.so \
      WAYLAND_DISPLAY="$WAYLAND_DISPLAY" DISPLAY="$DISPLAY" \
      sunshine /etc/sunshine/sunshine.conf
    echo "WARN: sunshine exited (rc=$?), restarting in 3s"; sleep 3
  done
}
start_sunshine &

wait "$SWAY_PID"
