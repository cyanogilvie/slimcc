# Backend invariants and gotchas

## Contents
- Parser/backend contract facts
- Register promotion of locals
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
- `parse()` calls `emit_text` for **every** function definition, including unused static inlines. There is no liveness pass in this backend. (The `__slimcc_bitint_*` helpers are no longer injected in lib mode — they're imports into host-compiled code; see _BitInt design.)
- `prepare_funcall` (parse.c) writes x86-64 ABI fields that parse.c never reads back — the MIR backend no-ops it; MIR's BLK/RBLK machinery does per-target aggregate ABI itself. parse.c:~5196 allocates `rtn_buf` for calls per `bitint_rtn_need_copy` (>64-bit here) — the backend passes it as the RBLK destination.
- slimcc emits per-function during parse (isomorphic to `MIR_new_func`…`MIR_finish_func`); there is no whole-program pass.
- Atomics arrive pre-lowered: parse turns atomic arithmetic into CAS loops; the backend only sees `ND_CAS`/`ND_EXCH`/`ND_THREAD_FENCE`, lowered to calls into `slimcc-mir-helpers.c` (host-compiled with real `__atomic_*`).
- `defer`, constexpr, `_Generic`, `_Countof` are all parser-side — the backend inherits them for free.
- **Objects carry an `ObjKind`** (`OBJ_GLOBAL`/`OBJ_TLS`/`OBJ_LOCAL`/`OBJ_INDIR`), not `is_local`/`is_tls`. **A VLA is an `OBJ_INDIR` Obj that is NOT a scope local**: its `vptr` (an ordinary `ptrdiff_t` local in the scope) holds the runtime allocation. `gen_addr` loads through `var->vptr`; the declaration's `ND_ALLOCA` stores into `node->m.var->vptr`; `DF_VLA_DEALLOC`'s `defr->vla` *is* the vptr. `TY_VLA` itself has size/align 0. Consequence for debug info: VLA variables don't appear (the vptr is nameless; they were void-typed before anyway).
- **Zero-sized objects are real**: `struct {}`/`T[0]` have size 0. Locals still get a ≥1-byte slot (`alloca_obj`'s `MAX(size,1)`), `gen_mem_zero`/`gen_mem_copy` early-out on `size <= 0`, and zero-size by-value args/params ride MIR BLK of size 0 fine (named + variadic validated). `return expr;` carries no value when the function returns void **or** the expr has size ≤0 — evaluate for side effects then `ret` with no operand. (`ty_void->size` is 1, so test `rt->kind == TY_VOID`, not size.)
- `prepare_funcall` may be asked (via `scope->has_alloca`) to build `node->call.alloca_args` so x86 can evaluate `alloca()`-containing args before pushing stack args. The MIR backend no-ops it: MIR evaluates every arg into a register before the CALL insn, so there is no interleaving hazard.
- Incomplete type kinds `TY_ENUM_INCMP`/`TY_FUNC_INCMP`/`TY_ARRAY_INCMP` exist; a *complete* enum is just its underlying integer kind with `ty->enums` set (no `TY_ENUM`).

## Register promotion of locals

Scalar locals whose address is never taken live in a MIR register (`var->ptr == mir_regval_marker`, register in `var->ofs`) instead of an ALLOCA slot (`mir_local_marker`) — MIR's generator does not promote alloca slots itself, so without this every local costs a load/store per access. `promote_local` decides per Obj after `scan_node` walks the body marking `Obj.addr_taken`:
- **The scan must mirror `gen_addr`'s recursion, not just look for `&var`.** `scan_lvalue` marks every variable `gen_addr` would take the address of: `ND_ADDR` operands, non-`ND_VAR` lhs of `ND_ASSIGN`/`ND_ARITH_ASSIGN`/`ND_POST_INCDEC`, `ND_MEMBER` bases, and the rhs of `ND_CHAIN`/`ND_COMMA` in those positions (compound literals arrive as `ND_CHAIN(init, ND_VAR tmp)`, so `&(int){5}` / `(int){5}++` have no direct `ND_ADDR(ND_VAR)`). `ND_INIT_SEQ` targets are marked too (zero-filled through their address). Defer chains (cleanup handlers take `&var` implicitly) are scanned via each node's `dfr_from..dfr_dest`.
- **Fail-safe:** `gen_addr` on a promoted local is `internal_error()` — a scan gap becomes a clean compile error, never a miscompile. Any node kind the scan doesn't know sets `no_promote` for the whole function; add new AST kinds to `scan_node` when the backend learns them.
- Disabled per function for `opt_g` (DWARF needs stable frame slots) and `dont_reuse_stk` (a `returns_twice` callee such as setjmp: register values don't survive longjmp). Never promoted: aggregates/VLAs/big `_BitInt` (`is_addr_value`), `volatile`/`_Atomic`, static locals.
- Writes go through `set_regval`, which canonicalizes exactly as a memory load would (narrow ints extended, small `_BitInt` normalized), so reads can return the register verbatim. `x++` snapshots the old value into a fresh register before the write.
- Promoted registers are named after their C variable via `cvar_reg_name` (`C%name`, shadowed names `C%name.1`, …; per-function `reg_names` map) so MIR dumps read against the source; incoming args stay `A<n>` (a promoted param is `C%a` copied from `A0`), unnamed temps `T<n>`. This needs local names retained on the Obj in lib mode (`push_var_name2`), not just under `-g`.
- A VLA's `vptr` is an ordinary promotable local: use `get_local`/`set_local` (register-or-slot) for it, never `local_addr`.
- Validated by jitc's `tests/promotion.test` (IR shape: no `alloca`/memory operands for promoted scalars; runtime: compound-assign, inc/dec, narrow ints, floats, mixed address-taken).

## _BitInt design

- **>64 bits**: value class = address of a chunk buffer. Buffers are function-entry **prepended ALLOCAs** (loop-safe, placed before any VLA `BSTART`). All operations call the `__slimcc_bitint_*` helpers, which in library mode are **host-compiled into libslimcc** (`slimcc-mir-helpers.c` #includes `slimcc_headers/include/bitint_builtins` — single source of truth with the CLI) and reached through imports resolved by `slimcc_register_helpers()`; modules do NOT carry compiled copies. The injection in `prepare_parse` is gated off by `slimcc_lib_mode`; the stock CLI still injects and compiles them per TU. Referenced via name forwards + per-name protos cached per module in `bitint_items`/`bitint_protos`; `codegen()` retypes undefined forwards as imports at module finish.
- Helper convention: `lh`/`src` are read-only; result lands in `rh`/`dst`; **div clobbers both operands**; `to_bool`/`first_set` canonicalize operands in place → copy before calling.
- **≤64 bits**: stays in registers with a canonical invariant (sign/zero-extended from `bit_cnt`) maintained at every producer: `load_scalar`, binop results via `bitint_norm`, call returns, casts.
- **_BitInt literals of ANY width live in `num.bitint_data`, NOT `num.val`** — reading `num.val` was a real bug (enum constants from _BitInt expressions evaluated to 0).

## wchar_t mechanism

`init_ty_lp64()` (type.c:60-67) defines `__WCHAR_TYPE__` as "int" and sets `ty_wchar_t = ty_int`; `platform/mir.c`'s aarch64 block then **overrides** both (`define_macro` silently replaces): `__WCHAR_TYPE__` = "unsigned int", `ty_wchar_t = ty_uint` (AAPCS64). The builtin `slimcc_headers/include/stddef.h` declares `typedef __WCHAR_TYPE__ wchar_t;` with an int fallback. This matters because **musl's bits/alltypes.h redeclares wchar_t per-arch** (glibc trusts the compiler's stddef.h) — a mismatch is a hard "incompatible redeclaration" error that fails every TU including stdio.h users.

## Over-aligned globals

Zero-initialized over-aligned globals: bss over-allocation + address rounding in `gen_addr` (MIR data items only guarantee 16-byte alignment). *Initialized* over-aligned data and `&overaligned_sym` inside static initializers are **compile errors** by design.

## Embedded headers / zero-filesystem VFS

`slimcc_headers/include/*` are embedded via the generated `libslimcc-headers.inc` and registered as `"<slimcc>/..."` vfiles, searched first; `get_realpath` passes vfile names through. Result: `slimcc_compile` works with zero filesystem access (verified by `libslimcc-smoke.c`). User vfiles come through `slimcc_options.vfiles`; contents must outlive all compilations.

Several embedded headers are `#include_next` wrappers (`math.h`, `limits.h` for `BITINT_MAXWIDTH`). `#include_next` resumes the search at the include path *after* `<slimcc>`, so an embedder must never also put the on-disk `slimcc_headers/include` on its include path — the wrapper would find its own duplicate (guard already defined → empty) instead of the system header, silently losing e.g. `INT_MAX`/`sqrt`. `mir-run.c` used to do exactly this until 2026-10-04.

Backend-internal libc calls (`memset`/`memcpy` for block zero/copy) are referenced through the shared symbol table (`libc_item`), not a private `MIR_new_import`: a TU that also declares/calls the same name would otherwise produce a forward plus a clashing import ("already defined as import").

## aarch64 specifics

- The injected `__builtin_va_list` text (preprocess.c, `prepare_parse`) is arch-conditional: AAPCS64 32-byte struct (`__stack`/`__gr_top`/`__vr_top`/`__gr_offs`/`__vr_offs`) under `#ifdef __aarch64__`, x86-64 SysV layout otherwise. Wrong layout = silent vararg corruption.
- Plain `char` is unsigned (`platform/mir.c` sets it + `__CHAR_UNSIGNED__`); gcc agrees.
- long double = binary128 via `MIR_T_LD` soft-fp; x86-64 = x87 80-bit. Matches host gcc ABI on both.
- HFA structs (e.g. `struct{float,float}`) currently pass in GPRs via plain BLK — consistent MIR↔MIR but wrong for host-ABI by-value interop. Known deferred gap.

## MIR interaction facts

- A named MIR data item plus its following anonymous items merge into **one malloc per named item** — each named object gets its own 16-aligned allocation (relied on by the over-aligned-global rounding).
- Computed goto emits `MIR_LADDR`/`MIR_JMPI` + `lref_data` static tables — this is why the fork's #424 (jump_opt lref UAF) and #430 (LADDR out-flag) fixes were mandatory. The GVN #423 fix is belt-and-braces here: slimcc is shielded by construction (explicit ext32 after every narrowing load), but direct MIR builders (tclmir/cmark) are exposed.
- Paramless C23 variadics (`int f(...)` + `va_start(ap)`) work, but only with the mir fork's fixes (upstream PRs #438/#439: MIR's ≥1-named-arg check removed; x86-64 va_start offsets rebuilt from the machinize counters). With an unpatched mir they fail as a clean compile error via the MIR error longjmp, not a crash. The va_start fix matters beyond C23: register-passed struct params weren't counted into gp/fp_offset (plain C11 via c2m misread varargs) and memory-passed params of non-multiple-of-8 size misplaced `overflow_arg_area`.
- aarch64 >128-byte struct args (named or vararg) need the fork's machinize_call fix (upstream PR #440): the by-ref copy is a `mir.blk_mov` CALL that was emitted after earlier args were already in caller-saved hard regs — FP args and x3-x7 ints before such a struct got clobbered (physically and via RA reuse). riscv64/s390x look similarly affected upstream (untested).
- **Two `va_block_arg` insns sharing one dest register SIGSEGV mir-gen's copy_prop** (upstream, both arches, fork included as of 2026-06-12). slimcc is shielded by construction — every struct va_arg site gets its own alloca buffer — but direct MIR builders (tclmir/cmark) must keep dest registers distinct. Also reachable from C via c2m (`va_arg(ap, struct S).c[i]` rvalue form twice); sometimes surfaces as a "Wrong alias number" error instead of the crash.

## Thread-local storage (emutls)

`_Thread_local` is lowered gcc `-femulated-tls` style — native TLS needs a dynamic-linker-assigned TLS module (or static-TLS offsets fixed at program start), neither obtainable for JIT-loaded MIR modules:
- Every address-of access becomes `__slimcc_emutls_get_address(&__emutls_v.<name>)` (gen_addr); the variable's own name never becomes a MIR symbol. The control object `{size, align, index, templ}` and the `__emutls_t.<name>` init image are emitted by `emit_data_obj`; cross-module extern TLS works via export/import of `__emutls_v.<name>` (validated). MIR identifiers accept `.` (mir.c scanner), so textual dumps round-trip.
- The runtime lives in `slimcc-mir-helpers.c`. Platform exposure is four shimmed primitives: pthread_key (destructor frees per-thread copies at thread exit) / mutex / posix_memalign on POSIX; **FlsAlloc** (not TlsAlloc — FLS has destructor callbacks) / SRWLOCK / `_aligned_malloc` on Win32. Index assignment doubles as key creation; the acquire-load of `index` is what makes the key visible to lock-skipping threads.
- Limitations: a host-defined native-TLS `_Thread_local` can't be accessed from JIT code (and vice versa) — same ABI split gcc documents for -femulated-tls; `errno` unaffected (`__errno_location()`). Every access is a call. `&tls_var` in a static initializer is a compile error (no TLS relocations). Over-aligned TLS works (runtime honors the control align word) — the template's alignment is deliberately clamped ≤16 in emission.
- Main thread's copies are reclaimed only at process exit (key destructors don't run for main) — valgrind-clean as reachable.

## Memory behavior

**Zero-leak under compile/release churn** (as of 2026-06): valgrind reports 0 definitely/indirectly/possibly lost on success, failure, and mixed paths, and reachable-at-exit is byte-identical across iteration counts; RSS is flat over 10k compiles. Four churn leaks were fixed to get there — re-check these invariants if touching any of them:
- `File` structs (`new_file`) are tracked in tokenize.c's `file_pool`, freed in `tokenize_reset` (mirrors `file_contents`).
- `FuncObj` comes from `cc1_arena`, not malloc (emit_text).
- `parse_free_scopes()` must run on the error path BEFORE `arenas_off()` — nested Scope structs live in the still-on AST arena, and their `vars`/`tags` bucket arrays are heap (leave_scope only frees them when `fnctx` is set; an error unwind skips it).
- Static-storage initializer images (`Obj.init_data`, malloc'd by `gvar_initializer`/`constexpr_initializer2`/string+numseq init arrays) are tracked by `alloc_init_data` and freed in `parse_reset` — too large in general for an arena pool (`ARENA_POOL_SIZE` cap). Was a per-initialized-global leak until 2026-10-04; the older harnesses only had uninitialized globals.
- No function-local `static` may cache an arena/per-compile pointer across compiles: hoist it to file scope and clear it in the matching `*_reset` (e.g. parse.c's `empty_name`, the `__FUNCTION__`-outside-a-function anonymous global — a dangling Obj from the previous compile otherwise). After each upstream rebase, grep the upstream diff for new `static` locals.
- The vfile registry is cleared in `tokenize_reset` (entries' names are strdup'd); `slimcc_compile` rebuilds it every call, so distinct TU names would otherwise accumulate forever.

The *mixed* harness (persistent consumer context) grows ~35 kB per retained module — live module data kept by design, freed by `MIR_finish` on the consumer context. NOT a leak. (Was ~640 kB before the bitint helpers became host-compiled imports instead of per-module compiled copies.) MIR has no per-module unload — consumers wanting compile/release churn should batch modules into expendable contexts.

## Operational gotchas

- GNUmakefile `%: force` doesn't match `.a` targets: `make -f Makefile libslimcc.a`.
- `mir-run.c` needs `stdbool.h` (gcc 13 defaults to C17).
- Test baselines are in SKILL.md; treat them as ground truth — every entry was investigated to root cause once already.
