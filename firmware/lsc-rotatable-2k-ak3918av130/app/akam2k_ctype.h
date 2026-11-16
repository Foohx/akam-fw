/* Force-included (-include) into every shared C file compiled for the 2K app, so that those files stay unmodified.
 *
 * Why: the pinned Bootlin uClibc toolchain's <ctype.h> expands isalpha(), isdigit(), ... into a call to
 * __ctype_b_loc(), which the camera's own libc (uClibc-ng 1.0.31, built without that locale glue) does NOT export:
 * the program would die at start-up with "can't resolve symbol '__ctype_b_loc'". The classification FUNCTIONS are
 * exported by both, so the macros are dropped and the plain calls stay. */
#ifndef AKAM2K_CTYPE_H
#define AKAM2K_CTYPE_H
#include <ctype.h>
#undef isalnum
#undef isalpha
#undef isblank
#undef iscntrl
#undef isdigit
#undef isgraph
#undef islower
#undef isprint
#undef ispunct
#undef isspace
#undef isupper
#undef isxdigit
#undef tolower
#undef toupper
#endif
