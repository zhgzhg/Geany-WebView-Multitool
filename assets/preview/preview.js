/*
 * preview view — renders the active Geany document over window.bridge.
 *
 * Channels (native -> page):
 *   preview.md    {text}   render GitHub-flavored Markdown
 *   preview.html  {html}   render raw HTML in a sandboxed iframe
 *   preview.empty {}       nothing previewable
 *   preview.pin   {state}  "on"|"off" — pin-button state (native-owned)
 * Channels (page -> native):
 *   preview.setMode  "auto"|"md"|"html"   force the render mode
 *   preview.setPin   {pin:0|1}             (un)pin the shown document
 *   preview.refresh  {}                    re-render the current document
 *   preview.reloadDoc {}                   reload the shown document in Geany from disk
 *   preview.rendered {...}                 diagnostic
 *
 * GFM via markdown-it (html:true, linkify) + task-lists + heading anchors
 * (hovering a heading shows a GitHub-style chain icon that copies the
 * in-document #anchor) + GitLab-style [[_TOC_]]/[TOC] tables of contents;
 * output sanitized with DOMPurify. ```mermaid fences render as diagrams:
 * the fence emits its raw source in a <pre class="mermaid"> and the SVG is
 * injected after sanitization (mermaid's securityLevel:"strict" sanitizes
 * diagram labels itself); rendered diagrams get hover zoom controls
 * (− / % / +, the percentage resets; zoom is keyed by diagram source so it
 * survives re-renders) and, above 100%, scrollbar-less drag-to-pan.
 * In-document (#section) links scroll within the pane; external links open
 * in the OS browser (host-side).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
(function () {
	"use strict";

	/* GitHub-ish heading slugs so [x](#heading) links resolve. */
	function slugify(s) {
		return String(s).trim().toLowerCase()
			.replace(/[^\w\s-]/g, "")
			.replace(/\s+/g, "-");
	}

	var md = window.markdownit({
		html: true,
		linkify: true,
		breaks: false,
		highlight: function (str, lang) {
			if (lang && window.hljs && hljs.getLanguage(lang)) {
				try {
					return hljs.highlight(str, { language: lang, ignoreIllegals: true }).value;
				} catch (e) { /* fall through */ }
			}
			return "";
		}
	})
	.use(window.markdownitTaskLists, { enabled: true, label: true })
	.use(window.markdownItAnchor, { slugify: slugify })
	/* GitLab-style [[_TOC_]] / [TOC] on its own line becomes a nested list of
	 * links to every markdown heading (the plugin's default placeholder covers
	 * both markers, case-insensitively). The slugify MUST be the anchor
	 * plugin's, so the TOC hrefs equal the heading ids. */
	.use(window.markdownItTocDoneRight, {
		slugify: slugify,
		listType: "ul"                       /* GitLab renders an unordered list */
	});

	/* ```mermaid fences: emit the raw source in a <pre class="mermaid"> (plain
	 * text survives DOMPurify untouched) for renderMermaid() to pick up. If the
	 * vendored mermaid failed to load, fall through to a normal code block. */
	var defaultFence = md.renderer.rules.fence;
	md.renderer.rules.fence = function (tokens, idx, options, env, self) {
		var token = tokens[idx];
		var info = token.info ? token.info.trim().split(/\s+/)[0] : "";
		if (info === "mermaid" && window.mermaid)
			return '<pre class="mermaid">' + md.utils.escapeHtml(token.content) + "</pre>\n";
		return defaultFence(tokens, idx, options, env, self);
	};

	/* Diagram colors are baked into the SVG at render time, so mermaid is
	 * (re)initialized per theme and shown diagrams re-render on toggle. */
	var mermaidTheme = null;
	/* Rendered SVGs cached by diagram source: re-renders while typing swap
	 * unchanged diagrams back in synchronously — no flicker, and the layout is
	 * final before the scroll position is restored. Object.create(null) so
	 * diagram text can never collide with Object.prototype keys. */
	var svgCache = Object.create(null), svgCacheN = 0;

	/* Per-diagram zoom, keyed by diagram source like the SVG cache, so the
	 * chosen level survives re-renders while typing (and theme toggles). */
	var ZOOM_STEP = 1.25, ZOOM_MIN = 0.25, ZOOM_MAX = 4;
	var zoomMap = Object.create(null), zoomMapN = 0;
	function zoomLevel(src) { return zoomMap[src] || 1; }
	function zoomSet(src, f) {
		if (zoomMap[src] === undefined) {
			if (zoomMapN > 64) { zoomMap = Object.create(null); zoomMapN = 0; }
			zoomMapN++;
		}
		zoomMap[src] = f;
	}

	/* Zoom controls (top-right, copy-btn styling): − / percentage(reset) / +.
	 * No listeners here — clicks are delegated on #content — so the markup
	 * can ride along inside cached diagram HTML. */
	function addZoomControls(pre) {
		if (pre.querySelector(".mmd-zoom") !== null)
			return;
		var box = document.createElement("span");
		box.className = "mmd-zoom";
		box.innerHTML =
			'<button data-mmd-zoom="out" title="Zoom out">' +
			String.fromCharCode(0x2212) +               /* − (minus sign, BMP) */
			'</button>' +
			'<button data-mmd-zoom="reset" title="Reset zoom">100%</button>' +
			'<button data-mmd-zoom="in" title="Zoom in">+</button>';
		pre.appendChild(box);
	}

	/* Scale a rendered diagram by giving it an explicit CSS width derived
	 * from its viewBox — real layout, so the block scrolls naturally (unlike
	 * a transform, which would clip). f == 1 restores the SVG's own
	 * responsive style. Zoom-ins cap the block's height so a blown-up
	 * diagram pans inside its box instead of stretching the whole page. */
	function mermaidZoomApply(pre, f) {
		var svg = pre.querySelector("svg");
		if (svg === null)
			return;
		var orig = pre.getAttribute("data-mmd-style");
		if (orig === null) {
			orig = svg.getAttribute("style") || "";
			pre.setAttribute("data-mmd-style", orig);
		}
		var lbl = pre.querySelector('[data-mmd-zoom="reset"]');
		if (lbl !== null)
			lbl.textContent = Math.round(f * 100) + "%";
		svg.setAttribute("style", orig);        /* pristine, then override */
		pre.style.maxHeight = "";
		pre.classList.remove("mmd-pan");
		if (f === 1)
			return;
		var vb = (svg.viewBox !== undefined) ? svg.viewBox.baseVal : null;
		var base = (vb !== null && vb.width) || svg.getBoundingClientRect().width;
		if (!base)
			return;
		svg.style.maxWidth = "none";
		svg.style.width = Math.round(base * f) + "px";
		svg.style.height = "auto";
		if (f > 1) {
			pre.style.maxHeight = "80vh";       /* pan inside the box */
			pre.classList.add("mmd-pan");       /* grab cursor, no scrollbars */
		}
	}

	function mermaidSetTheme(theme) {
		if (!window.mermaid || theme === mermaidTheme)
			return;
		mermaidTheme = theme;
		mermaid.initialize({
			startOnLoad: false,
			securityLevel: "strict",
			suppressErrorRendering: true,   /* no error bomb on half-typed diagrams */
			theme: (theme === "light") ? "default" : "dark"
		});
		svgCache = Object.create(null); svgCacheN = 0;   /* old-theme SVGs */
		renderMermaid(true);
	}

	/* A diagram must be laid out to be measurable: WebKitGTK hides a closed
	 * <details>' contents with display:none (offsetParent === null), while
	 * Chromium/WebView2 hides them with content-visibility — which
	 * offsetParent does NOT see — so check for a collapsed <details> ancestor
	 * explicitly. */
	function mermaidRenderable(pre) {
		return pre.offsetParent !== null &&
		       pre.closest("details:not([open])") === null;
	}

	/* Render every pre.mermaid in the markdown DOM; force re-renders blocks
	 * that already show an SVG (theme change). A block that fails to parse
	 * keeps showing its source as a plain code block. Blocks hidden inside a
	 * collapsed <details> are NOT rendered: mermaid measures labels with
	 * getBBox(), which reports 0x0 while hidden and bakes a mangled SVG —
	 * they defer until the <details> opens (toggle listener below). Returns
	 * the block count (diagnostic). */
	function renderMermaid(force) {
		var pres = content.querySelectorAll("pre.mermaid");
		if (!window.mermaid || pres.length === 0)
			return pres.length;
		var pending = [], hidden = 0;
		Array.prototype.forEach.call(pres, function (pre) {
			var src = pre.getAttribute("data-mmd");
			if (src === null) {
				src = pre.textContent;
				pre.setAttribute("data-mmd", src);
			}
			if (!force && svgCache[src] !== undefined) {   /* unchanged: instant swap */
				if (!pre.classList.contains("mmd-done")) { /* cached SVGs measured OK */
					pre.innerHTML = svgCache[src];
					pre.setAttribute("data-processed", "true");
					pre.classList.add("mmd-done");
					mermaidZoomApply(pre, zoomLevel(src));
				}
				return;
			}
			if (!mermaidRenderable(pre)) {      /* hidden — collapsed <details> etc. */
				if (force && pre.hasAttribute("data-processed")) {
					pre.textContent = src;      /* old-theme SVG: back to source */
					pre.removeAttribute("data-processed");
					pre.classList.remove("mmd-done");
				}
				if (!pre.hasAttribute("data-processed"))
					hidden++;                   /* renders on reveal */
				return;
			}
			if (force || !pre.hasAttribute("data-processed")) {
				pre.textContent = src;              /* restore the source for a re-render */
				pre.removeAttribute("data-processed");
				pre.classList.remove("mmd-done");
				pending.push(pre);
			}
		});
		if (pending.length !== 0) {
			mermaid.run({ nodes: pending, suppressErrors: true }).then(function () {
				var ok = 0;
				if (svgCacheN > 64) {               /* crude cap on a per-source cache */
					svgCache = Object.create(null); svgCacheN = 0;
				}
				pending.forEach(function (pre) {
					if (pre.querySelector("svg") === null)
						return;                     /* parse failure: source stays visible */
					pre.classList.add("mmd-done");
					ok++;
					addZoomControls(pre);           /* before the harvest: cached swap-ins
					                                 * then arrive with their controls */
					var key = pre.getAttribute("data-mmd");
					if (svgCache[key] === undefined) {
						svgCache[key] = pre.innerHTML;   /* pristine svg + controls */
						svgCacheN++;
					}
					mermaidZoomApply(pre, zoomLevel(key));   /* re-apply a kept zoom */
				});
				bridge.post("preview.rendered",
				            { mode: "mermaid", ok: ok, of: pending.length, deferred: hidden });
			}).catch(function () { /* suppressErrors already covers this */ });
		} else if (hidden !== 0) {
			bridge.post("preview.rendered",
			            { mode: "mermaid", ok: 0, of: 0, deferred: hidden });
		}
		return pres.length;
	}

	var content     = document.getElementById("content");
	var placeholder = document.getElementById("placeholder");
	var htmlframe   = document.getElementById("htmlframe");
	var scrollEl    = document.getElementById("scroll");

	/* <base> so relative image URLs (![](pic.png)) resolve against the current
	 * document's served directory; the native side sets it per render. */
	var baseEl = document.createElement("base");
	document.head.appendChild(baseEl);

	function showMode(mode) {
		/* Explicit "block", not "" — #htmlframe has display:none in the stylesheet,
		 * so clearing the inline style would leave it hidden. */
		content.style.display     = (mode === "md")    ? "block" : "none";
		placeholder.style.display = (mode === "empty") ? "block" : "none";
		htmlframe.style.display   = (mode === "html")  ? "block" : "none";
	}

	var sanitizeOpts = { ADD_TAGS: ["details", "summary"], ADD_ATTR: ["open", "id"] };

	/* Copy the code block straight to Geany's own clipboard via the bridge. */
	function copyText(text, btn) {
		bridge.post("ui.copy", text);
		btn.textContent = "Copied";
		setTimeout(function () { btn.textContent = "Copy"; }, 1200);
	}
	function addCopyButtons() {
		var pres = content.querySelectorAll("pre:not(.mermaid)");
		Array.prototype.forEach.call(pres, function (pre) {
			var btn = document.createElement("button");
			btn.className = "copy-btn";
			btn.textContent = "Copy";
			btn.addEventListener("click", function () {
				var code = pre.querySelector("code");
				copyText(code ? code.innerText : pre.innerText, btn);
			});
			pre.appendChild(btn);
		});
	}

	/* GitHub-style hover anchors: the vendored github-markdown CSS already
	 * carries the .anchor/.octicon-link chain-icon hover styling; only the
	 * matching markup is missing. Injected post-sanitize like the copy
	 * buttons. Clicking copies the in-document anchor (#slug) — the click
	 * handler below intercepts it before the #-link scroll path. The slug
	 * rides in data-anchor, NOT href: it is a button, not a link, and a real
	 * href makes the WebView pop its link-status bubble (the resolved
	 * https://geanyview.local/… URL, bottom-left) on hover. Raw-HTML
	 * headings carry no id (markdown-it-anchor skips them) and get none. */
	function addHeadingAnchors() {
		var hs = content.querySelectorAll(
			"h1[id], h2[id], h3[id], h4[id], h5[id], h6[id]");
		Array.prototype.forEach.call(hs, function (h) {
			var a = document.createElement("a");
			a.className = "anchor";
			a.setAttribute("data-anchor", "#" + h.id);
			a.title = "Copy anchor: #" + h.id;
			a.setAttribute("aria-hidden", "true");
			a.innerHTML = '<span class="octicon octicon-link"></span>';
			h.insertBefore(a, h.firstChild);
		});
	}

	/* Scroll policy: re-renders of the SAME document (typing) keep the scroll
	 * position; a document switch starts at the top. Native announces the
	 * document (preview.doc) before each render. */
	var currentDoc = null, docChanged = false;
	bridge.on("preview.doc", function (p) {
		var id = (p && p.id) || "";
		if (id !== currentDoc) {
			currentDoc = id;
			docChanged = true;
		}
	});

	bridge.on("preview.md", function (p) {
		var top = docChanged ? 0 : scrollEl.scrollTop;
		var dirty = md.render((p && p.text) || "");
		content.innerHTML = DOMPurify.sanitize(dirty, sanitizeOpts);
		addCopyButtons();                 /* added post-sanitize, so DOMPurify keeps them */
		addHeadingAnchors();
		var diagrams = renderMermaid(false);   /* SVGs are injected post-sanitize too */
		showMode("md");
		scrollEl.scrollTop = top;
		docChanged = false;
		bridge.post("preview.rendered", { mode: "md", chars: content.innerHTML.length,
		                                  mermaid: diagrams });
	});

	bridge.on("preview.html", function (p) {
		htmlframe.src = (p && p.url) || "about:blank";   /* real served resource */
		showMode("html");
		bridge.post("preview.rendered", { mode: "html" });
	});

	/* Set the document base (sent just before preview.md) so images resolve. An
	 * empty url falls back to the preview page's own dir (untitled documents). */
	bridge.on("preview.base", function (p) {
		baseEl.setAttribute("href", (p && p.url) || location.href);
	});

	bridge.on("preview.empty", function () {
		content.innerHTML = "";
		showMode("empty");
	});

	/* In-document (#section) links scroll within the pane instead of navigating
	 * (which would 404 the served page). External links fall through and are
	 * opened in the OS browser by the host. */
	content.addEventListener("click", function (e) {
		/* Zoom buttons (delegated: the markup travels through the SVG cache
		 * without listeners). */
		var zb = e.target && e.target.closest ? e.target.closest("[data-mmd-zoom]") : null;
		if (zb) {
			var zpre = zb.closest("pre.mermaid");
			if (zpre !== null) {
				var zsrc = zpre.getAttribute("data-mmd") || "";
				var f = zoomLevel(zsrc);
				var op = zb.getAttribute("data-mmd-zoom");
				if (op === "in")
					f = Math.min(ZOOM_MAX, f * ZOOM_STEP);
				else if (op === "out")
					f = Math.max(ZOOM_MIN, f / ZOOM_STEP);
				else
					f = 1;
				f = Math.round(f * 100) / 100;
				zoomSet(zsrc, f);
				mermaidZoomApply(zpre, f);
			}
			return;
		}
		var a = e.target && e.target.closest ? e.target.closest("a") : null;
		if (!a) return;
		/* Heading hover anchor (chain icon): copy the in-document anchor
		 * instead of scrolling — the reader is already at that heading. The
		 * copied feedback swaps the icon to a check (index.html). */
		if (a.classList.contains("anchor") &&
		    a.closest("h1,h2,h3,h4,h5,h6") !== null) {
			e.preventDefault();
			bridge.post("ui.copy", a.getAttribute("data-anchor") || "");
			a.classList.add("copied");
			setTimeout(function () { a.classList.remove("copied"); }, 1200);
			return;
		}
		var href = a.getAttribute("href");
		if (href && href.charAt(0) === "#") {
			e.preventDefault();
			var id = decodeURIComponent(href.slice(1));
			var target = document.getElementById(id) ||
			             (document.getElementsByName(id)[0]);
			if (target) target.scrollIntoView({ behavior: "smooth", block: "start" });
		}
	});

	/* Diagrams inside a collapsed <details> render on first expand. The toggle
	 * event does not bubble, so listen in the capture phase; it fires after
	 * the open state changed, so offsetParent is already meaningful. */
	content.addEventListener("toggle", function () { renderMermaid(false); }, true);

	/* Drag-to-pan a zoomed-in diagram (mmd-pan hides the scrollbars, but the
	 * box is still a scroll container). Delegated like the zoom clicks so the
	 * handlers survive re-renders; pointer capture keeps a drag alive when
	 * the pointer leaves the pane. */
	var pan = null;   /* {pre, x, y, left, top} while a drag is active */
	content.addEventListener("pointerdown", function (e) {
		if (e.button !== 0 || pan !== null)
			return;
		if (!e.target || !e.target.closest ||
		    e.target.closest(".mmd-zoom") !== null)   /* buttons keep working */
			return;
		var pre = e.target.closest("pre.mmd-pan");
		if (pre === null)
			return;
		if (pre.scrollWidth <= pre.clientWidth &&
		    pre.scrollHeight <= pre.clientHeight)
			return;                                   /* nothing to pan */
		pan = { pre: pre, x: e.clientX, y: e.clientY,
		        left: pre.scrollLeft, top: pre.scrollTop };
		pre.classList.add("mmd-panning");
		if (pre.setPointerCapture) {
			try { pre.setPointerCapture(e.pointerId); } catch (err) { /* moot */ }
		}
		e.preventDefault();                           /* no text selection */
	});
	content.addEventListener("pointermove", function (e) {
		if (pan === null)
			return;
		if (!pan.pre.isConnected) {                   /* re-rendered mid-drag */
			pan = null;
			return;
		}
		pan.pre.scrollLeft = pan.left - (e.clientX - pan.x);
		pan.pre.scrollTop  = pan.top  - (e.clientY - pan.y);
	});
	function panEnd() {
		if (pan === null)
			return;
		if (pan.pre.isConnected)
			pan.pre.classList.remove("mmd-panning");
		pan = null;
	}
	content.addEventListener("pointerup", panEnd);
	content.addEventListener("pointercancel", panEnd);

	/* Toolbar: mode selector + refresh. */
	var modeButtons = document.querySelectorAll("#bar [data-mode]");
	function highlightMode(mode) {
		if (mode === "markdown") mode = "md";   /* config name -> button name */
		Array.prototype.forEach.call(modeButtons, function (x) {
			x.classList.toggle("active", x.getAttribute("data-mode") === mode);
		});
	}
	Array.prototype.forEach.call(modeButtons, function (b) {
		b.addEventListener("click", function () {
			highlightMode(b.getAttribute("data-mode"));
			bridge.post("preview.setMode", b.getAttribute("data-mode"));
		});
	});

	/* Native pushes the saved/current mode (startup, Preferences change). */
	bridge.on("preview.mode", function (p) {
		if (p && p.mode) highlightMode(p.mode);
	});
	document.getElementById("refresh").addEventListener("click", function () {
		bridge.post("preview.refresh", {});
	});

	/* Reload: ask native to re-read the shown document from disk (Geany's
	 * Reload); the document-reload signal then re-renders the preview. */
	document.getElementById("reload").addEventListener("click", function () {
		bridge.post("preview.reloadDoc", {});
	});

	/* Pin: freeze the preview on the document it currently shows, so switching
	 * editor tabs no longer retargets it. Native owns the state and answers
	 * with preview.pin (also on sys.ready, restoring the button after a page
	 * reload, and when the pinned document is closed). The pinned document's
	 * full path comes from the last render's preview.doc — native pushes the
	 * pin state after the render, so currentDoc is the pinned document. */
	var PIN_GLYPH = String.fromCharCode(0x26b2) + " ";   /* ⚲ (BMP, no emoji font needed) */
	var pinBtn = document.getElementById("pin");
	pinBtn.addEventListener("click", function () {
		bridge.post("preview.setPin",
		            { pin: pinBtn.classList.contains("active") ? 0 : 1 });
	});
	bridge.on("preview.pin", function (p) {
		var on = !!(p && p.state === "on");
		var path = (currentDoc && currentDoc.indexOf("untitled-") !== 0)
		           ? currentDoc : "";
		pinBtn.classList.toggle("active", on);
		pinBtn.textContent = PIN_GLYPH + (on ? "Pinned" : "Pin");
		pinBtn.title = on
			? "Pinned to " + (path || "an unsaved document") + " — click to unpin"
			: "Pin the preview to this document";
	});

	/* Background theme: one toggle swaps the github-markdown + highlight
	 * stylesheets and the chrome palette between dark and light. The saved theme
	 * is pushed by the native side (preview.theme); toggling persists it back
	 * (ui.theme) so it survives restarts. */
	var themeToggle = document.getElementById("theme-toggle");
	function applyTheme(theme) {
		if (theme !== "light") theme = "dark";
		document.body.className = "theme-" + theme;
		/* Resolve against the page URL, NOT the document <base> (which points at
		 * the doc's dir for images) — otherwise the vendored CSS 404s. */
		document.getElementById("md-theme").href =
			new URL("vendor/github-markdown-" + theme + ".css", location.href).href;
		document.getElementById("hl-theme").href =
			new URL("vendor/highlight-github-" + theme + ".css", location.href).href;
		themeToggle.textContent = (theme === "dark")
			? String.fromCharCode(0x263e) + " Dark"      /* moon */
			: String.fromCharCode(0x2600) + " Light";    /* sun */
		mermaidSetTheme(theme);   /* re-renders shown diagrams in the new palette */
	}
	themeToggle.addEventListener("click", function () {
		var next = (document.body.className.indexOf("light") >= 0) ? "dark" : "light";
		applyTheme(next);
		bridge.post("ui.theme", next);   /* persist in the plugin config */
	});
	bridge.on("preview.theme", function (p) { if (p && p.theme) applyTheme(p.theme); });
	applyTheme("dark");                   /* default until the saved theme arrives */
})();
