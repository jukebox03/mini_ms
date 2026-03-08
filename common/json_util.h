#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <stddef.h>

/* Simple key-value store loaded from JSON file */
#define MAX_ENTRIES 256
#define MAX_KEY_LEN 64

typedef struct {
    char key[MAX_KEY_LEN];
    int  value;      /* integer or boolean (0/1) */
} kv_entry_t;

typedef struct {
    kv_entry_t entries[MAX_ENTRIES];
    int count;
} kv_store_t;

/* Load {"key": int_or_bool, ...} from file. Returns 0 on success. */
int kv_store_load(kv_store_t *store, const char *path);

/* Lookup key. Returns pointer to value or NULL if not found. */
const int *kv_store_get(const kv_store_t *store, const char *key);

/* Parse query string for a key: "name=alice&foo=bar" -> "alice" for key "name" */
int query_get(const char *qs, const char *key, char *out, size_t outsz);

#endif
