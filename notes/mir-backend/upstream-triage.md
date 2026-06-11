# Upstream MIR issue/PR triage for production readiness

Reviewed 2026-06-11: all 130 open issues and 11 open PRs on vnmakarov/mir,
against our profile — in-process JIT via `MIR_gen` (eager interface, O2),
modules built by libslimcc (later tclmir/cmark direct API building),
**aarch64+musl primary**, x86_64+glibc secondary, Windows eventually.
Upstream activity is near-zero since mid-2024; assume fixes land in our fork
(github.com/cyanogilvie/mir) first, PR'd upstream as courtesy.

Verification artifacts: reproducers run on x86_64 (this machine) and aarch64
(c8g EC2, Ubuntu 24.04) on 2026-06-11; test programs preserved in this file's
text and as /tmp scripts that session.

## A. Must fix in fork before shifting production load

1. **#423 — GVN sign-extension miscompile** (O2, x86_64 AND aarch64,
   confirmed open on master). GVN's redundant-load elimination
   (`(mem=x|x=mem); ...; r=mem → t=x; ...; r=t`) replaces a *narrowing,
   sign/zero-extending* reload with the un-extended stored register value.
   c2mir repro: `*p32 = a + b; return (int64_t)*p32;` returns garbage upper
   bits at O2/O3.
   - **slimcc is shielded by construction** (verified both arches): the
     parser materializes integer widenings as explicit casts, so codegen-mir
     emits `ext32`/`uext32` after every narrowing load whose value widens —
     GVN's substitution gets re-canonicalized. `/tmp/issue423.c` returns
     -273 correctly through slimcc-mir-run.
   - Still must fix: **tclmir/cmark will build MIR directly** and may
     legitimately rely on the documented load semantics (typed loads
     extend to 64-bit). A latent gen miscompile is unacceptable in the
     substrate. The reporter sketched the fix: insert an extension insn when
     the eliminated load was narrowing (in gvn, mir-gen.c:~4868-4905 where
     redundant exprs are rescued through a temp).
   - Effort: small-medium. Add a directed .mir regression test.

2. **#424 — jump_opt use-after-free with label references** (lref). ASAN:
   `get_label_disp` (mir-gen.c:768) reads a label insn freed by
   `gen_delete_insn` during BB removal (GVN/jump_opt path). Directly relevant:
   slimcc emits `lref_data` for `&&label` in static initializers (computed
   goto tables — test/control.c style code), and our 95/103 suite passing
   means we haven't hit the trigger shape yet, not that we can't.
   - Fix direction: treat labels referenced from `func->first_lref` as live
     in unreachable-BB/label cleanup (jump_opt and `remove_bb`), or make
     `gen_setup_lrefs` resolve through surviving labels. Note `simplify_func`
     already marks lref labels as used (mir.c:3833-3835) — the gen-side
     cleanup lacks the equivalent.
   - Effort: small once reproduced; the issue contains a full C repro
     (AngelScript bytecode2c output) to distill into a .mir case.

3. **aarch64 `% 16` round-up bugs** (found in our own source review, not
   filed upstream): mir-aarch64.c:94 (`va->__stack` alignment in
   `va_arg_builtin` for stack binary128 long double) and mir-aarch64.c:417
   (sp_offset in ff_call) use `% 16` where round-up-to-16 was intended; :94
   produces a garbage pointer if hit. Reachable only when LD is passed on
   the stack (>8 FP args / FP varargs past v7) on Linux aarch64. Trivial
   fix; file upstream with the fix.

## B. Verified NOT affecting current master (tested both arches, O0-O2)

- **#308 — DSE eliminates wrong store** (aliasing between different
  displacements): reproducer passes on master; the bug was on the bbv
  branch. We don't use BBV.
- **#249 — `mov` beyond 32-bit absolute address displacement** (2022):
  reproducer (MAP_FIXED at 0xdeadbe0000, store via absolute mem op) passes
  at O0-O2 on both arches — fixed since. Worth keeping the reproducer; the
  pattern (host pointers baked as constants) is exactly what jitc-style
  embedding produces.
- **#136/#142 — struct varargs miscompiles** (2020): struct-through-varargs
  (small char-struct ×2 + 32-byte struct) verified correct through
  slimcc-mir-run at O2 on both arches.
