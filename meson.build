project('vater', ['cpp'],
  version : '0.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=3',
    'optimization=3',
    'b_lto=true',
  ]
)

cpp = meson.get_compiler('cpp')

# ── Sovereign Dependency Policy (§1 & §3) ────────────────────────────────────
# Zero third-party dependencies: only libc/POSIX, Linux syscalls, Nisaba + its native deps.
nisaba_dep      = dependency('nisaba', required : true)
backend_os_lib  = cpp.find_library('nisaba_backend_os', required : true)
threads_dep     = dependency('threads', required : true)
wayland_dep     = dependency('wayland-client', required : false)
gl_dep          = dependency('gl', required : false)
glew_dep        = dependency('glew', required : false)
dl_dep          = cpp.find_library('dl', required : false)
m_dep           = cpp.find_library('m', required : false)
rt_dep          = cpp.find_library('rt', required : false)

# Locate nisaba-layout headers if not installed under /usr/include/nisaba/layout
layout_dir = get_option('nisaba_layout_dir')
extra_inc_args = []
fs = import('fs')
if fs.is_dir(layout_dir)
  extra_inc_args += '-I' + layout_dir
endif

# Enable native SIMD (SSE2/AVX2) for UTF-8/ASCII fast-path scanner
simd_args = []
if cpp.has_argument('-march=native')
  simd_args += '-march=native'
endif

vater_src_inc  = include_directories('src')
vater_core_inc = include_directories('src', 'core')

# ── L3: term_core static library — STRICTLY ZERO NISABA DEPENDENCY (§3) ──────
term_core_sources = files(
  'src/vt/parser.cpp',
  'src/vt/scrollback.cpp',
  'src/vt/grid.cpp',
  'src/vt/reflow.cpp',
  'src/vt/graphics/sixel.cpp',
  'src/vt/graphics/kitty_graphics.cpp',
  'src/vt/dispatch.cpp',
  'src/vt/terminal.cpp',
)

term_core_deps = [threads_dep]
if rt_dep.found()
  term_core_deps += rt_dep
endif

term_core_lib = static_library('term_core',
  term_core_sources,
  include_directories : vater_src_inc,
  cpp_args : simd_args,
  dependencies : term_core_deps,
)

term_core_dep = declare_dependency(
  include_directories : vater_src_inc,
  link_with : term_core_lib,
  dependencies : term_core_deps,
)

# ── L1: OS Layer static library (POSIX PTY, Spawn, epoll, Unix Socket IPC) ───
vater_os_sources = files(
  'src/os/pty.cpp',
  'src/os/spawn.cpp',
  'src/os/event_loop.cpp',
  'src/os/ipc.cpp',
)

vater_os_lib = static_library('vater_os',
  vater_os_sources,
  include_directories : vater_src_inc,
  dependencies : [threads_dep],
)

vater_os_dep = declare_dependency(
  include_directories : vater_src_inc,
  link_with : vater_os_lib,
  dependencies : [threads_dep],
)

# ── L2 Daemon: vaterd (Persistent Session Daemon, zero GUI/Nisaba deps!) ─────
vaterd_sources = files(
  'src/mux/daemon/vaterd.cpp',
  'src/mux/daemon/main.cpp',
)

vaterd_exe = executable('vaterd',
  vaterd_sources,
  include_directories : vater_src_inc,
  dependencies : [vater_os_dep, threads_dep],
  install : true,
)

# ── L2 + L4 + L5: vater Main Terminal Emulator Application ───────────────────
vater_cpp_args = [
  '-DNISABA_HAS_BACKEND_OS=1',
  '-D_USE_MATH_DEFINES',
  '-DVATER_VERSION="' + meson.project_version() + '"',
] + extra_inc_args + simd_args

vater_deps = [
  term_core_dep,
  vater_os_dep,
  nisaba_dep,
  backend_os_lib,
  threads_dep,
]

if wayland_dep.found()
  vater_deps += wayland_dep
endif
if gl_dep.found()
  vater_deps += gl_dep
endif
if glew_dep.found()
  vater_deps += glew_dep
  vater_cpp_args += '-DNISABA_GLEW=1'
endif
if dl_dep.found()
  vater_deps += dl_dep
endif
if m_dep.found()
  vater_deps += m_dep
endif

