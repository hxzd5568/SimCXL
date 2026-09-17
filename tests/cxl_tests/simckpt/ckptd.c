/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ckptd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_mbind
#define SYS_mbind 237
#endif
#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif
#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif
#ifndef MPOL_MF_STRICT
#define MPOL_MF_STRICT (1 << 0)
#endif

static long
do_mbind(void *addr, unsigned long len, int mode, unsigned long *nodemask,
         unsigned long maxnode, unsigned flags)
{
    return syscall(SYS_mbind, addr, len, mode, nodemask, maxnode, flags);
}

/* --- manifest ---------------------------------------------------------- */
void
ckpt_manifest_init(struct ckpt_manifest *m)
{
    memset(m, 0, sizeof(*m));
}

uint64_t
ckpt_manifest_begin(struct ckpt_manifest *m, uint64_t ckpt_id,
                    uint32_t expected_chunks)
{
    m->current_gen++;
    m->num_chunks = 0;
    m->expected_chunks = expected_chunks;
    memset(m->chunks, 0, sizeof(m->chunks));
    return m->current_gen;
}

int
ckpt_manifest_add(struct ckpt_manifest *m, uint32_t chunk_id,
                  uint64_t logical_offset, uint32_t length,
                  uint32_t crc, uint8_t source)
{
    if (m->num_chunks >= CKPT_MAX_CHUNKS)
        return -1;

    struct chunk_meta *c = &m->chunks[m->num_chunks++];
    c->checkpoint_id = m->current_gen;
    c->chunk_id = chunk_id;
    c->logical_offset = logical_offset;
    c->length = length;
    c->crc32 = crc;
    c->source = source;
    c->state = CKPT_STATE_IN_FLIGHT;
    return 0;
}

int
ckpt_manifest_commit(struct ckpt_manifest *m, uint32_t chunk_id)
{
    for (uint32_t i = 0; i < m->num_chunks; i++) {
        if (m->chunks[i].chunk_id == chunk_id) {
            m->chunks[i].state = CKPT_STATE_DISK_COMMITTED;
            return 0;
        }
    }
    return -1;
}

int
ckpt_manifest_all_committed(const struct ckpt_manifest *m)
{
    if (m->num_chunks != m->expected_chunks)
        return 0;
    for (uint32_t i = 0; i < m->num_chunks; i++)
        if (m->chunks[i].state != CKPT_STATE_COMMITTED)
            return 0;
    return 1;
}

void
ckpt_manifest_finish(struct ckpt_manifest *m)
{
    m->committed_gen = m->current_gen;
    m->version++;
}

void
ckpt_manifest_rollback(struct ckpt_manifest *m)
{
    /* Discard the in-flight generation; committed_gen stays put. */
    m->current_gen = m->committed_gen;
    m->num_chunks = 0;
}

/* --- chunk lifecycle state machine -------------------------------------- */
static int
find_chunk(const struct ckpt_manifest *m, uint32_t chunk_id)
{
    for (uint32_t i = 0; i < m->num_chunks; i++)
        if (m->chunks[i].chunk_id == chunk_id)
            return (int)i;
    return -1;
}

uint8_t
ckpt_chunk_state(const struct ckpt_manifest *m, uint32_t chunk_id)
{
    int i = find_chunk(m, chunk_id);
    return (i < 0) ? CKPT_STATE_FREE : m->chunks[i].state;
}

int
ckpt_chunk_set_state(struct ckpt_manifest *m, uint32_t chunk_id, uint8_t state)
{
    int i = find_chunk(m, chunk_id);
    if (i < 0)
        return -1;
    m->chunks[i].state = state;
    return 0;
}

static int
transition(struct ckpt_manifest *m, uint32_t chunk_id, uint8_t from,
           uint8_t to)
{
    if (ckpt_chunk_state(m, chunk_id) == from)
        return ckpt_chunk_set_state(m, chunk_id, to);
    return -1;
}

int
ckpt_chunk_pin(struct ckpt_manifest *m, uint32_t chunk_id)
{
    return transition(m, chunk_id, CKPT_STATE_FREE, CKPT_STATE_PINNED);
}

int
ckpt_chunk_promote_hot(struct ckpt_manifest *m, uint32_t chunk_id)
{
    return transition(m, chunk_id, CKPT_STATE_DISK_COMMITTED, CKPT_STATE_HOT);
}

