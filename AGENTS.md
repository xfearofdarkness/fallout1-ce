# AGENTS.md — notes for agents working on fallout1-ce

This is the Fallout 1 Community Edition engine (`fallout-ce`): a C++17
re-implementation of `falloutw.exe` v1.1, reconstructed from the original
binary, running on SDL2. Upstream is `alexbatalov/fallout1-ce`; this fork is
`xfearofdarkness/fallout1-ce`. The sister project `alexbatalov/fallout2-ce`
(F2) shares most of the engine and is better maintained. Compare against it
often (see "Relation to fallout2-ce").

`REVIEW.md` holds the most recent code review: the bug list, the Skilldex
crash analysis and a per-module checklist. Read it before you change memory
handling.

## Repository layout

| Path | What lives there |
|------|------------------|
| `src/plib/gnw/` | "GNW" window/graphics library: windows and buttons (`gnw.cc`, `button.cc`), blitters (`grbuf.cc`), bitmap fonts (`text.cc`), input/keyboard/mouse (`input.cc`, `kb.cc`, `mouse.cc`, `touch.cc`), SDL video (`svga.cc`), dialogs/menus (`intrface.cc`), the debug log (`debug.cc`), tracked malloc (`memory.cc`), VCR recording (`vcr.cc`), `main()` (`winmain.cc`). |
| `src/plib/db/` | DAT archive and file I/O (`db.cc`), LZSS decompression (`lzss.cc`). All game file access goes through `db_fopen`/`db_fread`. |
| `src/plib/color/` | Palette and color tables (`colorTable`, `intensityColorTable`, blend tables). |
| `src/plib/assoc/` | Associative array (string → data), used by the config code. |
| `src/game/` | The game itself: art and caches (`art.cc`, `cache.cc`, `heap.cc`), map/tiles/objects (`map.cc`, `tile.cc`, `object.cc`), animation (`anim.cc`), combat (`combat.cc`, `combatai.cc`), UI screens (`intface.cc` interface bar, `skilldex.cc`, `inventry.cc`, `editor.cc` character screen, `pipboy.cc`, `worldmap.cc`, `gdialog.cc` dialogue, `loadsave.cc`, `options.cc`, `bmpdlog.cc` file dialogs), mouse modes (`gmouse.cc`), sound glue (`gsound.cc`, `sfxcache.cc`, `sfxlist.cc`), scripting glue (`scripts.cc`), the event queue (`queue.cc`), config (`config.cc`, `gconfig.cc`), messages (`message.cc`), fonts (`fontmgr.cc`, the interface `.aaf` fonts 100–110). |
| `src/int/` | The "INT" script interpreter (`intrpret.cc`), its opcode libraries (`intlib.cc`, `support/intextra.cc`), a managed window/region/widget layer for scripts (`window.cc`, `region.cc`, `widget.cc`), sound library (`sound.cc`, `audio.cc`, `audiof.cc`), movie player front end (`movie.cc`), dialog helpers (`dialog.cc`). |
| `src/movie_lib.cc` | Interplay MVE decoder (video and audio). |
| `src/audio_engine.cc` | SDL audio device and mixing (runs on the SDL audio thread). |
| `src/pointer_registry.cc` | Maps 64-bit pointers to 32-bit ints where the original API passed pointers through `int`. |
| `src/platform_compat.cc` | Path, case and string helpers (`compat_*`). |
| `src/fps_limiter.cc` | Frame limiter (`sharedFpsLimiter`). |
| `third_party/` | SDL2 (vendored on non-Linux), `adecode` (ACM audio), `fpattern` (glob). The latter two are fetched by CMake. |
| `os/` | Packaging (macOS `Info.plist`, Android, iOS, Windows). |
| `tools/heap-test/` | Standalone tests for `src/game/heap.cc` (fuzzer and overrun reproducer, no SDL or game data needed). |

