/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "tlrucache.h"
#include "os.h"
#include "taoserror.h"
#include "tarray.h"
#include "tdef.h"
#include "tlog.h"
#include "tutil.h"

typedef struct SLRUEntry      SLRUEntry;
typedef struct SLRUEntryTable SLRUEntryTable;
typedef struct SLRUCacheShard SLRUCacheShard;
typedef struct SShardedCache  SShardedCache;

enum {
  TAOS_LRU_IN_CACHE = (1 << 0),  // Whether this entry is referenced by the hash table.

  TAOS_LRU_IS_HIGH_PRI = (1 << 1),  // Whether this entry is high priority entry.

  TAOS_LRU_IN_HIGH_PRI_POOL = (1 << 2),  // Whether this entry is in high-pri pool.

  TAOS_LRU_HAS_HIT = (1 << 3),  // Whether this entry has had any lookups (hits).
};

struct SLRUEntry {
  void                  *value;
  _taos_lru_deleter_t    deleter;
  _taos_lru_overwriter_t overwriter;
  void                  *ud;
  SLRUEntry             *nextHash;
  SLRUEntry             *next;
  SLRUEntry             *prev;
  size_t                 totalCharge;
  size_t                 keyLength;
  uint32_t               hash;
  uint32_t               refs;
  uint8_t                flags;
  char                   keyData[1];
};

#define TAOS_LRU_ENTRY_IN_CACHE(h)     ((h)->flags & TAOS_LRU_IN_CACHE)
#define TAOS_LRU_ENTRY_IN_HIGH_POOL(h) ((h)->flags & TAOS_LRU_IN_HIGH_PRI_POOL)
#define TAOS_LRU_ENTRY_IS_HIGH_PRI(h)  ((h)->flags & TAOS_LRU_IS_HIGH_PRI)
#define TAOS_LRU_ENTRY_HAS_HIT(h)      ((h)->flags & TAOS_LRU_HAS_HIT)

#define TAOS_LRU_ENTRY_SET_IN_CACHE(h, inCache) \
  do {                                          \
    if (inCache) {                              \
      (h)->flags |= TAOS_LRU_IN_CACHE;          \
    } else {                                    \
      (h)->flags &= ~TAOS_LRU_IN_CACHE;         \
    }                                           \
  } while (0)
#define TAOS_LRU_ENTRY_SET_IN_HIGH_POOL(h, inHigh) \
  do {                                             \
    if (inHigh) {                                  \
      (h)->flags |= TAOS_LRU_IN_HIGH_PRI_POOL;     \
    } else {                                       \
      (h)->flags &= ~TAOS_LRU_IN_HIGH_PRI_POOL;    \
    }                                              \
  } while (0)
#define TAOS_LRU_ENTRY_SET_PRIORITY(h, priority) \
  do {                                           \
    if (priority == TAOS_LRU_PRIORITY_HIGH) {    \
      (h)->flags |= TAOS_LRU_IS_HIGH_PRI;        \
    } else {                                     \
      (h)->flags &= ~TAOS_LRU_IS_HIGH_PRI;       \
    }                                            \
  } while (0)
#define TAOS_LRU_ENTRY_SET_HIT(h) ((h)->flags |= TAOS_LRU_HAS_HIT)

#define TAOS_LRU_ENTRY_HAS_REFS(h) ((h)->refs > 0)
#define TAOS_LRU_ENTRY_REF(h)      (++(h)->refs)

static bool taosLRUEntryUnref(SLRUEntry *entry) {
  --entry->refs;
  return entry->refs == 0;
}

static void taosLRUEntryFree(SLRUEntry *entry) {
  if (entry->deleter) {
    (*entry->deleter)(entry->keyData, entry->keyLength, entry->value, entry->ud);
  }

  taosMemoryFree(entry);
}

typedef void (*_taos_lru_table_func_t)(SLRUEntry *entry);

struct SLRUEntryTable {
  int         lengthBits;
  SLRUEntry **list;
  uint32_t    elems;
  int         maxLengthBits;
};

