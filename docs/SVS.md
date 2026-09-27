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
| Cruller | `_rt4k._tcp` | 80 | `id=<Pico id>`, `ver=<Cruller version>`, `api=/api` |
| Cruller | `_rfc2217._tcp` | 2217 | `id=<Pico id>` |
| Cruller | `_http._tcp` | 80 | (the web page) |
| SVS Bridge | `_svsbridge._tcp` | 443 | `id=svs-bridge-<mac>`, `version=1` (API version) |

`_rt4k._tcp` is generic on purpose: any RT4K bridge can announce it, and a Home Assistant integration
(hass-RT4K) can list `_rt4k._tcp.local.` in its manifest's `zeroconf` to be offered every one.

## The bridge reports the active input

The bridge browses `_rt4k._tcp`. To each Cruller it finds it sends, over plain HTTP:

```
POST http://<host>:<port>/api/svs
Content-Type: application/json

{"id": "svs-bridge-aabbccddeeff", "current_input": 3, "total_inputs": 8, "live": true}
```

- **When:** on every input change, as soon as it finds a Cruller (including one that just restarted
  and announced itself again), and every 60 s otherwise, in case a report was lost.
- **Fields:** the same names as the `svs` object of the bridge's `GET /api/v1/state`, plus `id`.
  `current_input` is required (`input` is accepted too); a port `name` may be added later.
- **Answer:** `{"ok": true, "changed": true|false}`. A repeat changes nothing on Cruller.
- **No token:** Cruller has no authentication anywhere (it's a LAN device, like its page and its
  RFC 2217 port), and it only records the report. The bridge's own API keeps its token.

Why the bridge pushes instead of Cruller polling it: the bridge's API is HTTPS with a token, and
Cruller would need a TLS client and the token for a single number. The bridge already has an HTTP
client (`esp_http_client`) and mDNS browsing (`mdns_query_ptr`) in ESP-IDF.

## What Cruller does with it

- `GET /api/svs`: what Cruller last heard:
  `{"known": true, "input": 3, "name": "", "id": "svs-bridge-…", "heard_s": 12, "since_s": 340}`
  (`heard_s`: seconds since the last report; `since_s`: since the input last changed).
- The `/status` JSON (and the page's status over the WebSocket) gets the same object as `"svs"` once a
  report has come in.
- gameID (coming): each console is assigned a switch input. Only the console on the active input
  changes the RT4K's profile, and switching inputs re-applies that console's game profile if it has
  one. Game profiles use their own SVS numbers (S100 and up) so they never overwrite the switch's
  S1…S8, and gameID sends nothing when there's no game (no "S0"), leaving the switch's profile.

## What the SVS Bridge needs

1. Browse `_rt4k._tcp` (ESP-IDF `mdns_query_ptr("_rt4k", "_tcp", …)`, repeated now and then, or on
   the mDNS announcements it sees).
2. For each result, `POST /api/svs` with the body above: on start, on each input change, every 60 s.
3. Optionally show the Crullers it found, and whether the last report went through, in its web UI.
