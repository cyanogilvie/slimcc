// Runtime helpers for code compiled by the MIR backend. MIR has no atomic
// instructions, so the backend lowers atomic operations to calls into
// these functions, which the host compiler implements with real atomics.
// Register them with a MIR context via slimcc_register_helpers().
#include <stdbool.h>
#include <stdint.h>
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
