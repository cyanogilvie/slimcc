// Runtime helpers for code compiled by the MIR backend. MIR has no atomic
// instructions, so the backend lowers atomic operations to calls into
// these functions, which the host compiler implements with real atomics.
// Register them with a MIR context via slimcc_register_helpers().
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mir.h"

#define CAS(n, ty)                                                          \
  bool __slimcc_jit_cas_##n(void *ptr, void *expected, ty desired) {        \
    return __atomic_compare_exchange_n((ty *)ptr, (ty *)expected, desired,  \
                                       false, __ATOMIC_SEQ_CST,             \
                                       __ATOMIC_SEQ_CST);                   \
  }

#define EXCH(n, ty)                                  \
  ty __slimcc_jit_exch_##n(void *ptr, ty val) {      \
    return __atomic_exchange_n((ty *)ptr, val, __ATOMIC_SEQ_CST); \
  }

CAS(1, uint8_t)
CAS(2, uint16_t)
CAS(4, uint32_t)
CAS(8, uint64_t)

EXCH(1, uint8_t)
EXCH(2, uint16_t)
EXCH(4, uint32_t)
EXCH(8, uint64_t)

void __slimcc_jit_fence(void) {
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// Emulated TLS. Native _Thread_local needs a dynamic-linker-assigned TLS
// module (or static TLS offsets fixed at program start), neither of which
// a JIT-loaded MIR module can get, so the backend lowers every TLS access
// to __slimcc_emutls_get_address(&__emutls_v.<name>) - the same scheme as
// gcc's -femulated-tls, with the runtime provided here instead of libgcc.
// The control object layout matches what codegen-mir.c emits:
// {size, align, index (0 until assigned here), init template}.
//
// Only the four thread primitives are platform-specific; Win32 uses fiber
// local storage because, unlike TlsAlloc, FlsAlloc takes a destructor
// callback that runs at thread exit (pthread_key_create parity).
typedef struct {
  uint64_t size;
  uint64_t align;
  uint64_t index;
  void *templ;
} SlimccEmutlsObj;

typedef struct {
  uint64_t cap;
  void *slot[];
} SlimccEmutlsArray;

#ifdef _WIN32

#include <windows.h>
static SRWLOCK emutls_mtx = SRWLOCK_INIT;
static DWORD emutls_key;
static void emutls_lock(void) { AcquireSRWLockExclusive(&emutls_mtx); }
static void emutls_unlock(void) { ReleaseSRWLockExclusive(&emutls_mtx); }
static void WINAPI emutls_thread_exit(void *p);
static void emutls_key_create(void) { emutls_key = FlsAlloc(emutls_thread_exit); }
static void *emutls_array_get(void) { return FlsGetValue(emutls_key); }
static void emutls_array_set(void *p) { FlsSetValue(emutls_key, p); }
static void *emutls_alloc(uint64_t align, uint64_t size) {
  return _aligned_malloc(size, align < sizeof(void *) ? sizeof(void *) : align);
}
static void emutls_free(void *p) { _aligned_free(p); }

#else

#include <pthread.h>
static pthread_mutex_t emutls_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t emutls_key;
static void emutls_lock(void) { pthread_mutex_lock(&emutls_mtx); }
static void emutls_unlock(void) { pthread_mutex_unlock(&emutls_mtx); }
static void emutls_thread_exit(void *p);
static void emutls_key_create(void) { pthread_key_create(&emutls_key, emutls_thread_exit); }
static void *emutls_array_get(void) { return pthread_getspecific(emutls_key); }
static void emutls_array_set(void *p) { pthread_setspecific(emutls_key, p); }
static void *emutls_alloc(uint64_t align, uint64_t size) {
  void *p = NULL;
  if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size))
    return NULL;
  return p;
}
static void emutls_free(void *p) { free(p); }

#endif

static uint64_t emutls_next_index;
static bool emutls_key_ready;