static int taosLRUEntryTableInit(SLRUEntryTable *table, int maxUpperHashBits) {
  table->lengthBits = 16;
  table->list = taosMemoryCalloc(1 << table->lengthBits, sizeof(SLRUEntry *));
  if (!table->list) {
    TAOS_RETURN(terrno);
  }

  table->elems = 0;
  table->maxLengthBits = maxUpperHashBits;

  TAOS_RETURN(TSDB_CODE_SUCCESS);
}

static void taosLRUEntryTableApply(SLRUEntryTable *table, _taos_lru_table_func_t func, uint32_t begin, uint32_t end) {
  for (uint32_t i = begin; i < end; ++i) {
    SLRUEntry *h = table->list[i];
    while (h) {
      SLRUEntry *n = h->nextHash;
      func(h);
      h = n;
    }
  }
}

static void taosLRUEntryTableFree(SLRUEntry *entry) {
  if (!TAOS_LRU_ENTRY_HAS_REFS(entry)) {
    taosLRUEntryFree(entry);
  }
}

static void taosLRUEntryTableCleanup(SLRUEntryTable *table) {
  taosLRUEntryTableApply(table, taosLRUEntryTableFree, 0, 1 << table->lengthBits);

  taosMemoryFree(table->list);
}

static int taosLRUEntryTableApplyF(SLRUEntryTable *table, _taos_lru_functor_t functor, void *ud) {
  int      ret = 0;
  uint32_t end = 1 << table->lengthBits;
  for (uint32_t i = 0; i < end; ++i) {
    SLRUEntry *h = table->list[i];
    while (h) {
      SLRUEntry *n = h->nextHash;
      ret = functor(h->keyData, h->keyLength, h->value, ud);
      if (ret) {
        return ret;
      }
      h = n;
    }
  }

  return ret;
}

static SLRUEntry **taosLRUEntryTableFindPtr(SLRUEntryTable *table, const void *key, size_t keyLen, uint32_t hash) {
  SLRUEntry **entry = &table->list[hash >> (32 - table->lengthBits)];
  while (*entry && ((*entry)->hash != hash || memcmp(key, (*entry)->keyData, keyLen) != 0)) {
    entry = &(*entry)->nextHash;
  }

  return entry;
}

static void taosLRUEntryTableResize(SLRUEntryTable *table) {
  int lengthBits = table->lengthBits;
  if (lengthBits >= table->maxLengthBits) {
    return;
  }

  if (lengthBits >= 31) {
    return;
  }

  uint32_t    oldLength = 1 << lengthBits;
  int         newLengthBits = lengthBits + 1;
  SLRUEntry **newList = taosMemoryCalloc(1 << newLengthBits, sizeof(SLRUEntry *));
  if (!newList) {
    return;
  }
  uint32_t count = 0;
  for (uint32_t i = 0; i < oldLength; ++i) {
    SLRUEntry *entry = table->list[i];
    while (entry) {
      SLRUEntry  *next = entry->nextHash;
      uint32_t    hash = entry->hash;
      SLRUEntry **ptr = &newList[hash >> (32 - newLengthBits)];
      entry->nextHash = *ptr;
      *ptr = entry;
      entry = next;
      ++count;
    }
  }

  taosMemoryFree(table->list);
  table->list = newList;
  table->lengthBits = newLengthBits;
}

static SLRUEntry *taosLRUEntryTableLookup(SLRUEntryTable *table, const void *key, size_t keyLen, uint32_t hash) {
  return *taosLRUEntryTableFindPtr(table, key, keyLen, hash);
}

static SLRUEntry *taosLRUEntryTableInsert(SLRUEntryTable *table, SLRUEntry *entry) {
  SLRUEntry **ptr = taosLRUEntryTableFindPtr(table, entry->keyData, entry->keyLength, entry->hash);
  SLRUEntry  *old = *ptr;
  entry->nextHash = (old == NULL) ? NULL : old->nextHash;
  *ptr = entry;
  if (old == NULL) {
    ++table->elems;
    if ((table->elems >> table->lengthBits) > 0) {
      taosLRUEntryTableResize(table);
    }
  }

  return old;
}

