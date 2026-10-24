/**
 * @file    jspan.c
 * @brief   Read-only in-place JSON access (see jspan.h).
 */
#include "jspan.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define JSPAN_MAX_DEPTH 32

static const JVal JV_MISSING = { NULL, NULL };

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Returns the character after the closing quote, or NULL if invalid. */
static const char *skip_string(const char *p, const char *end) {
    if (p >= end || *p != '"') return NULL;
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') return p + 1;
        if (c < 0x20U) return NULL;
        if (c == '\\') {
            if (p + 1 >= end) return NULL;
            char e = p[1];
            if (e == 'u') {
                if (p + 6 > end || !is_hex(p[2]) || !is_hex(p[3]) || !is_hex(p[4]) || !is_hex(p[5])) return NULL;
                p += 6;
                continue;
            }
            if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' &&
                e != 'n' && e != 'r' && e != 't') return NULL;
            p += 2;
            continue;
        }
        p++;
    }
    return NULL;
}

/* cJSON collects these characters and hands them to strtod. */
static int is_num_char(char c) {
    return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == 'e' || c == 'E' || c == '.';
}

static const char *skip_number(const char *p, const char *end) {
    const char *s = p;
    while (p < end && is_num_char(*p)) p++;
    if (p == s) return NULL;
    char tmp[48];
    size_t n = (size_t)(p - s);
    if (n >= sizeof(tmp)) return NULL;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    char *stop = NULL;
    (void)strtod(tmp, &stop);
    if (stop == tmp) return NULL;
    return s + (stop - tmp);   /* cJSON stops where strtod stops */
}

static const char *skip_value(const char *p, const char *end, int depth);

static const char *skip_container(const char *p, const char *end, int depth, char close, int is_obj) {
    if (depth >= JSPAN_MAX_DEPTH) return NULL;
    p = skip_ws(p + 1, end);
    if (p < end && *p == close) return p + 1;
    for (;;) {
        if (is_obj) {
            p = skip_string(skip_ws(p, end), end);
            if (!p) return NULL;
            p = skip_ws(p, end);
            if (p >= end || *p != ':') return NULL;
            p++;
        }
        p = skip_value(skip_ws(p, end), end, depth + 1);
        if (!p) return NULL;
        p = skip_ws(p, end);
        if (p >= end) return NULL;
        if (*p == close) return p + 1;
        if (*p != ',') return NULL;
        p++;
    }
}

static const char *skip_value(const char *p, const char *end, int depth) {
    if (p >= end) return NULL;
    switch (*p) {
        case '{': return skip_container(p, end, depth, '}', 1);
        case '[': return skip_container(p, end, depth, ']', 0);
        case '"': return skip_string(p, end);
        case 'n': return (end - p >= 4 && memcmp(p, "null", 4) == 0) ? p + 4 : NULL;
        case 't': return (end - p >= 4 && memcmp(p, "true", 4) == 0) ? p + 4 : NULL;
        case 'f': return (end - p >= 5 && memcmp(p, "false", 5) == 0) ? p + 5 : NULL;
        default:
            if (*p == '-' || (*p >= '0' && *p <= '9')) return skip_number(p, end);
            return NULL;
    }
}

JVal jv_parse(const char *buf, size_t len) {
    if (!buf) return JV_MISSING;
    const char *end = buf + len;
    const char *p = buf;
    if (len >= 3 && (unsigned char)p[0] == 0xEFU && (unsigned char)p[1] == 0xBBU && (unsigned char)p[2] == 0xBFU) p += 3;
    p = skip_ws(p, end);
    if (!skip_value(p, end, 0)) return JV_MISSING;
    JVal v = { p, end };
    return v;
}

JType jv_type(JVal v) {
    if (!v.p || v.p >= v.end) return JT_NONE;
    switch (*v.p) {
        case '{': return JT_OBJECT;
        case '[': return JT_ARRAY;
        case '"': return JT_STRING;
        case 'n': return JT_NULL;
        case 't': return JT_TRUE;
        case 'f': return JT_FALSE;
        default:  return JT_NUMBER;
    }
}

int jv_is_string(JVal v) { return jv_type(v) == JT_STRING; }
int jv_is_number(JVal v) { return jv_type(v) == JT_NUMBER; }
int jv_is_object(JVal v) { return jv_type(v) == JT_OBJECT; }
int jv_is_array(JVal v)  { return jv_type(v) == JT_ARRAY; }
int jv_is_true(JVal v)   { return jv_type(v) == JT_TRUE; }

/* Appends one unescaped UTF-8 code point; returns bytes written. */
static size_t put_utf8(char *out, size_t room, unsigned long cp) {
    char tmp[4];
    size_t n;
    if (cp < 0x80UL)        { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800UL)  { tmp[0] = (char)(0xC0UL | (cp >> 6)); tmp[1] = (char)(0x80UL | (cp & 0x3FUL)); n = 2; }
    else if (cp < 0x10000UL){ tmp[0] = (char)(0xE0UL | (cp >> 12)); tmp[1] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
                              tmp[2] = (char)(0x80UL | (cp & 0x3FUL)); n = 3; }
    else                    { tmp[0] = (char)(0xF0UL | (cp >> 18)); tmp[1] = (char)(0x80UL | ((cp >> 12) & 0x3FUL));
                              tmp[2] = (char)(0x80UL | ((cp >> 6) & 0x3FUL)); tmp[3] = (char)(0x80UL | (cp & 0x3FUL)); n = 4; }
    if (n > room) return 0;
    memcpy(out, tmp, n);
    return n;
}

