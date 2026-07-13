#!/bin/bash
# wine-stream launcher: runs a Windows executable inside a hardened, rootless
# container with GPU rendering (Wine + DXVK) and Moonlight streaming (Sunshine).
#
# Usage:
#   ./run.sh apps/<profile>.env          run the app
#   ./run.sh apps/<profile>.env prefix   one-time: create the wineprefix + DXVK
set -euo pipefail
cd "$(dirname "$0")"

PROFILE="${1:?usage: run.sh apps/<profile>.env [prefix]}"
MODE="${2:-run}"
# shellcheck source=/dev/null
source "$PROFILE"

: "${APP_NAME:?APP_NAME missing in profile}"
: "${APP_DIR:?APP_DIR missing in profile}"
: "${APP_EXE:?APP_EXE missing in profile}"
RESOLUTION="${RESOLUTION:-1920x1080}"
MEMORY="${MEMORY:-8g}"
IMAGE="${IMAGE:-wine-stream:latest}"
APP_PERSIST="${APP_PERSIST:-}"
APP_SCRATCH="${APP_SCRATCH:-}"

APP_DIR="$(realpath "$APP_DIR")"
[ -f "$APP_DIR/$APP_EXE" ] || { echo "executable not found: $APP_DIR/$APP_EXE" >&2; exit 1; }

# Create a named volume; on first creation, seed it from a directory if given.
ensure_volume() { # <volume> [seed-dir]
  local vol=$1 seed=${2:-}
  podman volume exists "$vol" && return 0
  podman volume create "$vol" >/dev/null
  if [ -n "$seed" ] && [ -d "$seed" ]; then
    podman run --rm --entrypoint /bin/cp --security-opt label=disable \
      -v "$seed":/seed:ro -v "$vol":/dst "$IMAGE" -a /seed/. /dst/
    echo "seeded volume $vol from $seed"
  fi
}

ensure_volume "${APP_NAME}-prefix"
ensure_volume "${APP_NAME}-cache"

# Allocate a TTY only when we have one (keeps scripted runs working).
TTY=()
[ -t 0 ] && TTY=(-it)

# Hardening baseline shared by both modes: rootless, no caps, read-only root,
# render node only (no card0, no uinput, no udev).
HARDEN=(
  --device /dev/dri/renderD128
  --group-add keep-groups
  --cap-drop=ALL
  --security-opt=no-new-privileges
  --security-opt label=disable
  --read-only
  --tmpfs /tmp:rw,nosuid,nodev,exec,size=1g
  --tmpfs /home/wine:rw,nosuid,size=64m
  -v "${APP_NAME}-prefix":/home/wine/.wine
  -v "${APP_NAME}-cache":/home/wine/.cache
)

if [ "$MODE" = "prefix" ]; then
  exec podman run --rm ${TTY:+"${TTY[@]}"} --name "wine-stream-${APP_NAME}-prefix" \
    "${HARDEN[@]}" \
    -e HOME=/home/wine \
    --entrypoint /usr/local/bin/build-prefix.sh \
    "$IMAGE"
fi

# Per-app persistent subdirs -> named volumes over the read-only app mount,
# seeded from the host app dir on first run.
APP_MOUNTS=(-v "$APP_DIR":/app:ro)
for dir in $APP_PERSIST; do
  vol="${APP_NAME}-$(echo "$dir" | tr '[:upper:]/' '[:lower:]-')"
  ensure_volume "$vol" "$APP_DIR/$dir"
  APP_MOUNTS+=(-v "$vol":"/app/$dir")
done

# Per-app scratch subdirs (dir:size) -> tmpfs, discarded on exit.
for spec in $APP_SCRATCH; do
  dir="${spec%%:*}" size="${spec##*:}"
  APP_MOUNTS+=(--tmpfs "/app/$dir":rw,nosuid,nodev,size="$size")
done

exec podman run --rm ${TTY:+"${TTY[@]}"} --name "wine-stream-${APP_NAME}" \
  "${HARDEN[@]}" \
  --tmpfs /run:rw,nosuid,nodev,size=64m \
  --tmpfs /run/user/1000:rw,nosuid,nodev,mode=0700,size=256m \
  --tmpfs /root/.local:rw,nosuid,size=64m \
  -v sunshine-state:/root/.config \
  "${APP_MOUNTS[@]}" \
  -e APP_EXE="/app/$APP_EXE" \
  -e RESOLUTION="$RESOLUTION" \
  --pids-limit=4096 \
  --memory="$MEMORY" \
  -p 127.0.0.1:47984:47984/tcp \
  -p 127.0.0.1:47989:47989/tcp \
  -p 127.0.0.1:47990:47990/tcp \
  -p 127.0.0.1:48010:48010/tcp \
  -p 127.0.0.1:47998:47998/udp \
  -p 127.0.0.1:47999:47999/udp \
  -p 127.0.0.1:48000:48000/udp \
  -p 127.0.0.1:48010:48010/udp \
  ${EXTRA_ARGS:+"${EXTRA_ARGS[@]}"} \
  "$IMAGE"
