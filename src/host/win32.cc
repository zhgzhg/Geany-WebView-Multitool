/*
 * host/win32.cc — Windows WebView host backend (C++14), exposing the C ABI
 * declared in wvhost.h.
 *
 * Real backend (HAVE_WEBVIEW2): parents a WebView2 controller onto the native
 * HWND of the host GtkWidget, drives bounds/visibility from GTK signals, and
 * loads WebView2Loader.dll dynamically (explicit link, MinGW-compatible). All
 * WebView2 callbacks arrive on the GTK main thread via GTK's Win32 message
 * pump, so no extra COM apartment or message loop is set up here.
 *
 * Stub backend (no HAVE_WEBVIEW2): paints a placeholder so the plugin still
 * loads without the SDK.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "host/wvhost.h"
#include "assets.h"

#include <glib.h>

/* ---------------- backend pieces shared by real and stub ---------------- */

/* WebView2 is parented onto a native HWND, so the area widget is a drawing
 * area whose GdkWindow can be made native. */
extern "C" GtkWidget *wv_host_new_area(void)
{
	return gtk_drawing_area_new();
}

/* Mapped folders are served by WebView2 at https://<host>/. */
extern "C" char *wv_host_format_url(const char *host_name, const char *path)
{
	return g_strdup_printf("https://%s/%s", host_name, path != nullptr ? path : "");
}

#ifdef HAVE_WEBVIEW2
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <shlwapi.h>     /* SHCreateMemStream */
#  include <gdk/gdkwin32.h>
#  include "WebView2.h"
#  include <string>
#  include <cstring>
#  include <glib/gstdio.h>

/* WebView2.h declares its interfaces with MIDL_INTERFACE (no uuid attribute
 * under MinGW), so associate the IIDs we need with __CRT_UUID_DECL to make
 * __uuidof link. GUIDs taken verbatim from WebView2.h. */
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
	0x4e8a3389, 0xc9d8, 0x4bd2, 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d)
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
	0x6c4819f3, 0xc9b7, 0x4260, 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c)
__CRT_UUID_DECL(ICoreWebView2WebMessageReceivedEventHandler,
	0x57213f19, 0x00e6, 0x49fa, 0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2)
__CRT_UUID_DECL(ICoreWebView2_3,
	0xa0d6df20, 0x3b92, 0x416d, 0xaa, 0x0c, 0x43, 0x7a, 0x9c, 0x72, 0x78, 0x57)
__CRT_UUID_DECL(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
	0xb99369f3, 0x9b11, 0x47b5, 0xbc, 0x6f, 0x8e, 0x78, 0x95, 0xfc, 0xea, 0x17)
__CRT_UUID_DECL(ICoreWebView2FocusChangedEventHandler,
	0x05ea24bd, 0x6452, 0x4926, 0x90, 0x14, 0x4b, 0x82, 0xb4, 0x98, 0x13, 0x5d)
__CRT_UUID_DECL(ICoreWebView2NavigationStartingEventHandler,
	0x9adbe429, 0xf36d, 0x432b, 0x9d, 0xdc, 0xf8, 0x88, 0x1f, 0xbd, 0x76, 0xe3)
__CRT_UUID_DECL(ICoreWebView2WebResourceRequestedEventHandler,
	0xab00b74c, 0x15f1, 0x4646, 0x80, 0xe8, 0xe7, 0x63, 0x41, 0xd2, 0x5d, 0x71)
__CRT_UUID_DECL(ICoreWebView2Settings3,
	0xfdb5ab74, 0xaf33, 0x4854, 0x84, 0xf0, 0x0a, 0x63, 0x1d, 0xeb, 0x5e, 0xba)
__CRT_UUID_DECL(ICoreWebView2NewWindowRequestedEventHandler,
	0xd4c185fe, 0xc81c, 0x4989, 0x97, 0xaf, 0x2d, 0x3f, 0xa7, 0xab, 0x56, 0x51)
__CRT_UUID_DECL(ICoreWebView2SourceChangedEventHandler,
	0x3c067f9f, 0x5388, 0x4772, 0x8b, 0x48, 0x79, 0xf7, 0xef, 0x1a, 0xb3, 0x7c)
#endif /* HAVE_WEBVIEW2 */

/* ------------------------------------------------------------------ common */

struct WvHost {
	GtkWidget       *container;
	WvHostCallbacks  cb;
	gpointer         user;
	gulong           draw_id;      /* placeholder "draw" handler, 0 if none */
	/* config (copied), NULL if unset */
	gchar           *cfg_virtual_host;
	gchar           *cfg_inject_js;
	gboolean         cfg_allow_browsing;  /* free navigation (browser view)  */
	GHashTable      *virtuals;     /* path -> VirtualDoc (on the asset host) */
#ifdef HAVE_WEBVIEW2
	gulong           realize_id;
	gulong           size_id;
	struct HostLink *link;         /* async-safe back-reference             */
	ICoreWebView2Environment *env;
	ICoreWebView2Controller  *controller;
	ICoreWebView2            *core;
	HWND             container_hwnd;  /* native HWND the WebView2 is parented to */
	gboolean         focus_filter_on;
	EventRegistrationToken    msg_token;
	EventRegistrationToken    focus_token;
	EventRegistrationToken    nav_token;
	EventRegistrationToken    webres_token;
	EventRegistrationToken    newwin_token;
	EventRegistrationToken    src_token;
	gboolean         in_focus_sync;   /* guards GTK<->WebView2 focus re-entrancy */
	int              controller_retries;
	gboolean         ready;
	gchar           *pending_url;  /* navigate requested before ready       */
	gchar           *pending_html; /* set_html requested before ready       */
	GtkWidget       *warmup_toplevel; /* deferred-warmup toplevel, one-shot  */
	gulong           warmup_map_id;
#endif
};

static void host_copy_config(WvHost *h, const WvHostConfig *cfg)
{
	if (cfg == NULL)
		return;
	h->cfg_virtual_host = g_strdup(cfg->virtual_host);
	h->cfg_inject_js    = g_strdup(cfg->inject_js);
	h->cfg_allow_browsing = cfg->allow_browsing;
}

static void host_free_config(WvHost *h)
{
	g_free(h->cfg_virtual_host);
	g_free(h->cfg_inject_js);
}

/* -------------------------------------------------------- placeholder paint */

