// SPDX-FileCopyrightText: Copyright 2026 Igor Augusto and TOport contributors (https://github.com/IgorAugustoHH/The-Order-1886-PC-Port)
// SPDX-License-Identifier: GPL-2.0-or-later
/* Kernel event flags: a 64-bit pattern that threads set/clear and wait on.
 * Wait modes: AND (all bits) or OR (any bit), optionally clearing all or the
 * waited bits on release. Waiters are woken by broadcast and re-check. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define EF_SINGLE 0x10
#define EF_MULTI 0x20
#define WAIT_AND 0x01
#define WAIT_OR 0x02
#define WAIT_CLEAR_ALL 0x10
#define WAIT_CLEAR_PAT 0x20
#define EF_MAGIC UINT32_C(0x45464C47)

typedef struct EventFlag {
    uint32_t magic, attr;
    uint64_t pattern;
    int waiters, deleted, cancelled;
    char name[32];
    pthread_cond_t changed;
    struct EventFlag *next;
} EventFlag;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static EventFlag *registry;
static size_t created, waits, timeouts;

/* winpthreads condition variables accept only CLOCK_REALTIME (see runtime_sema.c). */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(WAIT_CLOCK,&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static EventFlag *find(EventFlag *ef) {
    for (EventFlag *e=registry;e;e=e->next) if (e==ef && e->magic==EF_MAGIC && !e->deleted) return e;
    return NULL;
}
static int satisfied(uint64_t pattern,uint64_t bits,uint32_t mode) {
    return (mode&WAIT_AND) ? (pattern&bits)==bits : (pattern&bits)!=0;
}
static void release(EventFlag *e,uint64_t bits,uint32_t mode) {
    if (mode&WAIT_CLEAR_ALL) e->pattern=0;
    else if (mode&WAIT_CLEAR_PAT) e->pattern&=~bits;
}
static int valid_mode(uint32_t mode) {
    uint32_t wait=mode&3, clear=mode&0x30;
    return (wait==WAIT_AND || wait==WAIT_OR) && clear!=0x30 && !(mode&~0x33u);
}

static ABI int32_t ef_create(EventFlag **out,const char *name,uint32_t attr,uint64_t pattern,const void *options) {
    (void)options;
    if (!out || !name) return ERR(22);
    if (!(attr&(EF_SINGLE|EF_MULTI))) attr|=EF_SINGLE;
    EventFlag *e=calloc(1,sizeof(*e));
    if (!e) return ERR(12);
    e->magic=EF_MAGIC; e->attr=attr; e->pattern=pattern;
    snprintf(e->name,sizeof(e->name),"%s",name);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca,WAIT_CLOCK);
    pthread_cond_init(&e->changed,&ca);
    pthread_condattr_destroy(&ca);
    pthread_mutex_lock(&lock);
    e->next=registry; registry=e; ++created;
    pthread_mutex_unlock(&lock);
    *out=e;
    return 0;
}
static ABI int32_t ef_delete(EventFlag *ef) {
    pthread_mutex_lock(&lock);
    EventFlag *e=find(ef);
    if (!e) { pthread_mutex_unlock(&lock); return ERR(3); }
    e->deleted=1; /* the record stays: a late waiter must not touch freed memory */
    pthread_cond_broadcast(&e->changed);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ef_set(EventFlag *ef,uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *e=find(ef);
    if (!e) { pthread_mutex_unlock(&lock); return ERR(3); }
    e->pattern|=bits;
    pthread_cond_broadcast(&e->changed);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* PS4 clear keeps only the given bits: pattern &= bits. */
static ABI int32_t ef_clear(EventFlag *ef,uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *e=find(ef);
    if (!e) { pthread_mutex_unlock(&lock); return ERR(3); }
    e->pattern&=bits;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ef_poll(EventFlag *ef,uint64_t bits,uint32_t mode,uint64_t *result) {
    if (!bits || !valid_mode(mode)) return ERR(22);
    pthread_mutex_lock(&lock);
    EventFlag *e=find(ef);
    if (!e) { pthread_mutex_unlock(&lock); return ERR(3); }
    if ((e->attr&EF_SINGLE) && e->waiters) { pthread_mutex_unlock(&lock); return ERR(1); } /* EPERM */
    if (result) *result=e->pattern;
    if (!satisfied(e->pattern,bits,mode)) { pthread_mutex_unlock(&lock); return ERR(16); } /* EBUSY */
    release(e,bits,mode);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* timeout: microseconds, updated with the time left; NULL waits forever. */
static ABI int32_t ef_wait_inner(EventFlag *ef,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    if (!bits || !valid_mode(mode)) return ERR(22);
    pthread_mutex_lock(&lock);
    EventFlag *e=find(ef);
    if (!e) { pthread_mutex_unlock(&lock); return ERR(3); }
    if ((e->attr&EF_SINGLE) && e->waiters) { pthread_mutex_unlock(&lock); return ERR(1); }
    ++waits;
    uint64_t start=now_ns(), deadline=timeout ? start+(uint64_t)*timeout*1000 : 0;
    int32_t r=0;
    if (getenv("BB_TRACE_SEMA") && !satisfied(e->pattern,bits,mode)) /* deadlock diagnosis */
        fprintf(stderr,"Runtime: blocking wait on event flag '%s' (bits 0x%llx, mode 0x%x, pattern 0x%llx, timeout %s)\n",
                e->name,(unsigned long long)bits,mode,(unsigned long long)e->pattern,timeout ? "yes" : "no");
    ++e->waiters;
    while (!satisfied(e->pattern,bits,mode)) {
        if (e->deleted) { r=ERR(13); break; }      /* EACCES: deleted while waiting */
        if (timeout) {
            struct timespec ts={(time_t)(deadline/1000000000),(long)(deadline%1000000000)};
            if (pthread_cond_timedwait(&e->changed,&lock,&ts) && now_ns()>=deadline &&
                !satisfied(e->pattern,bits,mode)) { r=ERR(60); ++timeouts; break; } /* ETIMEDOUT */
        } else pthread_cond_wait(&e->changed,&lock);
    }
    --e->waiters;
    if (result) *result=e->pattern;
    if (!r) release(e,bits,mode);
    if (timeout) {
        uint64_t used=(now_ns()-start)/1000;
        *timeout=r==ERR(60) || used>=*timeout ? 0 : *timeout-(uint32_t)used;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

static ABI int32_t ef_wait(EventFlag *ef,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    const uint64_t wait_start=runtime_wait_begin();
    const int32_t r=ef_wait_inner(ef,bits,mode,result,timeout);
    runtime_wait_end("event flag wait",wait_start,"bits",(long long)bits);
    return r;
}
static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",ef_create}, {"sceKernelDeleteEventFlag",ef_delete},
    {"sceKernelSetEventFlag",ef_set}, {"sceKernelClearEventFlag",ef_clear},
    {"sceKernelPollEventFlag",ef_poll}, {"sceKernelWaitEventFlag",ef_wait},
};
uintptr_t runtime_eventflag_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_eventflag_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: event flags created=%zu, waits=%zu, timeouts=%zu\n",created,waits,timeouts);
    pthread_mutex_unlock(&lock);
}