#ifdef _WIN32
static void WINAPI emutls_thread_exit(void *p)
#else
static void emutls_thread_exit(void *p)
#endif
{
  SlimccEmutlsArray *arr = p;
  if (!arr)
    return;
  for (uint64_t i = 0; i < arr->cap; i++)
    if (arr->slot[i])
      emutls_free(arr->slot[i]);
  free(arr);
}

void *__slimcc_emutls_get_address(void *ctrl) {
  SlimccEmutlsObj *obj = ctrl;

  // Index assignment doubles as key creation: every nonzero index was
  // release-stored after the key existed, so the acquire load here makes
  // the key visible to any thread that skips the lock.
  uint64_t idx = __atomic_load_n(&obj->index, __ATOMIC_ACQUIRE);
  if (!idx) {
    emutls_lock();
    if (!emutls_key_ready) {
      emutls_key_create();
      emutls_key_ready = true;
    }
    idx = obj->index;
    if (!idx) {
      idx = ++emutls_next_index;
      __atomic_store_n(&obj->index, idx, __ATOMIC_RELEASE);
    }
    emutls_unlock();
  }

  SlimccEmutlsArray *arr = emutls_array_get();
  if (!arr || arr->cap < idx) {
    uint64_t cap = arr ? arr->cap * 2 : 16;
    while (cap < idx)
      cap *= 2;
    SlimccEmutlsArray *na = calloc(1, sizeof(*na) + cap * sizeof(void *));
    na->cap = cap;
    if (arr) {
      memcpy(na->slot, arr->slot, arr->cap * sizeof(void *));
      free(arr);
    }
    emutls_array_set(na);
    arr = na;
  }

  void *p = arr->slot[idx - 1];
  if (!p) {
    p = emutls_alloc(obj->align, obj->size);
    if (obj->templ)
      memcpy(p, obj->templ, obj->size);
    else
      memset(p, 0, obj->size);
    arr->slot[idx - 1] = p;
  }
  return p;
}

// The >64-bit _BitInt helpers. The same source the CLI compiler injects
// into each translation unit is host-compiled here instead, and JIT
// modules call it through imports - so modules don't each carry a
// compiled copy, at the cost of requiring slimcc_register_helpers() on
// the consuming context.
#include "slimcc_headers/include/bitint_builtins"

void slimcc_register_bitint_helpers(MIR_context_t ctx) {
  static const struct {
    const char *name;
    void *fn;
  } fns[] = {
    {"__slimcc_bitint_neg", (void *)__slimcc_bitint_neg},
    {"__slimcc_bitint_bitnot", (void *)__slimcc_bitint_bitnot},
    {"__slimcc_bitint_bitand", (void *)__slimcc_bitint_bitand},
    {"__slimcc_bitint_bitor", (void *)__slimcc_bitint_bitor},
    {"__slimcc_bitint_bitxor", (void *)__slimcc_bitint_bitxor},
    {"__slimcc_bitint_add", (void *)__slimcc_bitint_add},
    {"__slimcc_bitint_sub", (void *)__slimcc_bitint_sub},
    {"__slimcc_bitint_mul", (void *)__slimcc_bitint_mul},
    {"__slimcc_bitint_div", (void *)__slimcc_bitint_div},
    {"__slimcc_bitint_shl", (void *)__slimcc_bitint_shl},
    {"__slimcc_bitint_shr", (void *)__slimcc_bitint_shr},
    {"__slimcc_bitint_cmp", (void *)__slimcc_bitint_cmp},
    {"__slimcc_bitint_to_bool", (void *)__slimcc_bitint_to_bool},
    {"__slimcc_bitint_sign_ext", (void *)__slimcc_bitint_sign_ext},
    {"__slimcc_bitint_overflow", (void *)__slimcc_bitint_overflow},
    {"__slimcc_bitint_first_set", (void *)__slimcc_bitint_first_set},
    {"__slimcc_bitint_bitfield_load", (void *)__slimcc_bitint_bitfield_load},
    {"__slimcc_bitint_bitfield_save", (void *)__slimcc_bitint_bitfield_save},
  };
  for (size_t i = 0; i < sizeof(fns) / sizeof(*fns); i++)
    MIR_load_external(ctx, fns[i].name, fns[i].fn);
}
