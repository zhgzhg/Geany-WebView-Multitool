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
 *   preview.rendered {...}                 diagnostic
 *
 * GFM via markdown-it (html:true, linkify) + task-lists + heading anchors;
 * output sanitized with DOMPurify. ```mermaid fences render as diagrams:
 * the fence emits its raw source in a <pre class="mermaid"> and the SVG is
 * injected after sanitization (mermaid's securityLevel:"strict" sanitizes
 * diagram labels itself). In-document (#section) links scroll within the
 * pane; external links open in the OS browser (host-side).
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
	.use(window.markdownItAnchor, { slugify: slugify });

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
					var key = pre.getAttribute("data-mmd");
					if (svgCache[key] === undefined) {
						svgCache[key] = pre.innerHTML;
						svgCacheN++;
					}
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
		var a = e.target && e.target.closest ? e.target.closest("a") : null;
		if (!a) return;
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
