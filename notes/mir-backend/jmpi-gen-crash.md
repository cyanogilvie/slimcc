# MIR codegen crash: 3x (laddr -> store/load via alloca slot -> jmpi) + calls

`jmpi-gen-crash.mir` is self-contained. With MIR @ 99c65079 (2024-08-29) on x86-64 Linux:

    ./m2b < jmpi-gen-crash.mir > t.bmir
    MIR_TYPE=interp ./mir-bin-run t.bmir main   # prints 1 2 3, exit 0 (correct)
    MIR_TYPE=gen    ./mir-bin-run t.bmir main   # SIGSEGV (jumps to a stack address)

Notes from bisection:
- 2x the (laddr/jmpi/call) group: works. 3x: crashes.
- Removing the calls (3x laddr/jmpi alone): works.
- 1x jmpi + 2000 padding insns: works (not a code-size threshold).
- The crash jumps to a stack address, suggesting a label address was
  resolved against a stale code location or never patched.

Found via slimcc-MIR backend: test/defer.c and test/control.c in the slimcc
suite hit this (computed goto in stmt-exprs used as call arguments).