Main loop: `winmain.cc main()` → `gnw_main` → `main.cc` → `game.cc
game_init()` (config, databases, video, fonts, subsystems) → main menu →
`main_game_loop()` → `game_handle_input()`, which dispatches the hotkeys
(e.g. `S` → `skilldex_select()`, `I` inventory, `C` character, `P` pipboy).
Everything runs on the main thread except the SDL audio callback
(`audio_engine.cc`) and SDL timers (`sound.cc` fades, see REVIEW.md).

## Building

```sh
# Linux needs SDL2 headers: apt-get install libsdl2-dev
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j
```

- `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is needed with CMake ≥ 4, because
  vendored/fetched third-party CMakeLists still declare
  `cmake_minimum_required` < 3.5. Without it configure fails.
- macOS builds a universal `fallout-ce.app` (`CMAKE_OSX_ARCHITECTURES`
  `x86_64;arm64`, deployment target 10.13). For a faster arm64-only debug
  build, pass `-DCMAKE_OSX_ARCHITECTURES=arm64`.
- Sanitizers: `-DASAN=ON` (address, with `-fsanitize-recover=address`) and
  `-DUBSAN=ON`. Both work with Apple clang on arm64. ASan does **not** see
  overruns inside the art cache, because that is one big `malloc` block
  managed by `heap.cc`. Use `FALLOUT_HEAP_CHECK=1` for that (below).
  If your clang's UBSan includes `-fsanitize=function`, it will report the
  animation callback casts (`(AnimationCallback*)show_death` etc.). Those
  are known and harmless on x86-64/arm64.
- Useful extra warnings: `-DCMAKE_CXX_FLAGS="-Wall -Wextra
  -Wno-unused-parameter"`. The tree builds with about 130 warnings,
  mostly sign-compare and unused functions, all known.
- In the Linux CI container, clang lacked the ASan runtime. Use
  `CXX=g++` for sanitizer builds there.
- `clang-format` (repo `.clang-format`) is the style; run
  `clang-format --dry-run <files>` before committing.
- The heap tests: `cmake -S tools/heap-test -B build-heap-test
  [-DASAN=ON] && cmake --build build-heap-test`, then
  `./build-heap-test/heap_fuzz <seed> <iterations>` and
  `./build-heap-test/heap_overrun <bytes>`.

There are no other automated tests, and **game data is not in the repo**.
Without `master.dat`/`critter.dat` the game can't start, so most changes can
only be checked statically, with sanitizers and with standalone harnesses
like `tools/heap-test`.

## Running and configuration

The working directory must be the game directory. On macOS the bundle's
`Info.plist` sets `SDL_FILESYSTEM_BASE_DIR_TYPE=parent`, so the working
directory is **the folder containing `fallout-ce.app`**. To start with
environment variables, run the binary directly from that folder:
`FALLOUT_HEAP_CHECK=1 ./fallout-ce.app/Contents/MacOS/fallout-ce`.

- `fallout.cfg` (game config, defaults in `gconfig.cc`): `[system]`
  `master_dat`, `critter_dat`, `master_patches`, `critter_patches`
  (case-sensitive on macOS/Linux!), `language`, `art_cache_size` (MB,
  default 8, size of the art heap), `[sound] music_path1`, `[preferences]`
  …, `[debug] mode`.
- `[debug] mode` selects where `debug_printf` goes: `environment`
  (default: use the `DEBUGACTIVE` env var with values
  `log`/`screen`/`mono`/`gnw`), `log` (writes `debug.log` in the game
  directory), `screen`, `mono`, `gnw`. Heap/cache/memory guard errors
  ("Heap Error: Bad guard end…", "Memory header stomped.") only appear
  here. (Before this fork's fixes neither `[debug] mode` nor
  `DEBUGACTIVE=log` worked.)
- `f1_res.ini` `[MAIN]`: `SCR_WIDTH`, `SCR_HEIGHT` (clamped to at least
  640×480), `WINDOWED`, `SCALE_2X` (0 = 1×, 1 = 2×; the logical game
  resolution is the size divided by the scale, and it's only applied if the
  result stays ≥ 640×480). Example: 1512×982 with `SCALE_2X=1` gives a
  756×491 game screen.
