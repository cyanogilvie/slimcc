# libslimcc (MIR backend) platform support

Status as of 2026-10-07. jitc consumes libslimcc, so this is also jitc's
platform story; jitc's own OS dependencies are listed at the end.

## Summary

| Host | Status |
|---|---|
| Linux x86_64 (glibc, musl) | Supported. Secondary production target. |
| Linux aarch64 (musl, glibc) | Supported. Primary production target. |
| Linux riscv64 | Builds, but **not ABI-correct** (see below). Not validated. |
| macOS x86_64 / arm64 | Not supported: `platform/mir.c` refuses to build (`#error`). |
| Windows x64 / arm64 | Not supported: `platform/mir.c` refuses to build (`#error`). |

"Supported" means the JIT interoperates with host-compiled code per the
platform ABI. That's validated by `scripts/test_mir_abi.sh` (aggregates by
value in both directions, incl. register exhaustion and varargs) and by the
`test/*.c` sweep baselines in the developing-libslimcc skill.

Everything OS-specific lives in three places, which a port touches:

1. `platform/mir.c`: predefined macros (today `__linux__`, `__ELF__`
   unconditionally), the type model (`init_ty_lp64()`), and type policies
   (plain `char` / `wchar_t` signedness, `_BitInt` layout).
2. `preprocess.c` `prepare_parse`: the injected `__builtin_va_list`
   typedef (AAPCS64 struct under `__aarch64__`, x86-64 SysV struct
   otherwise). It must match the va_list MIR's `va_start` writes on that
   host *and* the host's own, so `va_list` can be passed to host
   `vprintf`-style functions.
3. `codegen-mir.c` "Host calling convention for aggregates": per-target
   classification of structs/unions/big `_BitInt`s passed or returned by
   value, plus `va_arg_align`. The `#if` blocks are `__x86_64__ &&
   !_WIN32` (SysV), `__aarch64__` (written for Linux AAPCS64), and a
   fallback that uses MIR's own conventions (correct only between JIT
   functions).

MIR itself supports all the combinations below (x86_64: Linux, macOS,
Windows; aarch64: Linux, macOS; plus riscv64, ppc64, s390x on Linux). It
handles scalar argument passing, varargs register save areas, and JIT
memory (Apple arm64 `MAP_JIT` + `pthread_jit_write_protect_np`, Windows
`VirtualAlloc`). MIR's BLK/RBLK block conventions don't classify aggregates
for the host ABI, which is why codegen-mir.c does.

## Remaining gaps on the supported targets

- **Empty structs as arguments** travel as size-0 MIR BLKs. That's
  consistent JIT-to-JIT, but not checked against gcc (which ignores them
  on both targets). Empty struct *returns* return nothing, matching gcc.

Closed 2026-10-07 (jitc 0.8.6):

- **aarch64 HFAs in variadic positions** (and named ones): MIR now has HFA
  block cases (`MIR_T_BLK + 1..3`, mir fork branch `aarch64-hfa-blk`,
  filed upstream). MIR passes them in FP registers or on the stack in
  memory layout, and `va_block_arg` fetches them from the FP save area or
  the stack, as GCC does. c2mir classifies HFAs too, for args and returns.
- **x86-64 packed structs with misaligned members** are MEMORY class
  (`agg_class`), as gcc and clang have them. On aarch64 these needed the
  fork's `fix-combine-unencodable-addr`: MIR folded misaligned FP accesses
  into unencodable scaled offsets and failed to compile them.

## Linux riscv64

It builds (`platform/mir.c` defines `__riscv`), but it isn't ABI-correct
and has never been run:

