#ifndef SAFE_FMT_H
#define SAFE_FMT_H

#include <stddef.h>

/*
 * Bounded string building for fixed-size buffers.
 *
 * The usual idiom `pos += snprintf(buf + pos, cap - pos, ...)` is unsafe:
 * snprintf returns the length it WOULD have written, so on truncation `pos`
 * passes `cap`, `cap - pos` goes negative and is converted to a huge size_t,
 * and the next call writes past the end of the buffer.
 *
 * These helpers never let `pos` exceed `cap - 1`, always keep the buffer
 * NUL-terminated, and turn every append after a truncation into a no-op.
 * Use sfmt_full() after building to detect that output was truncated.
 */

/* Append formatted text at buf[pos]; returns the new position (<= cap - 1). */
size_t sfmt_append(char *buf, size_t cap, size_t pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Advance `pos` by `n` characters written by a sub-serializer into buf+pos,
 * clamping exactly like sfmt_append (n may be negative on error). */
size_t sfmt_advance(size_t cap, size_t pos, int n);

/* 1 if the buffer is full, i.e. the output was (or may have been) truncated. */
static inline int sfmt_full(size_t cap, size_t pos) {
    return (cap == 0U) || (pos >= cap - 1U);
}

#endif /* SAFE_FMT_H */
