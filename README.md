# claudepanel

Claude Code session monitor + remote control on an Elecrow CrowPanel 1.28"
HMI ESP32-S3 Rotary Display. The panel shows live session status (state,
model, context %, cost, current tool) and the bezel knob answers permission
prompts, fires preset prompts into sessions, and switches between sessions.

> **Direction**: this is evolving into a 4-device notification fleet with
> ntfy as the hub — see [docs/ntfy-integration.md](docs/ntfy-integration.md)
> for the target architecture, notification schema, and roadmap.

## Architecture

Claude Code (statusline + hooks) POSTs to a local bridge daemon
(`127.0.0.1:8787`). The device dials the bridge's TCP server (port 8788,
newline-delimited JSON; `BRIDGE_HOST` is baked in from `firmware/.env`)
and keeps a persistent connection. Device actions flow back through the
bridge: permission decisions resolve the blocking PermissionRequest hook,
and preset-text injections go into the session's tmux pane via
`tmux send-keys`.

(v1 used a WebSocket server on the device; the links2004 server stopped
reading inbound frames after the handshake, so it was replaced with a
device-as-client raw TCP link.)

| piece | location | job |
|-|-|
| firmware | `firmware/` (PlatformIO, arduino-esp32 3.x via pioarduino) | GC9A01 UI, WS2812 ring, knob input, TCP link to bridge |
| bridge | `bridge/bridge.py` (launchd service) | session registry, device link, permission futures, tmux injection |
| hooks | `plugin/` (Claude Code plugin) | forward events / permission requests to bridge |

## Device controls

| screen | rotate | click | long press |
|-|-|-|-|
| status | switch session | open presets menu | show device IP |
| presets | move selection | send preset / open notifs | back |
| permission | Allow / Pass / Deny | confirm | Pass |
| notification | scroll stack / select action | enter actions / fire / dismiss | dismiss / cancel actions |

Permission "Pass" (the default) does nothing — the normal terminal dialog
continues. The LED ring (off by default; `"leds": true` in config enables
it): green = idle, orange chase = working, red pulse = permission wanted,
blue = attention.

## Claude Code wiring

Install the hooks as a Claude Code plugin:

```
/plugin marketplace add AnvilCortex/claudepanel
/plugin install claudepanel@claudepanel-marketplace
```

The plugin registers UserPromptSubmit, PreToolUse, Stop, Notification,
SessionStart, and SessionEnd → `claudepanel-event`, plus PermissionRequest →
`claudepanel-permission` (60s timeout). Set `CLAUDEPANEL_BRIDGE_URL` when the
bridge is not at `http://127.0.0.1:8787`.

The panel's session state, context %, and cost come from statusline posts.
The statusline is user configuration, not plugin-installable: copy
`plugin/scripts/claudepanel-statusline` from this repository to a stable
location on your PATH (for example `~/.local/bin/`) and set it as the
`statusLine` command in `settings.json`, or call it from your own statusline
script before rendering. It tees the JSON to the bridge and prints a minimal
line.

On macOS the bridge can autostart via
`claude-integration/com.claudepanel.bridge.plist` (update the `YOURUSER`
paths first); logs land in `~/Library/Logs/claudepanel-bridge.log`.

All hook scripts are fail-silent with ~1s timeouts: if the bridge or device
is down, Claude Code behaves exactly as before.

## Text injection needs tmux

The bridge can only type into sessions that run inside tmux (it records
`$TMUX_PANE` from hook events). Run `tmux` then `ccg`/`cca` inside it.
Sessions outside tmux still get full status display and permission control.

## Config

`~/.config/claudepanel/config.json`:

| key | default | meaning |
|-|-|-|
| `listen_port` | `8788` | TCP port the device dials in to |
| `http_port` | `8787` | bridge HTTP port |
| `enc_div` | `4` | encoder counts per UI step (higher = less sensitive) |
| `perm_timeout_s` | `55` | how long a permission waits on the bezel |
| `leds` | `false` | WS2812 ring animations |
| `beep` | `false` | chirp on permission request |
| `presets` | 5 entries | `{label, text}` list shown in the menu (max 10) |

Restart the bridge after editing:
`launchctl kickstart -k gui/501/com.claudepanel.bridge`

## Firmware

```sh
cd firmware
pio run -t upload --upload-port /dev/cu.usbmodem1101   # USB
pio run -t upload --upload-port claudepanel.local      # OTA (password: claudepanel)
```

`firmware/.env` (gitignored) bakes in credentials at build time:
`WIFI_SSID`, `WIFI_PASS`, `BRIDGE_HOST` (the Mac's LAN IP), optional
`BRIDGE_PORT`. Without a valid `.env`, first boot opens a WiFi captive
portal instead (AP `ClaudePanel`, password `claudepanel`, config at
http://192.168.4.1). If the Mac's IP changes, update `BRIDGE_HOST` and
reflash — or give the Mac a DHCP reservation.

## Protocol (bridge ↔ device, NDJSON over TCP)

Bridge → device: `status` (session upsert), `remove`, `perm` (id, tool,
detail, timeout), `perm_cancel`, `presets`, `cfg`.
Device → bridge: `hello`, `perm_result` (allow | deny | pass), `inject`
(sid, text).

## Restore factory firmware

Full 16MB flash backup taken before first flash:

```sh
python ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --port /dev/cu.usbmodem1101 --baud 921600 \
  write_flash 0x0 backup/factory-firmware-16MB.bin
```

## WiFi behavior

The firmware always scans all channels and connects to the strongest BSSID
of the SSID (the ESP32 default latches onto the first that answers). If the
link sits below -85 dBm for a minute it drops and re-picks; if WiFi is lost
it re-kicks association every 30s and reboots after 5 minutes offline. Every
10s heartbeat in the bridge log carries `rssi`, `drops`, and `rsn` (the
driver's last disconnect reason) — check there first for link trouble.
During association retries the serial console prints a scan survey
(SSID/channel/RSSI) for RF debugging.

## Hardware notes

CrowPanel 1.28" HMI ESP32-S3 (ESP32-S3R8, 8MB OPI PSRAM, 16MB flash).
GC9A01 240x240 on SPI (SCLK 10, MOSI 11, DC 3, CS 9, RST 14), backlight
PWM GPIO46, encoder A45/B42, knob switch 41, 5x WS2812 on GPIO48,
CST816D touch on I2C 6/7 (unused). Note: the product page links the old
ESP32-C3 repo; the correct one is
`Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen`.
