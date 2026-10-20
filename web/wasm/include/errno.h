/* errno shim: a single global, never set by our strtod (it never
   reports ERANGE ? the CLI uses the real libc for authoritative parses) */
#ifndef RIBO_ERRNO_H
#define RIBO_ERRNO_H
#define ERANGE 34
extern int errno;
#endif
