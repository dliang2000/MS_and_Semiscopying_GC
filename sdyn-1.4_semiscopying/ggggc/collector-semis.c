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

/* list of pointers to search and associated macros */
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
    if (descriptor->pointers[0] & 1l) { \
        /* it has pointers */ \
        curDescription = descriptor->pointers[0] >> 1; \
        for (curWord = 1; curWord < descriptor->size; curWord++) { \
            if (curWord % GGGGC_BITS_PER_WORD == 0) \
                curDescription = descriptor->pointers[++curDescriptorWord]; \
            if (curDescription & 1l) \
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
        ((ggc_size_t) hobj->descriptor__ptr | (ggc_size_t) 1l); \
} while (0)

/* unmark a pointer */
#define UNMARK_PTR(type, ptr) \
    ((type *) ((ggc_size_t) (ptr) & (ggc_size_t) ~1l))

/* get an object's descriptor, through markers */
#define MARKED_DESCRIPTOR(obj) \
    (UNMARK_PTR(struct GGGGC_Descriptor, (obj)->descriptor__ptr))

/* unmark an object */
#define UNMARK(obj) do { \
    struct GGGGC_Header *hobj = (obj); \
    hobj->descriptor__ptr = UNMARK_PTR(struct GGGGC_Descriptor, hobj->descriptor__ptr); \
} while (0)

/* is this pointer marked? */
#define IS_MARKED_PTR(ptr) ((ggc_size_t) (ptr) & 1l)

/* is this object marked? */
#define IS_MARKED(obj) IS_MARKED_PTR((obj)->descriptor__ptr)


/* unforward a pointer */
#define FORWARD(obj) do { \
    struct GGGGC_Header *hobj = (obj); \
    hobj->descriptor__ptr = (struct GGGGC_Descriptor *) \
        ((ggc_size_t) hobj->descriptor__ptr | (ggc_size_t) 2); \
} while (0)

#define UNFORWARD_PTR(type, ptr) \
    ((type *) ((ggc_size_t) (ptr) & (ggc_size_t) ~2l))

/* get an object's descriptor, through markers */
#define FORWARDED_DESCRIPTOR(obj) \
    (UNFORWARD_PTR(struct GGGGC_Descriptor, (obj)->descriptor__ptr))

#define IS_FORWARDED_PTR(ptr) ((ggc_size_t) (ptr) & 2l)

/* is this object marked? */
#define IS_FORWARDED(obj) IS_FORWARDED_PTR((obj)->descriptor__ptr)

static struct ToSearch toSearchList;


void ggggc_processAndForward()
{
    struct GGGGC_Pool *poolCur;
    struct GGGGC_Header *header;
    ggc_size_t *cur, tempSize;

    if (poolOrder == 0) {
        poolCur = ggggc_fromPool;
    } else {
        poolCur = ggggc_toPool;
    }

    while (poolCur) {
        for (cur = poolCur->start; cur < poolCur->free; cur += tempSize) {
            header = (struct GGGGC_Header *) cur;
            header = UNMARK_PTR(struct GGGGC_Header, header);
            if (IS_MARKED(header)) {
                UNMARK(header);
                //printf("header after unmark: %lu \n", header);
                tempSize = header->descriptor__ptr->size;
                //printf("tempSize after unmark: %lu \n", tempSize);
                memcpy(poolCur->poolBuddy->free, header, tempSize * sizeof(ggc_size_t));
                header->descriptor__ptr = (struct GGGGC_Descriptor *) poolCur->poolBuddy->free;
                poolCur->poolBuddy->free += tempSize;
                FORWARD(header);
            } else {
                tempSize = header->descriptor__ptr->size;
                //printf("tempSize: %lu \n", tempSize);
            }
        }
        poolCur = poolCur->next;
    }

    ggggc_updateRefs();
}

