/* fault.c — diagnostics plumbing, shared by every stage so an error message
   can never drift between them. */
#include "ribo_internal.h"
#include <stdarg.h>
#include <stdio.h>

void ribo_diag_set(RiboDiag *d, const char *stage, uint32_t line,
                   const char *line_text, const char *fmt, ...) {
    va_list ap;
    if (!d) return;
    snprintf(d->stage, sizeof d->stage, "%s", stage ? stage : "?");
    d->line = line;
    snprintf(d->line_text, sizeof d->line_text, "%s", line_text ? line_text : "");
    va_start(ap, fmt);
    vsnprintf(d->message, sizeof d->message, fmt, ap);
    va_end(ap);
}

void ribo_diag_to_public(const RiboDiag *d, RiboError *error) {
    if (!error) return;
    if (!d) { error->message[0] = '\0'; return; }
    /* bounded pieces, so truncation warnings are silenced by construction */
    if (d->line) {
        char head[600];
        snprintf(head, sizeof head, "%.15s: %.500s", d->stage, d->message);
        snprintf(error->message, sizeof error->message, "%.560s (line %u: %.400s)",
                 head, (unsigned)d->line, d->line_text);
    } else {
        snprintf(error->message, sizeof error->message, "%.15s: %.1000s",
                 d->stage, d->message);
    }
}
