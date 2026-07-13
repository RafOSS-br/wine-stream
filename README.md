# wine-stream

Run untrusted Windows executables (games included) on Linux inside a **hardened,
rootless podman container**, with GPU rendering and streaming to any Moonlight
client. No uinput, no udev, no extra capabilities — the container gets exactly
one device: the GPU render node.

## How it works

| Piece | Role |
|---|---|
| Wine + DXVK | Runs the `.exe`, renders D3D over Vulkan on the GPU |
| Sway (headless) + Xwayland | Gives the app a display |
| Sunshine (legacy XTest build) | Captures the display (wlr-screencopy), encodes (VAAPI), streams to Moonlight |

Input is the trick: Moonlight input is injected via **XTest** straight into
Xwayland over the X socket. That is what makes a fully rootless, `cap-drop=ALL`
container possible — the usual `/dev/uinput` path cannot work rootless.

## Isolation

Rootless podman · `cap-drop=ALL` · `no-new-privileges` · seccomp default ·
read-only root · only `/dev/dri/renderD128` (no card0) · app dir mounted
read-only · writable state confined to named volumes and tmpfs · ports bound
to `127.0.0.1` only.

## Usage

```bash
# 1) Build the image (once; compiles Sunshine legacy — slow the first time)
podman build -t wine-stream:latest image/

# 2) Create the wineprefix for your app (once per app)
./run.sh apps/wow335.env prefix

# 3) Run
./run.sh apps/wow335.env
```

Client side:

```bash
flatpak install -y flathub com.moonlight_stream.Moonlight
```

1. Sunshine web UI: `https://127.0.0.1:47990` → create the admin user.
2. Moonlight → Add Host `127.0.0.1` → approve the PIN in the web UI.
3. Open **Desktop**. Match the stream resolution to the profile's `RESOLUTION`.

## App profiles

An app is a small env file in `apps/`. Copy `apps/wow335.env` and edit:

| Variable | Meaning |
|---|---|
| `APP_NAME` | Prefix for container and volume names |
| `APP_DIR` | Host dir with the app, mounted read-only at `/app` |
| `APP_EXE` | Executable to launch, relative to `APP_DIR` |
| `APP_PERSIST` | Subdirs that keep state → named volumes, seeded on first run |
| `APP_SCRATCH` | Subdirs with throwaway writes → tmpfs (`dir:size`) |
| `RESOLUTION` / `MEMORY` | Virtual display size / container memory cap |

## Notes

- **Cursor**: headless capture never shows a hardware (X11) cursor — the app
  must draw its own. WoW 3.3.5a: `SET gxCursor "0"` in `WTF/Config.wtf`
  (the cvar `hardwareCursor` does not exist in that client).
- **Mouselook/relative motion** is the only fragile part of XTest under
  Xwayland; clicks and keyboard are solid.
- Wineprefix and DXVK cache live in `<APP_NAME>-prefix` / `<APP_NAME>-cache`
  volumes; Sunshine pairing state in the shared `sunshine-state` volume.