#ifdef HAVE_WEBVIEW2
static const char *kPlaceholder = "Geany WebView \xe2\x80\x94 starting WebView2\xe2\x80\xa6";
#else
static const char *kPlaceholder = "Geany WebView \xe2\x80\x94 stub host (WebView2 not compiled in)";
#endif

static gboolean placeholder_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
	(void) data;
	GtkAllocation a;
	gtk_widget_get_allocation(widget, &a);
	cairo_set_source_rgb(cr, 0.12, 0.12, 0.14);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0.82, 0.82, 0.88);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 13.0);
	cairo_move_to(cr, 14.0, 28.0);
	cairo_show_text(cr, kPlaceholder);
	return TRUE;
}

/* ==================================================================== */
#ifdef HAVE_WEBVIEW2
/* ============================ real backend ========================== */

/* Async-safe link: shared between a WvHost and any in-flight COM completion
 * handlers. host is nulled when the WvHost is destroyed, so a late callback
 * (e.g. Geany quit while WebView2 is still initializing) is a no-op. Everything
 * runs on the GTK main thread; the refcount just governs object lifetime. */
struct HostLink {
	LONG    ref;
	WvHost *host;
};

static HostLink *link_new(WvHost *h)  { HostLink *l = new HostLink{1, h}; return l; }
static void      link_ref(HostLink *l){ InterlockedIncrement(&l->ref); }
static void      link_unref(HostLink *l){ if (InterlockedDecrement(&l->ref) == 0) delete l; }

/* ------- dynamically resolved WebView2Loader.dll entry points ------- */

typedef HRESULT (STDMETHODCALLTYPE *CreateEnvFn)(
	PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
	ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
typedef HRESULT (STDMETHODCALLTYPE *GetVersionFn)(PCWSTR, LPWSTR *);

static CreateEnvFn  s_create_env  = nullptr;
static GetVersionFn s_get_version = nullptr;
static bool         s_loader_tried = false;

/* One WebView2 environment is shared process-wide (created once, never
 * released): destroying an environment and recreating it on the same user-data
 * folder can hang while the previous browser process is still exiting — which
 * froze Geany on plugin re-enable. Each WvHost owns only a controller. */
static ICoreWebView2Environment *s_env = nullptr;
static bool     s_env_creating = false;
static GSList  *s_env_waiters  = nullptr;   /* HostLink* (ref'd) awaiting s_env */

static std::wstring module_dir(void)
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   reinterpret_cast<LPCWSTR>(&module_dir), &self);
	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
	std::wstring s(path, n);
	size_t slash = s.find_last_of(L"\\/");
	return (slash == std::wstring::npos) ? std::wstring(L".") : s.substr(0, slash);
}

/* --- per-process user-data folder (isolates us from orphaned browser
 * processes of other/dead Geany instances that would otherwise lock a shared
 * folder and fail our controller creation with ERROR_BUSY) --- */

static gboolean pid_alive(DWORD pid)
{
	HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (h == nullptr)
		return FALSE;                       /* gone (or inaccessible) */
	DWORD r = WaitForSingleObject(h, 0);
	CloseHandle(h);
	return (r == WAIT_TIMEOUT);              /* still running */
}

static void rm_rf(const char *path)
{
	GDir *d = g_dir_open(path, 0, nullptr);
	if (d != nullptr) {
		const char *name;
		while ((name = g_dir_read_name(d)) != nullptr) {
			gchar *child = g_build_filename(path, name, nullptr);
			if (g_file_test(child, G_FILE_TEST_IS_DIR))
				rm_rf(child);
			else
				g_unlink(child);
			g_free(child);
		}
		g_dir_close(d);
	}
	g_rmdir(path);
}

/* Best-effort removal of inst-<pid> folders whose owning process has exited. */
static void clean_stale_instances(const char *base)
{
	GDir *d = g_dir_open(base, 0, nullptr);
	if (d == nullptr)
		return;
	DWORD self = GetCurrentProcessId();
	const char *name;
	while ((name = g_dir_read_name(d)) != nullptr) {
		if (g_str_has_prefix(name, "inst-")) {
			DWORD pid = (DWORD) g_ascii_strtoull(name + 5, nullptr, 10);
			if (pid != self && !pid_alive(pid)) {
				gchar *child = g_build_filename(base, name, nullptr);
				rm_rf(child);
				g_free(child);
			}
		}
	}
	g_dir_close(d);
}

/* Returns the per-process user-data folder (created), g_free the result. */
static gchar *user_data_folder(void)
{
	gchar *base = g_build_filename(g_get_user_cache_dir(), "geany-webview", nullptr);
	clean_stale_instances(base);
	gchar *inst = g_strdup_printf("inst-%lu", (unsigned long) GetCurrentProcessId());
	gchar *cache = g_build_filename(base, inst, nullptr);
	g_mkdir_with_parents(cache, 0700);
	g_free(inst);
	g_free(base);
	return cache;
}

static void ensure_loader(void)
{
	if (s_loader_tried)
		return;
	s_loader_tried = true;
	std::wstring p = module_dir() + L"\\WebView2Loader.dll";
	HMODULE m = LoadLibraryW(p.c_str());
	if (m == nullptr)
		m = LoadLibraryW(L"WebView2Loader.dll");  /* fall back to search path */
	if (m != nullptr) {
		s_create_env  = reinterpret_cast<CreateEnvFn>(
			reinterpret_cast<void *>(GetProcAddress(m, "CreateCoreWebView2EnvironmentWithOptions")));
		s_get_version = reinterpret_cast<GetVersionFn>(
			reinterpret_cast<void *>(GetProcAddress(m, "GetAvailableCoreWebView2BrowserVersionString")));
	}
}

/* ----------------------------- utf helpers -------------------------- */

static wchar_t *u8_to_w(const char *s)
{
	return reinterpret_cast<wchar_t *>(g_utf8_to_utf16(s, -1, nullptr, nullptr, nullptr));
}
static char *w_to_u8(const wchar_t *w)
{
	return g_utf16_to_utf8(reinterpret_cast<const gunichar2 *>(w), -1, nullptr, nullptr, nullptr);
}

/* -------------------------- host-side helpers ----------------------- */

static void host_fail(WvHost *h, const char *msg)
{
	if (h->cb.on_failed != nullptr)
		h->cb.on_failed(h, msg, h->user);
}

