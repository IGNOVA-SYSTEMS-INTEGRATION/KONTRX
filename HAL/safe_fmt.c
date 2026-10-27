#include "safe_fmt.h"
#include <stdarg.h>
#include <stdio.h>

size_t sfmt_append(char *buf, size_t cap, size_t pos, const char *fmt, ...) {
    if ((buf == NULL) || (cap == 0U)) {
        return 0U;
    }
    if (pos >= cap - 1U) {
        buf[cap - 1U] = '\0';
        return cap - 1U;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
    va_end(ap);

    return sfmt_advance(cap, pos, n);
}

size_t sfmt_advance(size_t cap, size_t pos, int n) {
    if (cap == 0U) {
        return 0U;
    }
    if (pos >= cap - 1U) {
        return cap - 1U;
    }
    if (n <= 0) {
        return pos;
    }
    size_t room = cap - 1U - pos;
    return ((size_t)n >= room) ? (cap - 1U) : (pos + (size_t)n);
}
