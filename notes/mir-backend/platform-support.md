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

- **aarch64, HFAs in variadic positions** (all-float/double/long double
  structs of 1-4 members, passed to a `...` parameter). AAPCS64 passes
  them in FP registers like named ones; libslimcc passes them as MIR BLKs
  (GPRs). MIR's `va_block_arg` can't fetch an HFA from the FP save area,
  so splitting them at call sites would break JIT-to-JIT varargs. Fix:
  see "Fixing variadic HFAs" below.
- **x86_64, packed structs with misaligned members** (e.g. `struct
  __attribute__((packed)) { char c; int i; }`). The psABI classifies any
  aggregate with an unaligned field as MEMORY: gcc and clang pass it on
  the stack and return it through the hidden pointer (verified
  2026-10-07). `agg_class` classifies by member type only, so these travel
  in registers. Fix: in `agg_class`, treat a non-bitfield member whose
  absolute offset isn't a multiple of its type's natural alignment as
  `AC_MEM` (a few lines), and add packed shapes to `test/mir-abi/gen.py`.
  Packed but naturally aligned structs (`{int; int}`) stay in registers,
  as with gcc. AAPCS64 has no such rule, so aarch64 needs no change.
- **Empty structs as arguments**: travel as size-0 MIR BLKs. That's
  consistent JIT-to-JIT, but not checked against gcc (which ignores them
  on both targets). Empty struct *returns* return nothing, matching gcc.

### Fixing variadic HFAs (aarch64 Linux)

Two ways:

- **In libslimcc only (no MIR change).** In `ND_VA_ARG` for an HFA, emit
  GCC's algorithm inline, like `va_arg_align` does:
  - `offs = __vr_offs`; if `offs >= 0`, take it from the stack.
  - `nr = offs + 16*n`; `__vr_offs = nr`; if `nr > 0`, take it from the
    stack.
  - Otherwise member *i* is the base type at `__vr_top + offs + 16*i`
    (each FP register occupies a 16-byte slot).
  - Stack: round `__stack` up to the HFA's alignment (8, or 16 for long
    double), copy `size` bytes, advance by `size` rounded up to 8.

  Then drop the `variadic` exemption in `hfa_split_arg`. On Linux,
  variadic FP args use the same registers as named ones, and MIR's
  aarch64 `va_start` already saves v0-v7 and fills `__vr_top`/`__vr_offs`.
  It relies on the mir fork's `fix-gvn-va-mem-clobber` (already pinned),
  and remove the `hfa` va skip in `gen.py`. Roughly 60 lines.
- **In MIR, so c2mir benefits too (upstream-able).** MIR has no notion of
  an HFA: BLK carries only a size. It would need an HFA encoding, e.g. in
  the free `ncase` operand of `va_block_arg` for the callee side, and an
  aarch64 BLK sub-type (base type x count) for args. Then:
  - `mir-gen-aarch64.c` `machinize_call` and incoming-arg handling would
    load/spill members to/from v registers or natural-layout stack.
  - `mir-aarch64.c`'s ff_call thunk would need the same for the
    interpreter.
  - `va_block_arg_builtin` would implement the algorithm above.
  - c2mir's `caarch64-ABI-code.c` would classify HFAs for args *and*
    returns. It currently returns HFAs in x0/x1, which is the same bug
    class jitc 0.8.0-0.8.4 had for small structs.

  Several hundred lines across four files.

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
    (aggregates <= 16 bytes by value, larger by reference), never in
    registers. The even-register padding in `agg_arg_blk_type` and HFA
    splitting must not apply to variadic positions there. MIR handles
    scalar variadic placement itself.
  - Named stack arguments are packed to their natural alignment, not
    8-byte slots: a `char` takes 1 byte. `hfa_split_arg`'s stack spill
    (8-byte slots, float pairs packed into doubles) assumes Linux and
    would need an Apple variant. Check how MIR's machinize lays out small
    scalar stack args on Apple.
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