static HWND host_hwnd(WvHost *h)
{
	GdkWindow *gw = gtk_widget_get_window(h->container);
	if (gw == nullptr)
		return nullptr;
	gdk_window_ensure_native(gw);
	return reinterpret_cast<HWND>(gdk_win32_window_get_handle(gw));
}

static HWND host_toplevel_hwnd(WvHost *h)
{
	GtkWidget *top = gtk_widget_get_toplevel(h->container);
	if (top == nullptr || !gtk_widget_is_toplevel(top))
		return nullptr;
	GdkWindow *gw = gtk_widget_get_window(top);
	if (gw == nullptr)
		return nullptr;
	return reinterpret_cast<HWND>(gdk_win32_window_get_handle(gw));
}

static void host_update_bounds(WvHost *h)
{
	if (h->controller == nullptr)
		return;
	HWND hwnd = reinterpret_cast<HWND>(gdk_win32_window_get_handle(gtk_widget_get_window(h->container)));
	if (hwnd == nullptr)
		return;
	RECT rc;
	GetClientRect(hwnd, &rc);
	h->controller->put_Bounds(rc);
}

/* ------------------------- COM completion handlers ------------------ */

/* Controller creation can transiently fail with ERROR_BUSY when the WebView2
 * user-data folder is momentarily locked (e.g. a just-closed instance's
 * browser process hasn't fully exited). Retry a bounded number of times. */
static const int   kMaxControllerRetries = 20;
static const guint kControllerRetryMs    = 250;
static gboolean    retry_controller_cb(gpointer data);
static void        host_finish_ready(WvHost *h);

/* Fires when page JS calls window.chrome.webview.postMessage(). Forwards the
 * raw string to the host's on_message callback. */
class WebMessageHandler : public ICoreWebView2WebMessageReceivedEventHandler {
	LONG      ref_ = 1;
	HostLink *link_;
public:
	explicit WebMessageHandler(HostLink *l) : link_(l) { link_ref(link_); }
	virtual ~WebMessageHandler() { link_unref(link_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2WebMessageReceivedEventHandler))) {
			*ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef(void) override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release(void) override
	{
		LONG r = InterlockedDecrement(&ref_);
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
	                                 ICoreWebView2WebMessageReceivedEventArgs *args) override
	{
		(void) sender;
		WvHost *h = link_->host;
		if (h == nullptr || args == nullptr)
			return S_OK;
		LPWSTR msg = nullptr;
		if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg != nullptr) {
			char *u8 = w_to_u8(msg);
			if (h->cb.on_message != nullptr)
				h->cb.on_message(h, u8, h->user);
			g_free(u8);
			CoTaskMemFree(msg);
		}
		return S_OK;
	}
};

/* WebView2 got the Win32 focus (e.g. the user clicked it). Mirror that into
 * GTK so GTK knows the container is focused; then when GTK focus later leaves
 * the container (user clicks the editor), our focus-out handler hands the Win32
 * focus back to the toplevel — without this, the webview keeps the keyboard and
 * the rest of Geany becomes unclickable. */
class FocusChangedHandler : public ICoreWebView2FocusChangedEventHandler {
	LONG      ref_ = 1;
	HostLink *link_;
public:
	explicit FocusChangedHandler(HostLink *l) : link_(l) { link_ref(link_); }
	virtual ~FocusChangedHandler() { link_unref(link_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2FocusChangedEventHandler))) {
			*ppv = static_cast<ICoreWebView2FocusChangedEventHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef(void) override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release(void) override
	{
		LONG r = InterlockedDecrement(&ref_);
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller *sender, IUnknown *args) override
	{
		(void) sender; (void) args;
		WvHost *h = link_->host;
		if (h != nullptr && h->container != nullptr) {
			h->in_focus_sync = TRUE;                 /* suppress our focus-in MoveFocus */
			gtk_widget_grab_focus(h->container);
			h->in_focus_sync = FALSE;
		}
		return S_OK;
	}
};

/* Keep a view pinned to its served page: any navigation to a URL outside the
 * served virtual host (external links) is cancelled and opened in the OS
 * browser instead, so the preview never turns into a stuck web browser. */
class NavigationStartingHandler
	: public ICoreWebView2NavigationStartingEventHandler {
	LONG      ref_ = 1;
	HostLink *link_;
public:
	explicit NavigationStartingHandler(HostLink *l) : link_(l) { link_ref(link_); }
	virtual ~NavigationStartingHandler() { link_unref(link_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2NavigationStartingEventHandler))) {
			*ppv = static_cast<ICoreWebView2NavigationStartingEventHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef(void) override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release(void) override
	{
		LONG r = InterlockedDecrement(&ref_);
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
	                                 ICoreWebView2NavigationStartingEventArgs *args) override
	{
		(void) sender;
		WvHost *h = link_->host;
		if (h == nullptr || args == nullptr)
			return S_OK;
		if (h->cfg_allow_browsing)
			return S_OK;                     /* browser view: navigate freely */
		LPWSTR uri = nullptr;
		if (FAILED(args->get_Uri(&uri)) || uri == nullptr)
			return S_OK;

		gboolean allowed = FALSE;
		if (h->cfg_virtual_host != nullptr) {
			wchar_t *vh = u8_to_w(h->cfg_virtual_host);
			std::wstring prefix = std::wstring(L"https://") + vh + L"/";
			if (wcsncmp(uri, prefix.c_str(), prefix.size()) == 0)
				allowed = TRUE;
			g_free(vh);
		}
		if (!allowed &&
		    (wcsncmp(uri, L"about:", 6) == 0 || wcsncmp(uri, L"data:", 5) == 0 ||
		     wcsncmp(uri, L"blob:", 5) == 0))
			allowed = TRUE;

		if (!allowed) {
			args->put_Cancel(TRUE);
			/* Mapped-host links (e.g. the doc host) can be handled by the
			 * plugin (opened in the editor); everything else -> OS browser. */
			gchar *u8 = w_to_u8(uri);
			gboolean handled = (h->cb.on_navigate_external != nullptr &&
			                    h->cb.on_navigate_external(h, u8, h->user));
			g_free(u8);
			if (!handled)
				ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
		}
		CoTaskMemFree(uri);
		return S_OK;
	}
};

/* target=_blank / window.open: browsing views navigate the same view; pinned
 * views hand the URL to the OS browser. Either way no popup window appears. */
