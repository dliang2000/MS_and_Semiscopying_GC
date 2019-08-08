/*
 * Object allocation and the actual garbage collector
 *
 * Copyright (c) 2014, 2015 Gregor Richards
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "ggggc/gc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "ggggc-internals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRUE 1
#define FALSE 0

/* Since my final project is to build partition type GC, I decide to follow the TeSearchList in collector-gembc for
 * marking */
#define TOSEARCH_SZ 1024
struct ToSearch {
    struct ToSearch *prev, *next;
    ggc_size_t used;
    void **buf;
};

#define TOSEARCH_INIT() do { \
    if (toSearchList.buf == NULL) { \
        toSearchList.buf = (void **) malloc(TOSEARCH_SZ * sizeof(void *)); \
        if (toSearchList.buf == NULL) { \
            /* FIXME: handle somehow? */ \
            perror("malloc"); \
            abort(); \
        } \
    } \
    toSearch = &toSearchList; \
    toSearch->used = 0; \
} while(0)
#define TOSEARCH_NEXT() do { \
    if (!toSearch->next) { \
        struct ToSearch *tsn = (struct ToSearch *) malloc(sizeof(struct ToSearch)); \
        toSearch->next = tsn; \
        tsn->prev = toSearch; \
        tsn->next = NULL; \
        tsn->buf = (void **) malloc(TOSEARCH_SZ * sizeof(void *)); \
        if (tsn->buf == NULL) { \
            perror("malloc"); \
            abort(); \
        } \
    } \
    toSearch = toSearch->next; \
    toSearch->used = 0; \
} while(0)
#define TOSEARCH_ADD(ptr) do { \
    if (toSearch->used >= TOSEARCH_SZ) TOSEARCH_NEXT(); \
    toSearch->buf[toSearch->used++] = (ptr); \
} while(0)
#define TOSEARCH_POP(type, into) do { \
    into = (type) toSearch->buf[--toSearch->used]; \
    if (toSearch->used == 0 && toSearch->prev) \
        toSearch = toSearch->prev; \
} while(0)
/* macro to add an object's pointers to the tosearch list */
#define ADD_OBJECT_POINTERS(obj, descriptor) do { \
    void **objVp = (void **) (obj); \
    ggc_size_t curWord, curDescription, curDescriptorWord = 0; \
    if (descriptor->pointers[0] & 1) { \
        /* it has pointers */ \
        curDescription = descriptor->pointers[0] >> 1; \
        for (curWord = 1; curWord < descriptor->size; curWord++) { \
            if (curWord % GGGGC_BITS_PER_WORD == 0) \
                curDescription = descriptor->pointers[++curDescriptorWord]; \
            if (curDescription & 1) \
                /* it's a pointer */ \
                TOSEARCH_ADD(&objVp[curWord]); \
            curDescription >>= 1; \
        } \
    } \
    TOSEARCH_ADD(&objVp[0]); \
} while(0)


/* mark an object */
#define MARK(obj) do { \
    struct GGGGC_Header *hobj = (obj); \
    hobj->descriptor__ptr = (struct GGGGC_Descriptor *) \
        ((ggc_size_t) hobj->descriptor__ptr | (ggc_size_t) 1); \
} while (0)

/* unmark a pointer */
#define UNMARK_PTR(type, ptr) \
    ((type *) ((ggc_size_t) (ptr) & (ggc_size_t) ~1))

/* get an object's descriptor, through markers */
#define MARKED_DESCRIPTOR(obj) \
    (UNMARK_PTR(struct GGGGC_Descriptor, (obj)->descriptor__ptr))

/* unmark an object */
#define UNMARK(obj) do { \
    struct GGGGC_Header *hobj = (obj); \
    hobj->descriptor__ptr = UNMARK_PTR(struct GGGGC_Descriptor, hobj->descriptor__ptr); \
} while (0)

/* is this pointer marked? */
#define IS_MARKED_PTR(ptr) ((ggc_size_t) (ptr) & 1)

/* is this object marked? */
#define IS_MARKED(obj) IS_MARKED_PTR((obj)->descriptor__ptr)


/* free an object */
#define FREE(obj) do { \
    struct GGGGC_Free *fobj = (obj); \
    fobj->next = (struct GGGGC_Free *) \
        ((ggc_size_t) fobj->next | 2); \
} while (0)

/* get a free object's next object, through markers */
#define FREED_OBJECT(obj) \
    (UNFREE_PTR(struct GGGGC_FREE, (obj)->next))

/* unfree a pointer*/
#define UNFREE_PTR(type, ptr) \
    ((type *) ((ggc_size_t) (ptr) & (ggc_size_t) ~2))

#define UNFREE(obj) do { \
    struct GGGGC_Free *fobj = (obj); \
    fobj->next = UNFREE_PTR(struct GGGGC_Free, fobj->next); \
} while (0)

