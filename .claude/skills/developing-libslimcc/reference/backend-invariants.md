# Backend invariants and gotchas

## Contents
- Parser/backend contract facts
- _BitInt design
- wchar_t mechanism
- Over-aligned globals
- Embedded headers / zero-filesystem VFS
- aarch64 specifics
- MIR interaction facts
- Memory behavior
- Operational gotchas

## Parser/backend contract facts

- **`ND_VA_ARG` yields the argument's ADDRESS** — the parser wraps it in `ND_DEREF`. Do not "normalize" it to a value. This happens to match `MIR_VA_ARG` semantics exactly.
- `parse()` calls `emit_text` for **every** function definition, including unused static inlines (notably the injected `__slimcc_bitint_*` helpers). There is no liveness pass in this backend; every module carries the compiled bitint helpers.
- `prepare_funcall` (parse.c) writes x86-64 ABI fields that parse.c never reads back — the MIR backend no-ops it; MIR's BLK/RBLK machinery does per-target aggregate ABI itself. parse.c:~5196 allocates `rtn_buf` for calls per `bitint_rtn_need_copy` (>64-bit here) — the backend passes it as the RBLK destination.
- slimcc emits per-function during parse (isomorphic to `MIR_new_func`…`MIR_finish_func`); there is no whole-program pass.
- Atomics arrive pre-lowered: parse turns atomic arithmetic into CAS loops; the backend only sees `ND_CAS`/`ND_EXCH`/`ND_THREAD_FENCE`, lowered to calls into `slimcc-mir-helpers.c` (host-compiled with real `__atomic_*`).
- `defer`, constexpr, `_Generic`, `_Countof` are all parser-side — the backend inherits them for free.

## _BitInt design

- **>64 bits**: value class = address of a chunk buffer. Buffers are function-entry **prepended ALLOCAs** (loop-safe, placed before any VLA `BSTART`). All operations call the source-injected `__slimcc_bitint_*` helpers (from `slimcc_headers/include/bitint_builtins`), compiled module-local into every module; referenced via name forwards + per-name protos, cached per module in `bitint_items`/`bitint_protos`.
- Helper convention: `lh`/`src` are read-only; result lands in `rh`/`dst`; **div clobbers both operands**; `to_bool`/`first_set` canonicalize operands in place → copy before calling.
- **≤64 bits**: stays in registers with a canonical invariant (sign/zero-extended from `bit_cnt`) maintained at every producer: `load_scalar`, binop results via `bitint_norm`, call returns, casts.
- **_BitInt literals of ANY width live in `num.bitint_data`, NOT `num.val`** — reading `num.val` was a real bug (enum constants from _BitInt expressions evaluated to 0).

## wchar_t mechanism

`init_ty_lp64()` (type.c:60-67) defines `__WCHAR_TYPE__` as "int" and sets `ty_wchar_t = ty_int`; `platform/mir.c`'s aarch64 block then **overrides** both (`define_macro` silently replaces): `__WCHAR_TYPE__` = "unsigned int", `ty_wchar_t = ty_uint` (AAPCS64). The builtin `slimcc_headers/include/stddef.h` declares `typedef __WCHAR_TYPE__ wchar_t;` with an int fallback. This matters because **musl's bits/alltypes.h redeclares wchar_t per-arch** (glibc trusts the compiler's stddef.h) — a mismatch is a hard "incompatible redeclaration" error that fails every TU including stdio.h users.

## Over-aligned globals

Zero-initialized over-aligned globals: bss over-allocation + address rounding in `gen_addr` (MIR data items only guarantee 16-byte alignment). *Initialized* over-aligned data and `&overaligned_sym` inside static initializers are **compile errors** by design.

## Embedded headers / zero-filesystem VFS

`slimcc_headers/include/*` are embedded via the generated `libslimcc-headers.inc` and registered as `"<slimcc>/..."` vfiles, searched first; `get_realpath` passes vfile names through. Result: `slimcc_compile` works with zero filesystem access (verified by `libslimcc-smoke.c`). User vfiles come through `slimcc_options.vfiles`; contents must outlive all compilations.

## aarch64 specifics

- The injected `__builtin_va_list` text (preprocess.c, `prepare_parse`) is arch-conditional: AAPCS64 32-byte struct (`__stack`/`__gr_top`/`__vr_top`/`__gr_offs`/`__vr_offs`) under `#ifdef __aarch64__`, x86-64 SysV layout otherwise. Wrong layout = silent vararg corruption.
- Plain `char` is unsigned (`platform/mir.c` sets it + `__CHAR_UNSIGNED__`); gcc agrees.
- long double = binary128 via `MIR_T_LD` soft-fp; x86-64 = x87 80-bit. Matches host gcc ABI on both.
- HFA structs (e.g. `struct{float,float}`) currently pass in GPRs via plain BLK — consistent MIR↔MIR but wrong for host-ABI by-value interop. Known deferred gap.

## MIR interaction facts

- A named MIR data item plus its following anonymous items merge into **one malloc per named item** — each named object gets its own 16-aligned allocation (relied on by the over-aligned-global rounding).
- Computed goto emits `MIR_LADDR`/`MIR_JMPI` + `lref_data` static tables — this is why the fork's #424 (jump_opt lref UAF) and #430 (LADDR out-flag) fixes were mandatory. The GVN #423 fix is belt-and-braces here: slimcc is shielded by construction (explicit ext32 after every narrowing load), but direct MIR builders (tclmir/cmark) are exposed.
- MIR rejects variadic functions with zero named args — source of the `function2`/`xxxof_vmtype` by-design failures.

## Memory behavior

**Zero-leak under compile/release churn** (as of 2026-06): valgrind reports 0 definitely/indirectly/possibly lost on success, failure, and mixed paths, and reachable-at-exit is byte-identical across iteration counts; RSS is flat over 10k compiles. Four churn leaks were fixed to get there — re-check these invariants if touching any of them:
- `File` structs (`new_file`) are tracked in tokenize.c's `file_pool`, freed in `tokenize_reset` (mirrors `file_contents`).
- `FuncObj` comes from `cc1_arena`, not malloc (emit_text).
- `parse_free_scopes()` must run on the error path BEFORE `arenas_off()` — nested Scope structs live in the still-on AST arena, and their `vars`/`tags` bucket arrays are heap (leave_scope only frees them when `fnctx` is set; an error unwind skips it).
- The vfile registry is cleared in `tokenize_reset` (entries' names are strdup'd); `slimcc_compile` rebuilds it every call, so distinct TU names would otherwise accumulate forever.

The *mixed* harness (persistent consumer context) grows ~640 kB per retained module — live module data kept by design, dominated by the compiled bitint helpers every module carries; freed by `MIR_finish` on the consumer context. NOT a leak. MIR has no per-module unload — consumers wanting compile/release churn should batch modules into expendable contexts.

## Operational gotchas

- GNUmakefile `%: force` doesn't match `.a` targets: `make -f Makefile libslimcc.a`.
- `mir-run.c` needs `stdbool.h` (gcc 13 defaults to C17).
- Test baselines are in SKILL.md; treat them as ground truth — every entry was investigated to root cause once already.