class NewWindowRequestedHandler
	: public ICoreWebView2NewWindowRequestedEventHandler {
	LONG      ref_ = 1;
	HostLink *link_;
public:
	explicit NewWindowRequestedHandler(HostLink *l) : link_(l) { link_ref(link_); }
	virtual ~NewWindowRequestedHandler() { link_unref(link_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2NewWindowRequestedEventHandler))) {
			*ppv = static_cast<ICoreWebView2NewWindowRequestedEventHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = InterlockedDecrement(&ref_);
		if (r == 0) delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
	                                 ICoreWebView2NewWindowRequestedEventArgs *args) override
	{
		WvHost *h = link_->host;
		if (h == nullptr || args == nullptr)
			return S_OK;
		args->put_Handled(TRUE);             /* never open a popup window */
		LPWSTR uri = nullptr;
		if (SUCCEEDED(args->get_Uri(&uri)) && uri != nullptr) {
			if (h->cfg_allow_browsing && sender != nullptr)
				sender->Navigate(uri);
			else
				ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
			CoTaskMemFree(uri);
		}
		return S_OK;
	}
};

/* Document URL changed (navigation/redirect/history) -> host callback. */
class SourceChangedHandler
	: public ICoreWebView2SourceChangedEventHandler {
	LONG      ref_ = 1;
	HostLink *link_;
public:
	explicit SourceChangedHandler(HostLink *l) : link_(l) { link_ref(link_); }
	virtual ~SourceChangedHandler() { link_unref(link_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2SourceChangedEventHandler))) {
			*ppv = static_cast<ICoreWebView2SourceChangedEventHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = InterlockedDecrement(&ref_);
		if (r == 0) delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
	                                 ICoreWebView2SourceChangedEventArgs *args) override
	{
		(void) args;
		WvHost *h = link_->host;
		if (h == nullptr || sender == nullptr || h->cb.on_url_changed == nullptr)
			return S_OK;
		LPWSTR uri = nullptr;
		if (SUCCEEDED(sender->get_Source(&uri)) && uri != nullptr) {
			gchar *u8 = w_to_u8(uri);
			h->cb.on_url_changed(h, u8, h->user);
			g_free(u8);
			CoTaskMemFree(uri);
		}
		return S_OK;
	}
};

/* An in-memory document published with wv_host_put_virtual(). */
typedef struct {
	GBytes *bytes;
	gchar  *mime;
} VirtualDoc;

static void virtual_doc_free(gpointer data)
{
	VirtualDoc *d = static_cast<VirtualDoc *>(data);
	g_bytes_unref(d->bytes);
	g_free(d->mime);
	g_free(d);
}

/* Serves every https://<virtual_host>/ request from the published in-memory
 * documents and the embedded assets (assets.h) — the from-memory replacement
 * for SetVirtualHostNameToFolderMapping, which can only map disk folders. */
class WebResourceRequestedHandler
	: public ICoreWebView2WebResourceRequestedEventHandler {
	LONG      ref_ = 1;
	HostLink *link_;
public:
	explicit WebResourceRequestedHandler(HostLink *l) : link_(l) { link_ref(link_); }
	virtual ~WebResourceRequestedHandler() { link_unref(link_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2WebResourceRequestedEventHandler))) {
			*ppv = static_cast<ICoreWebView2WebResourceRequestedEventHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef(void) override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release(void) override
	{
		LONG r = InterlockedDecrement(&ref_);
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
	                                 ICoreWebView2WebResourceRequestedEventArgs *args) override
	{
		(void) sender;
		WvHost *h = link_->host;
		if (h == nullptr || args == nullptr || h->env == nullptr)
			return S_OK;

		ICoreWebView2WebResourceRequest *request = nullptr;
		if (FAILED(args->get_Request(&request)) || request == nullptr)
			return S_OK;
		LPWSTR uri_w = nullptr;
		request->get_Uri(&uri_w);
		request->Release();
		if (uri_w == nullptr)
			return S_OK;

		/* Extract the decoded path (query ignored) via GLib's URI parser. */
		GBytes *bytes = nullptr;
		gchar  *mime = nullptr;
		gchar  *uri8 = w_to_u8(uri_w);
		CoTaskMemFree(uri_w);
		GUri *u = (uri8 != nullptr) ? g_uri_parse(uri8, G_URI_FLAGS_NONE, nullptr) : nullptr;
		if (u != nullptr) {
			const gchar *path = g_uri_get_path(u);
			if (path != nullptr && *path == '/') {
				const char *rel = path + 1;
				VirtualDoc *doc = static_cast<VirtualDoc *>(
					g_hash_table_lookup(h->virtuals, rel));
				if (doc != nullptr) {
					bytes = g_bytes_ref(doc->bytes);
					mime = g_strdup(doc->mime);
				} else {
					bytes = gwv_assets_lookup(rel);
					if (bytes != nullptr)
						mime = gwv_assets_mime(rel);
				}
			}
			g_uri_unref(u);
		}
		g_free(uri8);

		ICoreWebView2WebResourceResponse *response = nullptr;
		if (bytes != nullptr) {
			gsize len = 0;
			gconstpointer data = g_bytes_get_data(bytes, &len);
			IStream *stream = SHCreateMemStream(
				static_cast<const BYTE *>(data), (UINT) len);
			gchar *hdr8 = g_strdup_printf("Content-Type: %s", mime);
			wchar_t *hdr_w = u8_to_w(hdr8);
			h->env->CreateWebResourceResponse(stream, 200, L"OK", hdr_w, &response);
			g_free(hdr_w);
			g_free(hdr8);
			if (stream != nullptr)
				stream->Release();
		} else {
			h->env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
		}
		if (response != nullptr) {
			args->put_Response(response);
			response->Release();
		}
		g_bytes_unref(bytes);
		g_free(mime);
		return S_OK;
	}
};

/* Completion of AddScriptToExecuteOnDocumentCreated: the bridge shim is now
 * registered for every future document, so the host can go ready + navigate. */
class AddScriptHandler
	: public ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler {
	LONG      ref_ = 1;
	HostLink *link_;
public:
	explicit AddScriptHandler(HostLink *l) : link_(l) { link_ref(link_); }
	virtual ~AddScriptHandler() { link_unref(link_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler))) {
			*ppv = static_cast<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef(void) override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release(void) override
	{
		LONG r = InterlockedDecrement(&ref_);
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR id) override
	{
		(void) errorCode; (void) id;
		if (link_->host != nullptr)
			host_finish_ready(link_->host);
		return S_OK;
	}
};