- `FALLOUT_HEAP_CHECK=1` (environment, this fork): validates the art-cache
  heap on cache lock/unlock and `abort()`s at the first corruption with a
  log line naming the cache key (FID). See REVIEW.md "Skilldex crash".

## Decompilation conventions (don't "fix" these)

- `// 0x4CAE1C` comments are the function or variable address in the
  original `falloutw.exe`. Keep them, and add them when you split a
  function.
- Names like `v11`, `a2`, `field_1C`, `dword_51F018`, `byte_6B400D` are
  unknown originals. Odd style, `goto`, deep nesting, duplicated code and
  "NOTE: Uninline." blocks mirror the binary. Don't refactor them for style.
- `// CE:` marks a deliberate deviation from the original (bug fix,
  portability, larger resolution). `// FIXME:` marks a known original bug
  that was kept. `// NOTE:` explains a decompilation quirk. Use the same
  markers.
- Behaviour should match the original game. Gameplay changes need a clear
  reason, and mention them in the commit message. Fixes that only remove
  undefined behaviour or memory errors are fine.
- F2 sometimes has "SFALL:" fixes (from the sfall mod). Porting one is
  fine when it fixes an engine bug (example: `queue_clear_type`).

## Memory model: what you need to know

### `mem_malloc` (`plib/gnw/memory.cc`)

All engine allocations go through `mem_malloc`/`mem_realloc`/`mem_free`. On
64-bit each block has a **16-byte header** (`size_t size`, `int guard =
0xFEEDFACE`, 4 bytes padding) and a 4-byte footer `0xBEEFCAFE`, and sizes
are rounded up to 4. The returned pointer is 16-byte aligned. A pointer read
from `p[-1]` of a `mem_malloc`'d pointer array is therefore `0xFEEDFACE`
(+padding). If a crash address looks like `0xFEEDFAxx`, some code indexed
an array with `-1`.

### Art cache = `cache.cc` + `heap.cc`

- `art.cc` owns `Cache art_cache` (key = FID). `art_ptr_lock(fid, &key)`,
  `art_ptr_lock_data`, `art_lock(fid, &key, &w, &h)` lock an entry and
  return a pointer **into the cache heap**. `art_ptr_unlock(key)`
  decrements the entry's reference count.
- `heap.cc` is a handle-based allocator inside one `mem_malloc` block of
  `art_cache_size` MB. Block layout: 16-byte `HeapBlockHeader` {guard
  `0xDEADC0DE`, size, state, handle_index}, data, 4-byte footer
  `0xACDCACDC`, all in one contiguous chain. Blocks are FREE, MOVABLE
  (allocated, unlocked), LOCKED or SYSTEM (fallback `mem_malloc` for
  oversized entries).
- **Unlocked blocks move.** When no free block is large enough,
  `heap_find_free_block` compacts the heap by `memcpy`'ing MOVABLE blocks
  elsewhere. Any pointer into art data is only valid while that FID is
  locked. Using a pointer after `art_ptr_unlock` is a latent
  use-after-move.
- Reference counts are **per FID, shared by all users**. One extra
  `art_ptr_unlock` anywhere can drop another module's long-lived lock
  (gmouse locks its 3D cursor frames for the whole session) to 0 and make
  the block movable.
- Some code **writes into cached art**: `gmouse.cc` builds the action
  menu, pick and to-hit cursors in the locked frames of `actmenu.frm`,
  `actpick.frm`, `acttohit.frm` and `msef000.frm` (memset, `buf_to_buf`,
  `text_to_buf`, `buf_outline`), and `inventry.cc` draws "ALL" into the
  button FRM. Any overrun there clobbers the next heap block's header.
- `heap.cc` keeps static scratch lists (`heap_free_list`, …) shared by all
  heaps (the art cache and `sfxcache`). It isn't reentrant or thread-safe.
  Only call it from the main thread.
- Integrity: `heap_validate()` (verbose), `heap_check()` and
  `heap_check_block()` (silent, CE). `tools/heap-test` fuzzes the
  allocator. The allocator itself passed fuzzing, including compaction.
