# Cruller's API

For Home Assistant, scripts and the SVS Bridge: routes under `/api/v1`, which keep working for a
client written for them as Cruller changes. Plain HTTP on port 80, JSON in and out, no
authentication (Cruller is a LAN device: see the README's Security). Cruller serves one request at a
time, with 4 more waiting, so poll gently (every 30 s or so is plenty), or listen to
`/api/v1/events`, which tells a change at once.

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
| `GET /api/v1/events` | The same state as it changes, over a WebSocket. |
| `POST /api/v1/command` | Console commands to the RT4K, and their replies. |
| `GET /api/v1/svs` | The switch's active input, and the switch as its bridge describes it. |
| `POST /api/v1/svs` | The SVS Bridge's report. |
| `POST /api/v1/svs/unpair` | Forget the paired SVS Bridge. |
| `POST /api/v1/svs/profiles` | Each input's profile, as the page read the RT4K's card, to keep. |
| `GET /api/v1/gameid/state` | What gameID knows: each console's game, the one on screen, the profile loaded. |
| `GET /api/v1/gameid/consoles` | The consoles gameID asks which game they run. |
| `POST /api/v1/gameid/consoles` | Replace them. |
| `GET /api/v1/gameid/games` | The gameDB: each game and its profile. |
| `POST /api/v1/gameid/games` | Add a game, or change one. |
| `POST /api/v1/gameid/games/delete` | Remove a game. |

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
{"rt4k": {"connected": true, "power": "on", "firmware": "1.89.0", "model": "RT4K_Pro",
          "profile": "SVS/S2_Genesis.rt4"},
 "cruller": {"sw_version": "0.5.0", "uptime_s": 3600, "rssi": -52,
             "supply_v": 4.84, "supply_min_v": 4.71, "usb_power": true, "temperature_c": 31.4}}
```

- `rt4k.connected`: the RT4K is plugged in and its USB link is up.
- `rt4k.power`: `on`, `standby`, `starting` (it was asked to power on and hasn't shown it yet), or
  `unknown` (not connected, or not known yet). How Cruller knows: [DESIGN.md](DESIGN.md#power-state).
- `rt4k.firmware`, `rt4k.model` (since 0.5.0): the RT4K's firmware version and its model, as it
  last said them (its `ver` and `model` replies). Cruller asks each time the RT4K comes on and keeps
  them across restarts, so they're here while it sleeps too. Each is left out until Cruller has seen
  it once; they stay when the RT4K is unplugged (the last one it saw).
- `rt4k.profile` (since 0.6.0): the profile the RT4K has loaded, its path under `/profile`
  (`"SVS/S2_Genesis.rt4"`), `""` for none (settings that aren't a saved profile), or `null` when it
  isn't known: the RT4K isn't on. The RT4K doesn't say when it loads one (from its menu, its remote,
  Auto Load SVS), so Cruller asks (`prof get`) every 10 s without hearing it, and 3 s after the SVS
  switches inputs or a `prof load`: a change shows within 10 s, an SVS one within about 3.
- The SVS switch isn't here: its state comes from the SVS Bridge itself (its own API and Home
  Assistant integration), not through Cruller. Cruller 0.4.2 also had an `svs` key here.
- `cruller.sw_version`: Cruller's version, the running one (it changes with an update).
  `cruller.uptime_s`: seconds since it started. `cruller.rssi`: the Wi-Fi signal, in dBm.
- The board's own sensors, only on a board that has them (the Pico 2 W, since 0.4.4; the ESP32-S3
  leaves them out), so a client shows each only when it's there:
  - `cruller.supply_v`: the supply, in volts: the Pico 2 W's VSYS, USB's 5 V less its input diode
    (~0.3 V), so ~4.7-4.9 V on a good 5 V supply. Sampled 10 times a second; this is the last sample.
  - `cruller.supply_min_v`: the lowest single read since it started. A supply that sags when the RT4K
    draws more (switching inputs) shows here, and sags that deep reset the board.
  - `cruller.usb_power`: whether USB brings it 5 V (false: powered through VSYS).
  - `cruller.temperature_c`: the chip's temperature, in °C (its own sensor, ±a few degrees).

### GET /api/v1/events

A WebSocket that pushes events instead of waiting to be polled, for clients that want a change at
once (Home Assistant's automations). `?types=` picks the event types, comma-separated; without it,
the socket gets `state`:

```bash
websocat 'ws://cruller.local/api/v1/events?types=state'
```

```json
{"type": "hello", "api_version": 1, "types": ["state"], "subscribed": ["state"]}
{"type": "state", "state": {"rt4k": {"connected": true, "power": "standby"},
 "cruller": {"sw_version": "0.4.3", "uptime_s": 3600, "rssi": -52}}}
```

- **Messages:** text, each a JSON object with a `type`. The first is `hello`: the API's version,
  `types` (every type this Cruller can send) and `subscribed` (the ones this socket gets). Then only
  events of the subscribed types.
- **`state`:** the same object as `GET /api/v1/state`. It comes at once, as soon as something in it
  changes (today: the RT4K plugged in or out, its power, its firmware or model), and at least every
  60 s (for `uptime_s` and `rssi`).
- **Types:** names Cruller doesn't have are left out of `subscribed`, so a client can ask for a type
  a later Cruller adds and see in `hello` whether this one sends it.
- **The client's messages:** reserved. Today they're ignored (commands go through
  `POST /api/v1/command`). Messages a later v1 takes will be JSON objects with a `type`, announced in
  `hello`.
- **Keepalive:** Cruller pings every 10 s and drops a client it hasn't heard from in 25 s; WebSocket
  libraries answer pings on their own.
- **Room:** it takes one of Cruller's 8 client slots, shared with pages and RFC 2217 clients, with at
  most 2 events sockets at once. A new one replaces the one heard from least recently (a client
  reconnecting after a dropped connection). A page never replaces one, nor one a page; with no slot
  free and none of its kind to replace, the upgrade gets `503`.
- **Older firmware:** a Cruller from before it (0.4.2) answers `404`: poll `GET /api/v1/state` there.

How it grows within v1: new event types (only sockets that ask for them get them, so an existing
client's stream doesn't change), new keys in any message (`hello` included), and client messages
announced in `hello`. Changing what an existing type means, or dropping one, would take a v2.

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

`POST /api/v1/svs/profiles` keeps each input's profile: the file in the RT4K's `/profile/SVS` it
loads for that input, as the page last read the card. The body is a line per input that has one,
`<input>\t<file name>` (`1\tS1_SNES.rt4`); none at all is an empty body. Cruller keeps them across
restarts, writing its flash only when they changed, and `GET /api/v1/svs` gives them back as
`"profiles": {"1": "S1_SNES.rt4", "3": "S3_PS1.rt4"}`, so they're known while the RT4K sleeps.
`profiles_seq` (in both, and in the page's status) changes with them. Answers
`{"ok": true, "changed": true}`; a line that isn't an input 1-32, a tab and a name (150 bytes at
most), or more than 1 KB, gets `400`/`413 {"ok": false, "error": "…"}`.

### /api/v1/gameid

gameID loads a game's own RT4K profile when a console reports which game it runs
([GAMEID.md](GAMEID.md)). These routes keep what it works from: the consoles it asks, and the games,
each with its profile. Cruller keeps both across restarts in its flash (the gameDB holds 1000 games);
a factory reset erases them.

`GET /api/v1/gameid/consoles` answers them all; `POST /api/v1/gameid/consoles` replaces them all with
the body, in the same form:

```json
{"consoles": [{"name": "PS2", "url": "http://10.10.10.88/api/currentState", "other": "PS2/Generic.rt4",
               "svs_input": 0, "enabled": true}]}
```

| Key | Value |
|---|---|
| `name` | What it's called (47 bytes at most). |
| `url` | What's asked: `http://`, the console's address and its path. Its answer is JSON with `gameID` (a MemCard PRO's or PRO2's, `http://<address>/api/currentState`), or the ID as text (PS1Digital's and N64Digital's, `http://<address>/gameid`). |
| `other` | The profile for a game the gameDB hasn't (optional; `""`: none). |
| `svs_input` | With an SVS switch, the input it's on (1-8); `0` (the default): worked out from the console it is. |
| `enabled` | Asked or not (default `true`). |

Ten consoles at most. A profile is a `.rt4` or `.rt6` on the RT4K's SD card, its path under
`/profile` (`PS2/God of War II.rt4`). Answers `{"ok": true}`; anything else gets
`400 {"ok": false, "error": "…"}`, and nothing changes.

`GET /api/v1/gameid/games` answers every game, in the order they were added:

```json
{"games": [{"id": "SCUS-97481", "profile": "PS2/God of War II.rt4", "name": "God of War II"}]}
```

`POST /api/v1/gameid/games` with one of them (`name` optional) adds it, or replaces the one with its
`id` (compared as written): `{"ok": true, "replaced": false}`. `POST /api/v1/gameid/games/delete`
with `{"id": "SCUS-97481"}` removes it: `{"ok": true, "found": true}`. A body that isn't one of
these, more than 8 KB, or a full gameDB gets `400`/`413 {"ok": false, "error": "…"}`.

`GET /api/v1/gameid/state` answers what gameID knows as it asks the consoles (every 2 s):

```json
{"consoles": [{"name": "PS2", "on": true, "game": "SCUS-97481", "game_name": "God of War II", "kind": "ps2",
               "on_screen": true}],
 "svs_input": 3, "playing": {"console": "PS2", "game": "SCUS-97481", "game_name": "God of War II"},
 "profile": "PS2/God of War II.rt4", "from": "gamedb", "pending": "", "loaded": "PS2/God of War II.rt4",
 "note": "loaded PS2/God of War II.rt4", "note_age_s": 12}
```

| Key | Value |
|---|---|
| `consoles` | Each console, in the order kept: `on` (it answered lately), the `game` it runs (`""`: none it can tell) and its `game_name` when it says one, the `kind` of console (`ps2`, `n64`…: from what it reports, else its name; matched to the SVS Bridge's), `on_screen` (its game is the one that counts). |
| `svs_input` | The SVS switch's input, as its bridge last reported it (`0`: none). |
| `playing` | The game on screen (`null`: none). |
| `profile`, `from` | The profile for it, and from where: `gamedb`, `other` (its console's, for a game the gameDB hasn't), `svs` (its console went off, or was disabled: the input's own `S<n>`); `""`: none. |
| `pending` | A profile about to load: after an input change (the RT4K's own `S<n>` first), once the RT4K comes on, or while it sleeps. |
| `loaded` | What gameID last loaded. |
| `note`, `note_age_s` | What it last did, or why it waits, and how long ago. |
