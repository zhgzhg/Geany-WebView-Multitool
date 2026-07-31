project('geany-webview', 'c', 'cpp',
  version : files('VERSION'),   # the single source of the plugin version
  license : 'GPL-2.0-only',
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

# The plugin version C code registers with Geany, from the VERSION file.
add_project_arguments('-DGWV_VERSION="' + meson.project_version() + '"',
                      language : ['c', 'cpp'])

geany_dep = dependency('geany')   # pulls in GTK3 transitively
deps = [geany_dep]

sources = files(
  'src/plugin.c',
  'src/settings.c',
  'src/view.c',
  'src/findbar.c',
  'src/views/browser.c',
  'src/views/preview.c',
  'src/views/sideterm.c',
  'src/views/terminal.c',
  'src/bridge.c',
  'src/assets.c',
  'src/gwvutil.c',
)

# Embed assets/** into the module as a GResource (prefix /geany/webview/).
# The manifest is generated at configure time — after adding/removing asset
# files, re-run `meson setup --reconfigure`; content edits are tracked.
gnome = import('gnome')
sh = find_program('sh')
gres_xml = meson.current_build_dir() / 'gwv.gresource.xml'
run_command(sh, files('scripts/gen-gresource.sh'),
            meson.project_source_root() / 'assets', gres_xml, check : true)
gwv_resources = gnome.compile_resources('gwv-resources', gres_xml,
  source_dir : 'assets',
  c_name : 'gwv_res',
)
sources += gwv_resources

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

else
  # Linux, macOS (MacPorts GTK3/X11), other Unix: the WebKitGTK backend.
  sources += files('src/host/gtk.c', 'src/services/pty_unix.c')
  deps += dependency('webkit2gtk-4.1')
  # forkpty: in libc on glibc >= 2.34 and on macOS; older glibc/BSDs need
  # libutil (harmless where it merely exists).
  util_dep = cc.find_library('util', required : false)
  if util_dep.found()
    deps += util_dep
  endif
endif

# System-wide install (`meson install`): the module goes into Geany's plugin
# libdir and the assets next to it, matching the plugin's own-module-relative
# asset lookup. The dev loop keeps using `ninja -C build devinstall` (per-user).
geany_plugin_dir = geany_dep.get_variable(pkgconfig : 'libdir') / 'geany'

gwv = shared_module('geanywebview', sources,
  dependencies : deps,
  include_directories : inc,
  name_prefix : '',   # produce geanywebview.dll/.so, not libgeanywebview.*
  # Geany discovers plugins by G_MODULE_SUFFIX, which is "so" on every Unix
  # including macOS — don't let meson produce a .dylib there.
  name_suffix : host_sys == 'windows' ? 'dll' : 'so',
  install : true,
  install_dir : geany_plugin_dir,
)


# Standalone clipboard-crash reproduction harness (diagnostic; not installed).
# win32.cc serves the embedded assets since M6, so the harness needs the
# assets module + compiled resources too.
if host_sys == 'windows' and webview2.found()
  executable('clip_repro',
    ['src/tools/clip_repro.c', 'src/host/win32.cc', 'src/assets.c', gwv_resources],
    dependencies : deps,
    include_directories : inc,
    install : false,
  )
endif

# Standalone PTY service test (diagnostic; not installed).
if host_sys != 'windows'
  pty_test_deps = [dependency('glib-2.0')]
  if util_dep.found()
    pty_test_deps += util_dep
  endif
  executable('pty_test',
    ['src/tools/pty_test.c', 'src/services/pty_unix.c'],
    dependencies : pty_test_deps,
    include_directories : inc,
    install : false,
  )
endif

# Developer convenience: copy the freshly built plugin into the per-user Geany
# plugin dir (%APPDATA%/geany/plugins or ~/.config/geany/plugins).
run_target('devinstall',
  command : [sh, meson.project_source_root() / 'scripts' / 'devinstall.sh', gwv] + runtime_files,
)