- `art.cc load_frame_into()` trusts the FRM file: frame data is read
  sequentially by `frame->size` into a buffer sized from the header's
  `dataSize` (`artGetDataSize`). Inconsistent or modded FRMs can write past
  the cache block.

### Window buffers and screen size

- `win_add(x, y, w, h)` allocates a `w*h` buffer, clamps the position to
  the screen and refuses windows larger than the screen. `win_get_buf()`
  returns it, and the pitch is the **window** width.
- Many screens are fixed 640×480 (or 640-wide) windows centred on a larger
  screen. Their drawing code hardcodes 640 as pitch. That's correct **only
  because the window is 640 wide**. When you touch such code, check the
  `win_add` width. Example of the mismatch class: `display.cc` used the
  *art* width as pitch for the 640-wide interface window (fixed).
- The map window (`display_win`) is `screen width × (screen height −
  100)`. `tile.cc`, `object.cc` and `loadsave.cc` use the real window size
  (`buf_width`, `buf_full`, `win_width(display_win)`). Scroll bounds
  (`tile_update_bounds_rect`) deliberately emulate 640×480.
- The interface bar is always 640×100, centred
  (`INTERFACE_BAR_WIDTH`). The skilldex and indicator boxes are positioned
  relative to it.
- `button_draw()` doesn't clip a button to its window when called without
  a bound rect (register/press/release). Buttons must lie inside their
  window.
- Text: `text_to_buf(buf, str, swidth, pitch, color)` stops horizontally at
  `swidth` from `buf`, but writes `text_height()` rows unconditionally.
  Centering code like `buf + (w - text_width(s)) / 2` goes negative when
  the string is wider than the box (watch translated strings).

## Platform pitfalls (macOS / arm64 / 64-bit)

- `char` is **signed** on x86 and Apple arm64 (unsigned on Linux arm64).
  Bytes ≥ 0x80 (umlauts, `\x95`) are negative. Never index tables with a
  plain `char`; cast to `unsigned char` (text.cc and fontmgr.cc were fixed;
  use `-Wchar-subscripts` to find new cases).
- `long` and pointers are 8 bytes. File formats use 32-bit fields: use
  `db_freadInt32`/`db_freadInt16` with `int`/`short`, never read a struct
  or pointer straight from a file (see `text.cc load_font`). Pointers that
  the original stored in `int`s go through `pointer_registry` (`ptrToInt`,
  `intToPtr(ref, remove)`; remove the entry when the object dies) or
  through the interpreter's typed `ProgramValue` (`VALUE_TYPE_PTR`).
- Most data formats (FRM, PRO, MAP, SAV, MSK, AAF fonts, ...) are
  **big-endian**. `db_freadInt16/Int32/...` assemble values byte by byte,
  so they work on any host. A few readers copy raw little-endian structs
  with `db_fread` (e.g. `text.cc load_font` for `.fon`), which only works
  on little-endian hosts, like all current targets (x86-64, arm64).
  `fontmgr.cc` byte-swaps AAF headers explicitly.
- Unaligned 16- and 32-bit loads are legal on arm64 and x86-64.
  `movie_lib.cc` uses `loadUInt16LE`/`loadUInt32LE` helpers anyway. Prefer
  them for new code.
- Function pointer casts between `int (*)(Object*, int)` and `int
  (*)(void*, void*)` (animation callbacks) are UB, but ABI-compatible on
  SysV x86-64 and AAPCS64/Apple arm64 because all arguments are passed in
  registers. Don't add casts involving variadic functions, `float`/`double`
  parameters or struct-by-value: those *do* differ on arm64.
- `interpretError()` `longjmp`s out of the interpreter. Don't put objects
  with non-trivial destructors on the stack between `setjmp` and the
  opcode handlers.
- SDL timers run on their own thread. `soundFade` uses one and calls back
  into non-thread-safe sound/memory code (currently unreachable, see
  REVIEW.md).