class ControllerHandler
	: public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
	LONG      ref_ = 1;
	HostLink *link_;
public:
	explicit ControllerHandler(HostLink *l) : link_(l) { link_ref(link_); }
	virtual ~ControllerHandler() { link_unref(link_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler))) {
			*ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef(void) override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release(void) override
	{
		LONG r = InterlockedDecrement(&ref_);
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *controller) override
	{
		g_debug("GWV: ControllerHandler::Invoke result=0x%08lx host=%p",
		          (unsigned long) result, (void *) link_->host);
		WvHost *h = link_->host;
		if (h == nullptr)
			return S_OK;   /* host destroyed while initializing */
		if (FAILED(result) || controller == nullptr) {
			if (result == HRESULT_FROM_WIN32(ERROR_BUSY) &&
			    h->controller_retries < kMaxControllerRetries) {
				h->controller_retries++;
				g_debug("GWV: controller busy (0x%08lx); retry %d/%d in %ums",
				        (unsigned long) result, h->controller_retries,
				        kMaxControllerRetries, kControllerRetryMs);
				link_ref(link_);
				g_timeout_add(kControllerRetryMs, retry_controller_cb, link_);
				return S_OK;
			}
			host_fail(h, "WebView2 controller creation failed");
			return S_OK;
		}

		h->controller = controller;
		h->controller->AddRef();
		h->controller->get_CoreWebView2(&h->core);

		/* Enable the JS->native message channel and subscribe. */
		ICoreWebView2Settings *settings = nullptr;
		if (SUCCEEDED(h->core->get_Settings(&settings)) && settings != nullptr) {
			settings->put_IsWebMessageEnabled(TRUE);
			/* Browser accelerator keys (Ctrl+K, Ctrl+F, Ctrl+P, F5, …) would
			 * swallow combos before the page sees them — in the terminal those
			 * belong to the shell (the WebView2 counterpart of the GTK key
			 * snooper). Browsing views keep them: Ctrl+F find-in-page, F5,
			 * F12 devtools are the point of a browser pane, and they only
			 * fire while that pane has focus (per-instance setting — Geany's
			 * own keybindings are untouched). Editing shortcuts (Ctrl+C/V/X)
			 * are unaffected either way. */
			if (!h->cfg_allow_browsing) {
				ICoreWebView2Settings3 *s3 = nullptr;
				if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings3),
				                                       reinterpret_cast<void **>(&s3))) &&
				    s3 != nullptr) {
					s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
					s3->Release();
				}
			}
			settings->Release();
		}
		WebMessageHandler *mh = new WebMessageHandler(h->link);
		h->core->add_WebMessageReceived(mh, &h->msg_token);
		mh->Release();

		/* Keep GTK's focus model in sync (see FocusChangedHandler). */
		FocusChangedHandler *fh = new FocusChangedHandler(h->link);
		h->controller->add_GotFocus(fh, &h->focus_token);
		fh->Release();

		/* Open external links in the OS browser instead of navigating away
		 * (browsing views navigate freely — the handlers check the config). */
		NavigationStartingHandler *nh = new NavigationStartingHandler(h->link);
		h->core->add_NavigationStarting(nh, &h->nav_token);
		nh->Release();
		NewWindowRequestedHandler *nwh = new NewWindowRequestedHandler(h->link);
		h->core->add_NewWindowRequested(nwh, &h->newwin_token);
		nwh->Release();
		if (h->cb.on_url_changed != nullptr) {
			SourceChangedHandler *sch = new SourceChangedHandler(h->link);
			h->core->add_SourceChanged(sch, &h->src_token);
			sch->Release();
		}

		/* Serve all https://<virtual_host>/ requests from embedded assets +
		 * published in-memory documents via request interception (folder
		 * mapping only remains for map_dir'd disk hosts, e.g. the doc's dir). */
		if (h->cfg_virtual_host != nullptr) {
			gchar *filter8 = g_strdup_printf("https://%s/*", h->cfg_virtual_host);
			wchar_t *filter_w = u8_to_w(filter8);
			HRESULT fr = h->core->AddWebResourceRequestedFilter(
				filter_w, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
			WebResourceRequestedHandler *wh = new WebResourceRequestedHandler(h->link);
			h->core->add_WebResourceRequested(wh, &h->webres_token);
			wh->Release();
			g_debug("GWV: asset interception on '%s' hr=0x%08lx",
			        filter8, (unsigned long) fr);
			g_free(filter_w);
			g_free(filter8);
		}

		host_update_bounds(h);
		h->controller->put_IsVisible(TRUE);
		g_debug("GWV: controller ready");

		/* Inject the bridge shim before any page loads, then go ready inside
		 * its completion (so the first navigation already has the bridge).
		 * With no inject script, become ready immediately. */
		if (h->cfg_inject_js != nullptr) {
			wchar_t *js = u8_to_w(h->cfg_inject_js);
			AddScriptHandler *ash = new AddScriptHandler(h->link);
			h->core->AddScriptToExecuteOnDocumentCreated(js, ash);
			ash->Release();
			g_free(js);
		} else {
			host_finish_ready(h);
		}
		return S_OK;
	}
};

/* (Re)create the controller using the already-created environment. */
static void host_create_controller(WvHost *h)
{
	HWND hwnd = host_hwnd(h);
	if (hwnd == nullptr) {
		host_fail(h, "host widget has no native window");
		return;
	}
	h->container_hwnd = hwnd;
	ControllerHandler *ch = new ControllerHandler(h->link);
	HRESULT hr = h->env->CreateCoreWebView2Controller(hwnd, ch);
	ch->Release();
	if (FAILED(hr))
		host_fail(h, "CreateCoreWebView2Controller failed");
}

/* One-shot timer body for an ERROR_BUSY retry. */
static gboolean retry_controller_cb(gpointer data)
{
	HostLink *l = static_cast<HostLink *>(data);
	WvHost   *h = l->host;
	if (h != nullptr && h->controller == nullptr && h->env != nullptr)
		host_create_controller(h);
	link_unref(l);
	return G_SOURCE_REMOVE;
}

