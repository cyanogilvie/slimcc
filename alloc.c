#include "slimcc.h"

#define FREE_THRESHOLD (100 * 1024)
#define ARENA_POOL_SIZE 8160

// buf is ARENA_POOL_SIZE bytes, except for an oversized pool (see
// allocate_oversized), which holds a single allocation larger than that.
struct Pool {
  Pool *next;
  bool oversized;
  _Alignas(16) char buf[];
};

Arena ast_arena;
Arena cc1_arena;
Arena pp_arena;
bool free_alloc = EAGER_FREE;

static Pool *pool_freelist;

bool check_mem_usage(void) {
  struct rusage stat;
  getrusage(RUSAGE_SELF, &stat);
  return stat.ru_maxrss > FREE_THRESHOLD;
}

// Free the recycled-pool freelist. arena_off (when not eagerly freeing) keeps
// emptied pools here for reuse across compilations rather than returning them
// to the C allocator; this releases them for good. Intended for a final
// library-mode shutdown — after it, the freelist simply refills on demand.
void arena_free_pools(void) {
  for (Pool *p = pool_freelist; p;) {
    Pool *tmp = p;
    p = p->next;
    free(tmp);
  }
  pool_freelist = NULL;
}

static Pool *new_pool(void) {
  Pool *p;
  if (pool_freelist) {
    p = pool_freelist;
    pool_freelist = p->next;
  } else {
    p = malloc(sizeof(Pool) + ARENA_POOL_SIZE);
  }

#if defined(USE_ASAN)
  __asan_poison_memory_region(&p->buf, ARENA_POOL_SIZE);
#endif
  p->next = NULL;
  p->oversized = false;
  return p;
}

// An allocation too big for a pool gets a pool of its own, linked in at the
// head of the arena's chain (leaving cur, the pool being carved, at the tail)
// so that arena_off releases it with the rest.  These are never put on the
// freelist: they are freed by arena_off, and arena_off frees the oversized
// pools first.
static void *allocate_oversized(Arena *arena, size_t aligned_sz) {
  Pool *p = malloc(sizeof(Pool) + aligned_sz);
  p->oversized = true;
  p->next = arena->head;
  arena->head = p;
  return &p->buf;
}

static void *allocate(Arena *arena, size_t sz, bool clear) {
  size_t aligned_sz = (sz + 15) & -16LL;

  void *ptr;
  if ((arena->used + aligned_sz) <= ARENA_POOL_SIZE) {
    ptr = &arena->cur->buf[arena->used];
    arena->used += aligned_sz;
  } else {
    if (aligned_sz > ARENA_POOL_SIZE) {
      ptr = allocate_oversized(arena, aligned_sz);
    } else {
      arena->cur = arena->cur->next = new_pool();
      ptr = &arena->cur->buf;
      arena->used = aligned_sz;
    }
  }

#if defined(USE_ASAN)
  __asan_unpoison_memory_region(ptr, aligned_sz);
#endif

#if defined(USE_TYSAN)
  memset(ptr, 0, aligned_sz);
#else
  if (clear)
    memset(ptr, 0, aligned_sz);
#endif
  return ptr;
}

char *arena_format(Arena *arena, const char *fmt, ...) {
  void *ptr = &arena->cur->buf[arena->used];
  size_t n = ARENA_POOL_SIZE - arena->used;

#if defined(USE_ASAN)
  __asan_unpoison_memory_region(ptr, n);
#endif

  va_list ap;
  va_start(ap, fmt);

  size_t sz = 1 + (size_t)vsnprintf(ptr, n, fmt, ap);
  size_t aligned_sz = (sz + 15) & -16LL;

  if (aligned_sz <= n) {
    va_end(ap);
    arena->used += aligned_sz;
#if defined(USE_ASAN)
    __asan_poison_memory_region(ptr + aligned_sz, ARENA_POOL_SIZE - arena->used);
#endif
    return ptr;
  }
  if (aligned_sz > ARENA_POOL_SIZE) {
    ptr = allocate_oversized(arena, aligned_sz);
  } else {
    arena->cur = arena->cur->next = new_pool();
    ptr = &arena->cur->buf;
    arena->used = aligned_sz;
  }

#if defined(USE_ASAN)
  __asan_unpoison_memory_region(ptr, aligned_sz);
#endif

  va_start(ap, fmt);
  vsnprintf(ptr, aligned_sz, fmt, ap);
  va_end(ap);
  return ptr;
}

char *arena_strdup(Arena *arena, const char *str) {
  size_t len = 1 + strlen(str);
  return memcpy(arena_malloc(arena, len), str, len);
}

char *arena_copy_string(Arena *arena, const char *src, size_t slen) {
  char *p = memcpy(arena_malloc(arena, slen + 1), src, slen);
  p[slen] = '\0';
  return p;
}

void arena_on(Arena *arena) {
  arena->head = arena->cur = new_pool();
  arena->used = 0;
}

void arena_off(Arena *arena) {
  // Oversized pools are all at the head of the chain: free them rather than
  // recycling them as regular pools
  while (arena->head && arena->head->oversized) {
    Pool *tmp = arena->head;
    arena->head = tmp->next;
    free(tmp);
  }

  if (free_alloc) {
    for (Pool *p = arena->head; p;) {
      Pool *tmp = p;
      p = p->next;
      free(tmp);
    }
  } else {
    arena->cur->next = pool_freelist;
    pool_freelist = arena->head;
  }
  arena->cur = NULL;
}

void *arena_malloc(Arena *a, size_t sz) {
  return allocate(a, sz, false);
}

void *arena_calloc(Arena *a, size_t sz) {
  return allocate(a, sz, true);
}
