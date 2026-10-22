# Bab Be U — Sega Master System conversion report

## Deliverable

- ROM: `Bab-Be-U-SMS-v6.sms` (also copied to `Bab-Be-U-SMS.sms`; built as `bab-be-u-sms/babbeu.sms`)
- Size: 753,664 bytes (46 × 16 KiB banks; 736 KiB)
- SHA-256: `8b08f93071385f48a4534c92004adb83f78a7b5ef93e6bf85a660b198903c0a1`
- Version: SDSC 0.6 (diagonal-movement update; includes the SDSC 0.5 VBlank-interrupt audio update, the SDSC 0.4 performance update, the SDSC 0.3 campaign-map navigation fix and the SDSC 0.2 overworld `lvl`/`lin` graphics fix)
- Header/checksum: generated and updated by `ihx2sms`
- Upstream source: `lilybeevee/bab-be-u` at commit `bc707d9b630fbca6d1a66cabfb4e4efe1afa4849`
- SMS implementation: C/assembly for devkitSMS/SDCC, with the rule parser and movement/end-of-turn simulation in switchable ROM banks.

This is a real data conversion, not a placeholder remake: level files, tile definitions, sprite pixels, palettes, music and sound effects were parsed from the upstream project and converted into SMS formats. The SMS hardware limits do, however, make 65 of the 507 upstream levels unplayable in this build; they remain visible in the selector and are enumerated below rather than being silently replaced.

## Converted content

| Content | Converted |
|---|---:|
| Tile/object definitions | 896 |
| Properties | 118 |
| Verbs | 24 |
| Conditions | 39 |
| Dictionary entries for letter-built words | 556 |
| Music tracks | 14 XM tracks converted to PSG data (plus silence) |
| Sound effects | 22 WAV effects converted to PSG effects |
| Playable levels | 442 of 507 |

Sprites were derived pixel-by-pixel from the original 32×32 PNG artwork and coverage-downscaled to the SMS port's 16×16 cells (3-of-4 coverage for text, 2-of-4 for objects), then palette-quantised and baked into 2×2 groups of SMS tiles. Stacked cell graphics are composited at runtime through an eight-slot VRAM cache. The playfield shows 16×12 cells and scrolls with a camera on larger levels.

### Levels by collection

| Collection | Playable | Upstream total |
|---|---:|---:|
| Campaign (bab be u) | 239 | 249 |
| solo levels | 153 | 163 |
| minigames | 36 | 60 |
| gadgets | 14 | 35 |
| **Total** | **442** | **507** |

The level selector contains all 507 names in upstream/menu order. Selecting one of the 65 unsupported levels plays the failure sound and stays in the selector.

## Implemented game systems

- Dynamic sentence parsing with nouns, properties, verbs, negations, groups, conditions and relational rules.
- Dynamic `txt_…` classes, `n't` handling and longest-prefix segmentation for words built from letter tiles.
- Transformations, recursive pushing, movement rules, icy/glued behaviour, hop-over interactions and end-of-turn win/death/creation/paint effects.
- Undo snapshots, level restart, world/level entrances, title screen, level select and SRAM-backed completion marks.
- Upstream overworld rendering for `lvl`/`lin`: hidden paths use outline sprites, locked nodes use the locked artwork, open nodes show their number/dots/letter/custom icon, and cleared open nodes switch to the won artwork.
- Per-level palettes, active/inactive text colouring, wobble animation, camera scrolling, music and sound effects.
- VBlank-interrupt audio: the converted music, sound-effect and `sing` streams share one PSG player ticked by SMSlib's frame interrupt handler once per display frame. The handler preserves the interrupted slot-2 ROM bank and SRAM mapper state, and the main-thread audio API swaps stream state with interrupts briefly disabled, so playback keeps time during multi-frame turns without disturbing banked code or undo snapshots.
- Eight-way player movement: gameplay D-pad input combines the vertical and horizontal axes, so holding two adjacent directions requests the corresponding diagonal turn. The original's restrictions are preserved: `you`-only control remains orthogonal-only, and `diag`/`ortho` properties forbid cardinal/diagonal steps respectively for both self-movement and pushed units.
- Word-state parse caching: ordinary moves that do not move a word reuse the rule set, while pushing/breaking a word forces a reparse.
- Parse-time work summaries let ordinary turns skip direction, transform, relation, mover-collection and end-of-turn scans that cannot apply, while conditional/GIV rules retain the original property-evaluation path.
- Z80 assembly fast paths rebuild unit row chains, clear draw cells and find changed view cells; the renderer still uses the same baked tiles and composite cache.

