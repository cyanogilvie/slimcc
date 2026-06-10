// Platform definition for the MIR JIT backend (library mode).
// The host architecture only affects predefined macros and type layout;
// machine code generation is MIR's responsibility. No external assembler,
// linker, or standard include paths are assumed: the embedding application
// supplies include paths and/or virtual files.
#include "slimcc.h"

void platform_init_cc1(void) {
  define_macro("__ELF__", "1");

  define_macro("linux", "1");
  define_macro("__linux", "1");
  define_macro("__linux__", "1");
  define_macro("__gnu_linux__", "1");

#if defined(__x86_64__)
  define_macro("__amd64", "1");
  define_macro("__amd64__", "1");
  define_macro("__x86_64", "1");
  define_macro("__x86_64__", "1");
#elif defined(__aarch64__)
  define_macro("__aarch64__", "1");
  define_macro("__ARM_64BIT_STATE", "1");
  define_macro("__ARM_ARCH", "8");
  define_macro("__ARM_ARCH_ISA_A64", "1");
#elif defined(__riscv) && __riscv_xlen == 64
  define_macro("__riscv", "1");
  define_macro("__riscv_xlen", "64");
#else
#error "unsupported host architecture for the MIR backend"
#endif

  define_macro("__slimcc_mir__", "1");

  init_ty_lp64();

#if defined(__aarch64__)
  // Plain char is unsigned in the AAPCS64 ABI.
  ty_pchar->is_unsigned = true;
  define_macro("__CHAR_UNSIGNED__", "1");
#endif
}

void platform_init_driver(void) {
  dumpmachine_str = "slimcc-mir";
}

void platform_stdinc_paths(StringArray *paths) {
  // Intentionally empty: the embedding application provides include
  // paths (slimcc_options.include_paths) and virtual files.
  (void)paths;
}

void platform_search_dirs(StringArray *paths) {
  (void)paths;
  error("linking is not supported in MIR JIT mode");
}

void run_assembler(StringArray *as_args, const char *input, const char *output) {
  (void)as_args, (void)input, (void)output;
  error("assembling is not supported in MIR JIT mode");
}

void run_linker(StringArray *paths, StringArray *args, const char *output) {
  (void)paths, (void)args, (void)output;
  error("linking is not supported in MIR JIT mode");
}
