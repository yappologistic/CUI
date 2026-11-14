# CUI Bug-Fix and Improvement Report

**Repository:** https://github.com/yappologistic/CUI (cloned at `master`, v1.4.0)
**Date:** 2026-11-12
**Scope:** full review of the library sources (~52,000 lines of C), all fixes applied, all tests re-run.

---

## 1. How this was done

1. **Baseline.** Cloned the repo, built it with GCC 14 (`-Wall -Wextra`, zero warnings) and ran the project's own test suite: 17/17 passed. The library was already in good visible shape, so the hunt targeted *latent* bugs the existing tests do not reach.
2. **Sanitizers.** Built the whole project plus every test and fuzzer with `-fsanitize=address,undefined` and re-ran all 17 tests (all passed — confirming the bugs below are edge-case-triggered, not exercised by the suite).
3. **Line-by-line review.** Seven independent review passes over every file in `src/` (core, widgets, data/parsing, text/code, charts, platform/render, planning/emoji), each finding verified by hand against the surrounding code before fixing.
4. **Fixes + regression tests.** Every fix was implemented and, where headless testing is possible, locked in with a new regression test file (see §5).
5. **Re-verification.** Rebuilt everything: zero warnings, 18/18 tests pass, the full suite also passes under ASan+UBSan, extended fuzzing (28,000 frames of random input over the gallery and example apps) is clean, and the single-file amalgamation (`tools/amalgamate.py`) still compiles and runs a hello-world app.

**Totals: 40 distinct defects fixed across 29 source files (649 insertions), 1 new public function, 1 new test program with 174 checks.**

---

## 2. Crashes and memory-safety bugs (most severe first)

| # | Where | Bug | Fix |
|---|---|---|---|
| 1 | `src/cui_regex.c` | **Any pattern with a non-capturing group `(?:...)` corrupted memory.** The matcher indexed the capture array with the group index `-1`, overwriting the matcher's own text pointer and length (`cap[-1]` sits on top of them). Reproduced: segfault on `cui_regex_search("(?:a)b", ..., "ab")`. The test suite never used `(?: )`, which is why it survived. | Capture bookkeeping is skipped for `group == -1` in both the `RX_GROUP` and `RK_GROUP_END` paths. Verified: matches, capture offsets of neighbouring groups stay correct. |
| 2 | `src/cui_pickers.c` | `cui_histogram_slider_ex` copied `bin_count` floats after allocating only `max(1, bin_count)` — a negative count cast to a huge `size_t` memcpy'd off the heap; `bins == NULL` with a positive count crashed. | Negative count and NULL bins are rejected up front; the frame-arena copy (kept for lifetime safety) uses the validated count. |
| 3 | `src/cui_devviews.c` | `cui_blame_ex` explicitly allowed `line_commit == NULL` but dereferenced it on every row after the first (`pc != line_commit[i]`) — rendering any file without blame data with ≥2 lines crashed. | The run-start comparison guards for NULL (each line simply becomes its own run, matching the already-guarded reads). |
| 4 | `src/cui_grid.c` + `src/cui_data.c` | `cui_data_grid_cell_text` / `cui_table_cell_text` read the previous column's alignment with `col - 1 < ncols` — true when `col == 0` (signed compare), so they read `cols[-1]`, before the caller's column array. | Lower bound added (`col >= 1 &&`). |
| 5 | `src/cui_grid.c` | A caller-supplied grid `cursor`/`anchor` past the last row (e.g. rows deleted elsewhere) was adopted into persistent state verbatim, written back to the app, and used to index the selection array out of bounds on Space. | Stale indexes clamp to `nrows - 1` on adoption (and clear when the grid is empty); the Space toggle re-checks the bound. |
| 6 | `src/platform/cui_dialogs.c` | **Stack buffer overflows in file dialogs.** The kdialog and macOS AppleScript branches accumulated `snprintf` *would-be* lengths into an offset; one long filter extension pushed the offset past the buffer, and the unconditional final append then wrote past the end with an underflowed size. | The offset is clamped to the buffer before the final append (both branches). |
| 7 | `src/cui_core.c` | The state store's grow path rehashed **before** zeroing the new slot, so the rehash indexed uninitialized memory: the entry could be linked into two buckets (state returned for the wrong widget) or form a self-referential chain (lookup hang). | The new slot is zeroed before the rehash scan. |
| 8 | `src/cui_text.c` | Glyph atlas packing checked horizontal fit only before the row wrap; a glyph as wide as the atlas rasterized past the row end (heap overflow with huge font sizes or a corrupt font). Related: glyph boxes overflowing the 16-bit glyph-record fields. | Horizontal fit is re-checked after the wrap and grows the atlas; pathological outline boxes (`> CUI_ATLAS_MAX - 2`) are rejected. |
| 9 | `src/render/cui_render_sw.c` | A texture created with width or height 0 made the sampler's clamp degenerate (`CLAMP(x, 0, -1)` = -1) and read at index -1 of a zero-byte allocation. Texture updates also wrote past the right edge when the update rect overhung. | Degenerate textures become 1×1; updates clip to the texture. |
| 10 | `src/cui_extras.c` | Reorderable list: a drag start reused `press_pos` captured under the *previous* frame's item count — list shrank between press and drag threshold → out-of-bounds read of slot geometry and out-of-bounds write into the caller's `order` array on commit. | `press_pos` is re-validated against the current count when the drag starts. |
| 11 | `src/cui_files.c` (Win32) | When the name buffer filled exactly, `WideCharToMultiByte` ran in size-query mode (returns a length, writes nothing), leaving a file entry pointing one past the buffer. | Room is checked before the conversion, like the POSIX branch already did. |
| 12 | `src/cui_strings.c` | The time-zone helper never checked `localtime_r`/`gmtime_r`; an out-of-range file mtime left the `struct tm` untouched and rendered garbage dates from uninitialized memory. | Structs are zero-initialized and failures return UTC. |
| 13 | `src/cui_shape.c` | Mark-to-base/ligature attachment indexes computed `bi * classes + mc` in `int` — signed overflow (UB) with a crafted font. | Computed in unsigned. |

