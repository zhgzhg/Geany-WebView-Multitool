project('geany-webview', 'c', 'cpp',
  version : '0.0.1',
  license : 'GPL-2.0-or-later',
  meson_version : '>=1.1',
  default_options : [
    'c_std=c11',
    'cpp_std=c++14',
    'warning_level=2',
  ],
)

cc = meson.get_compiler('c')

# Internal headers are included relative to src/ (e.g. "host/wvhost.h",
# "views/preview.h"). Keeps includes stable as the tree grows.
inc = include_directories('src')

# Route the plugin's g_debug()/g_warning() under a dedicated log domain so
# traces can be enabled selectively with G_MESSAGES_DEBUG=geany-webview.
add_project_arguments('-DG_LOG_DOMAIN="geany-webview"', language : ['c', 'cpp'])

geany_dep    = dependency('geany')          # pulls in GTK3 transitively
jsonglib_dep = dependency('json-glib-1.0')  # bridge envelope parsing
deps = [geany_dep, jsonglib_dep]

sources = files(
  'src/plugin.c',
  'src/settings.c',
  'src/view.c',
  'src/views/preview.c',
  'src/views/terminal.c',
  'src/bridge.c',
  'src/util.c',
)

# Extra files devinstall must place next to the plugin DLL (e.g. the WebView2
# loader). Populated per-platform below.
runtime_files = []

host_sys = host_machine.system()

if host_sys == 'windows'
  sources += files('src/host/win32.cc', 'src/services/pty_win32.c')

  # Unlock Win10 1809+ API (ConPTY declarations, WebView2 controller options).
  add_project_arguments('-D_WIN32_WINNT=0x0A00', language : ['c', 'cpp'])

  # WebView2 SDK is optional until subprojects/webview2.wrap is added (M0.2).
  # Without it we build a stub host so the plugin still loads and the pane works.
  webview2 = subproject('webview2', required : false)
  if webview2.found()
    deps += webview2.get_variable('webview2_dep')
    runtime_files += webview2.get_variable('webview2_loader_dll')
    add_project_arguments('-DHAVE_WEBVIEW2=1', language : ['c', 'cpp'])
    # WebView2.h carries MSVC `#pragma warning(...)` directives; don't let them
    # spam the build under -Wall.
    add_project_arguments('-Wno-unknown-pragmas', language : 'cpp')
    message('WebView2 SDK found: building real WebView2 host backend.')
  else
    message('WebView2 SDK not present yet: building STUB host backend.')
  endif

  foreach lib : ['ole32', 'shlwapi', 'version', 'advapi32', 'user32']
    deps += cc.find_library(lib)
  endforeach

elif host_sys == 'darwin'
  error('macOS backend is deferred (milestone M5).')

else
  # Linux / other Unix (milestone M4)
  sources += files('src/host/gtk.c')
  deps += dependency('webkit2gtk-4.1')
endif

gwv = shared_module('geanywebview', sources,
  dependencies : deps,
  include_directories : inc,
  name_prefix : '',   # produce geanywebview.dll, not libgeanywebview.dll
  install : false,    # dev loop uses `ninja -C build devinstall`; global install is M4.4
)

# Standalone clipboard-crash reproduction harness (diagnostic; not installed).
if host_sys == 'windows' and webview2.found()
  executable('clip_repro',
    ['src/tools/clip_repro.c', 'src/host/win32.cc'],
    dependencies : deps,
    include_directories : inc,
    install : false,
  )
endif

# Developer convenience: copy the freshly built plugin + assets into the
# per-user Geany plugin dir (%APPDATA%/geany/plugins). Run: ninja -C build devinstall
sh = find_program('sh')
run_target('devinstall',
  command : [sh, meson.project_source_root() / 'scripts' / 'devinstall.sh', gwv] + runtime_files,
)
