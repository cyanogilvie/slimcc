// Runtime helpers for code compiled by the MIR backend. MIR has no atomic
// instructions, so the backend lowers atomic operations to calls into
// these functions, which the host compiler implements with real atomics.
// Register them with a MIR context via slimcc_register_helpers().
#include <stdbool.h>
#include <stdint.h>

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