/* Final step of bring-up: drop the placeholder, flush queued content, notify. */
static void host_finish_ready(WvHost *h)
{
	if (h->draw_id != 0) {
		g_signal_handler_disconnect(h->container, h->draw_id);
		h->draw_id = 0;
	}
	h->ready = TRUE;

	if (h->pending_html != nullptr) {
		g_debug("GWV: navigate set_html (%zu bytes)", strlen(h->pending_html));
		wchar_t *w = u8_to_w(h->pending_html);
		h->core->NavigateToString(w);
		g_free(w); g_free(h->pending_html); h->pending_html = nullptr;
	} else if (h->pending_url != nullptr) {
		g_debug("GWV: navigate %s", h->pending_url);
		wchar_t *w = u8_to_w(h->pending_url);
		h->core->Navigate(w);
		g_free(w); g_free(h->pending_url); h->pending_url = nullptr;
	}

	if (h->cb.on_ready != nullptr)
		h->cb.on_ready(h, h->user);
}

/* Drain the queue of hosts waiting for the shared environment. */
static void env_drain_waiters(gboolean ok)
{
	GSList *waiters = s_env_waiters;
	s_env_waiters = nullptr;
	for (GSList *l = waiters; l != nullptr; l = l->next) {
		HostLink *link = static_cast<HostLink *>(l->data);
		if (link->host != nullptr) {
			if (ok) {
				link->host->env = s_env;   /* borrowed; never released by host */
				host_create_controller(link->host);
			} else {
				host_fail(link->host, "WebView2 environment creation failed");
			}
		}
		link_unref(link);
	}
	g_slist_free(waiters);
}

class EnvHandler
	: public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
	LONG ref_ = 1;
public:
	EnvHandler() {}
	virtual ~EnvHandler() {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (ppv == nullptr)
			return E_POINTER;
		if (IsEqualGUID(riid, IID_IUnknown) ||
		    IsEqualGUID(riid, __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler))) {
			*ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
			InterlockedIncrement(&ref_);
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef(void) override { return InterlockedIncrement(&ref_); }
	ULONG STDMETHODCALLTYPE Release(void) override
	{
		LONG r = InterlockedDecrement(&ref_);
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment *env) override
	{
		g_debug("GWV: EnvHandler::Invoke result=0x%08lx", (unsigned long) result);
		s_env_creating = false;
		if (SUCCEEDED(result) && env != nullptr) {
			s_env = env;
			s_env->AddRef();
			env_drain_waiters(TRUE);
		} else {
			env_drain_waiters(FALSE);
		}
		return S_OK;
	}
};

/* ------------------------------ creation ---------------------------- */

static void host_request_controller(WvHost *h)
{
	g_debug("GWV: host_request_controller (s_env=%p)", (void *) s_env);
	ensure_loader();
	if (s_create_env == nullptr) {
		host_fail(h, "WebView2 runtime not found (install the Evergreen Runtime)");
		return;
	}

	/* Shared environment already up — go straight to a controller. */
	if (s_env != nullptr) {
		h->env = s_env;
		host_create_controller(h);
		return;
	}

	/* Queue for the single, shared environment creation. */
	link_ref(h->link);
	s_env_waiters = g_slist_append(s_env_waiters, h->link);
	if (s_env_creating)
		return;
	s_env_creating = true;

	gchar *cache = user_data_folder();
	wchar_t *cache_w = u8_to_w(cache);

	EnvHandler *eh = new EnvHandler();
	HRESULT hr = s_create_env(nullptr, cache_w, nullptr, eh);
	eh->Release();
	g_debug("GWV: CreateCoreWebView2EnvironmentWithOptions -> hr=0x%08lx (udf=%s)",
	        (unsigned long) hr, cache);

	g_free(cache_w);
	g_free(cache);

	if (FAILED(hr)) {
		s_env_creating = false;
		env_drain_waiters(FALSE);
	}
}

static void on_realize(GtkWidget *w, gpointer data)
{
	(void) w;
	WvHost *h = static_cast<WvHost *>(data);
	if (h->env == nullptr && h->controller == nullptr)
		host_request_controller(h);
}

static void on_size_allocate(GtkWidget *w, GdkRectangle *alloc, gpointer data)
{
	(void) w; (void) alloc;
	host_update_bounds(static_cast<WvHost *>(data));
}

/* Hide/show the WebView2 controller as the pane's tab is switched away/to, so
 * WebView2 can suspend rendering while off-screen. */
static void on_map(GtkWidget *w, gpointer data)
{
	(void) w;
	WvHost *h = static_cast<WvHost *>(data);
	if (h->controller != nullptr) {
		h->controller->put_IsVisible(TRUE);
		host_update_bounds(h);
	}
}

static void on_unmap(GtkWidget *w, gpointer data)
{
	(void) w;
	WvHost *h = static_cast<WvHost *>(data);
	if (h->controller != nullptr)
		h->controller->put_IsVisible(FALSE);
}

/* GTK gave the container keyboard focus (e.g. Tab): push it into the webview.
 * Suppressed while we are mirroring a WebView2 GotFocus (avoids a loop). */
static gboolean on_focus_in(GtkWidget *w, GdkEventFocus *e, gpointer data)
{
	(void) w; (void) e;
	WvHost *h = static_cast<WvHost *>(data);
	if (!h->in_focus_sync && h->controller != nullptr)
		h->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
	return FALSE;
}

/* GTK focus left the container (user clicked the editor/tabs). The webview's
 * child HWND still holds the Win32 keyboard focus, so hand it back to the
 * toplevel — but only while the toplevel is the active window (not on
 * alt-tab/deactivation). */
static gboolean on_focus_out(GtkWidget *w, GdkEventFocus *e, gpointer data)
{
	(void) w; (void) e;
	WvHost *h = static_cast<WvHost *>(data);
	HWND top = host_toplevel_hwnd(h);
	if (top != nullptr && GetActiveWindow() == top)
		SetFocus(top);
	return FALSE;
}

/*
 * The WebView2 is a native child HWND. When it takes the Win32 focus, GDK sees
 * its own toplevel lose focus (WM_KILLFOCUS) and marks the whole Geany window
 * inactive — after which clicks on GTK widgets do nothing. This filter drops
 * that focus-loss when the focus is going *to* our webview, so GTK stays active
 * and keeps processing clicks; the container's focus-out handler then hands the
 * keyboard back to the toplevel when the user clicks a GTK widget. Global so it
 * also covers native child GdkWindows (e.g. Scintilla) losing focus.
 */
