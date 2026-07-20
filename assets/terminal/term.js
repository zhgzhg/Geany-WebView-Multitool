/*
 * terminal view — xterm.js wired to the native ConPTY over window.bridge.
 *
 * Channels:
 *   -> pty.start  {cols,rows}   ask native to spawn the shell
 *   -> pty.data   "<base64>"    keyboard/paste input (UTF-8 bytes, base64)
 *   -> pty.resize {cols,rows}   pty resize
 *   <- pty.data   "<base64>"    shell output (UTF-8 bytes, base64)
 *   <- pty.exit   {code}        shell exited
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
(function () {
	"use strict";

	/* Workaround for the MacPorts webkit2-gtk JSC build (macOS, seen with
	 * 2.52.4): all numbers above 2^31 collapse to -2147483648 — including
	 * Date.now() — which breaks xterm.js mouse-selection click timing. When
	 * the clock is corrupted, substitute performance.now() (small,
	 * monotonic milliseconds since page load). */
	if (Date.now() < 0 && window.performance && performance.now() >= 0) {
		Date.now = function () { return Math.floor(performance.now()); };
	}

	var term = new Terminal({
		fontFamily: 'Consolas, "Cascadia Mono", "Courier New", monospace',
		fontSize: 13,
		cursorBlink: true,
		allowProposedApi: true,
		windowsPty: { backend: "conpty" },
		/* Explicit selectionBackground: without it xterm derives the highlight
		 * from the foreground via packed-RGBA math, and on the MacPorts JSC
		 * (numbers > 2^31 corrupt) any color with red >= 0x80 — like our
		 * #d4d4d4 foreground — yields an invalid, invisible highlight. A color
		 * with red <= 0x7f keeps the packed value in safe range everywhere. */
		theme: { background: "#1e1e1e", foreground: "#d4d4d4",
		         selectionBackground: "#264f78" }
	});
	var fit = new FitAddon.FitAddon();
	term.loadAddon(fit);
	term.open(document.getElementById("term"));
	fit.fit();

	/* Ctrl+Shift+E -> focus editor; Ctrl+Shift+C / Ctrl+Shift+V -> copy/paste
	 * via Geany's clipboard (plain Ctrl+C/V pass through to the shell).
	 * preventDefault matters: without it the event continues to WebKit's own
	 * editing commands (Ctrl+Shift+V = paste-as-plain-text into the hidden
	 * textarea), which would paste a second time. */
	term.attachCustomKeyEventHandler(function (e) {
		if (e.type === "keydown" && e.ctrlKey && e.shiftKey) {
			if (e.key === "E" || e.key === "e") {
				e.preventDefault(); e.stopPropagation();
				bridge.post("ui.focusEditor", {});
				return false;
			}
			if (e.key === "C" || e.key === "c") {
				var sel = term.getSelection();
				if (sel) {
					e.preventDefault(); e.stopPropagation();
					bridge.post("ui.copy", sel);
					return false;
				}
			}
			if (e.key === "V" || e.key === "v") {
				e.preventDefault(); e.stopPropagation();
				bridge.post("ui.pasteTerminal", {});
				return false;
			}
		}
		return true;
	});

	/* Native answers ui.pasteTerminal / ui.pastePrimary with the text. */
	bridge.on("term.paste", function (p) {
		if (p && p.text) term.paste(p.text);
	});

	/* PRIMARY-selection integration (native side gates it on the setting):
	 * selecting text offers it as the X11 primary selection. Middle-click
	 * paste is handled entirely natively (the GTK layer owns the middle
	 * button), so nothing to do here for it. Debounced — selection changes
	 * fire continuously while dragging. */
	var selTimer;
	term.onSelectionChange(function () {
		clearTimeout(selTimer);
		selTimer = setTimeout(function () {
			var sel = term.getSelection();
			if (sel) bridge.post("ui.setPrimary", sel);
		}, 150);
	});

	function b64encode(str) {
		var utf8 = new TextEncoder().encode(str);
		var bin = "";
		for (var i = 0; i < utf8.length; i++) bin += String.fromCharCode(utf8[i]);
		return btoa(bin);
	}
	function b64decode(b64) {
		var bin = atob(b64);
		var bytes = new Uint8Array(bin.length);
		for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
		return bytes;
	}

	var exited = false;
	function startShell() {
		exited = false;
		fit.fit();
		lastCols = term.cols;
		lastRows = term.rows;
		bridge.post("pty.start", { cols: term.cols, rows: term.rows });
	}

	bridge.on("pty.data", function (b64) { term.write(b64decode(b64)); });
	bridge.on("pty.exit", function (p) {
		exited = true;
		var code = (p && p.code != null) ? " code " + p.code : "";
		term.write("\r\n\x1b[90m[process exited" + code + "] — press Enter to restart\x1b[0m\r\n");
	});

	term.onData(function (data) {
		if (exited) {                    /* any key (Enter) restarts the shell */
			term.clear();
			startShell();
			return;
		}
		bridge.post("pty.data", b64encode(data));
	});
	term.onBinary(function (data) {
		if (exited) return;
		bridge.post("pty.data", btoa(data));
	});

	var resizeTimer, lastCols = 0, lastRows = 0;
	function applyFit() {
		fit.fit();
		if (term.cols !== lastCols || term.rows !== lastRows) {
			lastCols = term.cols;
			lastRows = term.rows;
			bridge.post("pty.resize", { cols: term.cols, rows: term.rows });
		}
	}
	/* A ResizeObserver catches message-window splitter drags too (those don't
	 * fire window 'resize'). Debounced. */
	var ro = new ResizeObserver(function () {
		clearTimeout(resizeTimer);
		resizeTimer = setTimeout(applyFit, 120);
	});
	ro.observe(document.getElementById("term"));

	bridge.ready.then(function () {
		startShell();
		term.focus();
	});
})();
