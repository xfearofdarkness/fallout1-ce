# Code review: fallout1-ce (fork `xfearofdarkness/fallout1-ce`)

Branch `review/agents-and-review`, based on `fix-movie-offset` (which already
contains the `getOffset` truncation fix in `movie_lib.cc` and native macOS
fullscreen Spaces in `svga.cc`). The fork is otherwise identical to upstream
`alexbatalov/fallout1-ce` at `0609bcf` ("Externalize adecode").

Context: macOS on Apple Silicon (arm64, signed `char`, LP64), internal
resolution 756×491 (`f1_res.ini`: `SCR_WIDTH=1512`, `SCR_HEIGHT=982`,
`SCALE_2X=1`). Reproducible crash when opening the Skilldex (`S`):
`EXC_BAD_ACCESS` at `0xfeedfad2` in `heap_allocate` ← `cache_lock` ←
`art_lock` ← `skilldex_select` ← `game_handle_input`.

The code is decompiled from the original binary (`// 0x...` addresses,
`v11`-style names). Naming and style are **not** findings.

## Contents

1. [Method](#1-method)
2. [Commits on this branch](#2-commits-on-this-branch)
3. [Skilldex crash: candidates](#3-skilldex-crash-candidates)
4. [Findings by severity](#4-findings-by-severity)
5. [Differences from fallout2-ce](#5-differences-from-fallout2-ce-where-f2-is-more-correct)
6. [macOS / arm64 specifics](#6-macos--arm64-specifics)
7. [Tool output triage](#7-tool-output-triage)
8. [Module checklist](#8-module-checklist)

Status legend: **verified** = code path traced end to end, or reproduced in a
harness; **suspected** = plausible from the code, but depends on data or
runtime state that couldn't be checked (no game data available).

## 1. Method

- **Build**: clang 18, `-Wall -Wextra` (`-Wno-unused-parameter`): builds
  cleanly with 127 warnings (50 sign-compare, 42 unused-function, 11
  missing-braces, the rest triaged in §7). The game can't be run without
  data, so everything below is static analysis plus harnesses.
- **Static analysis**: `cppcheck --enable=warning,portability
  --inconclusive` over all of `src/` and `clang-tidy`
  (`bugprone-*`, `clang-analyzer-*`) over the whole compile database. All
  non-style hits were triaged (§7).
- **Heap harnesses** (`tools/heap-test/`, committed):
  - `heap_fuzz`: random allocate/lock/unlock/free sequences including heap
    compaction, with `heap_validate()`, `heap_check()` and data-integrity
    checks after every step. 40 seeds × 5000 ops plus 25 seeds after the
    changes, under ASan/UBSan: **no failures**. `heap.cc` itself is sound.
  - `heap_overrun`: writes N bytes past a locked block, then allocates.
    With N ≥ 12 it faults at **`0xfeedfad2` in `heap_find_free_block`**
    (inlined into `heap_allocate` in release builds), exactly the
    reported crash signature. UBSan: "member access within misaligned
    address 0x0000feedface for type 'struct HeapBlockHeader'".
- **Systematic F1↔F2 comparison**: all 3316 F1 and 3732 F2 function bodies
  were extracted and tokenized, with identifiers normalized, `nullptr`≡`NULL`
  and `true`/`false`≡`1`/`0`. Each F1 function was paired with its most
  similar F2 function (`difflib.SequenceMatcher`). Result: 627 functions are
  token-identical, 648 are ≥ 97% and 464 are 90–97% similar. Every
  divergence in the ≥ 85% band was read; the relevant ones are in §5.
- **Manual review** of every directory under `src/`, prioritising code that
  *writes* memory: blitters, text rendering, window/button drawing,
  anything that writes into art data, decoders, file readers, the
  allocator, the cache and event queue code. A pattern scan of all 945
  drawing calls (`buf_to_buf`, `trans_buf_to_buf`, `text_to_buf`,
  `buf_fill`, `memcpy`, `memset`, `draw_line`, …) by destination argument
  identified which writes target something other than a window buffer.
  Depth per module is in §8.

## 2. Commits on this branch

Each is a separate commit with the failure scenario in the message.

| Commit | Change | Finding |
|--------|--------|---------|
| `9d905c1` | Treat characters as unsigned in `GNW_text_width`, `GNW_text_char_width`, `GNW_text_to_buf`, `FMtext_char_width` | M1 |
| `d0ad6fb` | `debug_register_log`: `&&` → `\|\|` (the log could never be opened) | M8 |
| `d9d0063` | Honour `[debug] mode` from `fallout.cfg` (ported from F2) | M8 |
| `2e4bfbd` | Apply `SCALE_2X` only if the result is ≥ 640×480 (ported from F2) | M7 |
| `919cfc4` | `display_redraw`: use the interface window width as pitch | M2 |
| `8959fea` | Opt-in art-heap integrity checks `FALLOUT_HEAP_CHECK=1` (`heap_check`, `heap_check_block`, hooks in `cache_lock`/`cache_unlock`) | C1 (diagnostics) |
| `16becaa` | `gsound_close`: remove the pointer registry entry (ported from F2) | L1 |
| `90608ad` | `queue_clear_type`: re-read the next event after the handler (UAF / double free; sfall fix in F2, plus the second branch) | H1 |
| `3bb6380` | `art_get_base_name`: always-false range check `&&` → `\|\|` (F2) | L2 |
| `5b53091` | `op_obj_can_hear_obj`: inverted NULL check (F2) | M9 |
| `55fe593` | `ai_find_attackers`: `*a4 != NULL` → `a4 != NULL` (F2) | L3 |
| `a711aab` | `draw_line`: vertical lines miss their middle pixel `<` → `<=` (F2) | L4 |
| `4a0fef4` | `PrefStart` error unwind unlocks `grphkey2[i - 1]` → `grphkey2[i]` (F2) | M3 |
| `28e8919` | `objs_area_turn_on_off`: endless loop and broken swap in the `turn_on/off_objs_in_area` opcodes | M12 |
| (docs) | `AGENTS.md`, `REVIEW.md`, `tools/heap-test/` | — |

## 3. Skilldex crash: candidates

### 3.1 What the crash actually is (verified)

1. `skilldex_start()` locks six interface FRMs with `art_lock()`. None of
   them is usually cached yet, so `cache_lock()` → `cache_add()` (inlined) →
   `heap_allocate()`.
2. `heap_allocate()` → `heap_find_free_block()` → `heap_build_free_list()`
   walks the art heap block by block (`src/game/heap.cc:1199ff.`) and
   **merges adjacent FREE blocks**, decrementing `heap->freeBlocks` for every
   merge (`heap.cc:1241`).
3. If the walk finds more adjacent free blocks than `heap->freeBlocks`
   records, the counter reaches 0. Then
   `biggestFreeBlock = heap_free_list[heap->freeBlocks - 1]`
   (`heap.cc:951`) reads `heap_free_list[-1]`. `heap_free_list` is a
   `mem_malloc` array, so index `-1` is the `mem_malloc` header's guard
   word `0xFEEDFACE` with 4 bytes of zero padding (`memory.cc:19-25`).
   Reading `->size` (offset 4) faults at **`0xFEEDFAD2`**.
4. The counter and the block chain only disagree if a **block header was
   overwritten**: a write past the end of a cache block (or before the
   start of the next one) turns a header's `state` or `size` into
   something else. `heap.cc` itself is not at fault (fuzzed, §1).

So the Skilldex is the *victim*: it is simply the first place after the
corruption that allocates new art. Any other screen with uncached art
(inventory, character screen, pipboy, a new map) would crash the same way.
`heap_unlock()` also logs `"Heap Error: Bad guard end detected during
unlock."` for the damaged block before the crash, but until this branch
`debug_printf` output went nowhere (M8).

### 3.2 How to find the culprit (new on this branch)

1. Build this branch (optionally with `-DASAN=ON` for overruns of normal
   `mem_malloc` buffers and globals).
2. In `fallout.cfg` set `[debug]` `mode=log`.
3. Start from a terminal in the game folder:
   `FALLOUT_HEAP_CHECK=1 ./fallout-ce.app/Contents/MacOS/fallout-ce`
4. Play until it crashes. With the check enabled, every `cache_unlock()`
   verifies the guards of the block being released and of the block after
   it. Every new cache entry triggers a full heap walk first. On the first
   inconsistency the game logs
   `Heap Error: Corrupted cache heap detected in cache_unlock, key 0x...`
   to `debug.log`/stderr and calls `abort()`.
5. The macOS crash report then shows the call stack of the code that **just
   used the overrun art**. The key is the FID: type = `(key >> 24) & 0xF`
   (6 = interface), index = `key & 0xFFF` into `art/<type>/<type>.lst`.
   `debug.log` also contains the earlier `Heap Error: Bad guard ...` lines.

### 3.3 Candidates, most likely first

| # | Candidate | Where | Why it fits | Status |
|---|-----------|-------|-------------|--------|
| 1 | **gmouse writes into the cached 3D-cursor art frames.** `gmouse_3d_build_menu_frame` blits the arrow and all menu items into `actmenu.frm` using **`gmouse_3d_pick_frame_width` (the width of `actpick.frm`) as the destination pitch** (`gmouse.cc:1775`, `:1780`) while advancing rows with the menu frame width. If the two FRMs differ in width, the last item's rows run past the end of the `actmenu.frm` block, straight into the next heap block header. The arrow is also shifted down by up to `n*itemHeight` rows (`:1757`, `:1770`; pick frame `:1651`, `:1668`) without checking the frame height. `buf_outline()` (used on the to-hit and hex frames, `:1876`, `:1904`) writes `*(ptr - pitch)` for any non-zero pixel in row 0, one row *before* the buffer (`grbuf.cc:321`), i.e. into the frame header or the previous heap block. | `src/game/gmouse.cc`, `src/plib/gnw/grbuf.cc` | These are the only routines that write into art-cache memory during normal play, and they run constantly (hover, action menu, combat to-hit and hex cursor). Identical code in F2 (shared original bug), so vanilla F2 art presumably has matching widths; F1's `actmenu`/`actpick` dimensions couldn't be checked without data. | suspected |
| 2 | **Inconsistent or modded FRM loaded into the cache.** `load_frame_into()` reads each frame with `db_fread(ptr + 12, frame->size, ...)` (`art.cc:1040`) into a block sized from the header's `dataSize` plus padding (`artGetDataSize`, `art.cc:1199`). Frames are read sequentially, but placed at `dataOffsets[i] + padding[i]` (`art.cc:1119`). If the per-frame sizes add up to more than `dataSize`, or the offsets don't match the stream order, the read runs past the cache block. | `src/game/art.cc` | Overwrites exactly the next heap block's header. Only happens with damaged or modded art (e.g. from a mod or patch pack in `data/art`). | suspected (data-dependent) |
| 3 | **Writing through an art pointer after its FID's lock count dropped to 0.** Reference counts are per FID and shared. One surplus `art_ptr_unlock` makes another module's still-used block MOVABLE; the next compaction (`heap_find_free_block`, `memcpy` of MOVABLE blocks) moves it, and the holder (e.g. gmouse, which writes into its frames) then writes into whatever now occupies the old location, often a header. One real surplus unlock existed: `PrefStart` unwinding unlocks `grphkey2[-1]`, a garbage `CacheEntry*` (fixed, M3, error path only). No other double unlock was found (heuristic scan of all `art_ptr_unlock` sites, §8). Lock *leaks* exist (L5) and only pin memory. | various | Fits "corrupted earlier, detected much later". | suspected |
| 4 | **Event-queue use-after-free** (`queue_clear_type`, H1). When a queue handler (drug/withdrawal/radiation, map-leave cleanup) frees the event that the loop already saved, the loop keeps reading and writing freed `QueueListNode`s. That corrupts ordinary `mem_malloc` memory, and freed memory may be reused for anything, including cache bookkeeping arrays (`cache->entries`, `heap->handles`, `heap_free_list`). | `src/game/queue.cc:372` | Known crash in F2/sfall ("leaving the map while waiting for someone to die of a super stimpak overdose"). A different symptom path, but a genuine source of heap corruption in long sessions. | verified (code path); fixed |
| 5 | **Out-of-bounds writes on globals next to `art_cache`.** `art_cache` (and its `Heap` counters `freeBlocks` etc.) is a global. An OOB write on a neighbouring global that lowers `art_cache.heap.freeBlocks` produces the same crash. Real OOB-global writers found: `PrefStart` `grphkey2[-1]` (fixed), `int/window.cc` `windows[-1]` (M5, script-only), `intlib.cc` `inputProc[key]` with negative keys (L9). Global layout on macOS couldn't be checked; ASan instruments globals, so `-DASAN=ON` would catch this class. | various | Consistent with the signature, but less likely. | suspected |

Ruled out (verified):
- `heap.cc` allocation, compaction and bookkeeping (fuzzed; see §1).
- `cache.cc` binary search, insertion, eviction and purge (read in full; the
  index revalidation after `readProc` is correct).
- `skilldex.cc` itself: identical to F2, all writes into its own window
  buffer or its own `mem_malloc` button buffers, sized from the locked
  FRMs (+512 slack).
- Sound fade timer thread (M6): `soundFade` is unreachable because
  `gsound_background_fade` is never set.
- arm64-specific causes in the heap/cache: all fields are `int`/pointer
  with natural alignment, no `long`, no packed structs. Block alignment is
  4 bytes, which is enough for `Art`/`ArtFrame`.

## 4. Findings by severity

### Critical

**C1 — Art-cache heap corruption, consumed by the Skilldex crash**
`src/game/heap.cc:951` (consumer); writers per §3.3.
Failure scenario: as §3.1.
Status: mechanism verified (reproduced in `tools/heap-test/heap_overrun.cc`);
writer suspected.
Fix: find the writer with `FALLOUT_HEAP_CHECK=1` (§3.2), then fix it.
Hardening options (not committed, they would mask the bug): make
`heap_build_free_list` and `heap_find_free_block` bail out to the system
allocator when `freeBlocks <= 0` or a guard is wrong.

### High

**H1 — Use-after-free in `queue_clear_type`** — `src/game/queue.cc:372-410`
Saves `curr = *ptr` before calling the handler `fn`; handlers can remove or
free queued events. Kept events were re-linked with a stale `next`.
Scenario: leaving a map while a drug/withdrawal event is being processed
(queue_leave_map → per-type clear handler adds or removes events) → loop
continues on a freed node → double free / list corruption.
Status: verified (code path; same bug fixed in F2 via sfall). **Fixed**
(`90608ad`).

**H2 — `gmouse_3d_build_menu_frame` uses the pick-frame width as pitch for
the menu frame** — `src/game/gmouse.cc:1775`, `:1780`
Rows are written with stride `gmouse_3d_pick_frame_width` into the
`actmenu.frm` cache block, while vertical advance uses
`gmouse_3d_menu_frame_width` (`:1781`) and the arrow shift uses the menu
width (`:1757`, `:1770`).
Scenario: if `actmenu.frm` is narrower than `actpick.frm`, the last menu
item's rows end `(itemHeight-1) * (pickW - menuW)` bytes past where they
should, which for the bottom item is past the end of the block → next heap
header destroyed → crash at the next allocation (Skilldex).
Status: suspected (depends on FRM sizes; same code in F2).
Fix: use `gmouse_3d_menu_frame_width` as destination pitch in both calls,
and clamp the arrow/item rectangles to `gmouse_3d_menu_frame_height`.
Validate in game that the menu still renders identically with vanilla art.

**H3 — FRM loader trusts sizes from the file** — `src/game/art.cc:1028-1050`,
`:1099-1130`, `:1199`
`art_readSubFrameData` reads `frame->size` bytes per frame with no bound;
the destination block was sized from the header's `dataSize`.
Scenario: a modded or damaged FRM in `data/art/...` with a frame table that
exceeds `dataSize` overwrites the next art-cache block.
Status: suspected (data-dependent; identical in F2).
Fix: pass the remaining block size into `load_frame_into`/
`art_readSubFrameData`, fail the load if `sizeof(ArtFrame) + frame->size +
padding` exceeds it, and also check `dataOffsets[i]` against `dataSize`.

### Medium

**M1 — Signed `char` used as glyph index** — `src/plib/gnw/text.cc:309-415`,
`src/game/fontmgr.cc:237-250`
`GNW_text_width`: `curr_font->info[str[i]]` with negative values for bytes
≥ 0x80 (umlauts, the display monitor bullet `'\x95'`). `GNW_text_to_buf`:
`ch < num` always true for negative `ch`, then `info[ch & 0xFF]` past
`info[num]`. `FMtext_char_width`: `glyphs[c]` with `c < 0`.
Scenario: German/French texts or the `'\x95'` bullet give garbage widths;
callers center with `(w - text_width(s)) / 2`, so the offset can go
negative or large → text written outside its buffer.
Status: verified (code path; F2 uses `& 0xFF`/`unsigned char`).
**Fixed** (`9d905c1`).

**M2 — Interface-bar message monitor used the art width as window pitch** —
`src/game/display.cc:305-330`
The window is 640 wide; `intface_full_wid` is the width of `iface.frm`.
Scenario: a replacement interface background wider than 640 px → blit and
text rows written up to `width*83+190` bytes into a 64000-byte buffer
(≈ 2.5 KB overflow at 800 px).
Status: verified (code path); only with non-640 art. **Fixed** (`919cfc4`).

**M3 — `PrefStart` unwinding unlocks `grphkey2[i - 1]`** —
`src/game/options.cc:913`
For `i == 0` it passes the global before the array to `cache_unlock`
(decrement through a garbage pointer, `heap_unlock` of a garbage handle),
and the last locked FRM is never unlocked.
Scenario: a preferences FRM fails to load → garbage write into memory the
art cache treats as a `CacheEntry`.
Status: verified (code path; F2 uses `[i]`). **Fixed** (`4a0fef4`).

**M4 — `buf_outline` writes one row above its buffer** —
`src/plib/gnw/grbuf.cc:321`
The vertical pass writes `*(ptr - pitch)` for a non-zero pixel in row 0.
Callers: `gmouse.cc:1876`, `:1904` (art-cache frames), `textobj.cc:290`
(own buffer with an empty first row).
Scenario: any caller whose row 0 is not empty writes up to `width` bytes
before the buffer. For the hex/to-hit frames that's the `ArtFrame` header
and `Art` struct, or, for wide frames, the previous heap block.
Status: suspected (current callers keep row 0 empty with vanilla fonts and
art; the code has a "TODO: Check in debugger, might be a bug" note, and F2
is identical).
Fix: skip the write when `y == 0` (`if (y > 0) *(ptr - pitch) = ...`),
mirroring the `x > 0` intent of the horizontal pass.

**M5 — `int/window.cc` inverted current-window checks** —
`src/int/window.cc:1676`, `:1810`, `:2112`, `:2137`, `:2161`
`if (currentWindow != -1) return false;` followed by
`&(windows[currentWindow])`: the functions never work when a window is
selected, and index `windows[-1]` when none is.
Scenario: a script calls `deletebutton`/`setbuttonflag` (intlib.cc:989,
:1118, :1123) with no managed window selected → reads and writes the
global before `windows[]`.
Status: verified (code path); script-reachable only; F2 identical.
Fix: `if (currentWindow == -1) return false;`. Changes script-visible
behaviour (the functions start to work), so it wasn't committed.

**M6 — Sound fades run on the SDL timer thread and `fadeSounds` never
advances** — `src/int/sound.cc:1507-1555`, `:1629`
`SDL_AddTimer` invokes `fadeSounds` on SDL's timer thread, which calls
`soundDelete`/`soundStop`/`soundVolume` and so `mem_free` and the sound
list, concurrently with the main thread. The `while (ptr != NULL)` loop
never sets `ptr = ptr->next`; after `removeFadeSound(ptr)` the next
iteration dereferences `ptr->sound == NULL` (target volume 0) or spins
forever.
Scenario: currently unreachable: `gsound_background_fade` is only ever set
to 0 (`gsound.cc:329`, `:357`). Any future use of `soundFade` would race and
hang or crash.
Status: verified (code path); latent; F2 identical.
Fix: drive fades from `soundUpdate()` on the main thread, and advance the
loop (`next = ptr->next` before `removeFadeSound`).

**M7 — `SCALE_2X` could produce a screen smaller than 640×480** —
`src/game/game.cc:150-190`
Clamp to 640×480 happened before the division; `SCALE_2X=-1` divided by 0.
Scenario: 1280×800 + `SCALE_2X=1` → 640×400 → every 640×480 window fails to
open.
Status: verified. **Fixed** (`2e4bfbd`, ported from F2).

**M8 — No way to get the debug log** — `src/plib/gnw/debug.cc:58`,
`src/game/game.cc:199`
`debug_register_log` checked `mode[0]=='w' && mode[1]=='a'` (never true),
and nothing read `[debug] mode`. Heap and memory guard errors were
discarded, which made the Skilldex crash undiagnosable.
Status: verified. **Fixed** (`d0ad6fb`, `d9d0063`).

**M9 — `op_obj_can_hear_obj` NULL dereference / always false** —
`src/int/support/intextra.cc:2191`
Status: verified. **Fixed** (`5b53091`, F2). Gameplay note: the opcode now
returns true for audible objects, as scripts intend.

**M10 — `inventry.cc` writes text into a cached FRM with the wrong width
limit** — `src/game/inventry.cc:5348-5349`
`text_to_buf(buttonUpData + (94 - length)/2 + 376, text, 200, 94, ...)`:
the destination is the locked 94×33 "ALL" button art; `swidth` 200 allows
glyphs to continue 106 px past the row (wrapping into the next rows), and a
string wider than 94 px makes the start offset negative. The text is also
re-drawn into the shared cached art every time the dialog opens.
Status: suspected (in bounds for the vanilla "ALL" string; F2 identical).
Fix: draw into a private copy of the button image (as `skilldex.cc` does),
with `swidth` 94 and a clamped x.

**M11 — Art lock leaks** — `src/game/gmouse.cc:1232`, `:1247`
(`gmouse_set_cursor` returns without `art_ptr_unlock` after locking: every
call for the animated wait cursor inside the frame delay leaks one lock),
`gmouse.cc:1763` (the mirrored arrow is locked into `arrowFrmHandle`
without unlocking the first arrow), `object.cc:4729` (`obj_render_object`
returns on `egg == NULL` without unlocking the object art), `skilldex.cc`
via `art_lock()` (returns NULL with the lock still held when the frame is
missing).
Scenario: pinned entries can never be evicted, so the art cache fills
faster, compacts more, and falls back to system blocks. No corruption by
itself.
Status: verified (code path).
Fix: unlock on every early return.

**M12 — `turn_on_objs_in_area`/`turn_off_objs_in_area` hang the game** —
`src/int/support/intextra.cc:1620-1675`
`objs_area_turn_on_off()` loops `while (a1 <= a2)` over elevations without
ever incrementing `a1`. Its range swap also does `a4 = a3` instead of
`a4 = temp`. (clang-tidy `bugprone-infinite-loop`.)
Scenario: any script using opcode 0x80E1/0x80E2 freezes the game.
F1-only: F2 reuses these opcode numbers.
Status: verified (code path). **Fixed** (`28e8919`): advance `a1`, fix the
swap, clamp elevations to `0..ELEVATION_COUNT-1`.

### Low

- **L1** `gsound.cc:1669` — pointer registry entries were never removed, so
  each opened sound left a dangling `DB_FILE*`. **Fixed** (`16becaa`, F2).
- **L2** `art.cc:472` — `art_get_base_name` range check could never trigger.
  **Fixed** (`3bb6380`, F2).
- **L3** `combatai.cc:538` — `*a4 != NULL` checked the pointee. **Fixed**
  (`55fe593`, F2). Latent only.
- **L4** `grbuf.cc:29` — vertical lines miss the middle pixel. **Fixed**
  (`a711aab`, F2).
- **L5** `object.cc:4548` — `obj_render_outline` right-edge write is guarded
  by `v22 < buf_size`, where `v22` is the offset of the *last drawn* pixel;
  the write goes one further, so an outlined object touching the
  bottom-right corner writes `back_buf[buf_size]` (1 byte past the map
  window buffer). Same in F2. Fix: check `dest14 - back_buf < buf_size`.
  Status: verified (arithmetic).
- **L6** `kb.cc:341` — SDL scancodes ≥ 256 are masked with `& 0xFF` into the
  256-entry `ascii_table`; media keys alias letters (e.g. Mute 262 → `C`,
  keyboard-backlight toggle 278 → `S`). F2 sizes the table
  `SDL_NUM_SCANCODES`. No OOB. Fix: enlarge the table and drop the mask.
- **L7** `wordwrap.cc:72` + `textobj.cc:237`, `:279` — the last breakpoint is
  `strlen + 1`, so `text_object_create` temporarily writes `'\0'` one byte
  past the string's terminator and restores it. A 1-byte transient OOB
  write when the string fills its allocation exactly (ASan would flag it).
  Same in F2.
- **L8** `db.cc:2706` — `db_preload_buffer` reads a raw block of up to
  0x7FFF bytes into the 0x4000-byte buffer; `lzss.cc:260` lets `length`
  underflow on a truncated stream. Malformed DAT only.
- **L9** `intlib.cc:1311`, `:1329` — `op_addkey`/`op_deletekey` only check
  the upper bound; keys < -1 index `inputProc[]` negatively (script data).
- **L10** `svga.cc:140` — if `SDL_CreateRGBSurface` fails, `svga_init`
  continues and dereferences `gSdlSurface`. Fix: `return false`.
- **L11** `cache.cc:543` — `cache_create_item` checks `cacheEntryPtr`
  instead of `*cacheEntryPtr` (FIXME), so OOM crashes in `cache_init_item`.
  `cache.cc:618` leaks `entries` in `cache_reset_counter` (FIXME; runs only
  after 2³² hits).
- **L12** `text.cc:241` — `text_remove_manager` `memmove`s with
  `sizeof(FontMgrPtr)` (8) instead of `sizeof(FontMgr)`, corrupting the
  manager table when a manager in the middle is removed. Unused in the
  game.
- **L13** `mouse.cc:351` — `unsigned char` pixel compared with `char`
  `mouse_trans`; transparency fails for a key colour ≥ 0x80. Callers pass
  0.
- **L14** Fixed-size stack buffers filled with `strcpy` from message files
  (max 1023 chars): `combat.cc:3684` `you[20]`, `combat.cc:3942`
  `text[40]`, `item.cc:2236` `v24[92]`, `skill.cc:549`, `:714` `text[60]`,
  `map.cc:596`, `:620` `name[16]`, `worldmap.cc:3570` `name[64]`,
  `editor.cc:4821ff.` `body1[64]`, `lip_sync.cc` `audioBaseName[16]`. Vanilla
  strings fit; long translations would overflow the stack. Fix:
  `snprintf`/`strncpy` with `sizeof`.
- **L15** `editor.cc:3477` and similar centering code: `width/2 -
  text_width/2` goes negative for strings wider than the button. The start
  lands in earlier rows of the same buffer (7 rows of slack), so it's
  in bounds unless the string is very long.
- **L16** `editor.cc:3492-3497` — OOM unwind frees `down[index]` after the
  loop index was already incremented (can read `down[5]`, out of bounds).
- **L17** `worldmap.cc:3512` — `HvrOffBtn` uses `brnpos[7]` if no button
  matches (cppcheck). Can't happen with registered buttons.
- **L18** `intface.cc:1830` — `id` is uninitialised for weapon animations
  not in the switch (clang-tidy), so a garbage FID is looked up (usually no
  art found, otherwise a wrong icon).
- **L23** `combat.cc:3735-3751` — if the "%s missed" combat message is
  missing, `text` stays uninitialised and `strcat(text, ".")` runs over
  stack garbage (stack overrun). Missing message files only.
- **L19** `intrpret.cc:272` — `interpretError` `longjmp`s across C++ frames
  (`std::vector` members of `Program` live on the heap, so it's fine
  today; adding RAII locals to opcode handlers would leak or skip
  destructors).
- **L20** Map/script variables can hold raw `Object*` (`map.cc:526-550`,
  `map_local_pointers`). Destroyed objects are not cleared, so a script that
  keeps a reference can later use freed memory. Original behaviour; noted
  as a heap-corruption vector in long sessions.
- **L21** `actions.cc:394`, `combatai.cc:1457` (and about 40 more
  `register_object_call*` sites) — callbacks with signatures `int(Object*,
  int)` called as `int(void*, void*)`. UB, but the AAPCS64/Apple arm64 and
  SysV x86-64 ABIs pass both in integer registers, so it works. Only
  clang's `-fsanitize=function` would object. Fix: wrapper functions with
  the exact signature.
- **L22** `gmouse_3d_build_hex_frame` (`gmouse.cc:1902`) — x offset
  `(width - length)/2` is negative for strings wider than the frame. With
  the row offset this stays inside the frame for realistic text.

## 5. Differences from fallout2-ce where F2 is more correct

Found with the structural comparison (§1). **Ported** on this branch:

| F1 location | F2 counterpart | Difference |
|-------------|----------------|------------|
| `queue.cc queue_clear_type` | `queue.cc _queue_clear_type` | F2 re-reads `curr` after the handler (sfall). Ported and extended to the "keep" branch. |
| `debug.cc debug_register_log` | `debug.cc _debug_register_log` | `\|\|` vs `&&`. |
| `game.cc game_init` | `game.cc gameInitWithOptions` | F2 honours `[debug] mode`. |
| `game.cc game_init` (`SCALE_2X`) | `svga.cc svga_init` | F2 rejects scaling below 640×480. |
| `text.cc GNW_text_width/_char_width/_to_buf` | `text_font.cc` | F2 masks with `& 0xFF`. |
| `fontmgr.cc FMtext_char_width` | `font_manager.cc` | F2 uses `unsigned char` in width/draw (char width takes `int`). |
| `art.cc art_get_base_name` | `art.cc artCopyFileName` | `\|\|` vs `&&` in the range check. |
| `gsound.cc gsound_close` | `game_sound.cc gameSoundFileClose` | F2 calls `intToPtr(handle, true)`. |
| `intextra.cc op_obj_can_hear_obj` | `interpreter_extra.cc opObjectCanHearObject` | F2 fixed the inverted NULL check. |
| `combatai.cc ai_find_attackers` | `combat_ai.cc aiFindAttackers` | `a4` vs `*a4`. |
| `grbuf.cc draw_line` | `draw.cc bufferDrawLine` | `p1 <= p2`. |
| `options.cc PrefStart` | `preferences.cc preferencesWindowInit` | Unlocks `[i]`, not `[i - 1]`. |

**Not ported (worth considering):**

- `kb.cc` — F2's logical key table has `SDL_NUM_SCANCODES` entries
  (`gLogicalKeyEntries`), F1 masks scancodes to 8 bits (L6).
- `intrpret.cc op_less_equal/op_greater` — F2 treats `ptr <= 0` as
  `ptr == nullptr` and compares as unsigned for positive ints. F1 compares
  `intptr_t`. Equivalent for user-space pointers on macOS/Linux.
- `gdialog.cc gdialog_review_display` — F2 stops before drawing the
  player's line once the reply reached the bottom (`y >= 407`). Visual only.
- `map.cc map_save_in_game` — F2 calls `animationStop()` before saving, so
  no animation callbacks hold object pointers across the save/load.
- `sound.cc soundClose` — F1 checks `gFadeSoundsTimerId != -1`, F2 `!= 0`
  (timer ids are 0 when unset). Harmless because `removeTimedEvent` checks
  0.
- `intrface.cc win_get_str` returns 0 instead of the `win_input_str`
  result (F2 propagates `rc`): a cancelled input looks like OK to callers.
- F2 converted many FRM users to RAII `FrmImage` (`.lock()`/`.unlock()`),
  which removes the lock-leak class (M11) structurally. A port would be a
  larger refactor.

**Shared original bugs that F2 also still has** (fixing them would benefit
both): H2 (menu frame pitch), H3 (FRM sizes), M4 (`buf_outline`), M5
(`windows[-1]`), M6 (sound fade timer), L5, L7.

## 6. macOS / arm64 specifics

- **Signed `char`**: Apple arm64 keeps `char` signed (like x86), unlike
  Linux arm64. So the F1 behaviour on the user's Mac matches Windows. The
  bugs in M1 exist on every platform; on macOS they bite with localized
  text. All `-Wchar-subscripts` hits (`text.cc:390`, `:401`,
  `fontmgr.cc:248`) are fixed. clang-tidy `bugprone-signed-char-misuse`
  remaining: `mouse.cc:351` (L13) and five in third-party `fpattern.c`
  (glob matching of file names; only matters for non-ASCII file names).
- **LP64**: no `long` is used for file I/O values except
  `db_freadLong(unsigned long*)` (two callers in `movie.cc`, used
  correctly) and `db_fwriteFloat`, which reads 8 bytes from a 4-byte float
  through `*(unsigned long*)&q` (`db.cc:1333`, stack over-read only, low
  32 bits used). Font headers and the interpreter use 32-bit fields
  explicitly; pointers go through `pointer_registry` or `ProgramValue`.
- **Alignment**: `mem_malloc` returns 16-byte-aligned memory; heap blocks are
  4-byte aligned, which is enough for every struct stored there.
  `movie_lib.cc` uses explicit LE load helpers. No `memcpy`-free type
  punning on unaligned data was found in hot paths.
- **Function-pointer casts** (L21): safe on arm64 because no variadic,
  floating-point or by-value struct parameters are involved. Checked all
  `(AnimationCallback*)`, `(AnimationCallback3*)`,
  `(InterpretCheckWaitFunc*)` and `doTimerEvent` casts.
- **Threads**: SDL audio callback (`audio_engine.cc`) locks each sound
  buffer's `std::recursive_mutex`; it doesn't touch game memory. The SDL
  timer path in `sound.cc` is the only other thread (M6, unreachable).
- **Fullscreen change on this fork** (`svga.cc`): `RESIZABLE` now delivers
  `SDL_WINDOWEVENT_SIZE_CHANGED`, so `handleWindowSizeChanged()` recreates the
  renderer and texture surface, and `input.cc:1120` refreshes all windows
  afterwards. Fine. If `createRenderer` fails there, `renderPresent()`
  dereferences a NULL texture surface (low).

## 7. Tool output triage

Compiler (`-Wall -Wextra`), non-trivial warnings:

| Warning | Location | Verdict |
|---------|----------|---------|
| char-subscripts | `text.cc:390`, `:401`; `fontmgr.cc:248` | real, fixed (M1) |
| int-to-void-pointer-cast | `actions.cc:394`, `:1372`; `combatai.cc:1455` | intentional (callback params), L21 |
| sometimes-uninitialized | `actions.cc:1406` keyCode, `anim.cc:2870` x/y, `intlib.cc:1490`, `:1535` v1 | read-before-init only on error paths; low |
| format-security | `protinst.cc:968`, `audio.cc:64` | format strings from message files or debug; low |
| self-assign | `intrpret.cc:175-176`, `window.cc:1833-1835` | decompilation artefacts, harmless |
| logical-op-parentheses | `endgame.cc:276` | precedence matches the original |
| unused-value | `elevator.cc:547` (`toupper` result ignored) | original bug; keys stay case-sensitive |
| switch | `mouse.cc:436` | harmless |

cppcheck: `cache.cc:525` (L11), `art.cc:472` (fixed L2), `anim.cc:1446`
(false positive: `-1000` is checked after every step), `gdialog.cc:1610`
(the index comes from button event codes `1200 + i` registered only for
existing options; keycodes up to 1250 are accepted, so a stray event code
1230-1250 would read past `options[30]`; low),
`worldmap.cc:3522/3528` (L17), `editor.cc:3492` (L16), `intlib.cc:1315`
(L9), `window.cc:1680…` (M5), `roll.cc:222` (debug self-test, value never
negative), `debug.cc:58` (fixed M8), `db.cc:1276/1333` (§6), `map.cc:1262`,
`scripts.cc:789` (`ctx.section` is passed uninitialised, but `town_map`
sets it before reading), `combat.cc:3751` (L23), `intrpret.cc:2794/2863`
(false positive: `env` is filled by `memcpy` before use), `mousemgr.cc:482`
(uninitialised width/height on an error path, not traced further).

clang-tidy: 302 `insecureAPI.strcpy` (sizes reviewed for message-text
sources, L14), 161 missing `default`, 58 multi-level pointer conversions
(cache arrays, fine), 20 `sizeof-expression` (all `sizeof(*ptrArray)` on
pointer arrays, fine), about 60 analyzer hits (mostly `tile_coord`
returning early for invalid tiles without setting its outputs, which some
callers then use; the garbage coordinates are clipped by the render rects),
`intextra.cc:1642` (real: M12, fixed), `intextra.cc:2191` (fixed M9),
`memory.cc:205` (zero-size `realloc`, handled).

## 8. Module checklist

Depth: **R** = read in full or nearly so; **T** = targeted (all writes into
buffers, allocation and size math, every 640/480 assumption, lock/unlock
pairing); **S** = covered by tools (warnings, cppcheck, clang-tidy), grep
scans (drawing destinations, `strcpy` targets, global arrays, art locks)
and the F1↔F2 structural diff. Every file got at least S.

### `src/` (top level)
| Module | Depth | Result |
|--------|-------|--------|
| `movie_lib.cc` | T | `getOffset` fix present (branch base); decoder writes to correctly placed 8×8 blocks; `_nfConfig` sizes surfaces from the stream header |
| `audio_engine.cc` | R | mixer thread-safe per buffer; no game-memory access |
| `pointer_registry.cc` | R | grows unbounded unless `remove` is used (L1 fixed) |
| `platform_compat.cc`, `fps_limiter.cc` | S | — |

### `src/plib/gnw`
| Module | Depth | Result |
|--------|-------|--------|
| `memory.cc` | R | layout documented (§3.1, AGENTS.md) |
| `gnw.cc` | R | refresh and clipping correct; `win_add` clamps; no unchecked `GNW_find` in hot paths |
| `grbuf.cc` | R | L4 fixed; M4 |
| `text.cc` | R | M1 fixed; L12 |
| `button.cc` | T | unclipped `button_draw` with NULL bound (documented); images are raw art pointers |
| `svga.cc` | R | L10; fullscreen change OK |
| `mouse.cc` | T | `mouse_show` clipping OK; L13 |
| `input.cc` | T | `dump_screen`/`buf_blit` OK; window events OK |
| `kb.cc` | T | L6 |
| `intrface.cc` | T | timed messages OK; `win_get_str` rc (§5) |
| `debug.cc` | R | M8 fixed |
| `rect.cc`, `vcr.cc`, `touch.cc`, `dxinput.cc`, `winmain.cc` | S | — |

### `src/plib/db`, `color`, `assoc`
| Module | Depth | Result |
|--------|-------|--------|
| `db.cc` | T | `db_fread`/`db_fgetc`/`db_fgets`/`db_preload_buffer` bounds OK for valid DATs; L8 |
| `lzss.cc` | R | L8 |
| `color.cc` | S | analyzer hits are uninitialised-on-unreachable paths |
| `assoc.cc` | S | `long` fields only in unused save/load helpers |

### `src/game`
| Module | Depth | Result |
|--------|-------|--------|
| `heap.cc` | R + fuzz | sound; diagnostics added |
| `cache.cc` | R | sound; L11; diagnostics hooks |
| `art.cc` | R | H3; L2 fixed |
| `skilldex.cc` | R | same as F2; victim, not cause |
| `gmouse.cc` | T | H2, M4 callers, M11, L22 |
| `intface.cc` | T | 640-pitch writes all go to the 640-wide window; item button buffers are static 188×67 arrays; L18 |
| `display.cc` | R | M2 fixed |
| `tile.cc` | T | render clipping correct (CE bounds clamp better than F2) |
| `object.cc` | T | update tables sized for the screen; L5; M11 (egg) |
| `textobj.cc`, `wordwrap.cc` | R | L7 |
| `inventry.cc` | T | M10; `scale_art` callers sized correctly |
| `loadsave.cc` | T | thumbnail uses real window size; buffers sized |
| `options.cc` | T | M3 fixed |
| `editor.cc` | T | L14–L16 |
| `gdialog.cc` | T | 640 pitches match the window; §5 note |
| `worldmap.cc` | T | 640 pitches on the 640×480 window; L17 |
| `pipboy.cc`, `automap.cc`, `endgame.cc`, `mainmenu.cc`, `select.cc`, `credits.cc`, `elevator.cc`, `bmpdlog.cc`, `main.cc` | T | fixed-size windows centred; writes consistent with window sizes |
| `queue.cc` | T | H1 fixed |
| `combat.cc`, `combatai.cc`, `actions.cc`, `anim.cc` | T | L3 fixed; L14; L21; `anim_set_check` false positive |
| `gsound.cc`, `sfxcache.cc`, `sfxlist.cc` | T | M6 caller analysis; L1 fixed |
| `message.cc` | R | field length bounded to 1023 |
| `map.cc`, `scripts.cc` | T | var accessors bounds-checked; L20 |
| `fontmgr.cc` | R | M1 fixed |
| `game.cc` | T | M7, M8 fixed |
| `item.cc`, `critter.cc`, `proto.cc`, `protinst.cc`, `stat.cc`, `skill.cc`, `perk.cc`, `trait.cc`, `party.cc`, `reaction.cc`, `roll.cc`, `light.cc`, `cycle.cc`, `palette.cc`, `moviefx.cc`, `gmovie.cc`, `lip_sync.cc`, `config.cc`, `gconfig.cc`, `gmemory.cc`, `graphlib.cc`, `selfrun.cc`, `version.cc`, `amutex.cc`, `gdebug.cc`, `worldmap_walkmask.cc` | S | no write-side issues beyond L14 |

### `src/int`
| Module | Depth | Result |
|--------|-------|--------|
| `intrpret.cc` | T | typed vector stacks with size checks; L19 |
| `intlib.cc` | T | L9 |
| `support/intextra.cc` | T | M9, M12 fixed |
| `window.cc` | T | M5 |
| `sound.cc` | T | M6 |
| `movie.cc` | T | alpha buffer size from file (trusted data) |
| `audio.cc`, `audiof.cc`, `datafile.cc`, `dialog.cc`, `export.cc`, `memdbg.cc`, `mousemgr.cc`, `nevs.cc`, `pcx.cc`, `region.cc`, `share1.cc`, `widget.cc` | S | — |