- Fullscreen on macOS (this fork): `FULLSCREEN_DESKTOP` + `RESIZABLE` +
  `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES` (`svga.cc`). Window resizes
  recreate the renderer in `handleWindowSizeChanged()` and then refresh
  all windows.

## Relation to fallout2-ce

- F1 and F2 descend from the same GNW/INT/game codebase. Most F1 modules
  have an F2 twin with different names: `heap.cc`↔`heap.cc`,
  `cache.cc`↔`cache.cc`, `art.cc`↔`art.cc`, `gnw.cc`↔`window_manager.cc`,
  `intrface.cc`↔`window_manager_private.cc`, `grbuf.cc`↔`draw.cc`,
  `text.cc`↔`text_font.cc`, `fontmgr.cc`↔`font_manager.cc`,
  `intface.cc`↔`interface.cc`, `gmouse.cc`↔`game_mouse.cc`,
  `editor.cc`↔`character_editor.cc`, `inventry.cc`↔`inventory.cc`,
  `gdialog.cc`↔`game_dialog.cc`, `intrpret.cc`↔`interpreter.cc`,
  `intextra.cc`↔`interpreter_extra.cc`, `db.cc`↔`db.cc`/`xfile.cc`,
  `assoc.cc`↔`dictionary.cc`, `display.cc`↔`display_monitor.cc`,
  `anim.cc`↔`animation.cc`, `textobj.cc`↔`text_object.cc`,
  `queue.cc`↔`queue.cc`, `options.cc`↔`options.cc`+`preferences.cc`.
  F1 names follow the original symbol names; F2 uses descriptive names.
- F2 has many CE/sfall fixes that F1 lacks. When you find a bug in F1,
  check the F2 twin first and port its fix if one exists. When a function
  looks wrong in F1, compare it with F2 before assuming the original game
  needed it.
- Not everything in F2 applies. F2 has a wide interface bar mode
  (`gInterfaceBarWidth`, `IFACE_BAR_MODE`), F2-only gameplay (gender
  texts, perks, Sfall opcodes, `settings` struct, `FrmImage` RAII
  wrappers) and different data constants (`anim_free_slot` 13 vs 20,
  pipboy limits, …).
- A structural F1↔F2 comparison (identifiers normalized, `difflib` on
  token streams) was used for this review. 627 F1 functions are
  token-identical to F2 and about 1100 are ≥ 90% similar; the meaningful
  divergences are listed in REVIEW.md. The script is easy to recreate:
  extract function bodies, replace identifiers with `I`, map
  `nullptr`→`NULL`, match by `SequenceMatcher.ratio()`.

## Where the known bugs are

See `REVIEW.md` for the full list with file:line. Highlights:

- **Skilldex crash** (`EXC_BAD_ACCESS 0xfeedfad2` in `heap_allocate` ←
  `cache_lock` ← `art_lock` ← `skilldex_select`). This is a *consumer* of
  earlier art-heap corruption. The mechanism is reproduced in
  `tools/heap-test/heap_overrun.cc`. To find the writer, run with
  `FALLOUT_HEAP_CHECK=1` and `[debug] mode=log`.
- Code that writes into cached art: `gmouse.cc` 3D cursor frames
  (wrong pitch in `gmouse_3d_build_menu_frame`, unbounded arrow shift,
  `buf_outline` writing one row above its buffer), `inventry.cc` "ALL"
  button.
- `int/window.cc`: five functions with an inverted `currentWindow != -1`
  check, which index `windows[-1]` (same in F2).
- Lock leaks in `gmouse_set_cursor`, `gmouse_3d_build_menu_frame` and
  `obj_render_object` (egg path).

## Working on this repo

- Work on a feature branch. Upstream PRs go to `alexbatalov/fallout1-ce`
  only when the user asks.
- One fix per commit, with a message that explains the failure scenario
  and, where applicable, the F2 counterpart.
- No game data is available: verify by building (ideally with
  `-DASAN=ON -DUBSAN=ON`), with standalone harnesses, and by comparing
  with F2. Say clearly in REVIEW.md or the commit whether a fix is
  "verified" (path traced or reproduced) or only "suspected".