int
ckpt_chunk_evictable(struct ckpt_manifest *m, uint32_t chunk_id)
{
    return transition(m, chunk_id, CKPT_STATE_HOT, CKPT_STATE_EVICTABLE);
}

int
ckpt_chunk_free(struct ckpt_manifest *m, uint32_t chunk_id)
{
    uint8_t s = ckpt_chunk_state(m, chunk_id);
    if (s == CKPT_STATE_EVICTABLE || s == CKPT_STATE_DISK_COMMITTED ||
        s == CKPT_STATE_FREE)
        return ckpt_chunk_set_state(m, chunk_id, CKPT_STATE_FREE);
    return -1;
}

void
ckpt_manifest_state_counts(const struct ckpt_manifest *m,
                           uint32_t counts[CKPT_STATE_EVICTABLE + 1])
{
    for (int s = 0; s <= CKPT_STATE_EVICTABLE; s++)
        counts[s] = 0;
    for (uint32_t i = 0; i < m->num_chunks; i++)
        counts[m->chunks[i].state]++;
}

/* --- pinned pool ------------------------------------------------------- */
void *
ckpt_pool_alloc(int node, size_t size)
{
    void *buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED)
        return NULL;

    memset(buf, 0, size); /* fault in */

    unsigned long mask = 1UL << node;
    if (do_mbind(buf, size, MPOL_BIND, &mask, 64,
                 MPOL_MF_MOVE | MPOL_MF_STRICT) != 0) {
        perror("mbind");
        /* non-fatal: fall back to the default node */
    }
    return buf;
}

void
ckpt_pool_free(void *buf, size_t size)
{
    if (buf)
        munmap(buf, size);
}

void
ckpt_pool_init(struct ckpt_pool *p, int node, uint64_t capacity)
{
    p->numa_node = node;
    p->capacity = capacity;
    p->pinned = 0;
    p->state = POOL_NORMAL;
}

void *
ckpt_pool_alloc_from(struct ckpt_pool *p, size_t size)
{
    if (p->state != POOL_NORMAL)
        return NULL;            /* FROZEN/SHRINK/STOPPED: reuse only */
    if (p->pinned + size > p->capacity)
        return NULL;            /* out of headroom                   */

    void *buf = ckpt_pool_alloc(p->numa_node, size);
    if (buf)
        p->pinned += size;
    return buf;
}

void
ckpt_pool_free_from(struct ckpt_pool *p, void *buf, size_t size)
{
    if (!buf)
        return;
    ckpt_pool_free(buf, size);
    if (p->pinned >= size)
        p->pinned -= size;
    else
        p->pinned = 0;
}

void
ckpt_pool_update(struct ckpt_pool *p, int pressure_pct)
{
    if (pressure_pct > 95)
        p->state = POOL_STOPPED;
    else if (pressure_pct > 90)
        p->state = POOL_SHRINK;
    else if (pressure_pct >= 80)
        p->state = POOL_FROZEN;
    else
        p->state = POOL_NORMAL;
}

/* --- CXL hot standby --------------------------------------------------- */
void
ckpt_hot_init(struct ckpt_hot_standby *h, uint32_t capacity)
{
    h->capacity = capacity;
    h->num_hot = 0;
}

int
ckpt_hot_is_hot(const struct ckpt_hot_standby *h, uint32_t chunk_id)
{
    for (uint32_t i = 0; i < h->num_hot; i++)
        if (h->order[i] == chunk_id)
            return 1;
    return 0;
}

void
ckpt_hot_add(struct ckpt_hot_standby *h, uint32_t chunk_id)
{
    /* Remove an existing entry for chunk_id (if any). */
    for (uint32_t i = 0; i < h->num_hot; i++) {
        if (h->order[i] == chunk_id) {
            for (uint32_t j = i; j + 1 < h->num_hot; j++)
                h->order[j] = h->order[j + 1];
            h->num_hot--;
            break;
        }
    }

    /* Insert at the front (most recent). */
    for (uint32_t i = h->num_hot; i > 0; i--)
        h->order[i] = h->order[i - 1];
    h->order[0] = chunk_id;
    h->num_hot++;

    /* LRU eviction. */
    if (h->num_hot > h->capacity)
        h->num_hot = h->capacity;
}