## Controls

| Input | Action |
|---|---|
| D-pad | Move / take a turn (8-way; hold two adjacent directions for a diagonal) |
| Button 1 | Undo (hold to repeat); confirm in menus |
| Button 2 | Wait a turn, or enter a level when standing on `lvl`; back in level select |
| Button 1 while holding Button 2 | Restart level |
| Pause | Return to level select |

On the title screen, Button 1 starts the campaign map and Button 2 opens the full level selector. In the selector, Up/Down move by one level and Left/Right move by one 14-level page; selector navigation remains cardinal.

## SMS runtime limits

The original game targets a modern Lua runtime; the Master System has 8 KiB RAM and 16 KiB VRAM. The engine therefore uses fixed limits:

| Resource | Limit |
|---|---:|
| Simultaneous units | 255 |
| Classes per level | 36 |
| Stored properties per class | 6 |
| Stored relations per class | 4 |
| Conditional rules | 20 |
| Paint records | 16 |

ROM layout: banks 0–1 contain fixed code/data (31,655 bytes in the fixed `_CODE` segment; 32,326 of 32,768 bytes used across the two banks), banks 2–43 contain converted game data, bank 44 contains the parser (9,477 bytes), and bank 45 contains movement/end-of-turn code (16,166 bytes). RAM data occupies 7,752 bytes from `0xC000` (ending at `0xDE48`); the lowest stack pointer observed in the SDSC 0.6 benchmark/regression set was `0xDEC1`, leaving a 121-byte gap in that test. The entrance-special record limit is 40, covering the largest included map (38 entrances).

## Verification performed

The final ROM was exercised headlessly with `smstest`:

- `tests/playtest.txt`: title → level select → campaign map → movement → undo, plus `expectpace 0` (no too-fast VRAM writes during active display). The campaign-map screenshot was visually checked for hidden/locked/open node and path artwork.
- `tests/map_nav.txt`: starting on the first campaign level node, verifies the selector cannot step off the path, follows `lin` cells to the prison node, returns into the first `lvl` cell, travels to prison again, and enters it (level 65). The script is synchronised to completed `turn_no`/`board_dirty` transitions rather than fixed frame holds. Screenshots also verify the curse selector is drawn on top of occupied level nodes.
- `tests/first_level.txt`: loads level 0 (`bab be u`), verifies 19 classes/75 units, pushes the rock, reaches the flag, wins, returns to level select, checks stack depth, and passes `expectpace 0`.
- `tests/text_push.txt`: loads level 117 (`overruled 2`), pushes `BAB` text, and screenshot comparison confirms the broken sentence dims after reparse.
- `tests/smoke_solo.txt`, `tests/smoke_minigame.txt`, `tests/smoke_gadget.txt`: load and render representative levels from the other three collections.
- `tests/vbl_audio.txt`: verifies `SMS_theFrameInterruptHandler` is registered (27739 = `0x6C5B` = `_audio_frame` in this build), then starts hook/PSG logging after `turn_no` has incremented for a level-0 turn. It recorded 17 `audio_frame` entries and PSG writes on frames 1030, 1040 and 1041 before that single turn's redraw completed, proving the player runs from VBlank while the main thread is busy. It also passes `expectpace 0`.
- `tests/diagonal.txt`: on level 0, holds Down+Right and then Up+Left; BAB (unit 10) moves `(6,7) -> (7,8) -> (6,7)`, proving both coordinates change in one turn rather than snapping to a cardinal direction.
- `tests/diagonal_ortho.txt`: on level 102 (`baba is you`), BAB starts with `u` + `ortho`; a Down+Right request leaves it at `(6,7)`, while Right moves it to `(7,7)`, verifying the diagonal movement gate.
- The five fixed-work profiles in `tests/bench_*.txt` were run on SDSC 0.3 and SDSC 0.4 for the performance comparison below, then rerun on SDSC 0.5 as `perf/vbl-*.prof` and on SDSC 0.6 as `perf/diag-*.prof`. Their whole-window totals include audio ticks on every VBlank; the SDSC 0.6 diagonal/gate changes had no material performance effect (for example, `bench_first4` was 5,178,279 cycles versus 5,178,515 on SDSC 0.5, with `draw_board` at 460,034 cycles/call).
- All 42 generated data banks were byte-compared against the corresponding banks in the final ROM.

