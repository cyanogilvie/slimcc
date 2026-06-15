---
name: developing-libslimcc
description: Architecture, build/test commands, suite baselines, and hard-won invariants for the slimcc MIR JIT backend (libslimcc, branch mir-backend). Use when working on codegen-mir.c, libslimcc, platform/mir.c, the precompiled-preamble (pch) header cache, the meson build, the embedded headers, or when running/diagnosing the JIT test suite.
---

# Developing libslimcc (slimcc → MIR JIT backend)

This fork of fuhsnn/slimcc (branch `mir-backend`, remote `fork` = cyanogilvie/slimcc) adds an embeddable JIT library that compiles C23+defer source into MIR modules, consumed by jitc/tclmir to replace libtcc/libjit. Production targets: aarch64+musl primary, x86_64+glibc secondary.

Companion docs — read before re-deriving anything:
- **Plan + rationale**: `~/.claude/plans/i-m-exploring-adding-support-frolicking-waterfall.md`
- **Upstream MIR triage + validation log**: `notes/mir-backend/upstream-triage.md`
- **MIR internals**: `/home/cyan/git/mir/INTERNALS.md` and that repo's `maintaining-mir-fork` skill
- **Invariants and gotchas**: [reference/backend-invariants.md](reference/backend-invariants.md) — read it before touching codegen-mir.c

## Architecture

The parser→backend boundary is exactly 7 symbols declared in slimcc.h (~line 1062): `codegen`, `emit_text`, `prepare_funcall`, `prepare_inline_asm`, `align_to`, `va_arg_need_copy`, `bitint_rtn_need_copy`. The MIR backend provides these from new files; `codegen.c`, `parse.c`, `main.c`, `type.c` stay byte-identical to upstream so the patch set rebases cleanly and interface changes break loudly at link time.

| File | Role |
|---|---|
| `codegen-mir.c` / `codegen-mir.h` | The backend: AST → MIR via the direct C API |
| `libslimcc.c` / `libslimcc.h` | Public API: `slimcc_compile()` + `slimcc_register_helpers()` + `slimcc_pch_*` (header cache) + `slimcc_debug_obj()` (GDB JIT-interface symbol ELF); error longjmp, global resets, vfile registry, embedded headers, dep/mtime recorder |
| `preprocess.c` (additive only) | `pp_snapshot`/`pp_install`/`pp_free_state` — the pch snapshot engine (lives here because `Macro` is private to this TU) |
| `slimcc-mir-helpers.c` | Host-compiled runtime helpers registered via `MIR_load_external` |
| `platform/mir.c` | Host-arch predefined macros, type policies (unsigned plain char + unsigned wchar_t on aarch64), stub assembler/linker hooks |
| `mir-run.c` → `slimcc-mir-run` | Test driver: compile + link + run a .c file under the JIT |
| `scripts/gen_embedded_headers.py` | Generates `libslimcc-headers.inc` (replaces the Makefile sed rule under meson) |

Compile flow: scratch `MIR_context_t` + `MIR_set_error_func`→longjmp; on success `MIR_change_module_ctx` migrates the module into the caller's context; on failure the scratch context is destroyed, caller context untouched. NOT thread-safe — callers serialize `slimcc_compile` (jitc already holds a mutex).

## Build & test

**Makefile gotcha**: the GNUmakefile's `%: force` pattern doesn't cover `.a` targets — use `make -f Makefile libslimcc.a`, or `make slimcc-mir-run` (needs `MIR_DIR` pointing at a built mir checkout, default `../mir`).

```sh
make test                  # stock compiler suite — MUST stay green after any change
make slimcc-mir-run        # JIT test driver

# Run one test under the JIT (x86_64 host shown; adjust the gnu include dir per arch):
./slimcc-mir-run -Islimcc_headers/platform_fix/linux_glibc -I/usr/local/include \
  -I/usr/include/x86_64-linux-gnu -I/usr/include -Itest test/arith.c test/host/common.c
```

Suite sweep: loop that command over `test/*.c`; pass = exit 0.

**Meson** (subproject-wrap consumption; library only — the CLI compiler stays Makefile-built):

```sh
meson setup build && meson compile -C build && meson test -C build
```

`dependency('mir')` falls back to `subprojects/mir.wrap` → cyanogilvie/mir branch `meson`. The lib is compiled with `-DSLIMCC_HEADERS_INC=<builddir .inc>` so a stale Makefile-generated in-tree `.inc` can't shadow it. Nested wrap lookup works: a superproject needs only `slimcc.wrap`; mir resolves through ours. Standalone (non-subproject) builds also get the `slimcc-mir-run` and `libslimcc-smoke` tests.

## Suite baselines — do not re-investigate these

