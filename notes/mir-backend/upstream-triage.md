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
targets. `support-musl-std-libs` filed upstream as **PR #435** (fixes #307).

### Re-verification on resized boxes (2026-06-11, later same day)

- **Ubuntu glibc aarch64** (ubuntu@56.124.86.243, c8g 1.8GB): slimcc suite
  re-baselined at **89/103** — unicode.c joined the failure set exactly as
  predicted after the wchar_t fix (same 14-test list as Alpine; the two
  aarch64 environments are now identical). Full upstream `make test` on the
  merged `meson` tip: 5 modes 1073/1073, gen-bb only the 3 pre-existing LD
  failures, all bootstraps passed, jcall.c passes on glibc.
- **Alpine musl aarch64** (alpine@15.229.2.189, 1.85GB RAM + 2GB swap,
  4GB disk): full suite green except jcall.c (gen modes, known) and
  `c2mir-bb-bootstrap-test`, which the kernel OOM-killed at 1.78GB anon-RSS
  / 4.4GB total-vm even with swap — the `-p4` parallel lazy-bb self-compile
  needs >2GB on musl (glibc fits in the same RAM; musl mallocng peak is
  higher). The lazy-bb *mode* itself is green in c-tests gen-bb. A sequential
  (-p1) retry died at the *same* numbers (anon-rss 1.78GB, total-vm
  4.47GB, swap untouched, swappiness 60) — the memory is one compilation
  context, not parallel workers: c2m's lazy-bb self-compile peaks ~2.5x
  higher under musl mallocng than glibc for the identical workload
  (compare upstream #411, "C2MIR high memory usage"). A c2mir-on-musl
  characteristic, not a codegen issue, and c2mir is not in our production
  path; run that one target on a >=4GB box if ever needed.

Validation: full upstream `make test` green on x86_64 (incl. all
bootstraps); c-tests gen suite 1072/1072 on each topic branch; slimcc
suite 95/103 on x86_64 (baseline, the 8 by-design rejections);
aarch64 suite run pending/green per session log. #383
(MIR_get_global_item definition) deferred — take when first needed.

### CORRECTION: the bb-bootstrap "musl memory characteristic" was a real bug (2026-06-12)

The previous section's conclusion was wrong. Root-caused on an 8GB Alpine
box (alpine@18.228.203.188): the OOM was **upstream bug #436**, not a
mallocng peak-memory characteristic, and not bounded — the run consumed
7.87GB in 17.6s before the OOM kill (would grow toward ~46GB), at -p4,
-p1, AND plain -eb. Reproduces identically on upstream master + musl
driver tables only, so none of our fork fixes were involved.

Mechanism (full analysis in mir INTERNALS.md §15 and upstream #436):

1. The aarch64 **bb thunk clobbers x9** — `mov x9, <bb_version>` followed
   by a branch to the bb wrapper emitted via `_MIR_redirect_thunk`, whose
   far form is `ldr x9,8; br x9`. A thunk published >±128MB (direct-branch
   reach) from the wrapper hands the wrapper **its own address** as
   bb_version; machine-code bytes read as `attrs[i].spot` (~2.9e9, stp
   opcodes — verified in gdb: bb_version == gen_ctx->bb_wrapper exactly)
   make set_spot2attr grow spot2attr toward tens of GB.
2. With that fixed, generated bb code next dies in `setup_rel` ("too big
   offset") on direct branches to far successor thunks — lazy-bb generally
   assumes all JIT code is mutually within direct-branch range.

Why musl: glibc grows the heap with brk (code-holder mmaps stay
clustered); mallocng's many mmaps interleave with code holders and push
them hundreds of MB apart at bootstrap scale (~400MB compiler heap).
Latent on glibc too for large enough -eb workloads. -eg/-ei were immune
(function thunks carry no payload in x9), which is why only bb-bootstrap
failed. riscv64/ppc64 already used a separate register for the far
redirect; x86_64 is safe (payload r10, redirect r11) modulo a ±2GB
analogue (jmp rel32 silently truncates).

Fix: fork branch `fix-aarch64-bb-thunk-clobber` (2 commits: x10-based far
redirect for the bb thunk; 128MB contiguous code-space reservation in
mir.c carving all code holders so direct-branch range holds by
construction — VA-only cost, 64-bit non-Windows). Filed upstream as
**issue #436 + PR #437**; merged into `meson`.

Results with the fix: Alpine bb-bootstrap **passes in 7.0s at 535MB peak**
(glibc x86_64: 575MB — musl is no outlier after all; measured matrix:
musl step1 379MB, -eg 550MB, -ei 507MB, all in line with glibc). Full
`make test` green on x86_64; Alpine full suite re-run in progress
(expect green except known jcall.c).

Investigation gotcha worth remembering: hand-built c2m **must** use the
GNUmakefile's `-fsigned-char -fno-tree-sra -fno-ipa-cp-clone` — without
-fsigned-char, aarch64's unsigned plain char makes out_insn's template
parser (`char d; (d = hex_value(*p)) >= 0`) loop/crash, producing
convincing but bogus failure modes that cost us a detour.

## Paramless C23 variadics + x86-64 va_start BLK rounding (2026-06-12)

`int f(...)` (C23, no named params) was rejected by mir.c's front-end
check "Variable arg function w/o any mandatory argument" in
new_func_arr. Investigation showed nothing downstream needs a named
arg: prologue register-save areas are gated on vararg_p alone, both
targets' VA_START lowering walks nargs from zero, protos with zero
named args were already accepted (call sites worked all along), and
the textual writer even anticipated the case (`nargs == 0 && nres == 0
? "..." : ", ..."`). Removing the check (fork branch
`c23-zero-named-vararg`, merged to meson) makes scan, .bmir
write/read, interp shim, and gen all handle it; MIR's full make test
stays green.

Lifting the check unmasked a REAL pre-existing x86-64 bug: VA_START
machinization accumulated `mem_offset += var.size` for memory-passed
BLK named args, but the argument area lays each BLK out rounded to 8
(`(size+7)/8*8`, same file). Named structs of non-multiple-of-8 size
before `...` put overflow_arg_area short of the true vararg start —
slimcc's function2.c struct_test131 (5×1-byte + 6×4-byte structs) read
every stack vararg 59 bytes low. One-line fix (round like the arg
area). aarch64 already rounds via qwords in machinize — unaffected.
Note relation to the earlier #136/#142 triage: those covered structs
passed AS varargs (fine); this is structs as NAMED params before the
ellipsis.

Filed upstream (2026-06-12): **PR #438** (fix-x86_64-va-start-offsets)
— deeper than the first-cut rounding fix: the va_start named-arg scan
also failed to count REGISTER-passed blocks (BLK+1/+2) into
gp_offset/fp_offset (c2m C11 repro: two struct{long} params before
`...` make va_arg return s2.a), and its fp-exhaustion test checked
gp_offset >= 176. The fix replaces the scan with the prologue walk's
int_arg_num/fp_arg_num/mem_size totals (as aarch64 already does);
regression test c-tests/new/va-struct-args.c fails on master, passes
all modes with the fix. **PR #439** (allow-paramless-vararg-func) —
check removal + c-tests/mir/paramless-vararg.mir; c2mir is unaffected
(its C11 grammar rejects `(...)` with its own syntax error, so no
diagnostic was delegated to the MIR layer). Both branches off upstream
master, full make test green each; both merged to `meson` (the
counter-based rewrite supersedes the earlier rounding-only commit).

slimcc effect: x86_64 JIT suite 95→97/103 (function2, xxxof_vmtype now
pass); remaining 6 are the by-design asm/TLS/weak/return-address
rejections. With an unpatched mir the paramless-variadic guard removal
degrades to a clean compile error via the MIR error longjmp.

## aarch64 confirmation sweeps + new vararg bug (2026-06-12)

Both aarch64 boxes re-swept on mir meson 48f6a8dd + slimcc b1385f4
(variadic fixes + emutls TLS): Ubuntu glibc and Alpine musl both
**92/103 with identical fail lists** = 4 by-design (asm, inline_asm,
attr_weak, builtin_return_address) + 6 x86-test-assumptions + function2.c.
tls/tls2 and xxxof_vmtype pass on aarch64 (emutls and the paramless
variadic fixes are target-clean).

function2.c is NOT a regression: it used to fail at compile time (it
contains `va_fn(...)`), so its struct_test130/131 bodies never ran on
aarch64. Now unmasked: an **aarch64 MIR-layer vararg bug** — a >16-byte
by-value struct vararg mixed with FP varargs desyncs the va_list cursors.
Minimal repro (aarch64-va-bigstruct-repro.c, probe ph, only two named
ints): vararg list `LD, BigStruct(999), int, 4×double, BigStruct, LD`
reads ld1=11.1 (the *last* LD's value), int=992 (= 999 & ~7, the
qword-rounded struct size — a cursor advanced by the struct size where an
8-byte step was meant, or vice versa), doubles=0; both structs and the
final LD read correctly. Bisect: named-arg shape irrelevant (full
G/F-struct named shape + short vararg lists all pass); shorter
`BigStruct, double, LD` list passes; x86-64 passes everything; identical
on -eg for glibc and musl.

c2m cannot act as the oracle: `va_arg(ap, BigStruct)` fails to compile
with "Wrong alias number" — a separate c2m bug worth its own upstream
report. Next: reduce to a raw .mir VA_BLOCK_ARG test to confirm
target-code locus (mir-aarch64.c va builtins / machinize), fix in fork,
file upstream alongside #438/#439.

## aarch64 by-ref block-arg clobber: root cause + fix (2026-06-12, PR #440)

The function2.c failure was NOT in the va machinery at all: machinize_call
(mir-gen-aarch64.c) emitted the copy of a by-reference (>2 qwords) block
arg right before the call insn — after earlier args were already loaded
into their hard regs. For >16-qword blocks the copy is a CALL to
mir.blk_mov; only x0-x2 were saved around it, so FP args in v0-v7 and
ints in x3-x7 were exposed both to physical clobber and to the RA
legitimately reusing "call-clobbered" arg regs for temporaries (that is
how ld1 read the *last* LD: its temp landed in v0). Not vararg-specific —
any call with FP/late-GPR args before a 129+ byte struct. Fix: emit the
copy in the pre-arg-load region next to the address ADD that already
lived there, and advance curr_prev_call_insn to the ADD itself (the old
DLIST_NEXT advance pointed at the first arg load, scattering later block
args' insns mid-loads). riscv64/s390x share the pattern (untested, noted
in PR). Branch fix-aarch64-blk-mov-clobber off master, **upstream PR
#440**, merged to meson (8667bd8b).

Tests: c-tests/mir/big-blk-arg2.mir (mirrors slimcc's emission: typed
temps live across the copies + per-call proto; fails master aarch64 -eg,
passes fixed; -ei unaffected) + big-blk-arg.mir (named-args canary —
NOTE it passes even at base: the simple shape doesn't create enough reg
pressure; the bug's visible symptoms are RA-dependent). c2mir can't
oracle any of this — it lowers big aggregates itself and never emits
>2-qword BLK call args.

Validation: full make test green on x86_64, aarch64 glibc, and (meson
tip, which has the musl std-libs fix) aarch64 musl; slimcc sweeps
**93/103 on both aarch64 boxes**, identical lists, function2.c passing.

Side discovery, NOT yet filed upstream: two va_block_arg insns sharing
one dest register SIGSEGV gen's copy_prop (mir-gen.c:3235, NULL ssa edge
on an input op) on both arches, master and fork meson. Reachable from C
via c2m with the rvalue form `va_arg(ap, struct S).c[i]` used twice (a
named-local assignment per read is fine) — this is also what the earlier
"Wrong alias number" error on the Alpine c2m was (same corruption,
different surfacing). slimcc shielded by construction (distinct alloca
per struct-va_arg site); tclmir/cmark direct builders exposed. Minimal
reproducers in /tmp/cp-crash.mir + /tmp/alias3.c (copy into the fork
when filing).
