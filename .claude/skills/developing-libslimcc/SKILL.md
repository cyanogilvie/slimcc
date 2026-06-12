---
name: developing-libslimcc
description: Architecture, build/test commands, suite baselines, and hard-won invariants for the slimcc MIR JIT backend (libslimcc, branch mir-backend). Use when working on codegen-mir.c, libslimcc, platform/mir.c, the meson build, the embedded headers, or when running/diagnosing the JIT test suite.
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
| `libslimcc.c` / `libslimcc.h` | Public API: `slimcc_compile()` + `slimcc_register_helpers()`; error longjmp, global resets, vfile registry, embedded headers |
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

## Status & next steps

Workstream A (this repo) is feature-complete and validated on both target arches. Next phases: Workstream B = tclmir (Tcl stubs package wrapping MIR, meson TEA, per-interp `MIR_context_t` via AssocData); Workstream C = cmark/xpath libjit→MIR migration + jitc consuming `slimcc_compile`. Deferred: per-target BLK classification for host-ABI struct interop (aarch64 HFA gap — fine MIR↔MIR, wrong for host by-value HFA calls).
