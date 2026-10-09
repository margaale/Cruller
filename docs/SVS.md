# Cruller and the SVS Bridge

The [SVS Bridge](https://github.com/margaale/svs-bridge) watches a Scalable Video Switch and knows
its active input. The RT4K doesn't tell Cruller when the switch changes input (the SVS talks to it over
the HD-15, and nothing shows up on the USB console), so the bridge tells Cruller. gameID uses the
active input to apply only the game profile of the console on screen.

Both boards find each other, and Home Assistant finds both, through mDNS / DNS-SD. No addresses to
type in.

## Discovery

| Board | Service | Port | TXT |
|---|---|---|---|
| Cruller | `_rt4k._tcp` | 80 | `id=<Pico id>`, `ver=<Cruller version>`, `api=1` (API version, [API.md](API.md)), `name=<name>` (once named) |
| Cruller | `_rfc2217._tcp` | 2217 | `id=<Pico id>` |
| Cruller | `_http._tcp` | 80 | (the web page) |
| SVS Bridge | `_svsbridge._tcp` | 443 | `id=svs-bridge-<mac>`, `version=1` (API version) |

`_rt4k._tcp` is generic on purpose: any RT4K bridge can announce it, and a Home Assistant integration
(hass-RT4K) can list `_rt4k._tcp.local.` in its manifest's `zeroconf` to be offered every one.

## The bridge reports the active input, and describes the switch

The bridge browses `_rt4k._tcp`. To each Cruller it finds it sends, over plain HTTP:

```
POST http://<host>:<port>/api/v1/svs
Content-Type: application/json

{"id": "svs-bridge-aabbccddeeff", "current_input": 3, "total_inputs": 8, "live": true,
 "inputs": [{"kind": "scart", "name": "Super Nintendo / Super Famicom", "device": "snes"}, ...],
 "output": {"kind": "component", "name": "RetroTINK 4K", "device": "rt4k"}}
```

- **Where:** `/api/v1/svs`, part of Cruller's versioned API ([API.md](API.md)). Bridges from before
  it send to `/api/svs`, which Cruller still takes the same way.
- **When:** on every input change, whenever the switch's description changes (its layout is edited),
  as soon as it finds a Cruller (including one that just restarted and announced itself again), and
  every 60 s otherwise, in case a report was lost.
- **The input:** the same names as the `svs` object of the bridge's `GET /api/v1/state`, plus `id`.
  `current_input` is required (`input` is accepted too). The active port's name can come as `name` or
  `current_input_name`; without either, Cruller takes it from `inputs`. `current_input: 0` means no
  input is active (the page shows "no input active", and gameID treats it as no console on screen).
- **The switch** (what the SVS can't report itself, as the bridge's SVS tab sets it up; the rest of
  what the bridge knows about the SVS stays on the bridge):
  - `inputs`, one per input in order (index 0 is input 1): `kind` is the module (`scart`, `component`,
    `vga`, `svideo`, `dterm`; `""` until it is picked), `device` the console on it, as its id in the
    bridge's list (`snes`, `ps2`, `megadrive`…; `""` if none is picked), and `name` its name (up to 32
    bytes). An empty list when no layout is set up.
  - `output`: the output that goes to the RetroTINK (the one whose device is `rt4k` or `rt4kce`), with
    the same keys (`kind`: `scart`, `component`, `vga`, `svideo`, `bnc`); absent or `null` if none is.
  - A report with either key replaces what Cruller had; one with neither (an older bridge) leaves it.
  - A body is 6 KB at most (32 inputs with long names take about 4.5 KB).
- **Answer:** `{"ok": true, "changed": true|false}` (`changed`: the input). A repeat changes nothing on
  Cruller. A body it can't read gets `400 {"ok": false, "error": "…"}`.
- **No token:** Cruller has no authentication anywhere (it's a LAN device, like its page and its
  RFC 2217 port), and it only records the report. The bridge's own API keeps its token.

Why the bridge pushes instead of Cruller polling it: the bridge's API is HTTPS with a token, and
Cruller would need a TLS client and the token for a single number. The bridge already has an HTTP
client (`esp_http_client`) and mDNS browsing (`mdns_query_ptr`) in ESP-IDF.

## Names and pairing (several SVS and RT4Ks)

