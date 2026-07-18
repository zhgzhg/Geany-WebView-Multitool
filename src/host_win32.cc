/*
 * host_win32.cc — Windows WebView host backend (C++14), exposing the C ABI
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
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "wvhost.h"

#include <glib.h>

#ifdef HAVE_WEBVIEW2
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <gdk/gdkwin32.h>
#  include "WebView2.h"
#  include <string>
#  include <cstring>

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
#endif /* HAVE_WEBVIEW2 */

/* ------------------------------------------------------------------ common */

struct WvHost {
	GtkWidget       *container;
	WvHostCallbacks  cb;
	gpointer         user;
	gulong           draw_id;      /* placeholder "draw" handler, 0 if none */
	/* config (copied), NULL if unset */
	gchar           *cfg_virtual_host;
	gchar           *cfg_asset_root;
	gchar           *cfg_inject_js;
#ifdef HAVE_WEBVIEW2
	gulong           realize_id;
	gulong           size_id;
	struct HostLink *link;         /* async-safe back-reference             */
	ICoreWebView2Environment *env;
	ICoreWebView2Controller  *controller;
	ICoreWebView2            *core;
	EventRegistrationToken    msg_token;
	int              controller_retries;
	gboolean         ready;
	gchar           *pending_url;  /* navigate requested before ready       */
	gchar           *pending_html; /* set_html requested before ready       */
#endif
};

static void host_copy_config(WvHost *h, const WvHostConfig *cfg)
{
	if (cfg == NULL)
		return;
	h->cfg_virtual_host = g_strdup(cfg->virtual_host);
	h->cfg_asset_root   = g_strdup(cfg->asset_root);
	h->cfg_inject_js    = g_strdup(cfg->inject_js);
}

static void host_free_config(WvHost *h)
{
	g_free(h->cfg_virtual_host);
	g_free(h->cfg_asset_root);
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
static const int   kMaxControllerRetries = 8;
static const guint kControllerRetryMs    = 150;
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
			settings->Release();
		}
		WebMessageHandler *mh = new WebMessageHandler(h->link);
		h->core->add_WebMessageReceived(mh, &h->msg_token);
		mh->Release();

		/* Serve local assets at https://<virtual_host>/ (ICoreWebView2_3). */
		if (h->cfg_virtual_host != nullptr && h->cfg_asset_root != nullptr) {
			ICoreWebView2_3 *c3 = nullptr;
			HRESULT qi = h->core->QueryInterface(__uuidof(ICoreWebView2_3),
			                                     reinterpret_cast<void **>(&c3));
			if (SUCCEEDED(qi) && c3 != nullptr) {
				wchar_t *host_w = u8_to_w(h->cfg_virtual_host);
				wchar_t *root_w = u8_to_w(h->cfg_asset_root);
				HRESULT mr = c3->SetVirtualHostNameToFolderMapping(
					host_w, root_w, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
				g_debug("GWV: vhost '%s' -> '%s' hr=0x%08lx",
				        h->cfg_virtual_host, h->cfg_asset_root, (unsigned long) mr);
				g_free(host_w); g_free(root_w);
				c3->Release();
			} else {
				g_debug("GWV: QI ICoreWebView2_3 failed hr=0x%08lx", (unsigned long) qi);
			}
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

	gchar *cache = g_build_filename(g_get_user_cache_dir(), "geany-webview", nullptr);
	g_mkdir_with_parents(cache, 0700);
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

	h->draw_id    = g_signal_connect(container, "draw", G_CALLBACK(placeholder_draw), h);
	h->realize_id = g_signal_connect(container, "realize", G_CALLBACK(on_realize), h);
	h->size_id    = g_signal_connect(container, "size-allocate", G_CALLBACK(on_size_allocate), h);
	g_signal_connect(container, "map", G_CALLBACK(on_map), h);
	g_signal_connect(container, "unmap", G_CALLBACK(on_unmap), h);

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

extern "C" void wv_host_destroy(WvHost *h)
{
	if (h == nullptr)
		return;
	g_debug("GWV: wv_host_destroy (controller=%p)", (void *) h->controller);

	if (h->container != nullptr)
		g_signal_handlers_disconnect_by_data(h->container, h);

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
extern "C" void wv_host_focus(WvHost *h)
{
	if (h != nullptr && h->container != nullptr)
		gtk_widget_grab_focus(h->container);
}
extern "C" void wv_host_set_visible(WvHost *, gboolean) {}
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
