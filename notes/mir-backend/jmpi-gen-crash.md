# MIR codegen bugs with computed goto (laddr/jmpi) — FIXED locally

`jmpi-gen-crash.mir` is a self-contained reproducer. With MIR @ 99c65079
(upstream master, 2024-08-29) on x86-64 Linux:

    ./m2b < jmpi-gen-crash.mir > t.bmir
    MIR_TYPE=interp ./mir-bin-run t.bmir main   # prints 1 2 3, exit 0 (correct)
    MIR_TYPE=gen    ./mir-bin-run t.bmir main   # SIGSEGV (jumps to a stack address)

Two distinct bugs, fixed on the `fix-laddr-out-flag` branch of
/home/cyan/git/mir (PR candidates for vnmakarov/mir):

1. **LADDR output flag** (mir.c): `insn_descs[MIR_LADDR]` lacked OUT_FLAG on
   the destination operand, so liveness/RA treated it as an input. Under
   register pressure the RA reloaded over the laddr result and never
   spilled it; jmpi then jumped through stale spill-slot memory.

2. **Edge splitting on jmpi successors** (mir-gen.c): the full RA places
   spill/restore code on edges and splits critical edges. Edges out of a
   jmpi cannot be split (runtime-computed target); split_edge_if_necessary
   silently overwrote jmpi's register operand with a label (NDEBUG builds),
   collapsing an N-way indirect jump into a direct jump (observed as an
   infinite loop in slimcc's test/defer.c). Fixed by using the simplified
   RA for functions containing JMPI, plus keeping busy_used_locs allocated
   in sync with used_locs now that the RA mode varies per function.

Deliberately NOT fixed: adding a `jmpi label` machine pattern. After fix 2
the substitution can no longer occur, and the "Fatal failure in matching
insn" abort is a useful safety net against invalid future substitutions.

Related upstream issues (upstream unresponsive since ~2024): #424 is the
same class (label refs + jump optimization use-after-free; a commenter
patched the lref half), #426 (label refs vs MIR_read_with_func).
Validation: MIR's own `make test` fully green with both fixes; slimcc's
suite under JIT went from 83 to 86/103 (control.c, control2.c, defer.c).
