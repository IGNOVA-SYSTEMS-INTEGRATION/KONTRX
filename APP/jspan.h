/**
 * @file    jspan.h
 * @brief   Read-only JSON access in place: no heap, no token array.
 *
 * A JVal is a span of the original text holding one JSON value. Lookups
 * scan the text, so they cost O(size of the parent) but need no memory;
 * fine for request bodies of a few KB. Semantics follow cJSON where the
 * firmware relied on them (first matching key wins, valueint saturates,
 * text after the first value is ignored).
 */
#ifndef JSPAN_H
#define JSPAN_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *p;     /* first character of the value; NULL = missing */
    const char *end;   /* end of the document (bound for every scan) */
} JVal;

typedef enum { JT_NONE = 0, JT_NULL, JT_FALSE, JT_TRUE, JT_NUMBER, JT_STRING, JT_ARRAY, JT_OBJECT } JType;

/* Parses the first value of buf[0..len). Returns a missing JVal if it is
 * not valid JSON (nesting deeper than 32 also counts as invalid). */
JVal  jv_parse(const char *buf, size_t len);

JType jv_type(JVal v);
int   jv_is_string(JVal v);
int   jv_is_number(JVal v);
int   jv_is_object(JVal v);
int   jv_is_array(JVal v);
int   jv_is_true(JVal v);

/* Member of an object by exact key, or a missing JVal. */
JVal  jv_get(JVal obj, const char *key);

int   jv_array_size(JVal arr);
JVal  jv_array_item(JVal arr, int index);

/* Unescaped string copied to out (at most cap-1 bytes, always NUL-
 * terminated). Returns the number of bytes written. */
size_t jv_string(JVal v, char *out, size_t cap);
/* 1 if the string value equals s exactly. */
int    jv_string_eq(JVal v, const char *s);

double jv_number(JVal v);
int    jv_int(JVal v);   /* like cJSON valueint: saturates at INT_MIN/INT_MAX */

#endif /* JSPAN_H */