- **Aggregates** take the fallback (MIR's own conventions). LP64D returns
  structs of up to 2xXLEN (16 bytes) in a0/a1, or in fa0/fa1 when the
  "hardware floating-point calling convention" flattening applies (one or
  two FP members, or one FP plus one integer member). It passes them in
  GPRs/FPRs the same way, and larger ones by reference. Every small-struct
  return from a host function would read garbage, the same failure as the
  jitc 0.8.4 production crash. c2mir's `criscv64-ABI-code.c` (462 lines)
  implements this classification against MIR's riscv64 BLK conventions
  and is the model to port into codegen-mir.c.
- **`va_list`** is `void *` on riscv64, but libslimcc injects the x86-64
  SysV struct (the `#else` branch). JIT-internal varargs may work, but
  passing a `va_list` to a host `v*printf` would not.
- **Type policies**: plain `char` is unsigned on riscv64 (not set);
  `long double` is 128-bit IEEE quad (check MIR's riscv64 `MIR_T_LD`
  handling); `_BitInt` layout per the riscv psABI (to verify).
- **Variadic rules**: FP varargs travel in GPRs, and 2xXLEN-aligned
  varargs start at an even GPR.

To support it: add the riscv64 classification block (from c2mir), fix the
va_list typedef and type policies, then validate with `test_mir_abi.sh`
and the sweep. QEMU user-mode in a `--platform linux/riscv64` container
works for that, as amd64 does for x86_64.

## macOS

MIR supports both macOS architectures. libslimcc needs:

- **`platform/mir.c`**: a Darwin branch. Define `__APPLE__`, `__MACH__`
  and the Darwin version macros, not `__linux__`/`__ELF__`. macOS SDK
  headers lean on clang extensions (`__has_feature`/`__has_attribute`,
  availability attributes, nullability qualifiers, `__asm`-labelled
  symbol variants such as `$DARWIN_EXTSN`/`$INODE64`), so expect header
  work. Mach-O symbols carry a leading `_` that `dlsym` names omit, so
  asm-labelled names need it stripped before resolution.
- **macOS x86_64**: SysV ABI, so codegen-mir.c's x86-64 block applies
  unchanged (it's gated on `!_WIN32`, not on Linux). The `va_list` layout
  is the same as Linux, and `long double` is x87 80-bit as on Linux.
  Mostly a predefines/headers job, then validate.
- **macOS arm64** (Apple's AAPCS64 variant, "DarwinPCS"); the differences
  from Linux all matter:
  - Plain `char` is **signed**, `wchar_t` is `int`, and `long double` is
    `double`. `platform/mir.c` currently forces unsigned `char` and
    `wchar_t` on all aarch64.
  - `va_list` is `char *`. libslimcc injects the 32-byte Linux struct,
    and `va_arg_align` writes Linux offsets into it. Both must be gated to
    Linux. MIR's Apple `va_arg`/`va_block_arg` builtins already use the
    pointer form.
  - **All variadic arguments go on the stack** in 8-byte slots
    (aggregates <= 16 bytes by value, larger by reference, HFAs by value),
    never in registers. MIR handles that, including its HFA blocks (the
    Apple paths of `aarch64-hfa-blk` are written but untested). The
    even-register padding in `agg_arg_blk_type` must not apply to variadic
    positions there.
  - Named stack arguments are packed to their natural alignment, not
    8-byte slots: a `char` takes 1 byte. MIR's machinize uses 8-byte slots
    on Apple too, which is a MIR gap for small scalar stack args.
  - x18 is reserved (MIR handles it). `_BitInt` layout: verify Apple
    clang against `bitint_chunk128`.
- **Validation needs a Mac** (qemu-user can't run macOS binaries): run
  `scripts/test_mir_abi.sh` with Apple clang as the host compiler, plus
  the sweep.

## Windows

MIR supports Windows x64; Windows arm64 isn't a MIR target. libslimcc
needs, for x64:

- **Type model**: Windows is **LLP64** (`long` is 32-bit). slimcc only
  has `init_ty_lp64()`, so a `init_ty_llp64()` and platform branch is the
  first step. `wchar_t` is 16-bit unsigned; `long double` is `double`
  under the MSVC ABI (mingw-w64 gcc uses x87 80-bit). Pick the toolchain
  the host Tcl was built with, which decides the CRT and the ABI of
  `long double`.
- **`va_list`** is `char *` (the `#else` SysV struct is wrong).
- **Aggregate calling convention (Win64)**, a new block in codegen-mir.c:
  - Arguments of size 1, 2, 4 or 8 pass by value in one GPR (even
    all-float structs). Other sizes pass **by reference** to a
    caller-made copy. MIR's `_WIN32` BLK path passes everything <= 8
    bytes by value, so 3/5/6/7-byte structs need an explicit
    copy-and-pointer from libslimcc.
  - Returns of size 1, 2, 4 or 8 come back in RAX as one result. MIR
    rejects multiple results on Windows, but one is all Win64 needs.
    Other sizes return through the hidden pointer in RCX, which MIR's
    RBLK already does.
  - There are 4 positional register slots shared between GPRs and XMMs,
    plus 32 bytes of shadow space; MIR handles both. Variadic float args
    are duplicated into GPRs, which MIR handles.
- **TLS**: the emutls runtime in `slimcc-mir-helpers.c` already has a
  Win32 (FLS) implementation.
- **Validation**: Windows (or Wine with a mingw toolchain, for a first
  pass).

## jitc's own OS dependencies

Beyond libslimcc, jitc (generic/jitc.c) assumes POSIX:

- Symbols resolve with `dlsym(RTLD_DEFAULT, ...)`, and a cdef's libraries
  are loaded with `dlopen(RTLD_NOW | RTLD_GLOBAL)`, with soname-style
  candidate names. That's fine on macOS (`.dylib` naming, frameworks).
  Windows needs `LoadLibrary`/`GetProcAddress` and a walk over loaded
  modules (`EnumProcessModules`) to emulate `RTLD_DEFAULT`; Tcl's
  `Tcl_LoadFile` could cover the loading half.
- `-g` debug objects are ELF, registered through the GDB JIT interface.
  On macOS, lldb's JIT loader may accept them (untested); on Windows
  there is no equivalent.
- Default include paths (`sys_includes`) are Linux-style; the platform
  SDK paths must be configured.
