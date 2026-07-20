/*
 * terminal view — xterm.js wired to the native ConPTY over window.bridge.
 * The page multiplexes up to 8 terminal instances (a tab row appears for > 1);
 * every pty.* payload carries the instance id, and the native side keeps one
 * PTY per id. Background instances tint their tab on new output.
 *
 * Channels:
 *   -> term.init  {}             ask native for the configuration
 *   <- term.config{count}        instance count (also pushed on settings apply)
 *   -> pty.start  {id,cols,rows} ask native to spawn the shell for id
 *   -> pty.data   {id,data}      keyboard/paste input (UTF-8 bytes, base64)
 *   -> pty.resize {id,cols,rows} pty resize
 *   -> pty.stop   {id}           tab removed: end that shell
 *   <- pty.data   {id,data}      shell output (UTF-8 bytes, base64)
 *   <- pty.exit   {id,code}      shell exited
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

	var tabbar = document.getElementById("tabbar");
	var termsEl = document.getElementById("terms");

	var slots = {};      /* id -> {id, btn, div, term, fit, exited, lastCols, lastRows} */
	var count = 0;
	var activeId = 0;

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

	function startShell(slot) {
		slot.exited = false;
		slot.fit.fit();
		slot.lastCols = slot.term.cols;
		slot.lastRows = slot.term.rows;
		bridge.post("pty.start", { id: slot.id, cols: slot.term.cols, rows: slot.term.rows });
	}

	/* The xterm instance is created on first activation — xterm can't measure
	 * itself inside a display:none slot. */
	function buildTerm(slot) {
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
		slot.term = term;
		slot.fit = new FitAddon.FitAddon();
		term.loadAddon(slot.fit);
		term.open(slot.div);

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

		/* PRIMARY-selection integration (native side gates it on the setting):
		 * selecting text offers it as the primary selection. Debounced — selection
		 * changes fire continuously while dragging. */
		var selTimer;
		term.onSelectionChange(function () {
			clearTimeout(selTimer);
			selTimer = setTimeout(function () {
				var sel = term.getSelection();
				if (sel) bridge.post("ui.setPrimary", sel);
			}, 150);
		});

		term.onData(function (data) {
			if (slot.exited) {           /* any key (Enter) restarts the shell */
				term.clear();
				startShell(slot);
				return;
			}
			bridge.post("pty.data", { id: slot.id, data: b64encode(data) });
		});
		term.onBinary(function (data) {
			if (slot.exited) return;
			bridge.post("pty.data", { id: slot.id, data: btoa(data) });
		});

		startShell(slot);
	}

	function applyFit(slot) {
		if (!slot || !slot.term) return;
		slot.fit.fit();
		if (slot.term.cols !== slot.lastCols || slot.term.rows !== slot.lastRows) {
			slot.lastCols = slot.term.cols;
			slot.lastRows = slot.term.rows;
			bridge.post("pty.resize", { id: slot.id, cols: slot.term.cols, rows: slot.term.rows });
		}
	}

	function activate(id) {
		var slot = slots[id];
		if (!slot) return;
		if (activeId !== id) {
			var old = slots[activeId];
			if (old) {
				old.btn.classList.remove("active");
				old.div.classList.remove("active");
			}
			activeId = id;
		}
		slot.btn.classList.add("active");
		slot.btn.classList.remove("activity");   /* seen now */
		slot.div.classList.add("active");
		if (!slot.term) buildTerm(slot);         /* first open: spawn lazily */
		else applyFit(slot);
		slot.term.focus();
	}

	function createSlot(id) {
		var btn = document.createElement("button");
		btn.textContent = String(id);
		btn.title = "Terminal " + id;
		btn.addEventListener("click", function () { activate(id); });
		tabbar.appendChild(btn);

		var div = document.createElement("div");
		div.className = "slot";
		termsEl.appendChild(div);

		slots[id] = { id: id, btn: btn, div: div, term: null, fit: null,
		              exited: false, lastCols: 0, lastRows: 0 };
	}

	function removeSlot(id) {
		var slot = slots[id];
		if (!slot) return;
		if (slot.term) bridge.post("pty.stop", { id: id });
		if (slot.term) slot.term.dispose();
		slot.btn.remove();
		slot.div.remove();
		delete slots[id];
	}

	/* Grow/shrink to the configured count (initial load and settings applies). */
	function sync(n) {
		n = Math.max(1, Math.min(8, n | 0 || 1));
		if (n === count) return;
		if (activeId > n) activate(n);           /* before the tab disappears */
		for (var id = count; id > n; id--) removeSlot(id);
		for (id = count + 1; id <= n; id++) createSlot(id);
		count = n;
		document.body.classList.toggle("tabs", count > 1);
		if (activeId === 0) activate(1);
		else applyFit(slots[activeId]);          /* tab row appeared/vanished */
	}

	bridge.on("term.config", function (p) { sync(p && p.count); });

	bridge.on("pty.data", function (p) {
		var slot = p && slots[p.id];
		if (!slot || !slot.term) return;
		slot.term.write(b64decode(p.data));
		if (p.id !== activeId)
			slot.btn.classList.add("activity");  /* unseen output */
	});
	bridge.on("pty.exit", function (p) {
		var slot = p && slots[p.id];
		if (!slot || !slot.term) return;
		slot.exited = true;
		var code = (p.code != null) ? " code " + p.code : "";
		slot.term.write("\r\n\x1b[90m[process exited" + code + "] — press Enter to restart\x1b[0m\r\n");
		if (p.id !== activeId)
			slot.btn.classList.add("activity");
	});

	/* Native answers ui.pasteTerminal / ui.pastePrimary with the text. */
	bridge.on("term.paste", function (p) {
		var slot = slots[activeId];
		if (p && p.text && slot && slot.term) slot.term.paste(p.text);
	});

	/* Middle-click paste. On the WebKitGTK platforms the native GTK handler
	 * owns button 2 and these DOM events never fire; on Windows (WebView2,
	 * native HWND input) this is the only path. preventDefault also stops
	 * Chromium's middle-click autoscroll. */
	termsEl.addEventListener("mousedown", function (e) {
		if (e.button === 1) {
			e.preventDefault();
			e.stopPropagation();
			bridge.post("ui.pastePrimary", {});
		}
	}, true);
	termsEl.addEventListener("auxclick", function (e) {
		if (e.button === 1) { e.preventDefault(); e.stopPropagation(); }
	}, true);

	/* A ResizeObserver catches message-window splitter drags too (those don't
	 * fire window 'resize'). Debounced; only the visible terminal is fitted —
	 * hidden ones re-fit when activated. */
	var resizeTimer;
	var ro = new ResizeObserver(function () {
		clearTimeout(resizeTimer);
		resizeTimer = setTimeout(function () { applyFit(slots[activeId]); }, 120);
	});
	ro.observe(termsEl);

	bridge.ready.then(function () {
		bridge.post("term.init", {});
	});
})();