static SLRUEntry *taosLRUEntryTableRemove(SLRUEntryTable *table, const void *key, size_t keyLen, uint32_t hash) {
  SLRUEntry **entry = taosLRUEntryTableFindPtr(table, key, keyLen, hash);
  SLRUEntry  *result = *entry;
  if (result) {
    *entry = result->nextHash;
    --table->elems;
  }

  return result;
}

struct SLRUCacheShard {
  size_t         capacity;
  size_t         highPriPoolUsage;
  bool           strictCapacity;
  double         highPriPoolRatio;
  double         highPriPoolCapacity;
  SLRUEntry      lru;
  SLRUEntry     *lruLowPri;
  SLRUEntryTable table;
  size_t         usage;     // Memory size for entries residing in the cache.
  size_t         lruUsage;  // Memory size for entries residing only in the LRU list.
  TdThreadMutex  mutex;
};

#define TAOS_LRU_CACHE_SHARD_HASH32(key, len) (MurmurHash3_32((key), (len)))

static void taosLRUCacheShardMaintainPoolSize(SLRUCacheShard *shard) {
  while (shard->highPriPoolUsage > shard->highPriPoolCapacity) {
    shard->lruLowPri = shard->lruLowPri->next;
    TAOS_LRU_ENTRY_SET_IN_HIGH_POOL(shard->lruLowPri, false);

    shard->highPriPoolUsage -= shard->lruLowPri->totalCharge;
  }
}

static void taosLRUCacheShardLRUInsert(SLRUCacheShard *shard, SLRUEntry *e) {
  if (shard->highPriPoolRatio > 0 && (TAOS_LRU_ENTRY_IS_HIGH_PRI(e) || TAOS_LRU_ENTRY_HAS_HIT(e))) {
    e->next = &shard->lru;
    e->prev = shard->lru.prev;

    e->prev->next = e;
    e->next->prev = e;

    TAOS_LRU_ENTRY_SET_IN_HIGH_POOL(e, true);
    shard->highPriPoolUsage += e->totalCharge;
    taosLRUCacheShardMaintainPoolSize(shard);
  } else {
    e->next = shard->lruLowPri->next;
    e->prev = shard->lruLowPri;

    e->prev->next = e;
    e->next->prev = e;

    TAOS_LRU_ENTRY_SET_IN_HIGH_POOL(e, false);
    shard->lruLowPri = e;
  }

  shard->lruUsage += e->totalCharge;
}

static void taosLRUCacheShardLRURemove(SLRUCacheShard *shard, SLRUEntry *e) {
  if (shard->lruLowPri == e) {
    shard->lruLowPri = e->prev;
  }
  e->next->prev = e->prev;
  e->prev->next = e->next;
  e->prev = e->next = NULL;

  shard->lruUsage -= e->totalCharge;
  if (TAOS_LRU_ENTRY_IN_HIGH_POOL(e)) {
    shard->highPriPoolUsage -= e->totalCharge;
  }
}

static void taosLRUCacheShardEvictLRU(SLRUCacheShard *shard, size_t charge, SArray *deleted) {
  while (shard->usage + charge > shard->capacity && shard->lru.next != &shard->lru) {
    SLRUEntry *old = shard->lru.next;

    taosLRUCacheShardLRURemove(shard, old);
    SLRUEntry *tentry = taosLRUEntryTableRemove(&shard->table, old->keyData, old->keyLength, old->hash);
    TAOS_LRU_ENTRY_SET_IN_CACHE(old, false);
    shard->usage -= old->totalCharge;

    if (!taosArrayPush(deleted, &old)) {
      // ignore this round's eviting
    }
  }
}

