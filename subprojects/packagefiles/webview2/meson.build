# Overlay meson.build for the Microsoft.Web.WebView2 NuGet package, copied over
# the extracted nupkg by meson (patch_directory in webview2.wrap).
#
# Exposes:
#   webview2_dep         - include-only dependency for <WebView2.h>
#   webview2_loader_dll  - path to the arch-matching WebView2Loader.dll, loaded
#                          at runtime by the plugin (explicit LoadLibrary), and
#                          installed next to the plugin by the parent project.
project('webview2', version : '1.0.4191.47', license : 'BSD-3-Clause')

inc = include_directories('build' / 'native' / 'include')

webview2_dep = declare_dependency(
  include_directories : inc,
)

# Pick the loader DLL matching the build CPU.
cpu = host_machine.cpu_family()
if cpu == 'x86_64'
  _loader = 'build' / 'native' / 'x64' / 'WebView2Loader.dll'
elif cpu == 'aarch64'
  _loader = 'build' / 'native' / 'arm64' / 'WebView2Loader.dll'
else
  _loader = 'build' / 'native' / 'x86' / 'WebView2Loader.dll'
endif

webview2_loader_dll = files(_loader)
