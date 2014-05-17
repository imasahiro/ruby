/****************************************************************************
 * Copyright (c) 2012-2014, Masahiro Ide <ide@konohascript.org>
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************/

#include <stdint.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

// This 64-bit-to-32-bit hash was copied from
// http://www.concentric.net/~Ttwang/tech/inthash.htm .
unsigned hash6432shift(VALUE *pc)
{
  long key = (long) pc;
  key = (~key) + (key << 18); // key = (key << 18) - key - 1;
  key = key ^ (key >> 31);
  key = key * 21;    // key = (key + (key << 2)) + (key << 4);
  key = key ^ (key >> 11);
  key = key + (key << 6);
  key = key ^ (key >> 22);
  return (unsigned) key;
}

#define DELTA 16
#define KMAP_INITSIZE 16
#ifndef LOG2
#define LOG2(N) ((uint32_t)((sizeof(void *) * 8) - __builtin_clzl(N - 1)))
#endif

typedef enum map_status_t {
  KMAP_FAILED = 0,
  KMAP_UPDATE = 1,
  KMAP_ADDED  = 2
} map_status_t;


typedef struct kmap_record {
  unsigned hash;
  VALUE        *k;
  struct Trace *v;
} __attribute__((__aligned__(8))) map_record_t;

typedef struct hashmap_t {
  map_record_t *records;
  unsigned used_size;
  unsigned record_size_mask;
} hashmap_t;

typedef struct hashmap_iterator {
  map_record_t *entry;
  unsigned      index;
} hashmap_iterator_t;

static unsigned hashmap_size(hashmap_t *m)
{
  return m->record_size_mask + 1;
}

static void map_record_copy(map_record_t *dst, const map_record_t *src)
{
  *dst = *src;
}

static inline map_record_t *hashmap_at(hashmap_t *m, unsigned idx)
{
  assert(idx < hashmap_size(m));
  return m->records+idx;
}

static void hashmap_record_reset(hashmap_t *m, unsigned newsize)
{
  unsigned alloc_size = (unsigned) (newsize * sizeof(map_record_t));
  m->used_size = 0;
  (m->record_size_mask) = newsize - 1;
  m->records = (map_record_t *) calloc(1, alloc_size);
}

static map_status_t hashmap_set_no_resize(hashmap_t *m, map_record_t *rec)
{
  unsigned i, idx = rec->hash & m->record_size_mask;
  for(i = 0; i < DELTA; ++i) {
    map_record_t *r = m->records+idx;
    if(r->hash == 0) {
      map_record_copy(r, rec);
      ++m->used_size;
      return KMAP_ADDED;
    }
    if(r->hash == rec->hash && r->k == rec->k) {
      map_record_copy(r, rec);
      return KMAP_UPDATE;
    }
    idx = (idx + 1) & m->record_size_mask;
  }
  return KMAP_FAILED;
}

static void hashmap_record_resize(hashmap_t *m, unsigned newsize)
{
  unsigned i;
  unsigned oldsize = hashmap_size(m);
  map_record_t *head = m->records;

  newsize *= 2;
  hashmap_record_reset(m, newsize);
  for(i = 0; i < oldsize; ++i) {
    map_record_t *r = head + i;
    if(r->hash && hashmap_set_no_resize(m, r) == KMAP_FAILED)
      continue;
  }
  free(head);
}

static void hashmap_set_(hashmap_t *m, map_record_t *rec)
{
  do {
    if((hashmap_set_no_resize(m, rec)) != KMAP_FAILED)
      return;
    hashmap_record_resize(m, hashmap_size(m)*2);
  } while(1);
}

static struct Trace *hashmap_get_(hashmap_t *m, unsigned hash, VALUE *key)
{
  unsigned i, idx = hash & m->record_size_mask;
  for(i = 0; i < DELTA; ++i) {
    map_record_t *r = m->records+idx;
    if(r->hash == hash && r->k == key) {
      return r->v;
    }
    idx = (idx + 1) & m->record_size_mask;
  }
  return NULL;
}

static void hashmap_init(hashmap_t *m, unsigned init)
{
  if(init < KMAP_INITSIZE)
    init = KMAP_INITSIZE;
  hashmap_record_reset(m, 1U << LOG2(init));
}

static void hashmap_dispose(hashmap_t *m, void (*Destructor)(void*))
{
  unsigned i, size = hashmap_size(m);
  if(Destructor) {
    for(i = 0; i < size; ++i) {
      map_record_t *r = hashmap_at(m, i);
      if(r->hash) {
        Destructor(r->v);
      }
    }
  }
  free(m->records);
}

static struct Trace *hashmap_get(hashmap_t *m, VALUE *key)
{
  unsigned hash = hash6432shift(key);
  return hashmap_get_(m, hash, key);
}

static void hashmap_set(hashmap_t *m, VALUE *key, struct Trace *val)
{
  map_record_t r;
  r.hash = hash6432shift(key);
  r.k    = key;
  r.v    = val;
  hashmap_set_(m, &r);
}

static int hashmap_next(hashmap_t *m, hashmap_iterator_t *itr)
{
  unsigned idx, size = hashmap_size(m);
  for (idx = itr->index; idx < size; idx++) {
    map_record_t *r = hashmap_at(m, idx);
    if(r->hash != 0) {
      itr->index = idx + 1;
      itr->entry = r;
      return 1;
    }
  }
  return 0;
}

static void hashmap_remove(hashmap_t *m, VALUE *key)
{
  unsigned hash = hash6432shift(key);
  unsigned i, idx = hash & m->record_size_mask;
  for(i = 0; i < DELTA; ++i) {
    map_record_t *r = hashmap_at(m, idx);
    if(r->hash == hash && r->k == key) {
      r->hash = 0;
      r->k    = 0;
      m->used_size -= 1;
      return;
    }
    idx = (idx + 1) & m->record_size_mask;
  }
}

#ifdef __cplusplus
} /* extern "C" */
#endif
