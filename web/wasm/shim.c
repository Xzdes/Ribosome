/* shim.c — the libc for the freestanding wasm32 build of the Ribosome core.
   K&R free-list malloc over wasm linear memory, ASCII ctype, bounded
   snprintf (%s %.*s %d %u %lld %llu %x/%X %c %a %%), exact hex-float
   strtod, best-effort decimal strtod (the CLI uses the real libc; the
   shell documents the 1-ulp caveat). No threads, no locale, no files. */
#include "ribo_libc.h"

/* ================= string ================= */

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return c == 0 ? (char *)s : NULL;
}
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, nl)) return (char *)h;
    return NULL;
}
char *strpbrk(const char *s, const char *set) {
    for (; *s; s++) if (strchr(set, *s)) return (char *)s;
    return NULL;
}

/* ================= ctype ================= */

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

/* ================= malloc (K&R free list over wasm memory) ================= */

extern uint8_t __heap_base; /* wasm-ld provides the end of static data */

typedef union header {
    struct { union header *next; size_t size; } s;
    max_align_t align;
} Hdr;

static Hdr base;       /* free-list sentinel */
static Hdr *freep;     /* free list, address-ordered */
static uintptr_t brk_cur, brk_end;

static void *wsbrk(size_t n) {
    void *p;
    n = (n + 15u) & ~15u;
    if (!brk_cur) {
        brk_cur = ((uintptr_t)&__heap_base + 15u) & ~15u;
        brk_end = (uintptr_t)__builtin_wasm_memory_size(0) * 65536u;
    }
    if (brk_cur + n > brk_end) {
        size_t need = brk_cur + n - brk_end;
        size_t pages = (need + 65535u) / 65536u;
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return NULL;
        brk_end += pages * 65536u;
    }
    p = (void *)brk_cur;
    brk_cur += n;
    return p;
}

#define NALLOC 4096 /* minimum units to request */

static Hdr *morecore(size_t nu) {
    Hdr *p;
    if (nu < NALLOC) nu = NALLOC;
    p = wsbrk(nu * sizeof(Hdr));
    if (!p) return NULL;
    p->s.size = nu;
    free(p + 1); /* insert into the free list */
    return freep;
}

void *malloc(size_t n) {
    size_t nu;
    Hdr *p, *prev;
    if (!n) n = 1;
    nu = (n + sizeof(Hdr) - 1) / sizeof(Hdr) + 1;
    prev = freep;
    if (!prev) { /* first call */
        base.s.next = freep = &base;
        base.s.size = 0;
        prev = freep;
    }
    for (p = prev->s.next;; prev = p, p = p->s.next) {
        if (p->s.size >= nu) {
            if (p->s.size == nu) prev->s.next = p->s.next;
            else {
                p->s.size -= nu;
                p += p->s.size;
                p->s.size = nu;
            }
            freep = prev;
            return p + 1;
        }
        if (p == freep) { /* wrapped around: need more memory */
            p = morecore(nu);
            if (!p) return NULL;
        }
    }
}

void free(void *ap) {
    Hdr *bp, *p;
    if (!ap) return;
    bp = (Hdr *)ap - 1;
    for (p = freep; !(bp > p && bp < p->s.next); p = p->s.next)
        if (p >= p->s.next && (bp > p || bp < p->s.next)) break; /* list wrap */
    if (bp + bp->s.size == p->s.next) { bp->s.size += p->s.next->s.size; bp->s.next = p->s.next->s.next; }
    else bp->s.next = p->s.next;
    if (p + p->s.size == bp) { p->s.size += bp->s.size; p->s.next = bp->s.next; }
    else p->s.next = bp;
    freep = p;
}

void *calloc(size_t n, size_t sz) {
    size_t total = n * sz;
    void *p;
    if (sz && total / sz != n) return NULL;
    p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ap, size_t n) {
    Hdr *bp;
    void *np;
    size_t old;
    if (!ap) return malloc(n);
    bp = (Hdr *)ap - 1;
    old = (bp->s.size - 1) * sizeof(Hdr);
    if (n <= old) return ap;
    np = malloc(n);
    if (!np) return NULL;
    memcpy(np, ap, old);
    free(ap);
    return np;
}

/* ================= snprintf ================= */

typedef struct { char *p; size_t left; } W;

static void wch(W *w, char c) { if (w->left > 1) { *w->p++ = c; w->left--; } }
static void wstrn(W *w, const char *s, size_t n) { while (n--) wch(w, *s++); }
static void wstr(W *w, const char *s) { while (*s) wch(w, *s++); }