static GdkFilterReturn focus_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
	(void) event;
	MSG *msg = static_cast<MSG *>(xevent);
	if (msg->message == WM_KILLFOCUS) {
		WvHost *h = static_cast<WvHost *>(data);
		HWND cw = h->container_hwnd;
		HWND gaining = reinterpret_cast<HWND>(msg->wParam);  /* window gaining focus */
		if (cw != nullptr && gaining != nullptr &&
		    (gaining == cw || IsChild(cw, gaining)))
			return GDK_FILTER_REMOVE;
	}
	return GDK_FILTER_CONTINUE;
}

/* Map/re-map a virtual host to a folder (e.g. the current document's directory)
 * so its images/files load from the page. Needs ICoreWebView2_3; no-op until the
 * engine is up. */
extern "C" void wv_host_map_dir(WvHost *h, const char *host_name, const char *folder)
{
	if (h == nullptr || h->core == nullptr || host_name == nullptr)
		return;
	ICoreWebView2_3 *c3 = nullptr;
	if (FAILED(h->core->QueryInterface(__uuidof(ICoreWebView2_3),
	                                   reinterpret_cast<void **>(&c3))) || c3 == nullptr)
		return;
	wchar_t *host_w = u8_to_w(host_name);
	c3->ClearVirtualHostNameToFolderMapping(host_w);      /* drop any prior mapping */
	if (folder != nullptr && *folder != '\0') {
		wchar_t *folder_w = u8_to_w(folder);
		HRESULT mr = c3->SetVirtualHostNameToFolderMapping(
			host_w, folder_w, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
		g_debug("GWV: map_dir '%s' -> '%s' hr=0x%08lx",
		        host_name, folder, (unsigned long) mr);
		g_free(folder_w);
	}
	g_free(host_w);
	c3->Release();
}

extern "C" void wv_host_put_virtual(WvHost *h, const char *path,
                                    const char *data, gssize len, const char *mime)
{
	if (h == nullptr || path == nullptr || h->virtuals == nullptr)
		return;
	if (data == nullptr) {
		g_hash_table_remove(h->virtuals, path);
		return;
	}
	VirtualDoc *doc = g_new0(VirtualDoc, 1);
	doc->bytes = g_bytes_new(data, len < 0 ? strlen(data) : (gsize) len);
	doc->mime = g_strdup(mime != nullptr ? mime : "text/html");
	g_hash_table_replace(h->virtuals, g_strdup(path), doc);
}

/* ------------------------------- C ABI ------------------------------ */

extern "C" WvHost *wv_host_new(GtkWidget *container, const WvHostConfig *config,
                               const WvHostCallbacks *cb, gpointer user)
{
	WvHost *h = g_new0(WvHost, 1);
	h->container = container;
	if (cb != nullptr)
		h->cb = *cb;
	h->user = user;
	host_copy_config(h, config);
	h->link = link_new(h);
	h->virtuals = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                    g_free, virtual_doc_free);

	gtk_widget_set_can_focus(container, TRUE);
	h->draw_id    = g_signal_connect(container, "draw", G_CALLBACK(placeholder_draw), h);
	h->realize_id = g_signal_connect(container, "realize", G_CALLBACK(on_realize), h);
	h->size_id    = g_signal_connect(container, "size-allocate", G_CALLBACK(on_size_allocate), h);
	g_signal_connect(container, "map", G_CALLBACK(on_map), h);
	g_signal_connect(container, "unmap", G_CALLBACK(on_unmap), h);
	g_signal_connect(container, "focus-in-event", G_CALLBACK(on_focus_in), h);
	g_signal_connect(container, "focus-out-event", G_CALLBACK(on_focus_out), h);

	gdk_window_add_filter(nullptr, focus_filter, h);   /* global mouse-down filter */
	h->focus_filter_on = TRUE;

	if (gtk_widget_get_realized(container))
		host_request_controller(h);
	gtk_widget_queue_draw(container);
	return h;
}

extern "C" void wv_host_navigate(WvHost *h, const char *url)
{
	if (h == nullptr || url == nullptr)
		return;
	if (h->ready && h->core != nullptr) {
		wchar_t *w = u8_to_w(url);
		h->core->Navigate(w);
		g_free(w);
	} else {
		g_free(h->pending_url);
		h->pending_url = g_strdup(url);
	}
}

extern "C" void wv_host_set_html(WvHost *h, const char *html)
{
	if (h == nullptr || html == nullptr)
		return;
	if (h->ready && h->core != nullptr) {
		wchar_t *w = u8_to_w(html);
		h->core->NavigateToString(w);
		g_free(w);
	} else {
		g_free(h->pending_html);
		h->pending_html = g_strdup(html);
	}
}

extern "C" void wv_host_post_message(WvHost *h, const char *json)
{
	if (h == nullptr || json == nullptr || !h->ready || h->core == nullptr)
		return;
	wchar_t *w = u8_to_w(json);
	h->core->PostWebMessageAsString(w);
	g_free(w);
}

/* Deferred warmup: the toplevel is up now, so realizing the (possibly still
 * hidden) container is harmless — do it and drop the one-shot handler. */
static void on_toplevel_map_warmup(GtkWidget *top, gpointer data)
{
	WvHost *h = static_cast<WvHost *>(data);
	g_signal_handler_disconnect(top, h->warmup_map_id);
	h->warmup_map_id = 0;
	h->warmup_toplevel = nullptr;
	if (h->container != nullptr && !gtk_widget_get_realized(h->container))
		gtk_widget_realize(h->container);
}

extern "C" void wv_host_warmup(WvHost *h)
{
	/* Realizing the container yields its HWND, which kicks off controller
	 * creation (the on_realize path). */
	if (h == nullptr || h->container == nullptr ||
	    gtk_widget_get_realized(h->container))
		return;
	/* At startup the plugin initializes before Geany shows its window.
	 * Realizing the container now would drag the whole unrealized toplevel
	 * into realization, forcing a size-allocate pass at a guessed (natural,
	 * i.e. much too small) window size that clobbers Geany's restored paned
	 * positions — the message window came up collapsed to the bottom. Defer
	 * the warmup to the toplevel's map instead, which keeps the view eager
	 * (the engine still spins up during startup, right after the window is
	 * shown at its real size). */
	GtkWidget *top = gtk_widget_get_toplevel(h->container);
	if (top != nullptr && gtk_widget_is_toplevel(top) &&
	    !gtk_widget_get_realized(top)) {
		if (h->warmup_map_id == 0) {
			h->warmup_toplevel = top;
			h->warmup_map_id = g_signal_connect(top, "map",
				G_CALLBACK(on_toplevel_map_warmup), h);
		}
		return;
	}
	gtk_widget_realize(h->container);
}