static void taosLRUCacheShardSetCapacity(SLRUCacheShard *shard, size_t capacity) {
  SArray *lastReferenceList = taosArrayInit(16, POINTER_BYTES);
  if (!lastReferenceList) {
    return;
  }

  (void)taosThreadMutexLock(&shard->mutex);

  shard->capacity = capacity;
  shard->highPriPoolCapacity = capacity * shard->highPriPoolRatio;
  taosLRUCacheShardEvictLRU(shard, 0, lastReferenceList);

  (void)taosThreadMutexUnlock(&shard->mutex);

  for (int i = 0; i < taosArrayGetSize(lastReferenceList); ++i) {
    SLRUEntry *entry = taosArrayGetP(lastReferenceList, i);
    taosLRUEntryFree(entry);
  }
  taosArrayDestroy(lastReferenceList);
}

static int taosLRUCacheShardInit(SLRUCacheShard *shard, size_t capacity, bool strict, double highPriPoolRatio,
                                 int maxUpperHashBits) {
  TAOS_CHECK_RETURN(taosLRUEntryTableInit(&shard->table, maxUpperHashBits));

  (void)taosThreadMutexInit(&shard->mutex, NULL);

  (void)taosThreadMutexLock(&shard->mutex);
  shard->capacity = 0;
  shard->highPriPoolUsage = 0;
  shard->strictCapacity = strict;
  shard->highPriPoolRatio = highPriPoolRatio;
  shard->highPriPoolCapacity = 0;

  shard->usage = 0;
  shard->lruUsage = 0;

  shard->lru.next = &shard->lru;
  shard->lru.prev = &shard->lru;
  shard->lruLowPri = &shard->lru;
  (void)taosThreadMutexUnlock(&shard->mutex);

  taosLRUCacheShardSetCapacity(shard, capacity);

  TAOS_RETURN(TSDB_CODE_SUCCESS);
}

static void taosLRUCacheShardCleanup(SLRUCacheShard *shard) {
  (void)taosThreadMutexDestroy(&shard->mutex);

  taosLRUEntryTableCleanup(&shard->table);
}

static LRUStatus taosLRUCacheShardInsertEntry(SLRUCacheShard *shard, SLRUEntry *e, LRUHandle **handle,
                                              bool freeOnFail) {
  LRUStatus  status = TAOS_LRU_STATUS_OK;
  SLRUEntry *toFree = NULL;
  SArray    *lastReferenceList = NULL;
  if (shard->usage + e->totalCharge > shard->capacity) {
    lastReferenceList = taosArrayInit(16, POINTER_BYTES);
    if (!lastReferenceList) {
      taosLRUEntryFree(e);
      return TAOS_LRU_STATUS_FAIL;
    }
  }

  (void)taosThreadMutexLock(&shard->mutex);

  if (shard->usage + e->totalCharge > shard->capacity && shard->lru.next != &shard->lru) {
    if (!lastReferenceList) {
      lastReferenceList = taosArrayInit(16, POINTER_BYTES);
      if (!lastReferenceList) {
        taosLRUEntryFree(e);
        (void)taosThreadMutexUnlock(&shard->mutex);
        return TAOS_LRU_STATUS_FAIL;
      }
    }
    taosLRUCacheShardEvictLRU(shard, e->totalCharge, lastReferenceList);
  }

  if (shard->usage + e->totalCharge > shard->capacity && (shard->strictCapacity || handle == NULL)) {
    TAOS_LRU_ENTRY_SET_IN_CACHE(e, false);
    if (handle == NULL) {
      toFree = e;
    } else {
      if (freeOnFail) {
        taosLRUEntryFree(e);

        *handle = NULL;
      }

      status = TAOS_LRU_STATUS_INCOMPLETE;
    }
  } else {
    SLRUEntry *old = taosLRUEntryTableInsert(&shard->table, e);
    shard->usage += e->totalCharge;
    if (old != NULL) {
      status = TAOS_LRU_STATUS_OK_OVERWRITTEN;

      if (old->overwriter) {
        (*old->overwriter)(old->keyData, old->keyLength, old->value, old->ud);
      }

      TAOS_LRU_ENTRY_SET_IN_CACHE(old, false);
      if (!TAOS_LRU_ENTRY_HAS_REFS(old)) {
        taosLRUCacheShardLRURemove(shard, old);
        shard->usage -= old->totalCharge;

        toFree = old;
      }
    }
    if (handle == NULL) {
      taosLRUCacheShardLRUInsert(shard, e);
    } else {
      if (!TAOS_LRU_ENTRY_HAS_REFS(e)) {
        TAOS_LRU_ENTRY_REF(e);
      }

      *handle = (LRUHandle *)e;
    }
  }

_exit:
  (void)taosThreadMutexUnlock(&shard->mutex);

  if (toFree) {
    taosLRUEntryFree(toFree);
  }

  for (int i = 0; i < taosArrayGetSize(lastReferenceList); ++i) {
    SLRUEntry *entry = taosArrayGetP(lastReferenceList, i);

    taosLRUEntryFree(entry);
  }
  taosArrayDestroy(lastReferenceList);

  return status;
}