static void wu64(W *w, uint64_t v, int base, int upper, int width, char pad) {
    char tmp[24];
    int i = 0, padlen;
    do {
        unsigned d = (unsigned)(v % (unsigned)base);
        tmp[i++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= (unsigned)base;
    } while (v);
    padlen = width - i;
    while (padlen-- > 0) wch(w, pad);
    while (i) wch(w, tmp[--i]);
}

static void wi64(W *w, int64_t v, int width, char pad) {
    if (v < 0) {
        wch(w, '-');
        wu64(w, (uint64_t)(-(v + 1)) + 1u, 10, 0, width > 0 ? width - 1 : 0, pad);
    } else {
        wu64(w, (uint64_t)v, 10, 0, width, pad);
    }
}

/* exact %a: hexadecimal float straight from the bits */
static void whexfloat(W *w, double d) {
    uint64_t bits, frac;
    int exp, sign, i;
    memcpy(&bits, &d, 8);
    sign = (int)(bits >> 63);
    exp = (int)((bits >> 52) & 0x7FF);
    frac = bits & 0xFFFFFFFFFFFFFull;
    if (exp == 0x7FF) {
        if (sign) wch(w, '-');
        wstr(w, frac ? "nan" : "inf");
        return;
    }
    if (sign) wch(w, '-');
    if (exp == 0 && frac == 0) { wstr(w, "0x0p+0"); return; }
    if (exp == 0) { wstr(w, "0x0."); exp = -1022; }
    else { wstr(w, "0x1."); exp -= 1023; }
    for (i = 12; i >= 0; i--) {
        unsigned nib = (unsigned)((frac >> (4 * i)) & 0xF);
        wch(w, (char)(nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
    wch(w, 'p');
    wi64(w, exp, 0, ' ');
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    W w;
    w.p = buf;
    w.left = cap ? cap : 1;
    for (; *fmt; fmt++) {
        char pad = ' ';
        int width = 0, prec = -1, longs = 0;
        char spec;
        if (*fmt != '%') { wch(&w, *fmt); continue; }
        fmt++;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') { longs++; fmt++; }
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        spec = *fmt;
        switch (spec) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t n = strlen(s);
            if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
            wstrn(&w, s, n);
            break;
        }
        case 'c': wch(&w, (char)va_arg(ap, int)); break;
        case 'd': case 'i':
            if (longs >= 2) wi64(&w, va_arg(ap, long long), width, pad);
            else wi64(&w, va_arg(ap, int), width, pad);
            break;
        case 'u':
            if (longs >= 2) wu64(&w, va_arg(ap, unsigned long long), 10, 0, width, pad);
            else wu64(&w, va_arg(ap, unsigned), 10, 0, width, pad);
            break;
        case 'x': case 'X':
            if (longs >= 2) wu64(&w, va_arg(ap, unsigned long long), 16, spec == 'X', width, pad);
            else wu64(&w, va_arg(ap, unsigned), 16, spec == 'X', width, pad);
            break;
        case 'a': whexfloat(&w, va_arg(ap, double)); break;
        case '%': wch(&w, '%'); break;
        default: wch(&w, '%'); wch(&w, spec); break;
        }
    }
    if (cap) *w.p = '\0';
    return (int)(w.p - buf);
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* ================= stdio stubs (no files in the browser build) ================= */

struct RIBO_FILE { int unused; };
FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int fclose(FILE *fp) { (void)fp; return 0; }
size_t fread(void *d, size_t s, size_t n, FILE *fp) { (void)d; (void)s; (void)n; (void)fp; return 0; }
size_t fwrite(const void *s, size_t z, size_t n, FILE *fp) { (void)s; (void)z; (void)n; (void)fp; return 0; }
int fseek(FILE *fp, long o, int w) { (void)fp; (void)o; (void)w; return -1; }
long ftell(FILE *fp) { (void)fp; return -1; }

/* ================= strtod / strtof ================= */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int tolow(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* exact hex float: 0x1.91eb851eb851fp+1 */
static double parse_hex(const char **sp) {
    const char *s = *sp + 2; /* past 0x */
    uint64_t mant = 0;
    int any = 0, sticky = 0, shift = 0, bexp = 0, frac_digits = 0, seen_dot = 0;
    for (;;) {
        int h = hexval((unsigned char)*s);
        if (h >= 0) {
            any = 1;
            if (seen_dot) frac_digits++;
            if (mant <= (UINT64_MAX >> 4)) mant = (mant << 4) | (uint64_t)h;
            else { sticky |= h != 0; shift++; }
            s++;
        } else if (*s == '.' && !seen_dot) { seen_dot = 1; s++; }
        else break;
    }
    if (!any) return 0.0;
    if (tolow((unsigned char)*s) == 'p') {
        int neg = 0, v = 0;
        s++;
        if (*s == '+' || *s == '-') neg = (*s++ == '-');
        while (isdigit((unsigned char)*s)) v = v * 10 + (*s++ - '0');
        bexp = neg ? -v : v;
    }
    *sp = s;
    bexp += 4 * shift - 4 * frac_digits;
    /* normalize mant to 53 bits with round-half-even */
    {
        int bits = 0;
        uint64_t m = mant;
        while (m) { bits++; m >>= 1; }
        if (bits > 53) {
            int drop = bits - 53;
            uint64_t dropped = mant & ((1ull << drop) - 1);
            uint64_t half = 1ull << (drop - 1);
            mant >>= drop;
            bexp += drop;
            if (dropped > half || (dropped == half && (sticky || (mant & 1)))) mant++;
            if (mant >> 53) { mant >>= 1; bexp++; }
        }
    }
    {
        double v = (double)mant;
        while (bexp > 0) { v *= 2.0; bexp--; }
        while (bexp < 0) { v *= 0.5; bexp++; }
        return v;
    }
}

double strtod(const char *s, char **end) {
    const char *p = s;
    int neg = 0;
    double v;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if (tolow((unsigned char)p[0]) == 'i' && tolow((unsigned char)p[1]) == 'n' && tolow((unsigned char)p[2]) == 'f') {
        if (end) *end = (char *)p + 3;
        return neg ? -1.0 / 0.0 : 1.0 / 0.0;
    }
    if (tolow((unsigned char)p[0]) == 'n' && tolow((unsigned char)p[1]) == 'a' && tolow((unsigned char)p[2]) == 'n') {
        if (end) *end = (char *)p + 3;
        return 0.0 / 0.0;
    }
    if (p[0] == '0' && tolow((unsigned char)p[1]) == 'x') {
        v = parse_hex(&p);
        if (end) *end = (char *)p;
        return neg ? -v : v;
    }
    /* decimal: accumulate 19 significant digits, correct the rest by the
       exponent; not guaranteed correctly rounded (documented 1-ulp caveat) */
    {
        uint64_t mant = 0;
        int digits = 0, frac = 0, seen_dot = 0, any = 0, exp10 = 0, eneg = 0;
        for (;;) {
            if (isdigit((unsigned char)*p)) {
                any = 1;
                if (seen_dot) frac++;
                if (digits < 19) { mant = mant * 10 + (uint64_t)(*p - '0'); digits++; }
                else exp10++;
                p++;
            } else if (*p == '.' && !seen_dot) { seen_dot = 1; p++; }
            else break;
        }
        if (!any) { if (end) *end = (char *)s; return 0.0; }
        exp10 -= frac;
        if (digits < 19 && frac > digits) exp10 += 0; /* leading zeros absorbed */
        if (tolow((unsigned char)*p) == 'e') {
            int ev = 0;
            p++;
            if (*p == '+' || *p == '-') eneg = (*p++ == '-');
            while (isdigit((unsigned char)*p)) ev = ev * 10 + (*p++ - '0');
            exp10 += eneg ? -ev : ev;
        }
        if (end) *end = (char *)p;
        v = (double)mant;
        while (exp10 >= 10) { v *= 1e10; exp10 -= 10; }
        while (exp10 > 0) { v *= 10.0; exp10--; }
        while (exp10 <= -10) { v /= 1e10; exp10 += 10; }
        while (exp10 < 0) { v /= 10.0; exp10++; }
    }
    return neg ? -v : v;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }

int errno;


/* ================= later additions ================= */

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strtok_s(char *s, const char *delim, char **save) {
    char *tok;
    if (!s) s = *save;
    if (!s) return NULL;
    while (*s && strchr(delim, *s)) s++;
    if (!*s) { *save = s; return NULL; }
    tok = s;
    while (*s && !strchr(delim, *s)) s++;
    if (*s) *s++ = 0;
    *save = s;
    return tok;
}

static long long strtoi_common(const char *s, char **end, int base) {
    const char *p = s;
    int neg = 0, any = 0;
    long long v = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0') base = 8;
        else base = 10;
    } else if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    for (;;) {
        int d = hexval((unsigned char)*p);
        if (d < 0 || d >= base) break;
        v = v * base + d;
        any = 1;
        p++;
    }
    if (end) *end = (char *)(any ? p : s);
    return neg ? -v : v;
}
long strtol(const char *s, char **end, int base) { return (long)strtoi_common(s, end, base); }
long long strtoll(const char *s, char **end, int base) { return strtoi_common(s, end, base); }
