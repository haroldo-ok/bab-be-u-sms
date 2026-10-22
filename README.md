# Bab Be U — Sega Master System

A Sega Master System conversion of [**Bab Be U**](https://github.com/lilybeevee/bab-be-u), the Love2D fangame inspired by *Baba Is You*. The SMS engine and data converter in this repository turn the original levels, tile definitions, sprite pixels, palettes, XM music and WAV sound effects into a banked Master System ROM.

This is a data conversion rather than a small homage or placeholder remake: the converter processes the complete upstream corpus, and the ROM contains every level that fits the port's fixed SMS RAM/VRAM representation. Levels that cannot fit are still listed in the selector and are enumerated, with reasons, in [`CONVERSION.md`](CONVERSION.md).

## Current release

| Item | Value |
|---|---|
| Version | **SDSC 0.6** |
| Release ROM | `Bab-Be-U-SMS-v6.sms` |
| Build output | `babbeu.sms` |
| ROM size | 753,664 bytes (46 × 16 KiB banks; 736 KiB) |
| SHA-256 | `8b08f93071385f48a4534c92004adb83f78a7b5ef93e6bf85a660b198903c0a1` |
| Upstream Bab Be U commit | `bc707d9b630fbca6d1a66cabfb4e4efe1afa4849` |
| Toolchain | devkitSMS / SDCC |

SDSC 0.6 adds eight-way diagonal player movement. It also retains the SDSC 0.5 VBlank-interrupt audio player, the SDSC 0.4 performance work, the campaign-map navigation fix, and the corrected overworld `lvl`/`lin` graphics.

## Status and converted content

- **442 playable levels** out of 507 upstream levels.
- All 507 level names appear in the level selector in upstream/menu order. Selecting an unsupported level plays the failure sound and remains in the selector.
- 896 tile/object definitions, 980 converted shapes, 118 properties, 24 verbs, 39 conditions and 556 dictionary entries for letter-built words.
- 14 XM music tracks converted to PSG streams, plus 22 WAV-derived sound effects.
- Original 32×32 artwork is coverage-downscaled to 16×16 SMS cells, palette-quantised, baked into 2×2 tile groups, and composited at runtime for stacked objects.
- A 16×12-cell scrolling view, per-level palettes, active/inactive sentence colouring, wobble animation, title screen, campaign map, level selector, SRAM completion marks and SRAM-backed undo snapshots are implemented.
- Music, sound effects and the `sing` effect are played by a bank-safe PSG player driven from the **VBlank (frame) interrupt**, so audio keeps time while a long turn or redraw is still running.
- Gameplay movement is **eight-way**, including D-pad diagonals. The original `you`-only orthogonal restriction and the `diag`/`ortho` movement gates are preserved.

The 65 unplayable levels exceed the SMS engine's fixed limits for initial units, classes per level, or class-tile VRAM. See the unsupported-level table and known-fidelity notes in [`CONVERSION.md`](CONVERSION.md).

## Playing

Load the `.sms` ROM in a Sega Master System emulator or on suitable flash hardware that supports the normal Sega mapper and cartridge SRAM. SRAM is used for completion marks and in-level undo history; enable battery/SRAM saving in your emulator if you want progress marks to persist.

The released ROM has been verified headlessly with `smstest`. It has not been claimed as real-hardware tested by this conversion project.

### Title screen

- **Button 1:** start at the campaign map.
- **Button 2:** open the complete level selector.

### Level selector

- **Up/Down:** previous/next level.
- **Left/Right:** previous/next 14-level page.
- **Button 1:** load the selected level.
- **Button 2:** return to the title screen.

Selector navigation is intentionally cardinal-only.

### In a level

| Input | Action |
|---|---|
| D-pad | Move / take a turn |
| Up+Right, Down+Right, Down+Left, or Up+Left | Diagonal turn |
| Button 1 | Undo; hold to repeat undo |
| Button 2 | Wait a turn, or enter a level while standing on its `lvl` tile |
| Button 1 while holding Button 2 | Restart the level |
| Pause | Return to the level selector |

Whether a diagonal request succeeds still depends on the current rules. For example, an `ortho` unit cannot step diagonally unless it is also `diag`, and a `diag` unit cannot step cardinally unless it is also `ortho`.

## Building the ROM

The source archive includes the generated `gen/` data used by SDSC 0.6, so a normal build does **not** require a checkout of the original Love2D game.

### Requirements

- SDCC, providing `sdcc` and `sdasz80` (the release was built with SDCC 4.5.0).
- devkitSMS, providing SMSlib, PSGlib, `crt0_sms.rel`, the peephole rules and `ihx2sms`. By default the Makefile expects it at `~/.devkitsms`.
- GNU Make.
- Optional: `smstest` for the scripted regression and benchmark suite.
- Optional, only for regenerating `gen/`: Python 3 with Pillow and NumPy.

### Build

```sh
export PATH="$HOME/.devkitsms/bin:$PATH"
make clean
make babbeu.sms
```

If devkitSMS is installed elsewhere, override `DEVKIT`:

```sh
make DEVKIT=/path/to/devkitsms babbeu.sms
```

The resulting ROM is `babbeu.sms`. As a sanity check:

```sh
sha256sum babbeu.sms
ls -l babbeu.sms
```

Because the SDSC release date is generated by the toolchain, a rebuild on another date may differ in header/date bytes even when the game code and data are unchanged.

## Running the tests

The scripted tests use `smstest` and the freshly built `babbeu.noi` symbol file. Make sure the ROM has just been built so the symbols and listings correspond to it.

```sh
export PATH="$HOME/.devkitsms/bin:$PATH"

# Core gameplay flows
smstest babbeu.sms tests/playtest.txt
smstest babbeu.sms tests/first_level.txt
smstest babbeu.sms tests/text_push.txt
smstest babbeu.sms tests/map_nav.txt

# Representative levels from the other collections
smstest babbeu.sms tests/smoke_solo.txt
smstest babbeu.sms tests/smoke_minigame.txt
smstest babbeu.sms tests/smoke_gadget.txt

# VBlank audio and diagonal movement
smstest babbeu.sms tests/vbl_audio.txt
smstest babbeu.sms tests/diagonal.txt
smstest babbeu.sms tests/diagonal_ortho.txt

# Fixed-work performance scripts
smstest babbeu.sms tests/bench_first4.txt
smstest babbeu.sms tests/bench_wait4.txt
smstest babbeu.sms tests/bench_text1.txt
smstest babbeu.sms tests/bench_map4.txt
smstest babbeu.sms tests/bench_idle300.txt
```

Test screenshots are written to `shots/`; audio/profile logs are written to `perf/`.

Notable regression checks include:

- `tests/map_nav.txt` — leaving and re-entering campaign-map level nodes and following `lin` paths.
- `tests/text_push.txt` — breaking a sentence and confirming that its text dims after a reparse.
- `tests/vbl_audio.txt` — proving `audio_frame` is called from the frame interrupt while a turn/redraw is still in progress.
- `tests/diagonal.txt` — proving Down+Right and Up+Left change both BAB coordinates in a single turn.
- `tests/diagonal_ortho.txt` — proving an `ortho` BAB refuses a diagonal step but still moves cardinally.

## Regenerating the converted data

You only need this step if you want to reconvert the upstream game or modify the converter. The release conversion was produced from upstream commit `bc707d9b630fbca6d1a66cabfb4e4efe1afa4849`.

1. Check out matching upstream source:

   ```sh
   git clone https://github.com/lilybeevee/bab-be-u.git
   cd bab-be-u
   git checkout bc707d9b630fbca6d1a66cabfb4e4efe1afa4849
   cd ..
   ```

2. Install the converter's Python packages:

   ```sh
   python3 -m pip install pillow numpy
   ```

3. From this SMS project directory, run the converter and rebuild:

   ```sh
   python3 tools/convert.py /path/to/bab-be-u
   # or: make convert BABSRC=/path/to/bab-be-u
   make clean
   make babbeu.sms
   ```

The converter rewrites `gen/gamedata.c`, `gen/gamedata.h`, `gen/font.h`, the bank images and the JSON conversion metadata.

## Project layout

```text
.
├── README.md              This file
├── CONVERSION.md          Detailed conversion, verification and limits report
├── UPSTREAM-LICENSE.md    The original Bab Be U MIT licence text
├── Makefile               SDCC/devkitSMS banked ROM build
├── src/
│   ├── main.c             State machine, input, saves/undo and level loading
│   ├── engine.c/.h        Shared state, turn flow and property helpers
│   ├── parsebank.c        Sentence/rule parser (ROM bank 44)
│   ├── movbank.c          Transforms, movement and end-of-turn rules (bank 45)
│   ├── render.c           Tile baking, compositing, camera and board drawing
│   ├── audio.c            VBlank-interrupt PSG music/SFX player
│   ├── bankio.c           Slot-2 banked data reads and metadata loading
│   └── propfast.s         Z80 assembly hot paths
├── tools/
│   ├── babdata.py         Upstream Bab Be U data parser
│   ├── convert.py         Main upstream-to-SMS data converter
│   ├── xm2psg.py          XM-to-PSG music-stream converter
│   ├── wav2sfx.py         WAV-to-PSG sound-effect converter
│   └── prof_bank.py       Bank-aware call-graph profile report
├── gen/                   Generated level/audio/shape banks and C metadata
└── tests/                 smstest gameplay, regression and benchmark scripts
```

## Implementation notes

- Fixed code occupies ROM banks 0–1; converted data occupies banks 2–43; the parser is in bank 44; movement/end-of-turn code is in bank 45.
- The SMS engine's main fixed limits are 255 units, 36 classes per level, six stored properties per class, four relations per class, 20 conditional rules and 16 paint records.
- The parser caches word state so ordinary moves can reuse the current rules. Parse-time summaries and a small amount of Z80 assembly keep common turns fast; the full measurements are in `CONVERSION.md`.
- The audio interrupt handler saves and restores both the slot-2 ROM bank and SRAM mapper state, and main-thread audio state changes are made in brief interrupt-disabled sections.

## Release history

- **SDSC 0.6** — eight-way diagonal movement, including `you`/`diag`/`ortho` restrictions.
- **SDSC 0.5** — music/SFX playback moved to the VBlank interrupt.
- **SDSC 0.4** — major turn and rendering performance improvements.
- **SDSC 0.3** — campaign-map level traversal and re-entry fixed.
- **SDSC 0.2** — original-derived overworld `lvl`/`lin` graphics fixed.

## Credits and licence

- *Bab Be U* is by [lilybeevee](https://github.com/lilybeevee/bab-be-u) and contributors. The upstream MIT licence text is included in [`UPSTREAM-LICENSE.md`](UPSTREAM-LICENSE.md).
- *Bab Be U* is a fangame inspired by Arvi “Hempuli” Teikari's *Baba Is You*.
- The Master System build uses devkitSMS/SMSlib/PSGlib and SDCC. Scripted verification was performed with `smstest` from the Sega Master System development skill/tooling.

See [`CONVERSION.md`](CONVERSION.md) for the complete conversion report, including content counts, performance results, every unsupported level, and the known semantic differences that remain.
