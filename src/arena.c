/*
 * Limceron Compiler — Arena Allocator
 *
 * One arena per compilation phase. When a phase ends, its entire arena
 * is freed in one operation — zero individual frees.
 */

#include "lcn.h"

Arena arena_new(size_t size) {
    Arena a;
    a.base = (uint8_t *)malloc(size);
    if (!a.base) {
        fprintf(stderr, "fatal: out of memory (requested %zu bytes)\n", size);
        exit(1);
    }
    a.size = size;
    a.used = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t bytes) {
    /* Align to 8 bytes */
    size_t aligned = (bytes + 7) & ~(size_t)7;
    if (a->used + aligned > a->size) {
        fprintf(stderr, "fatal: arena out of memory (%zu / %zu used, requested %zu)\n",
                a->used, a->size, bytes);
        exit(1);
    }
    void *ptr = a->base + a->used;
    memset(ptr, 0, aligned);
    a->used += aligned;
    return ptr;
}

char *arena_strdup(Arena *a, const char *s) {
    size_t len = strlen(s);
    char *copy = (char *)arena_alloc(a, len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *arena_strndup(Arena *a, const char *s, size_t len) {
    char *copy = (char *)arena_alloc(a, len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

void arena_reset(Arena *a) {
    a->used = 0;
}

void arena_free(Arena *a) {
    free(a->base);
    a->base = NULL;
    a->size = 0;
    a->used = 0;
}