- **#429 — ARM64 by-value struct >16B + pointer-to-static-array crash**:
  Apple Silicon report; did not reproduce on Linux aarch64 last session
  (plain-BLK >16B struct passing works). Apple is not a production target;
  re-test if it becomes one.

## C. Relevant if/when we use the feature

- **#426 — lref breaks binary read** ("A label not from any function in
  lref"): computed-goto modules can't round-trip through .bmir. Blocks the
  optional jitc module-cache feature (P8) for such modules only. Fix
  together with #424 (same machinery).
- **#425 — custom-alloc docs omit `MIR_change_module_ctx`**: the real issue
  is the allocator-pairing landmine (module allocated with scratch ctx's
  allocator, freed with consumer's). We use the default allocator in both —
  fine. tclmir must keep doing so, or implement migration-aware allocators.
- **#410 — `try_spilled_reg_mem` failure at O1**: RA robustness; the
  function still exists (mir-gen.c:7962). We run O2 (full RA); O1 uses the
  same simplified path we now route JMPI functions through, so worth
  re-testing once the (long) reproducer is distilled — especially since our
  jmpi fix sends *some* O2 functions down the simplified path.
- **#254 — MIR_output not re-scannable** (proto/forward ordering): affects
  textual round-trip debugging workflows only.
- **#385 / #341 / #356** — API conveniences (mixed text+API, bounded scan,
  callback user-data). Adopt opportunistically.

## D. Platform readiness notes

- **musl/Alpine (#307, #297)**: "can not load symbol printf" on Alpine is
  the *test drivers'* hardcoded glibc dlopen tables (c2mir-driver.c:69-124,
  mir-bin-run.c), not the library. libslimcc/jitc resolve symbols via their
  own import resolver, so production is unaffected. Remaining musl items:
  (1) patch driver tables to run the upstream suite on Alpine as a
  validation gate (worth doing before production cutover — we have
  alpine-tcl infra for this); (2) no stack probes + musl's 128KB default
  thread stacks — big frames/allocas in JIT'd code can blow past the guard
  page; document a minimum stack size for threads calling JIT'd code or set
  pthread attr in tclmir.