---

## 3. Wrong-behaviour (logic) bugs

| # | Where | Bug | Fix |
|---|---|---|---|
| 14 | `src/cui_charts.c` | `cui__nice_range` on an all-one-value series at \|value\| ≳ 3.4e7 (float ULP ≥ 1): `hi = lo + 1` rounds back to `lo`, `log10(0) = -inf`, step = 0/0 = **NaN** — every point and label NaN, chart never stops redrawing. | Range widens relative to magnitude; a non-positive step falls back to 1. |
| 15 | `src/cui_planning.c` | Week calendar headers showed the k-th weekday name in a Monday-first table instead of the weekday of each column's date — every header wrong for any `week_start` that is not a Monday. | Headers now name the column's actual date (`((base + k + 3) % 7 + 7) % 7`). |
| 16 | `src/cui_planning.c` | Event times like 9.99 h rendered "9:60" (no minute carry). | Minutes computed with carry (`10:00`). |
| 17 | `src/cui_media.c` | `cui_card_stack_item_begin` outside a card stack popped one id more than it pushed, shifting every later widget id for the rest of the frame (wrong hover/press state, duplicated ids). | The inert path pushes a compensating id, as every other widget does. |
| 18 | `src/cui_dials.c` | Knob and fader quantized the app's value to `step` and wrote it back **every frame**, even when disabled or untouched — a disabled knob rewrote live values and reported "changed". | Write-back happens only when an input path actually ran. |
| 19 | `src/cui_overlay.c` | `cui__days_in_month`/`cui__weekday` indexed month tables with no range check; `cui_calendar` seeded `view_m` from a caller date with month 0 (the natural "unset" value) → `t[-1]`/`dm[-1]`. | Helpers wrap defensively; the calendar seeds from today when the month is out of range. |
| 20 | `src/cui_overlay.c` | Popup build stack: the 17th nested popup aliased the 16th's record, mismatching the two deepest `push_disabled`/`pop_disabled` pairs (input globally wrong for the frame). | Nesting deeper than 16 is refused cleanly before any state changes. |
| 21 | `src/cui_data.c` | Accordion nesting ≥ 32 pushed "disabled" without recording it → the pop never ran and the rest of the frame was inert. | Push and pop stay paired at any depth. |
| 22 | `src/cui_data.c` | Virtual list: the 9th nested list overwrote the 8th's stack record → corrupted bottom spacer. | Deeper lists are marked and skip the spacer bookkeeping. |
| 23 | `src/cui_code.c` | A tab measured as three spaces but painted as four → tab-indented code clipped at the right edge. | Both use four. |
| 24 | `src/cui_code.c` | The code block tokenized into a fixed 256-token buffer and silently dropped the rest — lines with >256 tokens (minified JS/JSON) were truncated mid-line while still occupying width. | The remainder of the line is drawn as plain text after the buffer fills. |
| 25 | `src/cui_textedit.c` | Password fields measured caret/selection x through a fixed 1 KB mask buffer → past ~340 characters the caret desynced from the glyphs. | Mask width is computed arithmetically (one bullet advance per code point). |
| 26 | `src/cui_emoji.c` | The emoji scanner's private UTF-8 decoder accepted invalid lead bytes (0x80–0xC1, 0xF5–0xFF) and swallowed malformed continuations — one bad byte corrupted the following emoji's code points. | Validates lead and continuation bytes; emits one U+FFFD per bad byte, exactly like the core decoder. |
| 27 | `src/cui_media.c` | Rolling number: values with >24 digit characters aliased the last digit spring, making leading digits fight each other. | Digits beyond the spring slots render statically. |
| 28 | `src/cui_extras.c` | Tag input: >64 tags silently deleted everything past 64 when any chip was removed; the rebuild also truncated at a fixed 2 KB. | Split arrays sized from the buffer capacity; rebuild buffer sized from `cap`. |
| 29 | `src/cui_instrument.c` | A caller axis step far finer than the span (`x_step = 0.001` over 1000 units) looped for hundreds of millions of iterations (effective hang, frame-arena flood). | Step clamps to at most 1024 ticks per axis. |
| 30 | `src/cui_theme.c` | Switching theme/accent restored only *some* non-color fields (radii, motion, font scale, one shadow) and silently reverted the app's other customizations (control heights, text sizes, headings, remaining shadows). | The whole non-color tail of the theme is preserved through a retarget. |
| 31 | `src/cui_controls.c` | Slider and range slider: a track narrower than its two 9 pt pads divided by zero → NaN poisoned the value through the clamp. | Denominator guarded. |
| 32 | `src/cui_draw.c` | `cui__draw_check` on a degenerate (0×0) rect computed 0/0 → NaN vertices into the mesh, defeating clip tests. | Degenerate rects return early. |
| 33 | `src/cui_devviews.c` | Coverage view: a folder with more than 512 direct children silently lost the rest (not sorted, not shown, missing from rows). | Children past 512 flatten in chain order after the sorted first 512. |
| 34 | `src/cui_terminal.c` | SGR parameter accumulation had no bound → signed overflow (UB) on a long digit run in escape output. | Clamped during accumulation. |