Map navigation implements the upstream `curse` movement reason: at the top level only, a curse-controlled unit may move onto an open level entrance, a `lin` path cell, or a destination satisfying its `liek` condition, and `nogo` does not block that destination. Locked/hidden level entrances and cells outside the path network refuse the move. This matches the original selector behaviour while leaving ordinary `u`-controlled movement unchanged.

## Performance (SDSC 0.4 optimisations, retained in SDSC 0.6)

The game remains turn-based rather than smooth-scrolling, but the measured turn path is substantially faster. Profiles below compare the delivered SDSC 0.3 ROM with SDSC 0.4 over the same scripted, completed-turn windows. Cycle totals are for the whole benchmark window; the per-turn figures divide the four-turn windows by four. They include input repeat/VBlank pacing imposed by those scripts, so they are best read as fixed-work comparisons rather than a promise for every level.

| Benchmark | Work | SDSC 0.3 cycles/turn | SDSC 0.4 cycles/turn | Speedup |
|---|---:|---:|---:|---:|
| `bench_first4` | 4 held-right turns, level 0 | 3,119,358 | 1,264,258 | 2.47× |
| `bench_wait4` | 4 WAIT turns, level 0 | 2,332,456 | 677,455 | 3.44× |
| `bench_text1` | 1 text-push/reparse turn, level 117 | 1,781,958 | 1,168,062 | 1.53× |
| `bench_map4` | 4 selector moves, campaign map | 2,674,292 | 1,187,043 | 2.25× |
| `bench_idle300` | 300 idle frames, level 0 | 18,660,898 total | 18,660,898 total | unchanged (VBlank-bound) |

Representative hot paths changed as follows: level-0 `do_turn` fell from 1,774,445 to 311,046 cycles on held-right turns (5.70×) and from 1,648,036 to 243,512 on WAIT turns (6.77×); campaign-map `do_turn` fell from 1,269,900 to 225,680 (5.63×). `draw_board` fell from 826,041 to 460,030 cycles/draw on level 0 and from 924,780 to 505,696 on the campaign map. Composite generation roughly halved (804,663 → 399,138 cycles on level 0; 758,489 → 387,749 on the map), and assembly `build_rows` fell from about 42k to 14.4k cycles/call on level 0. In benchmark-window time at 59.94 Hz, a held-right level-0 turn is about 0.35 s instead of 0.87 s; a WAIT turn is about 0.19 s instead of 0.65 s.

The main optimisations are: exact parse-time summaries for direction work, transform work, relation families, end-of-turn/U-cell effects, mover candidates, uniform-flye fast paths and next-parse word eligibility; cached blocker answers in recursive movement; fewer and cheaper word-state hashes on ordinary turns; assembly row rebuilding/draw-cell clearing/changed-cell scanning; pointer-based composite, quadrant packing, bank-copy and undo snapshot loops; sequential name-table writes for 2×2 cells; and exact early exits for entrance lookup when a level has no entrance specials and for camera work after turns on view-sized boards at the origin. The parser bank itself was measured unchanged on the level-117 benchmark (675,503 cycles); the surrounding parse wrapper costs more because it now builds the summaries, but the complete reparse turn is still faster overall.

Raw logs, dumps and reports are retained under `perf/` (`bank-baseline-*.prof`, `final-*.prof`, `vbl-*.prof`, `diag-*.prof` and the intermediate stage files). The local profiler was patched only to bank-qualify slot-2 call-graph addresses so parser/movement functions are attributed correctly; the game ROM contains no profiler code. The VBlank audio introduced in SDSC 0.5 and retained in SDSC 0.6 means whole benchmark windows include the per-frame player work even while a turn is still running (84 `advance_music` calls in the four-turn level-0/map windows), rather than only when the main loop next reaches VBlank.

## Known fidelity limitations

- 65 levels exceed the unit/class/VRAM limits and are not playable (listed below).
- The upstream level editor and mouse-oriented UI are not included; mouse-only words are therefore inert on the controller-only SMS build.
- Ellipsis and parenthesis grammar are not parsed as completely as in the original.
- `corekt`, `rong`, `the`, `an`, `deez` and `nuek` are approximations of the original semantics.
- `tryagain` clears the undo history.
- Composite-tile cache keys do not include paint coordinates.
- Very dense dynamic levels can hit the fixed unit/property/relation limits during play even when their initial state fits.

## Unsupported levels

Reason key: the converter processed every upstream level. A level below was excluded only when its initial state could not fit the SMS engine's fixed RAM/VRAM representation.

