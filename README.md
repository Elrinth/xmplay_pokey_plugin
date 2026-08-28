# xmp-pokey 1.0.4

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
and restart XMPlay. The DLL carries a Windows VERSIONINFO resource (FILEVERSION 1.0.4.0) so XMPlay can include it in update notifications. Classic XMPlay is **32-bit only** — this DLL is PE32
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
A truncated last COM block is padded so TYPE B rips like Bomb Song load.

## Length + loops

Lengths are **measured**, not trusted blindly:

- A real `TIME` tag (anything other than 3:00) is kept. Example:
  *Spy vs Spy* `TIME 00:19.25` stays 19250 ms.
- Missing `TIME` (`GetDuration` = −1) or the **3:00 stub**
  (`180000` ms, also 179000–181000) is silence-detected:
  `ASAP_DetectSilence(2)`, generate until silence or a **10-minute** cap.
  If the stub was 3:00 and detect hits the cap (true long/looping tune),
  the tagged 3:00 is kept instead of reporting 10 minutes.
- Official xmp-asap's default song length is 180 seconds; ASMA often
  stamps `TIME 03:00` as a dummy. This plugin does not treat that as a
  measured loop.

`loop_count` is **1, 2, or 3** (default **1**), set in the plugin config.

- looping songs play `one_loop × loop_count`
- non-looping tagged songs play the tagged duration once
- detected (unknown / dummy) songs play `one_loop × loop_count`

Playlist / file-info length is the **calculated one-loop** duration.
GetFileInfo always runs this measurement so the list is not XMPlay's
3:00 fallback. Extra loops still play when configured — the seekbar
uses `SetLength(play_ms)` — but the playlist shows one loop.

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
