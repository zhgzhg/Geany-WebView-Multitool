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
			rawSend(JSON.stringify({ ch: channel, p: payload === undefined ? null : payload }));
		},
		on: function (channel, cb) { handlers[channel] = cb; return this; },
		ready: Promise.resolve()   // the shim is live the moment this runs
	};

	// Tell native the view has booted.
	window.addEventListener("DOMContentLoaded", function () {
		window.bridge.post("sys.ready", { url: location.href });
	});
})();
