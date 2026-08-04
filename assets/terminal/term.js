/*
 * terminal view — xterm.js wired to the native ConPTY over window.bridge.
 * The page multiplexes up to 8 terminal instances (a tab row appears for > 1);
 * every pty.* payload carries the instance id, and the native side keeps one
 * PTY per id. Background instances tint their tab on new output. Beyond the
 * configured count, a "+" button after the last tab opens session-only extra
 * tabs (same cap of 8, the setting is never touched) and a trash button at
 * the row's right edge — shown only while such a tab is active — closes it.
 *
 * Channels:
 *   -> term.init  {}             ask native for the configuration
 *   <- term.config{count,fontSize,fontFamily,scrollback,search}
 *                                 instance count, font, scrollback depth and
 *                                 whether Ctrl+F opens the find bar (all
 *                                 re-pushed on settings apply; with search
 *                                 off, Ctrl+F goes to the shell)
 *   -> pty.start  {id,cols,rows} ask native to spawn the shell for id
 *   -> pty.data   {id,data}      keyboard/paste input (UTF-8 bytes, base64)
 *   -> pty.resize {id,cols,rows} pty resize
 *   -> pty.stop   {id}           tab removed: end that shell
 *   <- pty.data   {id,data}      shell output (UTF-8 bytes, base64)
 *   <- pty.exit   {id,code}      shell exited
 *
 * SPDX-License-Identifier: GPL-2.0-only
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

	var slots = {};      /* id -> {id, btn, div, term, fit, search, exited, lastCols, lastRows} */
	var count = 0;       /* configured instance count (term.config) */
	var activeId = 0;
	var MAXT = 8;        /* total-tab cap — mirrors the settings clamp */
	var dynIds = {};     /* id -> true for "+"-added, session-only tabs */

	/* ------------------------- find in scrollback ------------------------- */

	var findbar   = document.getElementById("findbar");
	var findInput = document.getElementById("find-input");
	var findCount = document.getElementById("find-count");
	var searchEnabled = true;   /* replaced by term.config (the native setting) */

	/* Decoration colors stay red <= 0x7f: on the MacPorts JSC (numbers > 2^31
	 * corrupt) higher reds overflow xterm's packed-RGBA math — see the
	 * selectionBackground note below. */
	function searchOpts(incremental) {
		return { incremental: !!incremental,
		         decorations: { matchBackground: "#314365",
		                        activeMatchBackground: "#515c6a",
		                        matchOverviewRuler: "#264f78",
		                        activeMatchColorOverviewRuler: "#6b6b6b" } };
	}

	/* Search the ACTIVE terminal. Incremental (while typing) extends the match
	 * under the cursor instead of jumping to the next one. */
	function runFind(incremental, backwards) {
		var slot = slots[activeId];
		if (!slot || !slot.term || !slot.search) return;
		if (!findInput.value) {
			slot.search.clearDecorations();
			findCount.textContent = "";
			return;
		}
		if (backwards) slot.search.findPrevious(findInput.value, searchOpts(incremental));
		else           slot.search.findNext(findInput.value, searchOpts(incremental));
	}

	function openFind() {
		findbar.classList.add("open");
		findInput.focus();
		findInput.select();
		if (findInput.value) runFind(true);   /* re-highlight the previous term */
	}

	function closeFind(refocus) {
		if (!findbar.classList.contains("open")) return;
		findbar.classList.remove("open");
		findCount.textContent = "";
		for (var id in slots) {
			if (slots[id].search) slots[id].search.clearDecorations();
		}
		var slot = slots[activeId];
		if (refocus !== false && slot && slot.term) slot.term.focus();
	}

	findInput.addEventListener("input", function () { runFind(true); });
	findInput.addEventListener("keydown", function (e) {
		if (e.key === "Enter") {
			e.preventDefault();
			runFind(false, e.shiftKey);
		} else if (e.key === "Escape") {
			e.preventDefault();
			closeFind();
		}
	});
	document.getElementById("find-prev").addEventListener("click", function () {
		runFind(false, true);
		findInput.focus();
	});
	document.getElementById("find-next").addEventListener("click", function () {
		runFind(false, false);
		findInput.focus();
	});
	document.getElementById("find-close").addEventListener("click", function () {
		closeFind();
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

	function startShell(slot) {
		slot.exited = false;
		slot.fit.fit();
		slot.lastCols = slot.term.cols;
		slot.lastRows = slot.term.rows;
		bridge.post("pty.start", { id: slot.id, cols: slot.term.cols, rows: slot.term.rows });
	}

	/* Font and scrollback: replaced by term.config before the first terminal.
	 * The configured family is prepended to the default stack, so a name the
	 * platform can't resolve still falls back to a monospace font. zoomDelta
	 * is the Ctrl+= / Ctrl+- / Ctrl+0 temporary size offset — session-only,
	 * cleared whenever the configured font changes. */
	var DEFAULT_STACK = 'Consolas, "Cascadia Mono", "Courier New", monospace';
	var fontSize = 13;
	var fontFamily = DEFAULT_STACK;
	var scrollback = 30000;
	var zoomDelta = 0;

	function effFontSize() {
		return Math.max(6, Math.min(64, fontSize + zoomDelta));
	}

	/* Push the current font onto every live terminal; hidden slots re-fit on
	 * activation, later-built ones read the variables in buildTerm(). */
	function applyFont() {
		for (var id in slots) {
			if (slots[id].term) {
				slots[id].term.options.fontSize = effFontSize();
				slots[id].term.options.fontFamily = fontFamily;
			}
		}
		applyFit(slots[activeId]);
	}

	/* The xterm instance is created on first activation — xterm can't measure
	 * itself inside a display:none slot. */
	function buildTerm(slot) {
		var term = new Terminal({
			fontFamily: fontFamily,
			fontSize: effFontSize(),
			scrollback: scrollback,
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
		slot.search = new SearchAddon.SearchAddon();
		term.loadAddon(slot.search);
		/* Match position label ("3/12"); fires only with decorations on. */
		slot.search.onDidChangeResults(function (r) {
			if (slot.id !== activeId || !findbar.classList.contains("open"))
				return;
			if (!findInput.value || !r || !r.resultCount)
				findCount.textContent = findInput.value ? "0/0" : "";
			else
				findCount.textContent = (r.resultIndex + 1) + "/" + r.resultCount;
		});
		term.open(slot.div);

		/* Ctrl+F -> find bar (only while the native setting allows it — off, the
		 * chord belongs to whatever runs in the shell); Escape closes it even
		 * when focus moved back into the terminal.
		 * Ctrl+= / Ctrl+- / Ctrl+0 -> temporary font zoom (Ctrl+Shift+- comes
		 * through as key "_", so readline's Ctrl+_ undo is untouched; "+" is
		 * shifted "=" on many layouts, so shift is not filtered here).
		 * Ctrl+Shift+E -> focus editor; Ctrl+Shift+C / Ctrl+Shift+V -> copy/paste
		 * via Geany's clipboard (plain Ctrl+C/V pass through to the shell).
		 * preventDefault matters: without it the event continues to WebKit's own
		 * editing commands (Ctrl+Shift+V = paste-as-plain-text into the hidden
		 * textarea), which would paste a second time. */
		term.attachCustomKeyEventHandler(function (e) {
			if (e.type === "keydown" && searchEnabled && e.ctrlKey &&
			    !e.shiftKey && !e.altKey && (e.key === "f" || e.key === "F")) {
				e.preventDefault(); e.stopPropagation();
				openFind();
				return false;
			}
			if (e.type === "keydown" && e.ctrlKey && !e.altKey) {
				var zoomed = true;
				if (e.key === "+" || e.key === "=" || e.code === "NumpadAdd")
					zoomDelta = Math.min(zoomDelta + 1, 64 - fontSize);
				else if (e.key === "-" || e.code === "NumpadSubtract")
					zoomDelta = Math.max(zoomDelta - 1, 6 - fontSize);
				else if (e.key === "0" || e.code === "Numpad0")
					zoomDelta = 0;
				else
					zoomed = false;
				if (zoomed) {
					e.preventDefault(); e.stopPropagation();
					applyFont();
					return false;
				}
			}
			if (e.type === "keydown" && e.key === "Escape" &&
			    findbar.classList.contains("open")) {
				e.preventDefault(); e.stopPropagation();
				closeFind();
				return false;
			}
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
		refreshBar();                            /* trash follows the active tab */
		/* An open find follows the tab: highlight in the now-visible terminal. */
		if (findbar.classList.contains("open") && findInput.value)
			runFind(true);
	}

	function createSlot(id) {
		var btn = document.createElement("button");
		btn.textContent = String(id);
		btn.title = "Terminal " + id;
		btn.addEventListener("click", function () { activate(id); });
		/* Keep the row in numeric order: a "+" tab can fill a hole a lowered
		 * instance count left behind, so plain append would misplace it. */
		var next = null;
		for (var k in slots) {
			if (slots[k].id > id && (next === null || slots[k].id < next.id))
				next = slots[k];
		}
		tabbar.insertBefore(btn, next !== null ? next.btn : plusBtn);

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

	function totalTabs() {
		return Object.keys(slots).length;
	}

	/* Bar chrome: the row shows for > 1 tab, "+" greys out at the cap, and the
	 * trash appears only while a "+"-added tab is the active one. */
	function refreshBar() {
		document.body.classList.toggle("tabs", totalTabs() > 1);
		plusBtn.disabled = totalTabs() >= MAXT;
		trashBtn.style.display = dynIds[activeId] ? "" : "none";
	}

	/* "+": open one more terminal for this session only — the configured
	 * instance count is not touched. The smallest free id is used, so ids (and
	 * tab labels) always stay within 1..MAXT. */
	function addDynamic() {
		if (totalTabs() >= MAXT) return;
		var id = 1;
		while (slots[id]) id++;
		dynIds[id] = true;
		createSlot(id);
		refreshBar();      /* the row may just have appeared: layout, then fit */
		activate(id);
	}

	/* Trash: close the active "+"-added tab (the button only shows for those)
	 * and fall back to the nearest tab — lower first, else higher. */
	function removeDynamic() {
		if (!dynIds[activeId]) return;
		var dead = activeId, fall = 0, k, id;
		delete dynIds[dead];
		removeSlot(dead);
		for (k in slots) {
			id = slots[k].id;
			if (id < dead && (fall === 0 || id > fall)) fall = id;
		}
		if (fall === 0) {
			for (k in slots) {
				id = slots[k].id;
				if (id > dead && (fall === 0 || id < fall)) fall = id;
			}
		}
		refreshBar();
		if (fall !== 0) activate(fall);
	}

	/* The controls at the ends of the row (styled in index.html by id). */
	var plusBtn = document.createElement("button");
	plusBtn.id = "tab-add";
	plusBtn.textContent = "+";
	plusBtn.title = "Open another terminal (this session only)";
	plusBtn.addEventListener("click", addDynamic);
	tabbar.appendChild(plusBtn);

	var trashBtn = document.createElement("button");
	trashBtn.id = "tab-trash";
	trashBtn.title = "Close this terminal";
	trashBtn.innerHTML =
		'<svg width="11" height="11" viewBox="0 0 24 24" fill="none"' +
		' stroke="currentColor" stroke-width="2" stroke-linecap="round">' +
		'<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v6M14 11v6"/></svg>';
	trashBtn.style.display = "none";
	trashBtn.addEventListener("click", removeDynamic);
	tabbar.appendChild(trashBtn);

	/* Grow/shrink to the configured count (initial load and settings applies).
	 * "+"-added tabs are not the config's to manage: on shrink they survive,
	 * and on grow an extra tab occupying a wanted id is adopted as a configured
	 * one — its shell keeps running — instead of being recreated. */
	function sync(n) {
		n = Math.max(1, Math.min(MAXT, n | 0 || 1));
		if (n === count) return;
		if (activeId > n && !dynIds[activeId])
			activate(n);                         /* before the tab disappears */
		for (var id = count; id > n; id--) {
			if (!dynIds[id]) removeSlot(id);
		}
		for (id = count + 1; id <= n; id++) {
			if (slots[id]) delete dynIds[id];    /* adopt a "+" tab in place */
			else createSlot(id);
		}
		count = n;
		refreshBar();                            /* row/controls before fitting */
		if (activeId === 0) activate(1);
		else applyFit(slots[activeId]);          /* tab row appeared/vanished */
	}

	bridge.on("term.config", function (p) {
		var fs = (p && p.fontSize) | 0;
		var fam = (p && typeof p.fontFamily === "string")
		          ? p.fontFamily.replace(/["\\]/g, "") : "";
		var stack = fam ? '"' + fam + '", ' + DEFAULT_STACK : DEFAULT_STACK;
		var fontChanged = false;
		if (fs >= 6 && fs <= 32 && fs !== fontSize) {
			fontSize = fs;
			fontChanged = true;
		}
		if (stack !== fontFamily) {
			fontFamily = stack;
			fontChanged = true;
		}
		if (fontChanged) {
			zoomDelta = 0;           /* the configured font is what you get */
			applyFont();
		}
		var sb = (p && p.scrollback != null) ? p.scrollback | 0 : -1;
		if (sb >= 0 && sb !== scrollback) {
			scrollback = sb;
			for (var id in slots) {
				if (slots[id].term)
					slots[id].term.options.scrollback = scrollback;
			}
		}
		searchEnabled = !(p && p.search === false);
		if (!searchEnabled) closeFind();
		sync(p && p.count);
	});

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
