#!/usr/bin/env python3
# Generate libslimcc-headers.inc from the builtin headers, so libslimcc
# compiles work with no filesystem access. Equivalent to the Makefile
# libslimcc-headers.inc rule.
#
#   gen_embedded_headers.py OUTPUT HEADER...
import os
import sys


def c_quote(line):
    return '"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"'


def main():
    out_path = sys.argv[1]
    headers = sorted(sys.argv[2:], key=os.path.basename)
    with open(out_path, "w") as out:
        out.write("// Generated from slimcc_headers/include - do not edit.\n")
        out.write("static const struct { const char *name; const char *contents; }"
                  " embedded_headers[] = {\n")
        for path in headers:
            out.write('{"%s",\n' % os.path.basename(path))
            with open(path) as f:
                for line in f.read().splitlines():
                    out.write(c_quote(line) + "\n")
            out.write("},\n")
        out.write("};\n")


if __name__ == "__main__":
    main()
