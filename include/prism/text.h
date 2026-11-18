#ifndef PRISM_TEXT_H
#define PRISM_TEXT_H

#include <ctype.h>
#include <stdbool.h>

static inline bool text_equals_ignore_case(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

#endif
