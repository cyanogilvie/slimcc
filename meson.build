project('slimcc', 'c',
  license: 'MIT',
  meson_version: '>=1.1',
  default_options: ['warning_level=0'],
)

# Builds libslimcc (the embeddable C-to-MIR JIT frontend - see
# libslimcc.h), intended for consumption as a meson subproject wrap
# providing the 'slimcc' dependency. The stock slimcc CLI compiler is
# still built with the Makefile; this covers only the library.

mir_dep = dependency('mir', fallback: ['mir', 'mir_dep'])

gen_embedded_headers = find_program('scripts/gen_embedded_headers.py')

# Builtin headers embedded into the library, so compiles work with no
# filesystem access (the parser injects bitint_builtins into every TU).
embedded_headers = files(
  'slimcc_headers/include/alloca.h',
  'slimcc_headers/include/bitint_builtins',
  'slimcc_headers/include/float.h',
  'slimcc_headers/include/iso646.h',
  'slimcc_headers/include/math.h',
  'slimcc_headers/include/stdalign.h',
  'slimcc_headers/include/stdarg.h',
  'slimcc_headers/include/stdatomic.h',
  'slimcc_headers/include/stdbool.h',
  'slimcc_headers/include/stdckdint.h',
  'slimcc_headers/include/stdcountof.h',
  'slimcc_headers/include/stddef.h',
  'slimcc_headers/include/stddefer.h',
  'slimcc_headers/include/stdnoreturn.h',
)

headers_inc = custom_target('libslimcc-headers.inc',
  output: 'libslimcc-headers.inc',
  input: embedded_headers,
  command: [gen_embedded_headers, '@OUTPUT@', '@INPUT@'],
)

lib_srcs = files(
  'alloc.c',
  'bitint.c',
  'codegen-mir.c',
  'hashmap.c',
  'libslimcc.c',
  'parse.c',
  'platform/mir.c',
  'preprocess.c',
  'slimcc-mir-helpers.c',
  'strings.c',
  'tokenize.c',
  'type.c',
  'unicode.c',
)

libslimcc = static_library('slimcc', lib_srcs, headers_inc,
  c_args: '-DSLIMCC_HEADERS_INC="@0@"'.format(headers_inc.full_path()),
  include_directories: '.',
  dependencies: mir_dep,
  pic: true,
)

slimcc_dep = declare_dependency(
  link_with: libslimcc,
  include_directories: include_directories('.'),
  dependencies: mir_dep,
)
meson.override_dependency('slimcc', slimcc_dep)

if not meson.is_subproject()
  executable('slimcc-mir-run', 'mir-run.c', dependencies: slimcc_dep)

  smoke = executable('libslimcc-smoke', 'libslimcc-smoke.c', dependencies: slimcc_dep)
  test('zero-filesystem smoke', smoke)
endif
