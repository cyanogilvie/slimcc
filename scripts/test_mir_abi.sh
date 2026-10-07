#!/bin/sh
# Host-ABI interop test for the MIR backend: aggregates passed and returned by
# value between JIT code and host-compiled code (test/mir-abi/gen.py).
#
#   scripts/test_mir_abi.sh [hostcc]     (run from the repo root, after
#                                         `make slimcc-mir-run`)
set -eu

hostcc="${1:-cc}"
out="${TMPDIR:-/tmp}/slimcc-mir-abi.$$"
trap 'rm -rf "$out"' EXIT

# _BitInt by value, when the host compiler has it.
bitint=
if echo '_BitInt(100) f(_BitInt(100) x) { return x; }' \
	| "$hostcc" -std=c23 -x c -c -o /dev/null - 2>/dev/null; then
	bitint=--bitint
fi
python3 test/mir-abi/gen.py "$out" $bitint

"$hostcc" -std=c23 -O2 -fPIC -shared -I"$out" -o "$out/libhost.so" "$out/host.c"

incl=
for d in /usr/local/include /usr/include/$(uname -m)-linux-gnu /usr/include; do
	[ -d "$d" ] && incl="$incl -I$d"
done
[ -f /etc/alpine-release ] || incl="-Islimcc_headers/platform_fix/linux_glibc $incl"

# The driver resolves imports with dlsym(RTLD_DEFAULT), so preload the host half.
LD_PRELOAD="$out/libhost.so" ./slimcc-mir-run -I"$out" $incl "$out/jit.c"