void ggggc_updateRefs()
{
    struct GGGGC_Pool *poolCur, *tempPool;
    struct GGGGC_PointerStackList *pslCur;
    struct GGGGC_JITPointerStackList *jpslCur;
    struct GGGGC_PointerStack *psCur;
    struct GGGGC_Header *header, *tmpHeader;
    struct GGGGC_Descriptor *descriptor, *tempDescriptor;
    void **jpsCur;
    void **objVp;
    ggc_size_t i, *cur, tempSize;
    ggc_size_t curWord, curDescription, curDescriptorWord;

    if (poolOrder == 0) {
        poolOrder = 1;
        ggggc_pool = poolCur = ggggc_toPool;
    } else {
        poolOrder = 0;
        ggggc_pool = poolCur = ggggc_fromPool;
    }

    for (pslCur = ggggc_rootPointerStackList; pslCur; pslCur = pslCur->next) {
        for (psCur = pslCur->pointerStack; psCur; psCur = psCur->next) {
            ggc_size_t ***pointers = (ggc_size_t ***) psCur->pointers;
            for (i = 0; i < psCur->size; i++) {
                if (*pointers[i]) {
                    header = (struct GGGGC_Header *) *pointers[i];
                    if (IS_FORWARDED(header)) {
                        *pointers[i] = UNFORWARD_PTR(struct GGGGC_Header, (header->descriptor__ptr));
                    }
                }
            }
        }
    }

    for (jpslCur = ggggc_rootJITPointerStackList; jpslCur; jpslCur = jpslCur->next) {
        for (jpsCur = jpslCur->cur; *jpsCur && jpsCur < jpslCur->top; jpsCur++) {
            header = (struct GGGGC_Header *) *jpsCur;
            if (IS_FORWARDED(header)) {
                *jpsCur = UNFORWARD_PTR(struct GGGGC_Header, (header->descriptor__ptr));
            }
        }
    }

    while (poolCur) {
        for (cur = poolCur->start; cur < poolCur->free; cur += tempSize) {
            header = (struct GGGGC_Header *) cur;
            objVp = (void **) (header);
            if (IS_FORWARDED(((struct GGGGC_Header *) header->descriptor__ptr))) {
                /* Since after the casting, header->descriptor__ptr is forwarded, which means it must be pointing to another header */
                tmpHeader = (struct GGGGC_Header *) header->descriptor__ptr;
                descriptor = tmpHeader->descriptor__ptr;
                header->descriptor__ptr = UNFORWARD_PTR(struct GGGGC_Descriptor, descriptor);
            }

            descriptor = header->descriptor__ptr;
            curDescriptorWord = 0;
            if (descriptor->pointers[0] & 1l) {
                curDescription = descriptor->pointers[0] >> 1;
                for (curWord = 1; curWord < descriptor->size; curWord++) {
                    if (curWord % GGGGC_BITS_PER_WORD == 0) {
                        curDescription = descriptor->pointers[++curDescriptorWord];
                    }
                    if (objVp[curWord] && (curDescription & 1l)) {
                        if (IS_FORWARDED(((struct GGGGC_Header *) objVp[curWord]))) {
                            /* Same idea, objVp[curWord] is forwarded, then it must be pointing to another header */
                            tmpHeader = (struct GGGGC_Header *) objVp[curWord];
                            tempDescriptor = tmpHeader->descriptor__ptr;
                            objVp[curWord] = UNFORWARD_PTR(struct GGGGC_Header, tempDescriptor);
                        }
                    }
                    curDescription >>= 1;
                }
            }
            tempSize = descriptor->size;
        }
        poolCur->poolBuddy->free = poolCur->poolBuddy->start;
        poolCur = poolCur->next;
    }
}

