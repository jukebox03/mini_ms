#include "json_util.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

int kv_store_load(kv_store_t *store, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    buf[sz] = '\0';
    fclose(f);

    store->count = 0;
    char *p = buf;

    /* Skip to first '{' */
    while (*p && *p != '{') p++;
    if (*p == '{') p++;

    while (*p && *p != '}' && store->count < MAX_ENTRIES) {
        /* Skip whitespace and commas */
        while (*p && (isspace(*p) || *p == ',')) p++;
        if (*p == '}' || !*p) break;

        /* Parse key (quoted string) */
        if (*p != '"') { free(buf); return -1; }
        p++;
        char *kstart = p;
        while (*p && *p != '"') p++;
        if (!*p) { free(buf); return -1; }
        size_t klen = p - kstart;
        if (klen >= MAX_KEY_LEN) klen = MAX_KEY_LEN - 1;
        memcpy(store->entries[store->count].key, kstart, klen);
        store->entries[store->count].key[klen] = '\0';
        p++; /* skip closing quote */

        /* Skip colon */
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        while (*p && isspace(*p)) p++;

        /* Parse value: integer, true, or false */
        if (*p == 't') {
            store->entries[store->count].value = 1;
            p += 4; /* true */
        } else if (*p == 'f') {
            store->entries[store->count].value = 0;
            p += 5; /* false */
        } else {
            store->entries[store->count].value = (int)strtol(p, &p, 10);
        }
        store->count++;
    }

    free(buf);
    return 0;
}

const int *kv_store_get(const kv_store_t *store, const char *key) {
    for (int i = 0; i < store->count; i++) {
        if (strcmp(store->entries[i].key, key) == 0)
            return &store->entries[i].value;
    }
    return NULL;
}

int query_get(const char *qs, const char *key, char *out, size_t outsz) {
    if (!qs || !key) return -1;
    size_t klen = strlen(key);
    const char *p = qs;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < outsz - 1) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            return 0;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return -1;
}