- **x86_64 glibc: 99/103**. The 4 fails are by-design rejections: `asm`, `inline_asm`, `attr_weak`, `builtin_return_address`. (`tls`/`tls2` pass via the emutls lowering; `function2`/`xxxof_vmtype` — paramless C23 variadics — pass since the mir fork's variadic fixes (upstream PRs #438/#439); an unpatched mir turns those two back into clean compile errors.)
- **aarch64, glibc AND musl, identical lists: 93/103** (confirmed 2026-06-12 on both EC2 boxes, mir fork meson ≥ 8667bd8b) = the 4 above + 6 x86-test-assumption failures that gcc-on-aarch64 fails identically: `cast.c`, `literal.c`, `function.c`, `bitfield2.c` (plain char is unsigned per AAPCS64), `float2.c` (x87 layout memcmp), `unicode.c` (asserts `L'\xffffffff'>>31 == -1` but aarch64 wchar_t is unsigned). `function2.c` needs the mir fork's by-ref block-arg copy fix (upstream PR #440): machinize_call emitted >128-byte struct copies (a mir.blk_mov CALL) after earlier args were already in caller-saved hard regs. History + repro: `notes/mir-backend/aarch64-va-bigstruct-repro.c`.

A regression is a *change* against these lists, not membership in them.

## Patch-set discipline

Keep diffs out of `codegen.c`/`parse.c`/`main.c`/`type.c`. Embedding hooks in `tokenize.c`/`preprocess.c`/`parse.c` resets are small and append-biased (~200 changed lines total in churning files). Every backend switch over node kinds ends in `default: error_tok(...)` so upstream AST additions fail loudly. After any change, `make test` (stock binary) must stay green — it proves the patch set doesn't disturb the normal compiler.

## Precompiled preamble (pch) header cache

`slimcc_pch_create(preamble, opt)` snapshots the preprocessor state reached after a fixed preamble so repeated compiles skip re-tokenizing the header closure (for jitc: tcl.h via tclstuff.h, ~12k lines / ~37 files — ~4.7× faster on a header-heavy body, drops jitc's tiny-cdef floor ~4.8ms→~2.0ms). `slimcc_compile` with `opt->pch` set installs the snapshot, tokenizes only the body, splices `preamble_tokens ++ body`. The snapshot lives in a pch-owned `Arena` (never `arena_off`'d until `slimcc_pch_free`). Engine in `preprocess.c` (`pp_snapshot`/`pp_install`); cache key + dep/mtime recorder in `libslimcc.c`.

A pch is **purely an optimization, never a correctness dependency**: it records every real file opened (path+mtime+size via the `add_dep_file` hook), and `slimcc_compile` revalidates (option-key + re-stat) on each use, silently falling back to compiling `preamble + "\n" + body` inline when stale/mismatched. jitc caches one pch per `(preamble, defines, includes)` and rebuilds on staleness.

Invariants the snapshot must preserve (each cost a test-suite failure to find — don't re-break them):

- **Checkpoint is post-`preprocess()`, pre-`prepare_parse()`.** Main-stream tokens are still PP-form there (`TK_PP_NUM`, idents not yet keywords, `ty==NULL`), so the token graph needs no `Type` copy — *except* see the string-literal point below. `prepare_parse`→`preprocess3` (keyword conversion, `join_adjacent_string_literals`) must run per-compile on the spliced stream, so it can't be pre-baked.
- **Instantiate via `copy_token`, mark `is_root`.** Per-compile preamble tokens must be ordinary `tok_alloc`'d tokens (not `arena_malloc`'d) so every freeing path handles them: `parse`'s `free_parsed_tok`, `preprocess3`'s attribute/pragma `to_freelist`, and `prepare_parse`'s pre-`preprocess3` sweep (which keeps `is_root` tokens). An arena token reaching `tok_free` is a bad-free.
- **Macro bodies/params keep their terminating `TK_EOF`; the main chain drops it.** `subst` iterates a macro body `while (kind != TK_EOF)`; a dropped EOF walks off the end (SIGSEGV). The spliced main chain instead drops EOF so the body's EOF terminates the TU.
- **String-literal tokens in macro bodies carry dangling `ty`/`str`.** Macro bodies are tokenized at `#define` time, so their `TK_STR` tokens already have `ty = array_of(...)` and `str = <decoded bytes>` pointing into per-compile storage freed after `pch_create`. The snapshot must deep-copy both into the pch arena (shallow `Type` copy — base is a global element type). Numbers are still PP_NUM (re-decoded from `loc`); char constants' `ty` is a global singleton.
- **Per-compile `Macro` copies, shared read-only bodies.** `pp_install` copies each `Macro` struct into `pp_arena` (resetting `is_locked`/`locked_next`/`stop_tok`) but shares `body`/`params` from the pch arena — expansion reads them, never mutates. The macro/guard tables are rebuilt per-compile (the body `#define`s/`#include`s into them).
- **The fast path skips `init_macros`/`platform_init_cc1`/`lib_macros`** (all baked into the pch via the matching-key guarantee) but must NOT skip the non-macro target setup — except `init_ty_lp64()`'s type globals (`ty_size_t`, `enum_ty`, …) are process-persistent (`type_reset` only clears `void_ptr_cache`) and set when the pch was built, so re-running it would just re-`#define` its macros and churn/leak the table. So: call nothing.

Validation harness pattern: an *oracle* (same program compiled inline vs via pch must produce identical run output), ASAN for the freeing invariants, and an RSS-over-N-compiles loop for leaks (production build = `tok_pooled()`, bulk-freed; ASAN build = `EAGER_FREE`, individual free — both must be clean). The function-like-variadic and multi-line-`-D` cases only surface through the real jitc suite (`capply-6.5`), so run it.

## GDB JIT debug symbols (`slimcc_debug_obj`)

MIR emits no DWARF; `slimcc_debug_obj(syms, n, &buf, &size, &err)` builds an in-memory ELF (ET_REL, host machine) for registration via the GDB JIT interface (`__jit_debug_register_code` — the consumer, e.g. jitc, owns the descriptor/register/unregister). With `opt->debug` set it now gives **full source-level debugging** of a `-g` cdef: named frames, `break funcname`/`break file:line`, `step`/`next` with source, and `print x`/`info locals` for scalar locals + params. The object carries a `.symtab`, `.debug_line`, and `.debug_info` (`.debug_abbrev` too); `slimcc_debug_obj` is a generic ELF section assembler.

Depends on fork additions to MIR (all on `meson`): `MIR_func.code_len` (symbol sizes), per-insn `file_id/line` + `MIR_func.line_map` (`.debug_line`), `MIR_set_inline_permission` + `MIR_set_spill_all` + `MIR_func.reg_locs`/`MIR_reg_frame_offset` (variable locations). The frontend stamps `MIR_set_source_loc` per statement in `gen_stmt` and records each named local (encoding + alloca-address reg) via `slimcc_debug_add_local`; persistent `dbg_files`/`dbg_locals` tables (survive `reset_all`, cleared by `slimcc_debug_reset`) carry them to `slimcc_debug_obj`, which queries `MIR_reg_frame_offset` for each local's frame slot.

**Invariants (each cost the whole feature to find):**
- gdb's JIT reader only materializes symbols inside an **allocatable section** — a symtab-only / `SHN_ABS` object yields nothing breakable. Anchor everything to one `SHT_NOBITS .text` section spanning the address range, symbols section-relative; gdb reads instructions from inferior memory at `sh_addr`.
- **Variable inspection needs spill-all**: MIR's `-O0` keeps locals in *reused hard regs*, not stack, so there's no stable location. `MIR_set_spill_all` homes every local in its own slot; the alloca slot holds the variable's *address*, so the DWARF location is `DW_OP_fbreg(slot) DW_OP_deref` and `DW_AT_frame_base` is the host FP register.
- **Disable inlining for debug** (`MIR_set_inline_permission(ctx,0)`): MIR inlines small callees at link time regardless of opt level, scrambling stepping.
- Local **names aren't on the `Obj`** (the scope name-map is freed during parse) — `push_var_name2` retains the name on the Obj in `opt_g` mode.
- Per-function: reset `MIR_set_source_loc(ctx,0,0)` at each function's gen start (and before simplify's arg-ext insns) or one function's lines bleed into another's via stale `ctx->curr_source`.