void ggggc_nowExpand(struct GGGGC_Pool *poolList1, struct GGGGC_Pool *poolList2,
                     struct GGGGC_Pool *(*newPool)(struct GGGGC_Pool *),
                     int ratio, int expand)
                     {
    struct GGGGC_Pool *pool = poolList1;
    struct GGGGC_Pool *pool2 = poolList2;
    ggc_size_t space, survivors, poolCt;

    if (!pool || !pool2) return;

    /* first figure out how much space was used */
    space = 0;
    survivors = 0;
    poolCt = 0;
    while (1) {
        space += pool->end - pool->start;
        survivors += pool->survivors;
        pool->survivors = 0;
        poolCt++;
        if (!pool->next) break;
        pool = pool->next;
    }

    /* now decide if it's too much */
    if ((survivors<<ratio) > space || expand) {
        /* allocate more */
        ggc_size_t i;
        for (i = 0; i < poolCt; i++) {
            //printf("expanding pool. \n");
            pool->next = newPool(poolList1);
            pool = pool->next;
            if (!pool) break;
            pool2->next = newPool(poolList2);
            pool2 = pool2->next;
            pool->poolBuddy = pool2;
            pool2->poolBuddy = pool;
        }
    }
}


void *ggggc_mallocRaw(struct GGGGC_Descriptor **descriptor,  /*descriptor to protect, if applicable*/
                      ggc_size_t size  /*size of object to allocate*/
) {
    struct GGGGC_Pool *pool;
    struct GGGGC_Header *ret;
    size_t expand = FALSE;

retry:
    if (ggggc_pool) {
        pool = ggggc_pool;
    } else {
        poolOrder = 0;
        ggggc_fromPool = ggggc_pool = pool = ggggc_newPool(1);
        ggggc_toPool = ggggc_newPool(1);
        ggggc_fromPool->poolBuddy = ggggc_toPool;
        ggggc_toPool->poolBuddy = ggggc_fromPool;
    }

    if (pool->end - pool->free >= size) {
         /*good, allocate here*/
        ret = (struct GGGGC_Header *) pool->free;
        pool->free += size;

        ret->descriptor__ptr = NULL;
#ifdef GGGGC_DEBUG_MEMORY_CORRUPTION
         set its canary
        ret->ggggc_memoryCorruptionCheck = GGGGC_MEMORY_CORRUPTION_VAL;
#endif

         /*and clear the rest (necessary since this goes to the untrusted mutator) */
        memset(ret + 1, 0, size * sizeof(ggc_size_t) - sizeof(struct GGGGC_Header));
    } else if (pool->next) {
         /*move to the next pool since the current pool don't have enough space to allocate the object*/
        ggggc_pool = pool = pool->next;
        goto retry;
    } else {
         /*a collection is needed since all the pools don't have enough space to allocate the object
         we also create a new pool in this stage */
        GGC_PUSH_1(*descriptor);
        ggggc_collect0(0);
        GGC_POP();
        ggggc_nowExpand(ggggc_fromPool, ggggc_toPool, ggggc_newPool, 1, expand);
        if (poolOrder == 0) {
            ggggc_pool = pool = ggggc_fromPool;
        } else {
            ggggc_pool = pool = ggggc_toPool;
        }
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
    struct GGGGC_PoolList pool0Node, *plCur;
    struct GGGGC_Pool *poolCur, *pool1, *pool2;
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

    /* mark objects. The same marking phase as in mark and sweep GC. It marks all the objects reachable from the root
     * so they can be moved to the to-space and the references to be updated after.*/
    while (toSearch->used) {
        void **ptr;
        struct GGGGC_Header *obj;

        TOSEARCH_POP(void **, ptr);
        obj = (struct GGGGC_Header *) *ptr;
        if (obj == NULL) continue;
        obj = UNMARK_PTR(struct GGGGC_Header, obj);
        /*if the object isn't already marked...*/
        if (!IS_MARKED(obj)) {
            struct GGGGC_Descriptor *descriptor = obj->descriptor__ptr;

            /* then mark it */
            MARK(obj);
            GGGGC_POOL_OF(obj)->survivors += descriptor->size;

            /* add its pointers */
            ADD_OBJECT_POINTERS(obj, descriptor);
        }
    }

    ggggc_processAndForward();
}

int ggggc_yield()
{
    return 0;
}

#ifdef __cplusplus
}
#endif
