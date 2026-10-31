/*
 * Lantest OS - tiny user-space malloc on top of mmap/munmap
 *
 * Small allocations come from 64 KiB mmap chunks and stay in a free list.
 * Allocations >= 8 KiB get their own mmap and are unmapped by free().
 */
#pragma once

#include "userlib.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define MALLOC_ALIGN   16ULL
#define MALLOC_LARGE   8192ULL
#define MALLOC_CHUNK   65536ULL

struct malloc_block {
    u64 size;
    struct malloc_block *next;
    struct malloc_block *prev;
    unsigned int free;
    unsigned int large;
};

static struct malloc_block *malloc_free_list;

static u64 malloc_align(u64 n)
{
    return (n + MALLOC_ALIGN - 1) & ~(MALLOC_ALIGN - 1);
}

static void *malloc_from_block(struct malloc_block *b, u64 size)
{
    if (b->size >= size + sizeof(struct malloc_block) + MALLOC_ALIGN) {
        struct malloc_block *rest =
            (struct malloc_block *)((unsigned char *)b + sizeof(struct malloc_block) + size);

        rest->size = b->size - size - sizeof(struct malloc_block);
        rest->free = 1;
        rest->large = 0;
        rest->prev = NULL;
        rest->next = malloc_free_list;
        if (malloc_free_list != NULL)
            malloc_free_list->prev = rest;
        malloc_free_list = rest;
        b->size = size;
    }
    b->free = 0;
    return (unsigned char *)b + sizeof(struct malloc_block);
}

void *malloc(u64 size)
{
    struct malloc_block *b;

    if (size == 0)
        size = 1;
    size = malloc_align(size);

    if (size >= MALLOC_LARGE) {
        void *base = (void *)sys_mmap(size + sizeof(struct malloc_block));

        if ((i64)base < 0)
            return NULL;
        b = (struct malloc_block *)base;
        b->size = size;
        b->next = NULL;
        b->prev = NULL;
        b->free = 0;
        b->large = 1;
        return (unsigned char *)b + sizeof(struct malloc_block);
    }

    for (b = malloc_free_list; b != NULL; b = b->next) {
        if (b->free && b->size >= size)
            return malloc_from_block(b, size);
    }

    {
        void *base = (void *)sys_mmap(MALLOC_CHUNK);

        if ((i64)base < 0)
            return NULL;
        b = (struct malloc_block *)base;
        b->size = MALLOC_CHUNK - sizeof(struct malloc_block);
        b->next = malloc_free_list;
        b->prev = NULL;
        b->free = 1;
        b->large = 0;
        if (malloc_free_list != NULL)
            malloc_free_list->prev = b;
        malloc_free_list = b;
        return malloc_from_block(b, size);
    }
}

void free(void *ptr)
{
    struct malloc_block *b;

    if (ptr == NULL)
        return;

    b = (struct malloc_block *)((unsigned char *)ptr - sizeof(struct malloc_block));
    if (b->large) {
        sys_munmap((u64)b, b->size + sizeof(struct malloc_block));
        return;
    }
    b->free = 1;
}

void *calloc(u64 count, u64 size)
{
    u64 total = count * size;
    void *p;

    if (size != 0 && total / size != count)
        return NULL;

    p = malloc(total);
    if (p != NULL) {
        unsigned char *q = (unsigned char *)p;
        for (u64 i = 0; i < total; ++i)
            q[i] = 0;
    }
    return p;
}

void *realloc(void *ptr, u64 size)
{
    struct malloc_block *b;
    void *np;
    u64 copy;

    if (ptr == NULL)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    b = (struct malloc_block *)((unsigned char *)ptr - sizeof(struct malloc_block));
    if (b->size >= size)
        return ptr;

    np = malloc(size);
    if (np == NULL)
        return NULL;

    copy = b->size < size ? b->size : size;
    for (u64 i = 0; i < copy; ++i)
        ((unsigned char *)np)[i] = ((unsigned char *)ptr)[i];
    free(ptr);
    return np;
}
