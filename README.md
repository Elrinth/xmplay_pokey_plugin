# xmp-pokey 1.0.0

Native **32-bit** XMPlay input plugin for Atari 8-bit **POKEY** music.

Engine: official **ASAP** (Another Slight Atari Player) by Piotr Fusik,
compiled in statically (`asap.c`). No extra DLLs.

**This is not the official ASAP XMPlay plugin** (support.xmplay.com file 638,
xmp-asap 6.0.3). That plugin leaves songs without a `TIME` tag — for example
older rips of *Spy vs Spy* — playing forever (`ASAP_PlaySong(..., -1)`).
`xmp-pokey` uses the same ASAP engine but is a **separate** input plugin so
those songs get a real length, plus loop 1/2/3 and POKEY mute.

Intended later home: `Elrinth/xmplay_pokey_plugin` (not published from this
build). Do not confuse the DLL name with `xmp-asap.dll` / `xmp-sap.dll`.

## Install

Copy `xmp-pokey.dll` next to `xmplay.exe` (or into XMPlay's plugin folder)
and restart XMPlay. Classic XMPlay is **32-bit only** — this DLL is PE32
i386. A 64-bit build will not load.

XMPlay's *Supported file types* list shows **Atari SAP / POKEY** with
extensions `sap/cmc/cm3/cmr/cms/dmc/dlt/fc/mpt/mpd/rmt/tmc/tm8/tm2`.

## Formats

| Extension | Notes |
|-----------|--------|
| `.sap`    | Slight Atari Player (header + original player) |
| `.cmc` `.cm3` `.cmr` `.cms` `.dmc` | Chaos Music Composer family |
| `.dlt`    | Delta Music Composer |
| `.fc`     | Atari Future Composer only — Amiga `.fc` is rejected |
| `.mpt` `.mpd` | Music ProTracker |
| `.rmt`    | Raster Music Tracker |
| `.tmc` `.tm8` `.tm2` | Theta Music Composer |

CheckFile **loads** the file with ASAP (`ASAP_Load`), it does not trust the
extension. SAP modules are tiny (max 65 KB) so the whole file is slurped.

## Length + loops

When ASAP reports `GetDuration(song) >= 0` (a `TIME` tag):

- looping songs play `duration × loop_count`
- non-looping songs play the tagged duration once (extra loops ignored)

`loop_count` is **1, 2, or 3** (default **2**), set in the plugin config.

When duration is unknown (`-1`, the official-plugin infinite-play case):

1. `ASAP_DetectSilence(2)`
2. skip-render until generate returns 0 (silence) or a **10-minute** cap
3. that position is one-loop length, then multiplied by `loop_count`
4. reload and `PlaySong(song, play_ms)` so playback actually ends

The plugin **never** calls `ASAP_PlaySong(..., -1)`. The playhead is
seekable (`SetLength(seconds, TRUE)`).

## Seeking

`ASAP_Seek` with 1 ms granularity. Loop / auto-loop return −2 to XMPlay.

## Multi-track

Files with several songs become NSF-style **tracks**. Use **Shift+Left** /
**Shift+Right** in XMPlay (same as NSF / xmp-sc68). Title updates on change.

## Mute

Config checkboxes mute POKEY 1–4 (bits 0–3) and extra/stereo POKEY 1–4
(bits 4–7) via `ASAP_MutePokeyChannels`. Applied on start, seek, and
config change.

## Credits

- ASAP (Another Slight Atari Player) — Piotr Fusik
- CMC, MPT, TMC, TM2 players — Marcin Lewandowski
- RMT player — Radek Sterba
- DLT player — Marek Konopka
- CMS player — David Spilka
- FC player — Jerzy Kut
- XMPlay plugin SDK — un4seen / Ian Luck
- ASMA (Atari SAP Music Archive) — asma.atari.org

License: GPLv2 or later (ASAP is GPLv2+).
