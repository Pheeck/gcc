# Review: gimple-ssa-sprintf series (579b295e224..076963dd02e)

Commits:
1. 579b295e224 Fold sprintf with plain strings and %s into memcpy sequences
2. a76dedabf25 Handle directives %d, %i and %u for INTEGER_CST
3. 0b3cf1de639 Handle %d and %u when argument value is unknown at compile-time
4. 076963dd02e Update gimple-ssa-sprintf.cc's description comment

Nothing was built or run; everything below comes from reading the code.
Line numbers refer to gcc/gimple-ssa-sprintf.cc at HEAD.

## Definite bugs

### 1. `%.*s` uses the upper bound of the precision range, not the actual value (commit 1) -- wrong output

`dir.prec[]` is a *range*. When the precision comes from `*`,
`set_precision()` -> `get_int_range()` fills it from VRP. The fold only
rejects `prec[1] == INT_MAX` and then treats `prec[1]` as an exact precision,
both in the clamp of the constant-length path and in the `strnlen` argument.

```c
void f (char *d, const char *s, int p)
{
  if (p >= 1 && p <= 3)
    sprintf (d, "%.*s", p, s);   // folded to strnlen (s, 3) + memcpy; wrong for p == 1, 2
}
```

- Negative precision is also mishandled. A range like p in [-3, 3] becomes
  `prec = [-1, 3]`. A negative precision means "no precision", yet the fold
  still caps at 3.
- 16-bit-int targets (AVR, msp430) are hit even with no range info. An unknown
  `*` precision becomes `[-1, 32767]`, which never equals the host `INT_MAX`.
- Fix: only fold when `prec[0] == prec[1]`, or when `prec[1] < 0`.
- Tests: `g_prec_star` doesn't catch this because there `p` is fully unknown.
  Add a test with a bounded `p`.

### 2. Non-integer argument to `%d`/`%i`/`%u` crashes the compiler (commit 3)

The runtime path only excludes `INTEGER_CST`. It never checks that `dir.arg`
has an integral type before building `NOP_EXPR`s to `dirtype` and `long`.

```c
void f (char *d, double x) { sprintf (d, "%d", x); }   // only a -Wformat warning
```

1. `format_integer` returns an empty `fmtresult` for the double argument.
2. `format_directive` hits the `range.min > target_dir_max ()` early return,
   which reports success.
3. `compute_format_length` therefore succeeds, and with the return value
   unused `is_call_safe` passes.
4. The fold then emits `NOP_EXPR <double>` -> `int`, which is invalid GIMPLE:
   an ICE in checking builds, wrong code otherwise. Aggregate and vector
   arguments break the same way.

Fix: require `INTEGRAL_TYPE_P (TREE_TYPE (dir.arg))` in the `%d`/`%u` case,
the way the `%s` case already checks `POINTER_TYPE_P`. Pointer arguments are
probably fine (truncating pointer->int is valid GIMPLE), but it's simpler to
reject them too.

## Design and ABI risk (commit 3)

### 3. The shared `itoa.gcc_strlen` / `utoa.gcc_strlen` helpers can be wrong after linking

The helpers are one-only/COMDAT, hidden and public, so the linker keeps one
copy chosen arbitrarily. That is only safe if every translation unit's copy
behaves the same and uses the same calling convention. That doesn't hold
across:

- Character set: `-fexec-charset`. `'0'` and `'-'` come from
  `lang_hooks.to_target_charset`, so a non-ASCII translation unit can end up
  using an ASCII copy, or the reverse.
- Calling convention: options like `-mregparm`, `-mrtd` (i386, where `long` is
  32 bits so the helper is used) and `-mabi=ms`/`sysv`.
- Contract stability: the helper's behaviour has to stay the same across all
  future GCC versions, since objects built by different versions can link
  together.

The alternatives are a libgcc routine, a local (non-COMDAT) static function,
or a name that encodes the relevant ABI and options.

Related smaller points:

- `TREE_NOTHROW` is not set on the decl, so in `-fexceptions` code the new
  call looks like it may throw, while the `sprintf` it replaced was nothrow.
- No cgraph edge is created from the caller to the new function. It's still
  emitted because of `force_output`, but it's worth a check with
  `--enable-checking=yes,extra`.

## Smaller issues

4. Work is thrown away on bail-out (commits 1 and 3). Each directive is
   validated only while statements are being built. A later unsupported
   directive therefore leaves behind:
   - a leaked temporary, SSA names whose defining statements are never
     inserted, and a half-built sequence;
   - in commit 3, a fully built `itoa`/`utoa` function that is emitted because
     of `force_output` but never called. For example, `"%d%x"` creates `itoa`
     and then gives up on `%x`.

   Splitting this into a validation pass followed by a build pass would fix
   all of it and simplify the code.
5. `strnlen` uses `builtin_decl_explicit` (commit 1). The comment says "may
   not be available", but the explicit decl always exists. The fold will emit
   `strnlen` calls on targets or C libraries that lack it. Use
   `builtin_decl_implicit` (or check `builtin_decl_implicit_p`).
6. Offset types are mixed (commit 1). `len` is built as `size_type_node`
   (plain-string and `strlen` paths) or as `sizetype` (`%s`-constant and `%d`
   paths), and used as the `POINTER_PLUS_EXPR` offset. The verifier requires
   `ptrofftype_p`, which `size_type_node` doesn't satisfy on every target. Use
   `sizetype` consistently.
7. No size check. One `sprintf` call becomes N x (`strlen` + `memcpy`), and
   commit 3 adds a whole function. Nothing like `optimize_bb_for_size_p`
   guards this, so `-Os` gets bigger code.
8. `build_string_from_int` (commit 2) doesn't use the usual helper. It builds
   the `ADDR_EXPR`/array type by hand instead of using `build_string_literal`.
   It also calls `print_dec`, which falls back to hex for values needing more
   than one HWI. That can't happen for in-range values today, but it's a trap
   for whoever adds base 16 or wider types later.

## Tests and ChangeLog

- Missing file: `builtin-sprintf-35.c` says "Calls left alone on all targets
  are in builtin-sprintf-36.c", but no `-36.c` exists in the series.
- Wrong `dg-require` in `-35.c`: it uses `dg-require-weak`, but the code
  requires `supports_one_only ()` and `TYPE_PRECISION (dirtype) <= long`. On
  LLP64 (mingw64) `%zu` isn't folded, so `scan-tree-dump-not "sprintf \\("`
  fails there.
- Stale test: `test_unhandled` in `builtin-sprintf-30.c` still lists
  `"%d", i` and `"%s%d"` as unhandled, but commit 3 now handles them on most
  targets.
- No tests for bugs 1 and 2: nothing covers a bounded `%.*s` precision or a
  non-integer argument to `%d`.
- ChangeLog for commit 1:
  - "Move to gimple_fold_builtin_sprintf" should say "Move to
    gimple-ssa-sprintf.cc".
  - It lists `directive::create_fmt`, but the function is called
    `to_string_literal`.
  - The `gcc.dg/tree-ssa` test entries don't match what that commit actually
    adds.
- ChangeLog for commit 3: typo "INTEGER_CSTby".
- Commit 4 comment: it still names the long-gone `-Wformat-length` (now
  `-Wformat-overflow`/`-Wformat-truncation`). Since that paragraph is being
  rewritten anyway, it's worth fixing; there are also a couple of missing
  periods and an inconsistent space after "(if possible).".

## Checked and fine

- The terminating NUL is always written (the trailing NUL directive has
  `len == 1` and is the last directive, so the head pointer isn't advanced
  past it).
- The return value via `POINTER_DIFF_EXPR` is correct.
- Sign and zero extension in the `itoa`/`utoa` argument conversions are
  correct.
- `LONG_MIN` works through `ABSU_EXPR`.
- `make_decl_one_only` does set `TREE_PUBLIC`.