static LRUStatus taosLRUCacheShardInsert(SLRUCacheShard *shard, const void *key, size_t keyLen, uint32_t hash,
                                         void *value, size_t charge, _taos_lru_deleter_t deleter,
                                         _taos_lru_overwriter_t overwriter, LRUHandle **handle, LRUPriority priority,
                                         void *ud) {
  SLRUEntry *e = taosMemoryCalloc(1, sizeof(SLRUEntry) - 1 + keyLen);
  if (!e) {
    if (deleter) {
      (*deleter)(key, keyLen, value, ud);
    }
    return TAOS_LRU_STATUS_FAIL;
  }

  e->value = value;
  e->flags = 0;
  e->deleter = deleter;
  e->overwriter = overwriter;
  e->ud = ud;
  e->keyLength = keyLen;
  e->hash = hash;
  e->refs = 0;
  e->next = e->prev = NULL;
  TAOS_LRU_ENTRY_SET_IN_CACHE(e, true);

  TAOS_LRU_ENTRY_SET_PRIORITY(e, priority);
  memcpy(e->keyData, key, keyLen);
  // TODO: e->CalcTotalCharge(charge, metadataChargePolicy);
  e->totalCharge = charge;

  return taosLRUCacheShardInsertEntry(shard, e, handle, true);
}

static LRUHandle *taosLRUCacheShardLookup(SLRUCacheShard *shard, const void *key, size_t keyLen, uint32_t hash) {
  SLRUEntry *e = NULL;

  (void)taosThreadMutexLock(&shard->mutex);
  e = taosLRUEntryTableLookup(&shard->table, key, keyLen, hash);
  if (e != NULL) {
    if (!TAOS_LRU_ENTRY_HAS_REFS(e)) {
      taosLRUCacheShardLRURemove(shard, e);
    }
    TAOS_LRU_ENTRY_REF(e);
    TAOS_LRU_ENTRY_SET_HIT(e);
  }

  (void)taosThreadMutexUnlock(&shard->mutex);

  return (LRUHandle *)e;
}

static void taosLRUCacheShardErase(SLRUCacheShard *shard, const void *key, size_t keyLen, uint32_t hash) {
  bool lastReference = false;
  (void)taosThreadMutexLock(&shard->mutex);

  SLRUEntry *e = taosLRUEntryTableRemove(&shard->table, key, keyLen, hash);
  if (e != NULL) {
    TAOS_LRU_ENTRY_SET_IN_CACHE(e, false);
    if (!TAOS_LRU_ENTRY_HAS_REFS(e)) {
      taosLRUCacheShardLRURemove(shard, e);

      shard->usage -= e->totalCharge;
      lastReference = true;
    }
  }

  (void)taosThreadMutexUnlock(&shard->mutex);

  if (lastReference) {
    taosLRUEntryFree(e);
  }
}

static int taosLRUCacheShardApply(SLRUCacheShard *shard, _taos_lru_functor_t functor, void *ud) {
  int ret;

  (void)taosThreadMutexLock(&shard->mutex);

  ret = taosLRUEntryTableApplyF(&shard->table, functor, ud);

  (void)taosThreadMutexUnlock(&shard->mutex);

  return ret;
}