| ID | Upstream level | Units | Classes | Class VRAM tiles | Reason |
|---:|---|---:|---:|---:|---|
| 16 | `bab be u/meta+/infloop/Map` | 284 | 29 | 212 | More than 255 initial units |
| 31 | `bab be u/meta+/infloop/wal be seeping` | 379 | 25 | 196 | More than 255 initial units |
| 32 | `bab be u/meta+/infloop/weebs don't interact` | 376 | 15 | 112 | More than 255 initial units |
| 70 | `bab be u/w1 (beach)/beach` | 335 | 26 | 156 | More than 255 initial units |
| 74 | `bab be u/w1 (beach)/esac erats` | 438 | 19 | 140 | More than 255 initial units |
| 93 | `bab be u/w1 (beach)/stare caser` | 438 | 18 | 132 | More than 255 initial units |
| 180 | `bab be u/w5 (hole)/remote txt` | 282 | 16 | 132 | More than 255 initial units |
| 196 | `bab be u/w6 (city)/faster than light` | 179 | 37 | 268 | More than 36 classes or class-tile VRAM budget exceeded |
| 231 | `bab be u/w7 (heaven)/obses` | 54 | 38 | 292 | More than 36 classes or class-tile VRAM budget exceeded |
| 243 | `bab be u/w8 (grove)/f is for` | 292 | 32 | 244 | More than 255 initial units |
| 249 | `gadgets/5 step steev` | 207 | 43 | 320 | More than 36 classes or class-tile VRAM budget exceeded |
| 250 | `gadgets/a ditto` | 150 | 94 | 744 | More than 36 classes or class-tile VRAM budget exceeded |
| 255 | `gadgets/chessboard` | 191 | 63 | 468 | More than 36 classes or class-tile VRAM budget exceeded |
| 256 | `gadgets/colors` | 116 | 48 | 352 | More than 36 classes or class-tile VRAM budget exceeded |
| 257 | `gadgets/counter` | 244 | 45 | 340 | More than 36 classes or class-tile VRAM budget exceeded |
| 258 | `gadgets/duane _the roc_ johnson` | 561 | 1 | 4 | More than 255 initial units |
| 259 | `gadgets/every1 be sans` | 68 | 68 | 480 | More than 36 classes or class-tile VRAM budget exceeded |
| 260 | `gadgets/every1 be slep` | 118 | 189 | 1428 | More than 36 classes or class-tile VRAM budget exceeded |
| 261 | `gadgets/every1 sing c` | 154 | 139 | 988 | More than 36 classes or class-tile VRAM budget exceeded |
| 262 | `gadgets/feature testing` | 96 | 42 | 308 | More than 36 classes or class-tile VRAM budget exceeded |
| 266 | `gadgets/lvl be` | 93 | 40 | 304 | More than 36 classes or class-tile VRAM budget exceeded |
| 267 | `gadgets/main menu` | 124 | 48 | 376 | More than 36 classes or class-tile VRAM budget exceeded |
| 270 | `gadgets/ms paint deluxe` | 302 | 50 | 388 | More than 255 initial units |
| 271 | `gadgets/ms paint deluxer` | 458 | 76 | 572 | More than 255 initial units |
| 272 | `gadgets/ms paint` | 480 | 66 | 488 | More than 255 initial units |
| 274 | `gadgets/object palette` | 174 | 174 | 1072 | More than 36 classes or class-tile VRAM budget exceeded |
| 278 | `gadgets/poor toll gunne` | 182 | 47 | 352 | More than 36 classes or class-tile VRAM budget exceeded |
| 279 | `gadgets/prop palette` | 133 | 125 | 992 | More than 36 classes or class-tile VRAM budget exceeded |
| 280 | `gadgets/slots` | 172 | 63 | 460 | More than 36 classes or class-tile VRAM budget exceeded |
| 282 | `gadgets/spirograph` | 84 | 42 | 324 | More than 36 classes or class-tile VRAM budget exceeded |
| 283 | `gadgets/what level should you make` | 317 | 177 | 1388 | More than 255 initial units |
| 284 | `minigames/2048 is a big number _(` | 367 | 50 | 392 | More than 255 initial units |
| 285 | `minigames/Rif Collapse` | 266 | 64 | 488 | More than 255 initial units |
| 287 | `minigames/beginner` | 352 | 53 | 388 | More than 255 initial units |
| 288 | `minigames/better bab snek` | 267 | 53 | 412 | More than 255 initial units |
| 298 | `minigames/cleste` | 138 | 43 | 332 | More than 36 classes or class-tile VRAM budget exceeded |
| 300 | `minigames/color maze 1` | 167 | 38 | 268 | More than 36 classes or class-tile VRAM budget exceeded |
| 307 | `minigames/hedge be maze` | 939 | 85 | 600 | More than 255 initial units |
| 310 | `minigames/insane` | 352 | 53 | 388 | More than 255 initial units |
| 311 | `minigames/intermediate` | 352 | 53 | 388 | More than 255 initial units |
| 318 | `minigames/n step steve` | 287 | 58 | 416 | More than 255 initial units |
| 320 | `minigames/pong 2p` | 165 | 37 | 288 | More than 36 classes or class-tile VRAM budget exceeded |
| 322 | `minigames/pong` | 168 | 40 | 308 | More than 36 classes or class-tile VRAM budget exceeded |
| 323 | `minigames/robbery` | 192 | 40 | 304 | More than 36 classes or class-tile VRAM budget exceeded |
| 325 | `minigames/slenderbab` | 129 | 47 | 380 | More than 36 classes or class-tile VRAM budget exceeded |
| 326 | `minigames/space inboiders` | 351 | 55 | 416 | More than 255 initial units |
| 327 | `minigames/square fill 1` | 319 | 44 | 304 | More than 255 initial units |
| 328 | `minigames/square fill 2` | 261 | 49 | 340 | More than 255 initial units |
| 329 | `minigames/square fill 3` | 401 | 50 | 348 | More than 255 initial units |
| 330 | `minigames/square fill 4` | 274 | 54 | 376 | More than 255 initial units |
| 331 | `minigames/square fill 5` | 395 | 54 | 376 | More than 255 initial units |
| 332 | `minigames/square fill 6` | 334 | 56 | 388 | More than 255 initial units |
| 337 | `minigames/sudoku` | 517 | 84 | 576 | More than 255 initial units |
| 342 | `minigames/wave in bab` | 449 | 23 | 176 | More than 255 initial units |
| 343 | `minigames/whack a bab` | 174 | 41 | 304 | More than 36 classes or class-tile VRAM budget exceeded |
| 344 | `solo levels/DIY stairs` | 152 | 52 | 388 | More than 36 classes or class-tile VRAM budget exceeded |
| 397 | `solo levels/campaign/drifting` | 258 | 19 | 140 | More than 255 initial units |
| 417 | `solo levels/campaign/troll/bab get cleen` | 628 | 46 | 340 | More than 255 initial units |
| 418 | `solo levels/campaign/troll/false truth` | 226 | 51 | 384 | More than 36 classes or class-tile VRAM budget exceeded |
| 420 | `solo levels/campaign/troll/merrim_weebster` | 225 | 46 | 352 | More than 36 classes or class-tile VRAM budget exceeded |
| 470 | `solo levels/scraps/i don c no pumkin!` | 163 | 44 | 312 | More than 36 classes or class-tile VRAM budget exceeded |
| 496 | `solo levels/taek noetts` | 6143 | 35 | 260 | More than 255 initial units |
| 499 | `solo levels/tim's true power___` | 130 | 40 | 312 | More than 36 classes or class-tile VRAM budget exceeded |
| 501 | `solo levels/twin stick controls` | 273 | 20 | 148 | More than 255 initial units |
| 505 | `solo levels/with our powers combined___` | 339 | 70 | 532 | More than 255 initial units |

## Rebuilding

From `bab-be-u-sms/` with SDCC and the devkitSMS tools on `PATH`:

```sh
export PATH="$HOME/.devkitsms/bin:$PATH"
make babbeu.sms
smstest babbeu.sms tests/playtest.txt
smstest babbeu.sms tests/first_level.txt
smstest babbeu.sms tests/text_push.txt
smstest babbeu.sms tests/map_nav.txt
smstest babbeu.sms tests/vbl_audio.txt
smstest babbeu.sms tests/diagonal.txt
smstest babbeu.sms tests/diagonal_ortho.txt
# Fixed-work performance profiles (requires the bank-aware profiler in perf/):
perf/smstest-bankprof babbeu.sms tests/bench_first4.txt
perf/smstest-bankprof babbeu.sms tests/bench_wait4.txt
perf/smstest-bankprof babbeu.sms tests/bench_text1.txt
perf/smstest-bankprof babbeu.sms tests/bench_map4.txt
```

The converter sources are in `tools/`, generated ROM banks and metadata in `gen/`, and the SMS engine source in `src/`.