**Type coverage** is a full interned graph: base types, typed pointers, struct/union (member DIEs at byte/bit offsets), arrays, enums (enumerator names — enum *variables* keep the list even though slimcc resolves them to the underlying int kind), and function-pointer subroutine types. `slimcc_debug_add_local` takes the `Type*` and interns it *immediately* (the arena recycles per compile) into a persistent self-contained graph (`dbg_types`/`dbg_members`); the emitter walks that, never slimcc internals. Recursive/forward type refs (`struct node { struct node *next; }`) are handled by laying out all type DIEs first then backpatching ref4 fields. `print s`/`print *p`/`p->field`/`print arr` all work.

Validate with `readelf --debug-dump=info,decodedline` on the emitted object and a real gdb session (`break`, `next`, `info scope FUNC`, `print`, `ptype struct X`). Known imperfection: statement-boundary line attribution is occasionally off by one insn (a value's store can land on the next line).

## Status & next steps

Workstream A (this repo) is feature-complete and validated on both target arches. Next phases: Workstream B = tclmir (Tcl stubs package wrapping MIR, meson TEA, per-interp `MIR_context_t` via AssocData); Workstream C = cmark/xpath libjit→MIR migration + jitc consuming `slimcc_compile`. Deferred: per-target BLK classification for host-ABI struct interop (aarch64 HFA gap — fine MIR↔MIR, wrong for host by-value HFA calls).