static void taosLRUCacheShardEraseUnrefEntries(SLRUCacheShard *shard) {
  SArray *lastReferenceList = taosArrayInit(16, POINTER_BYTES);

  (void)taosThreadMutexLock(&shard->mutex);

  while (shard->lru.next != &shard->lru) {
    SLRUEntry *old = shard->lru.next;
    taosLRUCacheShardLRURemove(shard, old);
    SLRUEntry *tentry = taosLRUEntryTableRemove(&shard->table, old->keyData, old->keyLength, old->hash);
    TAOS_LRU_ENTRY_SET_IN_CACHE(old, false);
    shard->usage -= old->totalCharge;

    if (!taosArrayPush(lastReferenceList, &old)) {
      taosLRUEntryFree(old);
      return;
    }
  }

  (void)taosThreadMutexUnlock(&shard->mutex);

  for (int i = 0; i < taosArrayGetSize(lastReferenceList); ++i) {
    SLRUEntry *entry = taosArrayGetP(lastReferenceList, i);

    taosLRUEntryFree(entry);
  }

  taosArrayDestroy(lastReferenceList);
}

static bool taosLRUCacheShardRef(SLRUCacheShard *shard, LRUHandle *handle) {
  SLRUEntry *e = (SLRUEntry *)handle;
  (void)taosThreadMutexLock(&shard->mutex);

  TAOS_LRU_ENTRY_REF(e);

  (void)taosThreadMutexUnlock(&shard->mutex);

  return true;
}

static bool taosLRUCacheShardRelease(SLRUCacheShard *shard, LRUHandle *handle, bool eraseIfLastRef) {
  if (handle == NULL) {
    return false;
  }

  SLRUEntry *e = (SLRUEntry *)handle;
  bool       lastReference = false;

  (void)taosThreadMutexLock(&shard->mutex);

  lastReference = taosLRUEntryUnref(e);
  if (lastReference && TAOS_LRU_ENTRY_IN_CACHE(e)) {
    if (shard->usage > shard->capacity || eraseIfLastRef) {
      SLRUEntry *tentry = taosLRUEntryTableRemove(&shard->table, e->keyData, e->keyLength, e->hash);
      TAOS_LRU_ENTRY_SET_IN_CACHE(e, false);
    } else {
      taosLRUCacheShardLRUInsert(shard, e);

      lastReference = false;
    }
  }

  if (lastReference && e->value) {
    shard->usage -= e->totalCharge;
  }

  (void)taosThreadMutexUnlock(&shard->mutex);

  if (lastReference) {
    taosLRUEntryFree(e);
  }

  return lastReference;
}

static size_t taosLRUCacheShardGetUsage(SLRUCacheShard *shard) {
  size_t usage = 0;

  (void)taosThreadMutexLock(&shard->mutex);
  usage = shard->usage;
  (void)taosThreadMutexUnlock(&shard->mutex);

  return usage;
}

static int32_t taosLRUCacheShardGetElems(SLRUCacheShard *shard) {
  int32_t elems = 0;

  (void)taosThreadMutexLock(&shard->mutex);
  elems = shard->table.elems;
  (void)taosThreadMutexUnlock(&shard->mutex);

  return elems;
}

static size_t taosLRUCacheShardGetPinnedUsage(SLRUCacheShard *shard) {
  size_t usage = 0;

  (void)taosThreadMutexLock(&shard->mutex);

  usage = shard->usage - shard->lruUsage;

  (void)taosThreadMutexUnlock(&shard->mutex);

  return usage;
}

static void taosLRUCacheShardSetStrictCapacity(SLRUCacheShard *shard, bool strict) {
  (void)taosThreadMutexLock(&shard->mutex);

  shard->strictCapacity = strict;

  (void)taosThreadMutexUnlock(&shard->mutex);
}

struct SShardedCache {
  uint32_t      shardMask;
  TdThreadMutex capacityMutex;
  size_t        capacity;
  bool          strictCapacity;
  uint64_t      lastId;  // atomic var for last id
};

struct SLRUCache {
  SShardedCache   shardedCache;
  SLRUCacheShard *shards;
  int             numShards;
};

