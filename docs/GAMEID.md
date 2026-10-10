# gameID

gameID loads a game's own RT4K profile when a console reports which game it runs. DonutShop did it
before Cruller ([margaale/DonutShop](https://github.com/margaale/DonutShop)); this is how Cruller does
it, and how far it has got.

Status: working. The consoles and the gameDB are kept, with their API (`/api/v1/gameid`,
[API.md](API.md)); the page's Consoles view puts each console under its SVS input, edits them and the
games, and shows what Cruller knows; Cruller asks the consoles and loads the profiles. Finding consoles on the network, `.local` names and HTTPS come next.

## The consoles

A console with a Wi-Fi add-on answers which game it runs over HTTP. Cruller asks each one it knows
every 2 s, one after another (its own task, `gameid_run.c`): a console that doesn't take the connection
within 0.8 s, or misses two rounds, is off, so one that's off doesn't hold the others up.

| Device | What's asked | The answer |
|---|---|---|
| MemCard PRO2 (PS1, PS2), firmware 1.6.0 | `http://<address>/api/currentState` | JSON: `{"currentMode": "PS2", "gameName": "God of War II", "gameID": "SCUS-97481", ...}` |
| MemCard PRO (PS1, GameCube), firmware 3.x | the same | JSON with `gameID` |
| PS1Digital, N64Digital | `http://<address>/gameid` | the ID as text |

- A MemCard PRO2 serves this only with its own web page on: with WebUI v2 (a page on the internet
  that reaches the card) off. Its FTP server answers `220 MemCardPRO FTP Server`. It doesn't announce
  itself over mDNS.
- The MemCard PRO's firmware 2.x answers over HTTPS only, with its own certificate: not supported yet.
- An answer is the console on and its game. No answer (refused, timed out) is the console off. Any
  other answer (an error, a body that isn't one) changes nothing.

## The gameDB

Each game: the ID a console reports, compared as written (`SCUS-97481`, an N64's
`3E5055B6-2E92DA52-N-45`), the profile to load, and a name. 1000 games at most. A game the gameDB
hasn't loads its console's own profile ("other"), when it has one.

A profile is a `.rt4` or `.rt6` on the RT4K's SD card, its path under `/profile`. Cruller loads it with
`prof load <path>`: the RT4K answers whether it did, it doesn't depend on Auto Load SVS, and it never
overwrites the SVS's `S1`…`S8` (DonutShop sent `SVS NEW INPUT=n` or `remote prof<n>` instead).

## Several consoles, and an SVS switch

- The last console whose game changed wins, as in DonutShop.
- With an [SVS switch](SVS.md), a console counts only while its input is on screen: the input it's
  added under in the Consoles view (`svs_input`), or on Auto (`0`, as the API takes it) the input the
  SVS Bridge says has that console (a PRO2 says `"currentMode": "PS2"`). Two inputs with the same
  console on Auto: the last that changed wins (the page shows a console on Auto under its input
  only when just one has its console, and saving it sets that input). Back on a console's input, after the SVS's own `S<n>` loads,
  Cruller loads its game's profile again.
- A console not on the SVS (a PS1Digital or an N64Digital on HDMI): set so (`svs_input` -1), it counts
  while the RT4K shows another input than the SVS's. The RT4K doesn't say when its input changes, so
  while there's one, Cruller asks it every round (a bare `input` only reads it: `input=0 HDMI ...`) and
  tells the SVS's by the output the bridge says goes to the RT4K (VGA: an `HD15` input; SCART; component:
  `RCA YPbPr`; never HDMI). Back on the SVS's input, as after an input change, 3 s later.
- A console turning off: with an SVS, its input's profile again (while the RT4K shows the SVS); without,
  nothing.
- The RT4K asleep: the profile is kept and loaded once it's on.

## Its address changed

A console's address may change (DHCP gave it another). Cruller learns each console's MAC when it
answers (from its ARP table: only a console on the same local network as Cruller has one there) and
keeps it with it. When the console the RT4K may be showing doesn't answer for 30 s while the RT4K is
on, Cruller looks for its MAC on the local network (an ARP request to each address of its /24, a few at
a time, about 5 s) and, found at another address, saves it there. Which console: with an SVS input
active and the RT4K on the SVS, the one on that input; with none active, or the RT4K on another input,
those not on the SVS. Again every 2 min at most. It never adds a console: it only finds those it has.

## Where it's kept

`gameid-consoles.json` and `gameid-games.jsonl` in Cruller's files (`cfgfs.h`): littlefs in the Pico's
data partition after the small records (300 KB), or in the ESP32's `cruller` partition (1 MB). The
gameDB is a game a line, so it's searched and rewritten a line at a time, never held in RAM whole;
each file is replaced whole (written beside it, then renamed), so a power cut leaves the old one or the
new one. A factory reset erases both. DonutShop's settings aren't imported.
