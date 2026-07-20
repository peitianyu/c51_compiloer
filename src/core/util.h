#ifndef TTCC_UTIL_H
#define TTCC_UTIL_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"

typedef struct {
    char *body;
    int nalloc, len;
} String;

static List *cstrings = &EMPTY_LIST;

#define INIT_SIZE 8

static inline String make_string(void)
{
    return (String){
        .body = calloc(1, INIT_SIZE),
        .nalloc = INIT_SIZE,
        .len = 0,
    };
}

static inline void realloc_body(String *s)
{
    int newsize = s->nalloc * 2;
    char *body = realloc(s->body, newsize);
    s->body = body;
    s->nalloc = newsize;
}

static inline char *get_cstring(const String s)
{
    char *r = s.body;
    list_push(cstrings, r);
    return r;
}

static inline void string_append(String *s, char c)
{
    if (s->nalloc == (s->len + 1))
        realloc_body(s);
    s->body[s->len++] = c;
    s->body[s->len] = '\0';
}

static inline void string_appendf(String *s, char *fmt, ...)
{
    va_list args;
    while (1) {
        int avail = s->nalloc - s->len;
        va_start(args, fmt);
        int written = vsnprintf(s->body + s->len, avail, fmt, args);
        va_end(args);
        if (written < 0 || avail <= written) {
            realloc_body(s);
            continue;
        }
        s->len += written;
        return;
    }
}



static inline char *quote_cstring(char *p)
{
    String s = make_string();
    for (; *p; p++) {
        if (*p == '\"' || *p == '\\')
            string_appendf(&s, "\\%c", *p);
        else if (*p == '\n')
            string_appendf(&s, "\\n");
        else
            string_append(&s, *p);
    }
    return get_cstring(s);
}

static inline FILE *TTCC_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

static inline char *TTCC_resolve_path(const char *path)
{
    return path ? strdup(path) : NULL;
}

/* debug */
#define error(...) errorf(__FILE__, __LINE__, __VA_ARGS__)
static inline void errorf(char *file, int line, char *fmt, ...)
{
    #if TTCC_DEBUG
    fprintf(stderr, "%s:%d: ", file, line);
    #else 
    (void)file; (void)line;
    TokenInfo info = get_current_token_info();
    fprintf(stderr, "%s:%d: ", info.file, info.line);
    #endif 
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(1);
}

#endif /* TTCC_UTIL_H */