- **Each Cruller has a name**, set in its setup wizard or the Cruller tab ("Living", "Game room"). It
  announces itself as "Cruller Living" on `cruller-living.local`, with `name=Living` in the `_rt4k`
  TXT. An unnamed Cruller is plain `cruller.local`.
- **The bridge picks its Cruller**: its web UI lists the `_rt4k._tcp` it finds (name, id) and the user
  chooses one; the bridge stores that Cruller's `id` (not its address) and reports only to it.
- **Cruller keeps the first bridge that reports** (with an `id`) in its settings. Reports from another
  bridge get `409 {"ok": false, "error": "paired with another SVS Bridge", "paired": "<id>"}`, so a
  bridge set up for another RT4K can't change this one's profiles. `POST /api/v1/svs/unpair` (the Unpair
  button in the Cruller tab) frees it; a factory reset does too.

## What Cruller does with it

- `GET /api/v1/svs`: what Cruller last heard:
  `{"known": true, "input": 3, "total": 8, "name": "PS2", "id": "svs-bridge-…", "paired": "svs-bridge-…",
  "heard_s": 12, "since_s": 340, "switch_seq": 2, "history": [[3, 340], [1, 900]],
  "switch": {"inputs": [{"kind", "name"}, ...], "output": {"kind", "name"} | null}}` (`heard_s`: seconds since the last report; `since_s`:
  since the input last changed; `history`: the last input changes, `[input, seconds ago]`, newest
  first; `switch`: the description, as the bridge sent it, once one has come in).
- The page's `/status` JSON (and its status over the WebSocket) has the same object as `"svs"`,
  without `switch`, once a report has come in: `switch_seq` changes with each new description, and
  the page fetches `GET /api/v1/svs` then. `GET /api/v1/state` doesn't have it: Home Assistant and
  scripts get the switch's state from the SVS Bridge itself, not through Cruller. Cruller keeps it in RAM: after a restart, the bridge's next report
  (it reports as soon as Cruller announces itself) brings it back.
- The Consoles view shows each input's console as an icon (by its `device`: its controller or the machine,
  with the buttons in their own colours; a plain pad and its name when Cruller has no drawing of
  it) and its module, and the output to the RetroTINK.
- Each input's profile: the Consoles view has a combo per input with the profiles in the RT4K's
  `/profile/SVS`. The profile is the RT4K's own: with Auto Load SVS on, when the switch tells it input
  n is on (over the HD-15), it loads the first `/profile/SVS/S<n>_<anything>.rt4` it finds, and only
  from that folder, so nothing goes through Cruller then. Picking one makes it that file: renamed to
  `S<n>_<name>`, or copied when it's another input's (each input keeps its own). The files the input
  had become unassigned: `X_` in place of their `S<n>_` (`S1_SNES.rt4`: `X_SNES.rt4`, or
  `X_SNES (2).rt4` when that's taken), kept in the folder to be picked again. Not an `S`: were the
  RT4K to read the input loosely, "SX_" could read as input 0. The combos show each by its name alone
  (`SNES`). When the input is on screen, Cruller loads it right away (`prof load`). It all runs in the
  page, with the SD card routes (`ls`, `get`, `put`, `mv`). After each read the page sends each
  input's profile to Cruller (`POST /api/v1/svs/profiles`), which keeps it in flash (written only when
  it changed): with the RT4K in standby the cards still show each input's profile, not to be changed.
- gameID (coming): each console is assigned a switch input. Only the console on the active input
  changes the RT4K's profile, and switching inputs re-applies that console's game profile if it has
  one. Game profiles use their own SVS numbers (S100 and up) so they never overwrite the switch's
  S1…S8, and gameID sends nothing when there's no game (no "S0"), leaving the switch's profile.

## What the SVS Bridge needs

1. Browse `_rt4k._tcp` (ESP-IDF `mdns_query_ptr("_rt4k", "_tcp", …)`, repeated now and then, or on
   the mDNS announcements it sees).
2. In its web UI, list the Crullers found (TXT `name`, `id`) and let the user pick one; store its `id`.
3. `POST /api/v1/svs` to the picked one (found by `id` each time) with the body above: on start, on each
   input change, when the layout changes, every 60 s. Show whether the last report
   went through, and a `409` as "this Cruller is paired with another bridge".
