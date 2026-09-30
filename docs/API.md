# Cruller's API

For Home Assistant, scripts and the SVS Bridge: routes under `/api/v1`, which keep working for a
client written for them as Cruller changes. Plain HTTP on port 80, JSON in and out, no
authentication (Cruller is a LAN device: see the README's Security). Cruller serves one request at a
time, with 4 more waiting, so poll gently: every 30 s or so is plenty.

## Finding Cruller

Cruller announces `_rt4k._tcp` on port 80 over mDNS / DNS-SD, as "Cruller Living" on
`cruller-living.local` once it's named "Living" (plain "Cruller" on `cruller.local` before). TXT:

| Key | Value |
|---|---|
| `id` | The board's unique id, the same as `/api/v1/info`'s: it never changes, so use it to tell Crullers apart. |
| `ver` | Cruller's version (`0.4.1`). |
| `api` | The API's version: `1`. |
| `name` | Its name, once it has one. |

`_rfc2217._tcp` (port 2217) is the RT4K's serial port on the network, for pyserial's `rfc2217://`;
`_http._tcp` is the page. [SVS.md](SVS.md#discovery) has them all.

## Versions

The API's version is a whole number: `api` in the TXT, `api_version` in `/api/v1/info`, and the
`v1` in the paths.

- **The same version:** new routes, new keys in an answer, new optional keys in a request. Clients
  ignore keys they don't know, and take a value they don't know (a new power state) as unknown.
- **A new version:** a route or a key removed or renamed, a key's type or meaning changed, something
  new required in a request. `/api/v2/…` would come next to `/api/v1/…`, not instead of it, until
  clients have moved.
- **Not part of it:** `/status`, `/ws`, `/log`, `/rt4k/*`, `/update*`, `/settings`, `/setup`,
  `/wifi*`, `/restart`, `/factory-reset` and `/debug/*` belong to the page, which comes with the same
  firmware, and change with it.
- **Old names:** `/api/command`, `/api/svs` and `/api/svs/unpair` came before the version. They still
  answer, as their `/api/v1` routes do, for the SVS Bridges and scripts that use them; new clients
  use `/api/v1`.

## Routes

| Route | What for |
|---|---|
| `GET /api/v1/info` | Who this Cruller is. |
| `GET /api/v1/state` | The RT4K's power, and Cruller's own. |
| `POST /api/v1/command` | Console commands to the RT4K, and their replies. |
| `GET /api/v1/svs` | The switch's active input, and the switch as its bridge describes it. |
| `POST /api/v1/svs` | The SVS Bridge's report. |
| `POST /api/v1/svs/unpair` | Forget the paired SVS Bridge. |

### GET /api/v1/info

What doesn't change while Cruller runs (a rename restarts it). Read once, when setting it up.

```json
{"id": "e6614c311b2a5f2e", "name": "Living", "hostname": "cruller-living", "sw_version": "0.4.1",
 "platform": "rp2", "api_version": 1}
```

- `id`: as in the TXT.
- `name`: its name, `""` until it has one. `hostname`: its mDNS name, without `.local`.
- `sw_version`: Cruller's version. `platform`: the board (`rp2`: Pico 2 W; `esp32`: ESP32-S3).
- `api_version`: the API's version, a number.

### GET /api/v1/state

What changes: the RT4K's and Cruller's own.

```json
{"rt4k": {"connected": true, "power": "on"},
 "cruller": {"sw_version": "0.4.1", "uptime_s": 3600, "rssi": -52}}
```

- `rt4k.connected`: the RT4K is plugged in and its USB link is up.
- `rt4k.power`: `on`, `standby`, `starting` (it was asked to power on and hasn't shown it yet), or
  `unknown` (not connected, or not known yet). How Cruller knows: [DESIGN.md](DESIGN.md#power-state).
- The SVS switch isn't here: its state comes from the SVS Bridge itself (its own API and Home
  Assistant integration), not through Cruller. Cruller 0.4.2 also had an `svs` key here.
- `cruller.sw_version`: Cruller's version, the running one (it changes with an update).
  `cruller.uptime_s`: seconds since it started. `cruller.rssi`: the Wi-Fi signal, in dBm.

### POST /api/v1/command

Console commands to the RT4K, and their own replies (Cruller sends one at a time and keeps each
command's replies apart from everyone else's: [DESIGN.md](DESIGN.md#rt4k-console)). The body is one of:

- `{"command": "ver"}`
- `{"commands": ["remote menu", "remote down"]}` (up to 8, in order)
- `{"button": "menu"}`, with hass-RT4K's button names, in any case: `power_on` (`pwr on`),
  `power_off` or `power` (`remote pwr`), the RT4K's own keys as `remote <key>` (`menu`, `up`, `ok`,
  `diag`, `prof1`…), and hass-RT4K's other names as the key they stand for (`enter` → `remote ok`,
  `diagnostics` → `remote diag`, `profile1` → `remote prof1`, `1080p` → `remote res1080p`…; the list
  is in `src/core/buttons.c`)
- plain text, one command per line

```bash
curl -X POST http://cruller.local/api/v1/command -d '{"command": "ver"}'
```

```json
{"ok": true, "power": "on",
 "results": [{"command": "ver", "reply": ["[COM] RT4KPRO, FW Version: 1.89.0", "[COM] Build tag: b0928e"],
              "sent": true}]}
```

- `power`: as in `/api/v1/state`, after the commands.
- `results`: one per command, in order. `sent`: it reached the RT4K. `reply`: the lines it answered.
- If a command couldn't be sent (the RT4K isn't connected), `ok` is `false` and the answer is a
  `503`. A body with no command gets `400 {"ok": false, "error": "…"}`.

### /api/v1/svs

The SVS Bridge reports the switch's active input with `POST /api/v1/svs`, and `GET /api/v1/svs`
answers what Cruller last heard. `POST /api/v1/svs/unpair` forgets the paired bridge, so the next one
to report is kept. The formats, and pairing, are in [SVS.md](SVS.md).
