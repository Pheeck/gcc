/* Verify that sprintf calls with integer directives whose arguments are not
   known at compile time are not folded into calls to the itoa and utoa
   functions with a non-default execution character set.  The functions are
   shared with other translation units, which may have been compiled with
   a different execution character set.  */

/* { dg-do compile } */
/* { dg-require-iconv "IBM1047" } */
/* { dg-options "-O2 -fexec-charset=IBM1047 -fno-printf-return-value -fdump-tree-optimized" } */

void f_d (char *d, int i)
{
  __builtin_sprintf (d, "%d", i);
}

void f_u (char *d, unsigned u)
{
  __builtin_sprintf (d, "%u", u);
}

/* { dg-final { scan-tree-dump-times "sprintf \\(" 2 "optimized" } } */