---

## 4. Robustness / protocol fixes

| # | Where | Bug | Fix |
|---|---|---|---|
| 35 | `src/platform/cui_app_x11.c` | `SelectionRequest` was answered for **any** selection — another client requesting PRIMARY received the CLIPBOARD text — and `property == None` (ICCCM self-naming case) raised BadAtom. | The handler answers only for the clipboard; when the property is unset the target atom names it, per ICCCM. |
| 36 | `src/platform/cui_app_x11.c` | Clipboard reads had no INCR (multi-chunk) handling: a large selection's announced byte count was interpreted as text (garbage paste). | INCR replies now yield an empty paste instead of garbage (full INCR support is a larger feature; documented in the changelog). |
| 37 | `src/cui_layout.c` | The painter kept a node pointer across paint callbacks; a callback that builds UI reallocates the node array and the pointer dangles (the code already refetched at one site, acknowledging the invariant). | Refetch after **every** paint callback. |
| 38 | `src/cui_overlay.c` | `cui__popup_is_topmost` ended in `return ui->dialog_depth == 0 \|\| true` — dead code that looked like a bug. Analysis showed popups-over-dialogs *is* the design (dialogs already defer to any open popup; making both yield would deadlock Escape), so the dead operand was replaced with a comment documenting the design. | Comment + `return true` with rationale. |

---

## 5. Added

### `cui_regex_replace` (new public function)

```c
CUI_API int cui_regex_replace(const char *pattern, int flags, const char *text, int len,
                              const char *replacement, char *out, size_t out_cap, const char **error);
```

Replace every match of a pattern with a template in which `$0`–`$9` stand for the whole match and the capture groups (JavaScript style — the engine already advertises the JS/PCRE/Python common subset, but had no replace). Semantics:

- snprintf-style return: the number of bytes the *whole* result needs; `>= out_cap` means truncated. `out == NULL` measures without writing.
- No match: text copied unchanged. Bad pattern: returns -1 with `*error` set.
- An empty match advances one character, so `x*` terminates; this is the JavaScript empty-match rule (over `a`, `a*?` yields `-a-` in JavaScript and `---` in Python 3.7's `re.sub`).
- UTF-8-safe: an empty match at a continuation byte steps a whole character.

### `tests/test_fixes.c` (new regression suite, 174 checks)

Registered as `cui_test_fixes` in CTest. Covers, headlessly: `(?: )` groups (the former segfault), capture offsets around non-capturing groups, optional groups, `[\b]` backspace, `[\B]` rejection, all `cui_regex_replace` edge cases; the flat-series chart (previously NaN, now settles without endless redraw); the stale grid cursor clamp; negative/NULL histogram bins; accordion nesting past 32 (input stays enabled); the disabled knob (value untouched); the unset calendar month; blame with NULL commits; theme customization surviving an accent switch; and malformed UTF-8 through the emoji API.

### CHANGELOG entry

All fixes and additions are documented in `CHANGELOG.md` under *Unreleased*, in the project's own style.

---

## 6. Verification (all commands and results)

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
    → 0 warnings (GCC 14, -Wall -Wextra)
ctest --test-dir build -j4
    → 100% tests passed (18/18, including the new cui_test_fixes)
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined ..." (plus tests, fuzzers)
ctest --test-dir build-asan -j4
    → 100% tests passed (18/18) under ASan + UBSan
./tests/cui_fuzz 20000        → fuzzed 20000 frames OK
./tests/cui_fuzz_tracker 8000 → fuzzed 8000 frames OK
python3 tools/amalgamate.py /tmp/cui_single
    → cui.h + cui.c + cui_single.h compile clean; a hello-world app built from the
      single file runs correctly
Standalone repro of the old crash: cui_regex_search("(?:a)b", 0, "ab", ...) → segfault before, matches after
```

Known limitation left open deliberately: full ICCCM INCR clipboard transfer on X11 (garbage-paste fixed by returning empty; a complete implementation needs an event-loop rework and a second X client to test against).

---

## 7. Files changed

```
CHANGELOG.md                  | 45 +++++
include/cui/cui.h             | 12 ++      (cui_regex_replace + docs)
src/cui_charts.c              |  4 +-     (nice_range NaN)
src/cui_code.c                | 10 +-     (tab width, token truncation)
src/cui_controls.c            |  6 +-     (slider div-by-zero)
src/cui_core.c                |  3 +      (state store, spring NaN)
src/cui_data.c                | 33 +++--   (table col[-1], accordion, virtual list)
src/cui_devviews.c            |  6 +-     (blame NULL deref, coverage 512 cap)
src/cui_dials.c               | 20 ++-    (knob/fader write-back)
src/cui_draw.c                |  1 +      (check-mark NaN)
src/cui_emoji.c               | 11 +-     (UTF-8 validation)
src/cui_extras.c              | 28 ++--   (reorder OOB, tag input, deep states)
src/cui_files.c               |  3 +-     (Win32 name buffer)
src/cui_grid.c                | 11 +-     (col[-1], cursor clamp)
src/cui_instrument.c          |  1 +      (axis step clamp)
src/cui_internal.h            |  1 +      (<stddef.h>)
src/cui_layout.c              |  2 +      (paint-callback refetch)
src/cui_media.c               | 10 +-     (card stack id, odometer)
src/cui_overlay.c             | 14 +-     (popup stack, month tables, topmost comment)
src/cui_pickers.c             | 21 ++-    (histogram guards, signature buffer)
src/cui_planning.c            |  5 +-     (week headers, time carry)
src/cui_regex.c               | 69 +++++-  ((?: ) crash, [\b], cui_regex_replace)
src/cui_shape.c               |  4 +-     (unsigned attachment index)
src/cui_strings.c             |  6 +-     (localtime checks)
src/cui_terminal.c            |  2 +-     (SGR clamp)
src/cui_text.c                |  6 +      (atlas packing)
src/cui_textedit.c            |  8 +-     (password mask)
src/cui_theme.c               | 11 +-     (retarget preservation)
src/platform/cui_app_x11.c    | 21 ++-    (selection/ICCCM/INCR)
src/platform/cui_dialogs.c    |  2 +      (dialog buffer clamps)
src/render/cui_render_sw.c    |  8 +-     (texture guards)
tests/CMakeLists.txt          |  5 +      (cui_test_fixes)
tests/test_fixes.c            | 351 +++++++ (new)
```