static int getDefaultCacheShardBits(size_t capacity) {
  int    numShardBits = 0;
  size_t minShardSize = 512 * 1024;
  size_t numShards = capacity / minShardSize;
  while (numShards >>= 1) {
    if (++numShardBits >= 6) {
      return numShardBits;
    }
  }

  return numShardBits;
}

SLRUCache *taosLRUCacheInit(size_t capacity, int numShardBits, double highPriPoolRatio) {
  if (numShardBits >= 20) {
    return NULL;
  }
  if (highPriPoolRatio < 0.0 || highPriPoolRatio > 1.0) {
    return NULL;
  }
  SLRUCache *cache = taosMemoryCalloc(1, sizeof(SLRUCache));
  if (!cache) {
    return NULL;
  }

  if (numShardBits < 0) {
    numShardBits = getDefaultCacheShardBits(capacity);
  }

  int numShards = 1 << numShardBits;
  cache->shards = taosMemoryCalloc(numShards, sizeof(SLRUCacheShard));
  if (!cache->shards) {
    taosMemoryFree(cache);
    return NULL;
  }

  bool   strictCapacity = 1;
  size_t perShard = (capacity + (numShards - 1)) / numShards;
  for (int i = 0; i < numShards; ++i) {
    if (TSDB_CODE_SUCCESS !=
        taosLRUCacheShardInit(&cache->shards[i], perShard, strictCapacity, highPriPoolRatio, 32 - numShardBits)) {
      taosMemoryFree(cache->shards);
      taosMemoryFree(cache);
      return NULL;
    }
  }

  cache->numShards = numShards;

  cache->shardedCache.shardMask = (1 << numShardBits) - 1;
  cache->shardedCache.strictCapacity = strictCapacity;
  cache->shardedCache.capacity = capacity;
  cache->shardedCache.lastId = 1;

  (void)taosThreadMutexInit(&cache->shardedCache.capacityMutex, NULL);

  return cache;
}

void taosLRUCacheCleanup(SLRUCache *cache) {
  if (cache) {
    if (cache->shards) {
      int numShards = cache->numShards;
      for (int i = 0; i < numShards; ++i) {
        taosLRUCacheShardCleanup(&cache->shards[i]);
      }
      taosMemoryFree(cache->shards);
      cache->shards = 0;
    }

    (void)taosThreadMutexDestroy(&cache->shardedCache.capacityMutex);

    taosMemoryFree(cache);
  }
}

LRUStatus taosLRUCacheInsert(SLRUCache *cache, const void *key, size_t keyLen, void *value, size_t charge,
                             _taos_lru_deleter_t deleter, _taos_lru_overwriter_t overwriter, LRUHandle **handle,
                             LRUPriority priority, void *ud) {
  uint32_t hash = TAOS_LRU_CACHE_SHARD_HASH32(key, keyLen);
  uint32_t shardIndex = hash & cache->shardedCache.shardMask;

  return taosLRUCacheShardInsert(&cache->shards[shardIndex], key, keyLen, hash, value, charge, deleter, overwriter,
                                 handle, priority, ud);
}

LRUHandle *taosLRUCacheLookup(SLRUCache *cache, const void *key, size_t keyLen) {
  uint32_t hash = TAOS_LRU_CACHE_SHARD_HASH32(key, keyLen);
  uint32_t shardIndex = hash & cache->shardedCache.shardMask;

  return taosLRUCacheShardLookup(&cache->shards[shardIndex], key, keyLen, hash);
}

void taosLRUCacheErase(SLRUCache *cache, const void *key, size_t keyLen) {
  uint32_t hash = TAOS_LRU_CACHE_SHARD_HASH32(key, keyLen);
  uint32_t shardIndex = hash & cache->shardedCache.shardMask;

  return taosLRUCacheShardErase(&cache->shards[shardIndex], key, keyLen, hash);
}

void taosLRUCacheApply(SLRUCache *cache, _taos_lru_functor_t functor, void *ud) {
  int numShards = cache->numShards;
  for (int i = 0; i < numShards; ++i) {
    if (taosLRUCacheShardApply(&cache->shards[i], functor, ud)) {
      break;
    }
  }
}

