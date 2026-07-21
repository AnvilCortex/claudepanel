# ClaudePanel contributor guide

ClaudePanel displays Claude Code session status on an Elecrow CrowPanel ESP32-S3 and sends panel actions back to the active session. The repository contains ESP32 firmware, a local Python bridge, Claude Code hooks, and an optional ntfy notification hub.

## Repository layout

- `firmware/`: PlatformIO firmware for the display, encoder, LED ring, WiFi, and TCP client.
- `bridge/`: Local `aiohttp` service that tracks Claude Code sessions and communicates with the panel.
- `claude-integration/`: Claude Code event and permission hooks plus the macOS launchd definition.
- `hub/`: Optional ntfy-to-panel service for a notification fleet.
- `docs/`: Architecture and integration notes.
- `styles/`: Vale rules and the prose styleguide.

The panel connects as a TCP client. The bridge or hub listens for newline-delimited JSON (NDJSON) on port `8788`. The local bridge also exposes an HTTP API on `127.0.0.1:8787` for Claude Code hooks.

## Prerequisites

- Python 3.12 or a compatible Python 3 release
- PlatformIO CLI (`pio`)
- Vale for documentation linting
- `tmux` for preset text injection
- A CrowPanel 1.28-inch ESP32-S3 device for hardware testing

## Set up the Python services

From the repository root:

```sh
python3 -m venv bridge/.venv
bridge/.venv/bin/python -m pip install -r bridge/requirements.txt -r hub/requirements.txt
```

Run the local bridge:

```sh
bridge/.venv/bin/python bridge/bridge.py
curl http://127.0.0.1:8787/health
```

Run the optional notification hub:

```sh
cp hub/config.example.json hub/config.json
bridge/.venv/bin/python hub/hub.py
curl http://127.0.0.1:8080/health
```

The hub can also run as a container:

```sh
docker build -t claudepanel-hub hub
```

## Build and flash the firmware

Create the local firmware configuration before building:

```sh
cp firmware/.env.example firmware/.env
```

Set `WIFI_SSID`, `WIFI_PASS`, and `BRIDGE_HOST` in `firmware/.env`. Then build from the firmware directory:

```sh
cd firmware
pio run
```

Flash over USB or OTA:

```sh
pio run -t upload --upload-port /dev/cu.usbmodem1101
pio run -t upload --upload-port claudepanel.local
```

Use the port reported by your system if the USB device has a different name. The OTA password is `claudepanel`.

## Test and lint

The repository does not currently have an automated test suite or a code linter. Run the available build and syntax checks before submitting a change:

```sh
python3 -m compileall -q bridge hub claude-integration/claudepanel-permission
bash -n claude-integration/claudepanel-event
(cd firmware && pio run)
vale .
```

For bridge or hub changes, start the affected service and check its `/health` endpoint. Firmware changes that affect networking, controls, or rendering need a device test.

GitHub Actions runs Vale on changed Markdown and MDX lines. The check currently reports findings without blocking a pull request, so fix reported errors before merging.

## Conventions and gotchas

- Preserve the NDJSON message shapes used by `bridge/bridge.py`, `hub/hub.py`, and `firmware/src/`. Protocol changes usually require coordinated Python and firmware updates.
- Keep hook scripts fail-silent and bounded by short timeouts. A stopped bridge or disconnected panel must not interrupt normal Claude Code use.
- Permission `pass` leaves the terminal permission flow unchanged. Do not treat it as `allow` or `deny`.
- Text injection depends on `$TMUX_PANE`. Sessions outside tmux can report status and accept permission decisions, but preset prompts cannot be inserted.
- Keep WiFi credentials, ntfy tokens, local configuration, and firmware images out of Git. `firmware/.env`, `hub/config.json`, and `backup/*.bin` are local files.
- `firmware/env_flags.py` converts values from `firmware/.env` into compile-time definitions. Changing `BRIDGE_HOST` requires rebuilding and flashing the firmware.
- The firmware uses pioarduino with Arduino ESP32 3.x. Do not replace it with the older Espressif Arduino 2.x platform without testing WiFi behavior on the target hardware.
- `claude-integration/com.claudepanel.bridge.plist` contains user-specific absolute paths. Update them before installing the launchd service.
- The bridge configuration lives at `~/.config/claudepanel/config.json`. Restart the launchd service after changing it.
- Write documentation against `styles/prose-styleguide.md` and keep Vale configuration changes under `styles/`.