vater_app_sources = files(
  'core/csd_frame.cpp',
  'src/config/loader.cpp',
  'src/config/keymap.cpp',
  'src/mux/session.cpp',
  'src/mux/pane_tree.cpp',
  'src/mux/daemon/vaterd.cpp',
  'src/render/font_stack.cpp',
  'src/render/shaper.cpp',
  'src/render/atlas.cpp',
  'src/render/damage.cpp',
  'src/render/cpu_renderer.cpp',
  'src/render/cell_renderer.cpp',
  'src/render/effects/crt_effect.cpp',
  'src/ui/input_encoder.cpp',
  'src/ui/tab_bar.cpp',
  'src/ui/search_overlay.cpp',
  'src/ui/command_palette.cpp',
  'src/ui/context_menu.cpp',
  'src/ui/quake_mode.cpp',
  'src/ui/preview_panel.cpp',
  'src/ui/app.cpp',
  'src/main.cpp',
)

vater_exe = executable('vater',
  vater_app_sources,
  include_directories : vater_core_inc,
  cpp_args : vater_cpp_args,
  dependencies : vater_deps,
  install : true,
)

# ── Desktop Integration, Icon, Terminfo & Shell Integration Install Rules ────
install_data('assets/vater.desktop',
  install_dir : join_paths(get_option('datadir'), 'applications'),
)
install_data('assets/vater.svg',
  install_dir : join_paths(get_option('datadir'), 'icons/hicolor/scalable/apps'),
)
install_data('terminfo/vater.ti',
  install_dir : join_paths(get_option('datadir'), 'vater/terminfo'),
)
install_data(
  files('docs/shell-integration/vater.bash', 'docs/shell-integration/vater.zsh'),
  install_dir : join_paths(get_option('datadir'), 'vater/shell-integration'),
)

# ── Tests (Unit + Conformance + Golden CPU Render + Daemon IPC) ──────────────
if get_option('build_tests')
  test_vt_unit = executable('test_vt_unit',
    files('tests/unit/test_vt_unit.cpp'),
    include_directories : vater_src_inc,
    cpp_args : simd_args,
    dependencies : [term_core_dep, vater_os_dep],
  )
  test('vt_unit', test_vt_unit)

  test_conformance = executable('test_conformance',
    files('tests/conformance/test_conformance.cpp'),
    include_directories : vater_src_inc,
    cpp_args : simd_args,
    dependencies : [term_core_dep],
  )
  test('vt_conformance', test_conformance)

  test_regex_input = executable('test_regex_input',
    files(
      'tests/unit/test_regex_input.cpp',
      'src/ui/input_encoder.cpp',
      'src/ui/search_overlay.cpp',
      'src/ui/context_menu.cpp',
      'src/config/loader.cpp',
      'src/config/keymap.cpp',
      'src/mux/session.cpp',
      'src/mux/pane_tree.cpp',
      'src/mux/daemon/vaterd.cpp',
    ),
    include_directories : vater_core_inc,
    cpp_args : vater_cpp_args,
    dependencies : vater_deps,
  )
  test('regex_input_config_ipc', test_regex_input)

  test_golden_render = executable('test_golden_render',
    files(
      'tests/conformance/test_golden_render.cpp',
      'src/render/font_stack.cpp',
      'src/render/shaper.cpp',
      'src/render/atlas.cpp',
      'src/render/cpu_renderer.cpp',
    ),
    include_directories : vater_core_inc,
    cpp_args : vater_cpp_args,
    dependencies : vater_deps,
  )
  test('golden_cpu_render', test_golden_render)
endif

# ── Benchmarks (§7 Performance Budgets) ──────────────────────────────────────
if get_option('build_bench')
  bench_vt = executable('bench_vt',
    files('tests/bench/bench_vt.cpp'),
    include_directories : vater_src_inc,
    cpp_args : simd_args,
    dependencies : [term_core_dep],
  )
endif

# ── Fuzz Target (libFuzzer + ASan/UBSan when enabled with clang++) ───────────
if get_option('build_fuzz')
  executable('fuzz_parser',
    files('tests/fuzz/fuzz_parser.cpp'),
    include_directories : vater_src_inc,
    cpp_args : ['-fsanitize=fuzzer,address,undefined'],
    link_args : ['-fsanitize=fuzzer,address,undefined'],
    dependencies : [term_core_dep],
  )
endif

summary({
  'Version'      : meson.project_version(),
  'Nisaba'       : nisaba_dep.found(),
  'Backend OS'   : backend_os_lib.found(),
  'Layout Dir'   : layout_dir,
  'Tests'        : get_option('build_tests'),
  'Benchmarks'   : get_option('build_bench'),
  'Fuzzer'       : get_option('build_fuzz'),
}, section : 'Vater Sovereign Terminal', bool_yn : true)

