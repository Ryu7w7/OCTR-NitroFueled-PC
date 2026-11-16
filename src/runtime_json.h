#ifndef RUNTIME_JSON_H
#define RUNTIME_JSON_H
#include <ctype.h>
#include <string.h>

/* Find an unescaped top-level JSON field. Skip nested and escaped strings; reject duplicates. */
static inline const char *runtime_json_field(const char *json, const char *key) {
    if (!json || *json != '{') return NULL;
    int depth = 0;
    const char *found = NULL;
    for (const char *p = json; *p; ++p) {
        if (*p == '"') {
            const char *start = ++p;
            int escaped = 0;
            while (*p && *p != '"') {
                if (*p == '\\') { escaped = 1; if (!*++p) return NULL; }
                ++p;
            }
            if (!*p) return NULL;
            const char *after = p + 1;
            while (isspace((unsigned char)*after)) ++after;
            if (depth == 1 && !escaped && *after == ':' &&
                (size_t)(p - start) == strlen(key) && !memcmp(start, key, strlen(key))) {
                if (found) return NULL;
                found = after + 1;
                while (isspace((unsigned char)*found)) ++found;
            }
        } else if (*p == '{' || *p == '[') ++depth;
        else if (*p == '}' || *p == ']') { if (--depth < 0) return NULL; }
    }
    return depth ? NULL : found;
}

#endif