static unsigned long hex4(const char *p) {
    unsigned long v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else v |= (unsigned long)(c - 'A' + 10);
    }
    return v;
}

/* Decodes the string at p (opening quote) into out; stops at cap-1 bytes. */
static size_t decode_string(const char *p, const char *end, char *out, size_t cap) {
    size_t w = 0;
    if (cap == 0U) return 0;
    p++;
    while (p < end && *p != '"') {
        size_t room = cap - 1U - w;
        if (room == 0U) break;
        if (*p != '\\') { out[w++] = *p++; continue; }
        char e = p[1];
        unsigned long cp;
        switch (e) {
            case 'b': out[w++] = '\b'; p += 2; continue;
            case 'f': out[w++] = '\f'; p += 2; continue;
            case 'n': out[w++] = '\n'; p += 2; continue;
            case 'r': out[w++] = '\r'; p += 2; continue;
            case 't': out[w++] = '\t'; p += 2; continue;
            case 'u':
                cp = hex4(p + 2);
                p += 6;
                if (cp >= 0xD800UL && cp <= 0xDBFFUL && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                    unsigned long lo = hex4(p + 2);
                    if (lo >= 0xDC00UL && lo <= 0xDFFFUL) {
                        cp = 0x10000UL + ((cp - 0xD800UL) << 10) + (lo - 0xDC00UL);
                        p += 6;
                    }
                }
                {
                    size_t n = put_utf8(out + w, room, cp);
                    if (n == 0U) goto done;
                    w += n;
                }
                continue;
            default:   /* \" \\ \/ */
                out[w++] = e;
                p += 2;
                continue;
        }
    }
done:
    out[w] = '\0';
    return w;
}

/* Calls back for each member/element; stops when cb returns non-zero. */
typedef int (*jv_visit_fn)(const char *key_p, JVal val, void *ctx);

static void visit(JVal c, jv_visit_fn cb, void *ctx) {
    JType t = jv_type(c);
    if (t != JT_OBJECT && t != JT_ARRAY) return;
    char close = (t == JT_OBJECT) ? '}' : ']';
    const char *end = c.end;
    const char *p = skip_ws(c.p + 1, end);
    if (p < end && *p == close) return;
    for (;;) {
        const char *key = NULL;
        if (t == JT_OBJECT) {
            key = p;
            p = skip_string(p, end);
            if (!p) return;
            p = skip_ws(p, end);
            p++;   /* ':' (validated by jv_parse) */
            p = skip_ws(p, end);
        }
        JVal v = { p, end };
        if (cb(key, v, ctx)) return;
        p = skip_value(p, end, 0);
        if (!p) return;
        p = skip_ws(p, end);
        if (p >= end || *p != ',') return;
        p = skip_ws(p + 1, end);
    }
}

struct get_ctx { const char *key; JVal found; };

static int key_eq(const char *key_p, const char *end, const char *key) {
    char buf[64];
    size_t klen = strlen(key);
    if (klen >= sizeof(buf)) return 0;
    size_t n = decode_string(key_p, end, buf, sizeof(buf));
    return n == klen && memcmp(buf, key, klen) == 0;
}

static int get_cb(const char *key_p, JVal val, void *ctx) {
    struct get_ctx *g = (struct get_ctx *)ctx;
    if (key_eq(key_p, val.end, g->key)) { g->found = val; return 1; }
    return 0;
}

JVal jv_get(JVal obj, const char *key) {
    struct get_ctx g = { key, { NULL, NULL } };
    if (jv_type(obj) != JT_OBJECT || !key) return JV_MISSING;
    visit(obj, get_cb, &g);
    return g.found;
}

struct idx_ctx { int want; int i; JVal found; };

static int idx_cb(const char *key_p, JVal val, void *ctx) {
    (void)key_p;
    struct idx_ctx *c = (struct idx_ctx *)ctx;
    if (c->i == c->want) { c->found = val; return 1; }
    c->i++;
    return 0;
}

int jv_array_size(JVal arr) {
    struct idx_ctx c = { -1, 0, { NULL, NULL } };
    if (jv_type(arr) != JT_ARRAY) return 0;
    visit(arr, idx_cb, &c);
    return c.i;
}

JVal jv_array_item(JVal arr, int index) {
    struct idx_ctx c = { index, 0, { NULL, NULL } };
    if (jv_type(arr) != JT_ARRAY || index < 0) return JV_MISSING;
    visit(arr, idx_cb, &c);
    return c.found;
}

size_t jv_string(JVal v, char *out, size_t cap) {
    if (!out || cap == 0U) return 0;
    if (!jv_is_string(v)) { out[0] = '\0'; return 0; }
    return decode_string(v.p, v.end, out, cap);
}

int jv_string_eq(JVal v, const char *s) {
    char buf[64];
    size_t slen = strlen(s);
    if (!jv_is_string(v) || slen >= sizeof(buf)) return 0;
    size_t n = decode_string(v.p, v.end, buf, sizeof(buf));
    return n == slen && memcmp(buf, s, slen) == 0;
}

double jv_number(JVal v) {
    if (!jv_is_number(v)) return 0.0;
    char tmp[48];
    size_t n = 0;
    while (v.p + n < v.end && n < sizeof(tmp) - 1U && is_num_char(v.p[n])) { tmp[n] = v.p[n]; n++; }
    tmp[n] = '\0';
    return strtod(tmp, NULL);
}

int jv_int(JVal v) {
    double d = jv_number(v);
    if (d >= (double)INT_MAX) return INT_MAX;
    if (d <= (double)INT_MIN) return INT_MIN;
    return (int)d;
}
