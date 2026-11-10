# Comparison: my review vs. Sashiko's review of the gimple-ssa-sprintf series

Sashiko review: https://gcc.sashiko.dev/#/patchset/avHlscY-_JnBzZ1F%40localhost.localdomain
(fetched via its JSON API, `/api/patchset?id=...`; model gemini-4-argon)

My review: `~/sprintf-series-review.md`

Sashiko's review overlaps with mine on the main bugs. It also caught several
real issues I missed, two of which I had looked at and wrongly dismissed. I
checked its two most surprising claims (the heap over-read and the LTO
assembler name) against the source and both hold.

## Found by both

- **`%.*s` precision range (commit 1):** both found the same three variants:
  a bounded range, a range that includes negatives, and 16-bit-int targets.
- **Non-integer argument to `%d`/`%u` crashes the compiler (commit 3):** same
  reasoning, all the way through `format_directive`'s early return and
  `is_call_safe`.
- **Leftovers when the fold gives up partway:** both flagged the leaked
  temporaries, SSA names and half-built statement list, and the `itoa`/`utoa`
  helper that gets emitted but never called. Both suggested validating every
  directive first.
- **`size_type_node` vs `sizetype` offsets:** Sashiko adds a concrete affected
  target (64-bit-pointer VMS), which makes this stronger than my "may not
  hold on every target".

## Caught by Sashiko, missed by me

1. **Host heap over-read in `to_string_literal` (High). Confirmed.** For the
   trailing NUL directive (`len == 1`, `beg` points at `'\0'`),
   `xstrndup (beg, 1)` allocates 1 byte. Then `build_string_literal (2, dup)`
   copies 2 bytes from it. This happens on every folded call and will fire
   under bootstrap-asan or Valgrind. I traced this exact path, but only
   checked the output was right and missed the out-of-bounds read.
2. **`-fexec-charset` breaks constant `%d` (commit 2).** `build_string_from_int`
   uses `print_dec`, which writes host ASCII, while the plain-text pieces are
   copied as target bytes. Under `-fexec-charset=IBM1047`,
   `sprintf (buf, "x=%d", -42)` produces mixed EBCDIC and ASCII. I only raised
   the charset problem for the COMDAT helper and didn't check the constant
   path. This is real wrong code.
3. **`%.1s` on a 1-byte array (or 1-byte tail) folds to a 0-byte copy.** With
   a precision, C allows the array to have no NUL terminator. In this mode,
   `get_range_strlen` derives the length from the array size and reports
   min = max = 0. I considered this case and wrongly decided it was fine.
4. **LTO assembler name. Confirmed.** `DECL_ASSEMBLER_NAME (fn_decl)` is
   computed before `make_decl_one_only` sets `TREE_PUBLIC`. Under LTO,
   `lto_set_decl_assembler_name` then takes the private-name path and
   produces names like `itoa.gcc_strlen.0`, so copies from different
   partitions or objects don't get merged.
5. **Warning suppression lands on the wrong location.** `copy_warning` /
   `suppress_warning` run while the new statements still have
   `UNKNOWN_LOCATION`, before `gsi_replace_with_seq_vops` sets their location.
   So `-Warray-bounds` suppression either doesn't take effect or suppresses
   every warning on those statements. Subtle and plausible; not verified.
6. **`_BitInt` arguments.** The sprintf code runs inside the strlen pass,
   after `_BitInt` lowering, so a new conversion from a large `_BitInt` would
   reach RTL expansion still unlowered.
7. **Smaller ones:**
   - `copy_warning` is missing on the `%d` memcpy.
   - `build_string_from_int` doesn't set `TREE_READONLY`/`TREE_STATIC`, so the
     RTL memory isn't marked read-only (I only said "use
     `build_string_literal`").
   - Its fixed-size buffer can overflow for very wide `INTEGER_CST`s.

One claim of Sashiko's is overstated. It says a pointer argument ICEs
whenever its precision differs from `dirtype`. The GIMPLE verifier does allow
pointer->integer truncation, so only extension is a problem (e.g. x32 `%ld`),
and the `POINTERS_EXTEND_UNSIGNED` exception may cover even that.

## Caught by me, missed by Sashiko

- **COMDAT helper design and ABI risk:** the linker keeps one arbitrary copy
  across translation units built with different `-fexec-charset`,
  `-mregparm`/`-mrtd`/`-mabi`, or GCC versions. Sashiko only spotted the LTO
  naming problem.
- **`TREE_NOTHROW`** isn't set on the helpers, and no cgraph edge is created
  to them.
- **`strnlen`** uses `builtin_decl_explicit` where it should use `implicit`.
- **No size check:** nothing stops the fold from growing code at `-Os`.
- **Test problems:**
  - `-35.c` points to a `builtin-sprintf-36.c` that doesn't exist.
  - Its `dg-require-weak` doesn't match the actual conditions, so it fails on
    mingw64.
  - `test_unhandled` lists cases that commit 3 now handles.
  - There are no tests for the two crash/miscompile bugs.
- **ChangeLog and comment problems** in commits 1, 3 and 4. Sashiko reported
  "No issues found" for commit 4.

## Overall

Sashiko was stronger on low-level GCC internals: memory safety,
warning-suppression machinery and LTO naming. I was stronger on design, ABI
and the tests. The over-read and the exec-charset bug in commit 2 are the
most important things I missed.