- **Windows (#154, #95, #163, #149, #136 history)**: x86_64 Win64 codegen
  and runtime exist and are CI'd via CMake+MSVC, but: no SEH unwind info
  for generated code, no `__chkstk` probes (>4KB frames skip guard pages),
  single return value, long double == double, c2mir driver issues (PR #149
  pending). For restoring jitc's Windows target: feasible, needs a
  dedicated validation pass; mingw-gcc build of MIR itself has an open
  asm issue (#163). Defer as planned.
- **32-bit (#218)**: explicitly unsupported (`#error` on Win32; no 32-bit
  target files). Not a goal.

## E. Open PRs worth cherry-picking into the fork

| PR | What | Verdict |
|---|---|---|
| #420 | error-path null deref (`curr_func` NULL'd before message formats `curr_func->nres`) | take — we longjmp from error funcs, error paths matter |
| #418 | build break with `MIR_NO_GEN_DEBUG=1` | take if we ever set that flag (size-trimmed builds) |
| #383 / #141 | `MIR_get_global_item` declared in mir.h:621 but **never defined** | take — verified missing on master |
| #341 | `MIR_scan_string_s` (bounded) | optional |
| #289 / #162 | wasm/-m32, armv7 | not goals |
| #149 | Windows c2mir driver fixes | revisit with Workstream-Windows |

## F. Not relevant (c2mir frontend bugs — slimcc replaces it)

#411 (memory), #366, #365, #364, #362, #361, #360, #353, #296, #294, #286,
#256, #250, #247, #245, #200, #154 — all c2mir parser/checker issues. We
keep c2mir only as test driver. Feature requests (#394 TLS — slimcc rejects
TLS with a diagnostic; #320 NOT insn; #397 debug info; #374 syscalls; #87
ELF output) — none block production.

## Bottom line

Two real library bugs stand between current master+our-fixes and "production
grade for our targets": **#423** (latent gen miscompile; we're shielded but
tclmir/cmark won't be) and **#424** (lref use-after-free; we're exposed via
computed goto). Both are small, well-understood fixes. Add the aarch64
`% 16` fixes and the #420/#383 cherry-picks, run the upstream suite on
Alpine/musl once with patched driver tables, and the open-bug surface
relevant to our use case is clear. Everything else open upstream is either
c2mir-only, feature requests, platforms we don't target, or already fixed on
master (verified by reproducer).

## Status update (2026-06-11, same day)

All three fixes implemented in the fork, each on a topic branch off
upstream master with a regression test, all merged into `meson`:

- **#423** → branch `fix-gvn-load-ext` + c-tests/mir/issue423.mir
  (i32/u32/i8 store-forward cases; fails rc=1 pre-fix, passes O0-O3
  post-fix; the original C repro now prints -273 at -O3).
- **#424** → branch `fix-jump-opt-lref-labels` + c-tests/mir/issue424.mir
  (label-only BB referenced only via lref; pre-fix the merged label
  produced a garbage table and the test looped forever, post-fix passes
  O0-O3).
- **`% 16`** → branch `fix-aarch64-ld-stack-align` +
  c-tests/new/va-ld-stack.c. Pre-fix on real aarch64 (c8g):
  **SIGSEGV in both -eg and -ei** — any binary128 long double vararg
  taken from the stack crashed, worse than triaged. Post-fix passes.
- **PR #420** cherry-picked onto `meson` with `-x` (authorship preserved).

Upstream filings: PR #432 (fixes #423), PR #433 (fixes #424), issue #431 +
PR #434 (aarch64 LD stack alignment) — all open alongside PR #430.

## Alpine/musl validation (2026-06-11, Alpine 3.23.4 aarch64 EC2, gcc 15.2)

Branch `support-musl-std-libs` (off upstream master, merged into `meson`)
made the upstream suite runnable on musl; four distinct fixes were needed:

1. **Driver dlopen tables** (the #307 root cause): c2mir-driver.c,
   mir-bin-run.c, mir-bin-driver.c get a `__linux__ && !__GLIBC__` branch
   dlopening `/lib/ld-musl-<arch>.so.1` (musl = one DSO for
   libc/libm/libpthread/libdl).
2. **runtests.sh**: BusyBox diff lacks `--strip-trailing-cr`; every .expect
   comparison errored and counted as a mismatch (30 phantom failures/mode).
   Now probed once.
3. **c2mir aarch64 wchar_t** (in the same branch): builtin stddef said
   `typedef int wchar_t;` but AAPCS64 wchar_t is unsigned; musl's
   bits/alltypes.h *redeclares* wchar_t per-arch (glibc never does), so the
   c2m bootstrap died with "repeated declaration wchar_t". Fixed to
   unsigned (Apple kept signed) + __WCHAR_MAX__/__WCHAR_MIN__ predefines
   corrected to gcc values.
4. **slimcc had the same wchar_t bug** (slimcc commit 7094821): builtin
   stddef.h now uses __WCHAR_TYPE__ (gcc-style, defined per-arch by
   platform/mir.c), ty_wchar_t retyped to uint on aarch64.

Results on Alpine aarch64:
- c-tests: interp fully green 1073/1073; gen/O0/O1/O3 fail ONLY
  c-tests/new/jcall.c (segfault *after* main returns; exotic
  __builtin_jcall/jret + global register var, passes in interp, passed on
  glibc-aarch64 — upstream bug in a feature we never emit; not chased).
  gen-bb mode: also only jcall.c — the 3 glibc-aarch64 gen-bb long-double
  failures do NOT reproduce on musl.
- Bootstraps: all stages passed except `c2mir-bb-bootstrap-test`, which was
  OOM-killed — the box has 922MB RAM and a full 953MB disk (no room for
  swap). Environmental: bb bootstrap passed on the 2GB glibc box.
- slimcc suite: **89/103** = the 8 by-design rejections + 6 x86-specific
  test assumptions. unicode.c is the 6th: it asserts L'\xffffffff'>>31 ==
  -1, but wchar_t is unsigned on aarch64 (gcc agrees: result 1); we
  previously "passed" only because our wchar_t was wrongly signed. Zero
  musl-specific failures.

Conclusion: aarch64+musl is validated to the same level as the glibc
targets. `support-musl-std-libs` is a candidate upstream PR (fixes #307).

Validation: full upstream `make test` green on x86_64 (incl. all
bootstraps); c-tests gen suite 1072/1072 on each topic branch; slimcc
suite 95/103 on x86_64 (baseline, the 8 by-design rejections);
aarch64 suite run pending/green per session log. #383
(MIR_get_global_item definition) deferred — take when first needed.