#define IS_FREE_PTR(ptr) ((ggc_size_t) (ptr) & 2)

#define IS_FREE(obj) IS_FREE_PTR((obj)->next)

static struct ToSearch toSearchList;

void ggggc_markPhase()
{
    struct GGGGC_PoolList pool0Node, *plCur;
    struct GGGGC_Pool *poolCur;
    struct GGGGC_PointerStackList pointerStackNode, *pslCur;
    struct GGGGC_JITPointerStackList jitPointerStackNode, *jpslCur;
    struct GGGGC_PointerStack *psCur;
    void **jpsCur;
    struct ToSearch *toSearch;
    ggc_size_t i;

    TOSEARCH_INIT();

    /* initialize our roots */
    ggc_mutex_lock_raw(&ggggc_rootsLock);
    pointerStackNode.pointerStack = ggggc_pointerStack;
    pointerStackNode.next = ggggc_blockedThreadPointerStacks;
    ggggc_rootPointerStackList = &pointerStackNode;
    jitPointerStackNode.cur = ggc_jitPointerStack;
    jitPointerStackNode.top = ggc_jitPointerStackTop;
    jitPointerStackNode.next = ggggc_blockedThreadJITPointerStacks;
    ggggc_rootJITPointerStackList = &jitPointerStackNode;
    ggc_mutex_unlock(&ggggc_rootsLock);


    /* add our roots to the to-search list */
    for (pslCur = ggggc_rootPointerStackList; pslCur; pslCur = pslCur->next) {
        for (psCur = pslCur->pointerStack; psCur; psCur = psCur->next) {
            for (i = 0; i < psCur->size; i++) {
                TOSEARCH_ADD(psCur->pointers[i]);
            }
        }
    }
    for (jpslCur = ggggc_rootJITPointerStackList; jpslCur; jpslCur = jpslCur->next) {
        for (jpsCur = jpslCur->cur; jpsCur < jpslCur->top; jpsCur++) {
            TOSEARCH_ADD(jpsCur);
        }
    }

    /* The marking phase */
    while (toSearch->used) {
        void **ptr;
        struct GGGGC_Header *obj;

        TOSEARCH_POP(void **, ptr);
        obj = (struct GGGGC_Header *) *ptr;
        if (obj == NULL) continue;
        obj = UNMARK_PTR(struct GGGGC_Header, obj);

        /* if the object isn't already marked... */
        if (!IS_MARKED(obj)) {
            struct GGGGC_Descriptor *descriptor = obj->descriptor__ptr;

            /* then mark it */
            MARK(obj);
            GGGGC_POOL_OF(obj)->survivors += descriptor->size;

            /* add its pointers */
            ADD_OBJECT_POINTERS(obj, descriptor);
        }
    }
}

void ggggc_markAllFreeObjects ()
{
    struct GGGGC_Pool *poolCur;
    struct GGGGC_Free *curFree, *tempFree;

    for (poolCur = ggggc_rootPool; poolCur; poolCur = poolCur->next) {
        curFree = poolCur->freeList;
        while (curFree) {
            tempFree = curFree->next;
            FREE(curFree);
            curFree = tempFree;
        }
    }
}

void ggggc_sweep()
{
    struct GGGGC_Pool *poolCur;
    struct GGGGC_Header *header;
    struct GGGGC_Free *curFree;
    struct GGGGC_Free *prevFree = NULL;
    ggc_size_t *cur, tempSize;

    for (poolCur = ggggc_rootPool; poolCur; poolCur = poolCur->next) {
        for (cur = poolCur->start; cur < poolCur->free; cur += tempSize) {
            header = (struct GGGGC_Header *) cur;
            header = UNMARK_PTR(struct GGGGC_Header, header);
            if (IS_MARKED(header)) {
                /*a marked object*/
                UNMARK(header);
                tempSize = header->descriptor__ptr->size;
                if (tempSize == 1) {
                    tempSize = 2;
                }
            } else if (IS_FREE((struct GGGGC_Free *) header)) {
                /*already a free object*/
                curFree = (struct GGGGC_Free *) header;
                UNFREE(curFree);
                curFree->next = prevFree;
                tempSize = curFree->size;
                prevFree = curFree;
            } else {
                /*a unmarked object, make it free*/
                tempSize = header->descriptor__ptr->size;
                if (tempSize == 1) {
                    tempSize = 2;
                }
                curFree = (struct GGGGC_Free *) header;
                curFree->size = tempSize;
                curFree->next = prevFree;
                prevFree = curFree;
            }
        }
        poolCur->freeList = prevFree;
    }
}

