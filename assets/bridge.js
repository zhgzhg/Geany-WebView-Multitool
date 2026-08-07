/*
 * bridge.js — injected into every view before its own scripts run
 * (WebView2: AddScriptToExecuteOnDocumentCreated; WebKitGTK: user script).
 *
 * Exposes one small, platform-agnostic API:
 *   bridge.post(channel, payload)   send to native
 *   bridge.on(channel, cb)          receive from native   (cb(payload))
 *   bridge.ready                    Promise resolved once the shim is live
 *
 * Views code against this only; they never touch chrome.webview / webkit.*.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
(function () {
	"use strict";
	/* Top-level documents only. All of the plugin's own views are top-level
	 * pages; the one iframe (the sandboxed HTML preview) must see neither the
	 * bridge API nor the token below. WebKitGTK injects this script into the
	 * top frame only, but WebView2 injects into child frames too — bail
	 * before the token is captured by anything reachable. */
	if (window.self !== window.top)
		return;
	/* Per-view secret, substituted by the native side (view.c). Envelopes
	 * must echo it or native drops them, so content that reaches the raw
	 * message channel without this shim (WebKitGTK exposes the handler in
	 * EVERY frame, including the sandboxed HTML-preview iframe) cannot speak
	 * to the plugin. */
	var TOKEN = "__GWV_TOKEN__";
	var handlers = {};

	function rawSend(text) {
		if (window.chrome && window.chrome.webview) {
			window.chrome.webview.postMessage(text);           // WebView2
		} else if (window.webkit && window.webkit.messageHandlers &&
		           window.webkit.messageHandlers.bridge) {
			window.webkit.messageHandlers.bridge.postMessage(text); // WebKitGTK [M4]
		}
	}

	function onRaw(text) {
		var m;
		try { m = JSON.parse(text); } catch (e) { return; }
		if (m && m.ch && handlers[m.ch]) {
			handlers[m.ch](m.p);
		}
	}

	if (window.chrome && window.chrome.webview) {
		window.chrome.webview.addEventListener("message", function (e) { onRaw(e.data); });
	}
	// WebKitGTK delivers native->JS via evaluate_javascript calling
	// window.__bridgeRecv(text); expose it for that backend. [M4]
	window.__bridgeRecv = onRaw;

	window.bridge = {
		post: function (channel, payload) {
			rawSend(JSON.stringify({ t: TOKEN, ch: channel,
			                         p: payload === undefined ? null : payload }));
		},
		on: function (channel, cb) { handlers[channel] = cb; return this; },
		ready: Promise.resolve()   // the shim is live the moment this runs
	};

	// Tell native the view has booted.
	window.addEventListener("DOMContentLoaded", function () {
		window.bridge.post("sys.ready", { url: location.href });
	});
})();