void taosLRUCacheEraseUnrefEntries(SLRUCache *cache) {
  int numShards = cache->numShards;
  for (int i = 0; i < numShards; ++i) {
    taosLRUCacheShardEraseUnrefEntries(&cache->shards[i]);
  }
}

bool taosLRUCacheRef(SLRUCache *cache, LRUHandle *handle) {
  if (handle == NULL) {
    return false;
  }

  uint32_t hash = ((SLRUEntry *)handle)->hash;
  uint32_t shardIndex = hash & cache->shardedCache.shardMask;

  return taosLRUCacheShardRef(&cache->shards[shardIndex], handle);
}

bool taosLRUCacheRelease(SLRUCache *cache, LRUHandle *handle, bool eraseIfLastRef) {
  if (handle == NULL) {
    return false;
  }

  uint32_t hash = ((SLRUEntry *)handle)->hash;
  uint32_t shardIndex = hash & cache->shardedCache.shardMask;

  return taosLRUCacheShardRelease(&cache->shards[shardIndex], handle, eraseIfLastRef);
}

void *taosLRUCacheValue(SLRUCache *cache, LRUHandle *handle) { return ((SLRUEntry *)handle)->value; }

void taosLRUCacheUpdate(SLRUCache *cache, LRUHandle *handle, void *value) { ((SLRUEntry *)handle)->value = value; }

size_t taosLRUCacheGetUsage(SLRUCache *cache) {
  size_t usage = 0;

  for (int i = 0; i < cache->numShards; ++i) {
    usage += taosLRUCacheShardGetUsage(&cache->shards[i]);
  }

  return usage;
}

int32_t taosLRUCacheGetElems(SLRUCache *cache) {
  int32_t elems = 0;

  for (int i = 0; i < cache->numShards; ++i) {
    elems += taosLRUCacheShardGetElems(&cache->shards[i]);
  }

  return elems;
}

size_t taosLRUCacheGetPinnedUsage(SLRUCache *cache) {
  size_t usage = 0;

  for (int i = 0; i < cache->numShards; ++i) {
    usage += taosLRUCacheShardGetPinnedUsage(&cache->shards[i]);
  }

  return usage;
}

void taosLRUCacheSetCapacity(SLRUCache *cache, size_t capacity) {
  uint32_t numShards = cache->numShards;
  size_t   perShard = (capacity + (numShards - 1)) / numShards;

  (void)taosThreadMutexLock(&cache->shardedCache.capacityMutex);

  for (int i = 0; i < numShards; ++i) {
    taosLRUCacheShardSetCapacity(&cache->shards[i], perShard);
  }

  cache->shardedCache.capacity = capacity;

  (void)taosThreadMutexUnlock(&cache->shardedCache.capacityMutex);
}

size_t taosLRUCacheGetCapacity(SLRUCache *cache) {
  size_t capacity = 0;

  (void)taosThreadMutexLock(&cache->shardedCache.capacityMutex);

  capacity = cache->shardedCache.capacity;

  (void)taosThreadMutexUnlock(&cache->shardedCache.capacityMutex);

  return capacity;
}

void taosLRUCacheSetStrictCapacity(SLRUCache *cache, bool strict) {
  uint32_t numShards = cache->numShards;

  (void)taosThreadMutexLock(&cache->shardedCache.capacityMutex);

  for (int i = 0; i < numShards; ++i) {
    taosLRUCacheShardSetStrictCapacity(&cache->shards[i], strict);
  }

  cache->shardedCache.strictCapacity = strict;

  (void)taosThreadMutexUnlock(&cache->shardedCache.capacityMutex);
}

bool taosLRUCacheIsStrictCapacity(SLRUCache *cache) {
  bool strict = false;

  (void)taosThreadMutexLock(&cache->shardedCache.capacityMutex);

  strict = cache->shardedCache.strictCapacity;

  (void)taosThreadMutexUnlock(&cache->shardedCache.capacityMutex);

  return strict;
}