extern "C" void wv_host_focus(WvHost *h)
{
	if (h == nullptr)
		return;
	if (h->controller != nullptr)
		h->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
	else if (h->container != nullptr)
		gtk_widget_grab_focus(h->container);
}

extern "C" void wv_host_set_visible(WvHost *h, gboolean visible)
{
	if (h != nullptr && h->controller != nullptr)
		h->controller->put_IsVisible(visible ? TRUE : FALSE);
}

extern "C" void wv_host_go_back(WvHost *h)
{
	if (h != nullptr && h->core != nullptr)
		h->core->GoBack();
}

extern "C" void wv_host_go_forward(WvHost *h)
{
	if (h != nullptr && h->core != nullptr)
		h->core->GoForward();
}

extern "C" void wv_host_reload(WvHost *h)
{
	if (h != nullptr && h->core != nullptr)
		h->core->Reload();
}

/* WebView2 ships the browser's own find bar (Ctrl+F is a browser accelerator,
 * kept enabled for browsing views) — no plugin-side find UI or API needed. */
extern "C" gboolean wv_host_needs_find_ui(void) { return FALSE; }
extern "C" void wv_host_find(WvHost *, const char *) {}
extern "C" void wv_host_find_next(WvHost *, gboolean) {}
extern "C" void wv_host_find_stop(WvHost *) {}

extern "C" void wv_host_destroy(WvHost *h)
{
	if (h == nullptr)
		return;
	g_debug("GWV: wv_host_destroy (controller=%p)", (void *) h->controller);

	if (h->focus_filter_on) {
		gdk_window_remove_filter(nullptr, focus_filter, h);
		h->focus_filter_on = FALSE;
	}
	if (h->warmup_map_id != 0 && h->warmup_toplevel != nullptr)
		g_signal_handler_disconnect(h->warmup_toplevel, h->warmup_map_id);
	if (h->container != nullptr)
		g_signal_handlers_disconnect_by_data(h->container, h);

	if (h->core != nullptr && h->webres_token.value != 0)
		h->core->remove_WebResourceRequested(h->webres_token);
	if (h->core != nullptr && h->newwin_token.value != 0)
		h->core->remove_NewWindowRequested(h->newwin_token);
	if (h->core != nullptr && h->src_token.value != 0)
		h->core->remove_SourceChanged(h->src_token);
	if (h->controller != nullptr) {
		h->controller->Close();
		h->controller->Release();
	}
	if (h->core != nullptr)
		h->core->Release();
	/* h->env is the shared, process-wide environment (borrowed) — never
	 * released here; that is what makes re-enable safe and fast. */

	if (h->link != nullptr) {
		h->link->host = nullptr;   /* neutralize any in-flight callback */
		link_unref(h->link);
	}
	g_free(h->pending_url);
	g_free(h->pending_html);
	g_clear_pointer(&h->virtuals, g_hash_table_unref);
	host_free_config(h);
	g_free(h);
}

extern "C" gboolean wv_host_runtime_available(char **version_out)
{
	ensure_loader();
	if (s_get_version == nullptr) {
		if (version_out != nullptr)
			*version_out = g_strdup("WebView2Loader.dll not found");
		return FALSE;
	}
	LPWSTR ver = nullptr;
	HRESULT hr = s_get_version(nullptr, &ver);
	if (SUCCEEDED(hr) && ver != nullptr) {
		if (version_out != nullptr)
			*version_out = w_to_u8(ver);
		CoTaskMemFree(ver);
		return TRUE;
	}
	if (ver != nullptr)
		CoTaskMemFree(ver);
	if (version_out != nullptr)
		*version_out = g_strdup("WebView2 Runtime not installed");
	return FALSE;
}

/* ==================================================================== */
#else  /* !HAVE_WEBVIEW2 */
/* ============================ stub backend ========================== */

extern "C" WvHost *wv_host_new(GtkWidget *container, const WvHostConfig *config,
                               const WvHostCallbacks *cb, gpointer user)
{
	WvHost *h = g_new0(WvHost, 1);
	h->container = container;
	if (cb != nullptr)
		h->cb = *cb;
	h->user = user;
	host_copy_config(h, config);
	h->draw_id = g_signal_connect(container, "draw", G_CALLBACK(placeholder_draw), h);
	gtk_widget_queue_draw(container);
	return h;
}

extern "C" void wv_host_navigate(WvHost *, const char *) {}
extern "C" void wv_host_set_html(WvHost *, const char *) {}
extern "C" void wv_host_post_message(WvHost *, const char *) {}
extern "C" void wv_host_map_dir(WvHost *, const char *, const char *) {}
extern "C" void wv_host_put_virtual(WvHost *, const char *, const char *, gssize, const char *) {}
extern "C" void wv_host_warmup(WvHost *) {}
extern "C" void wv_host_focus(WvHost *h)
{
	if (h != nullptr && h->container != nullptr)
		gtk_widget_grab_focus(h->container);
}
extern "C" void wv_host_set_visible(WvHost *, gboolean) {}
extern "C" void wv_host_go_back(WvHost *) {}
extern "C" void wv_host_go_forward(WvHost *) {}
extern "C" void wv_host_reload(WvHost *) {}
extern "C" gboolean wv_host_needs_find_ui(void) { return FALSE; }
extern "C" void wv_host_find(WvHost *, const char *) {}
extern "C" void wv_host_find_next(WvHost *, gboolean) {}
extern "C" void wv_host_find_stop(WvHost *) {}
extern "C" void wv_host_destroy(WvHost *h)
{
	if (h == nullptr)
		return;
	if (h->container != nullptr)
		g_signal_handlers_disconnect_by_data(h->container, h);
	host_free_config(h);
	g_free(h);
}
extern "C" gboolean wv_host_runtime_available(char **version_out)
{
	if (version_out != nullptr)
		*version_out = g_strdup("stub backend (WebView2 not compiled in)");
	return FALSE;
}

#endif /* HAVE_WEBVIEW2 */
