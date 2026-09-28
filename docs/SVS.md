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
| Cruller | `_rt4k._tcp` | 80 | `id=<Pico id>`, `ver=<Cruller version>`, `api=/api`, `name=<name>` (once named) |
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

## Names and pairing (several SVS and RT4Ks)

- **Each Cruller has a name**, set in its setup wizard or the Cruller tab ("Living", "Game room"). It
  announces itself as "Cruller Living" on `cruller-living.local`, with `name=Living` in the `_rt4k`
  TXT. An unnamed Cruller is plain `cruller.local`.
- **The bridge picks its Cruller**: its web UI lists the `_rt4k._tcp` it finds (name, id) and the user
  chooses one; the bridge stores that Cruller's `id` (not its address) and reports only to it.
- **Cruller keeps the first bridge that reports** (with an `id`) in its settings. Reports from another
  bridge get `409 {"ok": false, "error": "paired with another SVS Bridge", "paired": "<id>"}`, so a
  bridge set up for another RT4K can't change this one's profiles. `POST /api/svs/unpair` (the Unpair
  button in the Cruller tab) frees it; a factory reset does too.

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
2. In its web UI, list the Crullers found (TXT `name`, `id`) and let the user pick one; store its `id`.
3. `POST /api/svs` to the picked one (found by `id` each time) with the body above: on start, on each
   input change, every 60 s. Show whether the last report went through, and a `409` as "this Cruller
   is paired with another bridge".
