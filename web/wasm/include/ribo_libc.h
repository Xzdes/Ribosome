/* ribo_libc.h — the entire libc the Ribosome core needs, freestanding.
   The forwarding headers (stdio.h, stdlib.h, string.h, ctype.h, math.h)
   all include this one file. Implementations live in shim.c.
   No FILE I/O happens in the browser build: the file-based APIs keep
   compiling but their fopen stub always fails, which is correct — the
   shell uses the *_mem / *_text entry points. */
#ifndef RIBO_LIBC_H
#define RIBO_LIBC_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>

/* ---- stdio (subset) ---- */
typedef struct RIBO_FILE FILE; /* opaque; never actually opened */
int snprintf(char *buf, size_t cap, const char *fmt, ...);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
FILE *fopen(const char *path, const char *mode);      /* always NULL */
int fclose(FILE *fp);
size_t fread(void *dst, size_t sz, size_t n, FILE *fp);
size_t fwrite(const void *src, size_t sz, size_t n, FILE *fp);
int fseek(FILE *fp, long off, int whence);
long ftell(FILE *fp);
#define SEEK_SET 0
#define SEEK_END 2

/* ---- stdlib ---- */
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void free(void *p);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
long strtol(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);

/* ---- string ---- */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strpbrk(const char *s, const char *set);
char *strcpy(char *dst, const char *src);
char *strtok_s(char *s, const char *delim, char **save);

/* ---- ctype (ASCII-only, locale-free by design) ---- */
int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);

/* ---- math (clang builtins map to wasm opcodes) ---- */
#define isnan(x) __builtin_isnan(x)
#define signbit(x) __builtin_signbit(x)
#define sqrtf(x) __builtin_sqrtf(x)
#define sqrt(x) __builtin_sqrt(x)
#define fabsf(x) __builtin_fabsf(x)
#define fabs(x) __builtin_fabs(x)
#define NAN ((float)(0.0f / 0.0f))

#endif
