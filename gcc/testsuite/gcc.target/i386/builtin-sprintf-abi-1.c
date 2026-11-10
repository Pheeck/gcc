/* Verify that sprintf calls with integer directives whose arguments are not
   known at compile time are not folded into calls to the itoa and utoa
   functions when an option affecting their calling convention is specified.
   The functions are shared with other translation units, which may have
   been compiled with different options.  */
/* { dg-do compile { target lp64 } } */
/* { dg-options "-O2 -Wno-format -fno-printf-return-value -mabi=ms -fdump-tree-optimized" } */

void f_d (char *d, int i)
{
  __builtin_sprintf (d, "%d", i);
}

void f_u (char *d, unsigned u)
{
  __builtin_sprintf (d, "%u", u);
}

/* { dg-final { scan-tree-dump-times "sprintf \\(" 2 "optimized" } } */