void *ggggc_mallocRaw(struct GGGGC_Descriptor **descriptor, /* descriptor to protect, if applicable */
                      ggc_size_t size /* size of object to allocate */
) {
    struct GGGGC_Pool *pool, *newPool;
    struct GGGGC_Header *ret;
    size_t expand = FALSE;

retry:
    if (ggggc_pool) {
        pool = ggggc_pool;
    } else {
        ggggc_rootPool = ggggc_pool = pool = ggggc_newPool(1);
    }

    if (size==1) {
        size = 2;
    }

    /* check the free list of the pool in use, if there is enough space, we allocate the object there
      * my thought is to use the global variable ggggc_pool here */
    if (pool->freeList) {
        /* check if there is enough space in the freelist */
        struct GGGGC_Free *freeCur, *tempFree, *newFree;
        /* the GGGGC_Free pointer temp is to store the previous GGGGC_Free of freeCur pointer, to assist making
         * the free-list correct*/
        struct GGGGC_Free *temp = NULL;
        for (freeCur = pool->freeList; freeCur; freeCur = freeCur->next){
            if (freeCur->size >= (sizeof(struct GGGGC_Free)/sizeof(ggc_size_t) + size)) {
                ggc_size_t tempSize = freeCur->size;
                tempFree = freeCur->next;
                ret = (struct GGGGC_Header *) freeCur;

                newFree = (struct GGGGC_Free *) ((ggc_size_t *) freeCur + size);
                newFree->next = tempFree;
                newFree->size = tempSize - size;
                /* the space of the current free object on free list is sufficient to store the new object */
                ret->descriptor__ptr=NULL;

#ifdef GGGGC_DEBUG_MEMORY_CORRUPTION
                /* set its canary */
            ret->ggggc_memoryCorruptionCheck = GGGGC_MEMORY_CORRUPTION_VAL;
#endif

                if (temp) {
                    temp->next = newFree;
                } else {
                    pool->freeList = newFree;
                }
                /* and clear the rest (necessary since this goes to the untrusted mutator) */
                memset(ret + 1, 0, size * sizeof(ggc_size_t) - sizeof(struct GGGGC_Header));
                return ret;
            } else if (freeCur->size == size) {
                tempFree = FREED_OBJECT(freeCur);
                ret = (struct GGGGC_Header *) freeCur;
                ret->descriptor__ptr = NULL;
#ifdef GGGGC_DEBUG_MEMORY_CORRUPTION
                /* set its canary */
        		ret->ggggc_memoryCorruptionCheck = GGGGC_MEMORY_CORRUPTION_VAL;
#endif

                if (temp) {
                    temp->next = tempFree;
                } else {
                    pool->freeList = tempFree;
                }
                /* and clear the rest (necessary since this goes to the untrusted mutator) */
                memset(ret+1, 0,  size * sizeof(ggc_size_t) - sizeof(struct GGGGC_Header));
                return ret;
            }
            temp = freeCur;
        }
    }

    /* if there is not enough space in the free list, or the free list does not exist, then
     * we move on to check the unused space in the pool */
    if (pool->end - pool->free >= size) {
        /* good, allocate here */
        ret = (struct GGGGC_Header *) pool->free;
        pool->free += size;

        ret->descriptor__ptr = NULL;
#ifdef GGGGC_DEBUG_MEMORY_CORRUPTION
        /* set its canary */
        ret->ggggc_memoryCorruptionCheck = GGGGC_MEMORY_CORRUPTION_VAL;
#endif

        /* and clear the rest (necessary since this goes to the untrusted mutator) */
        memset(ret + 1, 0, size * sizeof(ggc_size_t) - sizeof(struct GGGGC_Header));
    } else if (pool->next) {
        /* move to the next pool since the current pool don't have enough space to allocate the object*/
        ggggc_pool = pool = pool->next;
        goto retry;
    } else {
        /* a collection is needed since all the pools don't have enough space to allocate the object*/
        /* we also create a new pool in this stage */
        GGC_PUSH_1(*descriptor);
        ggggc_collect0(0);
        GGC_POP();
        /* modified the ggggc_expandPoolList function in allocate.c slightly */
        ggggc_expandPoolList(ggggc_rootPool, ggggc_newPool, 1, expand);
        ggggc_pool = pool = ggggc_rootPool;
        expand = TRUE;
        goto retry;
    }
    return ret;
}

/* allocate an object */
void *ggggc_malloc(struct GGGGC_Descriptor *descriptor)
{
    struct GGGGC_Header *ret = (struct GGGGC_Header *) ggggc_mallocRaw(&descriptor, descriptor->size);
    ret->descriptor__ptr = descriptor;
    return ret;
}

void ggggc_collect0(unsigned char gen)
{
    ggggc_markPhase();
    ggggc_markAllFreeObjects();
    ggggc_sweep();
}

int ggggc_yield()
{
    return 0;
}

#ifdef __cplusplus
}
#endif