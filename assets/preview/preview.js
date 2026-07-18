/*
 * preview view — renders the active Geany document over window.bridge.
 *
 * Channels (native -> page):
 *   preview.md    {text}   render GitHub-flavored Markdown
 *   preview.html  {html}   render raw HTML in a sandboxed iframe
 *   preview.empty {}       nothing previewable
 * Channels (page -> native):
 *   preview.setMode  "auto"|"md"|"html"   force the render mode
 *   preview.refresh  {}                    re-render the current document
 *   preview.rendered {...}                 diagnostic
 *
 * GFM via markdown-it (html:true, linkify) + task-lists + heading anchors;
 * output sanitized with DOMPurify. In-document (#section) links scroll within
 * the pane; external links open in the OS browser (host-side).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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
		var pres = content.querySelectorAll("pre");
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

	bridge.on("preview.md", function (p) {
		var top = scrollEl.scrollTop;
		var dirty = md.render((p && p.text) || "");
		content.innerHTML = DOMPurify.sanitize(dirty, sanitizeOpts);
		addCopyButtons();                 /* added post-sanitize, so DOMPurify keeps them */
		showMode("md");
		scrollEl.scrollTop = top;         /* preserve scroll across re-renders */
		bridge.post("preview.rendered", { mode: "md", chars: content.innerHTML.length });
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

	/* Toolbar: mode selector + refresh. */
	var modeButtons = document.querySelectorAll("#bar [data-mode]");
	Array.prototype.forEach.call(modeButtons, function (b) {
		b.addEventListener("click", function () {
			Array.prototype.forEach.call(modeButtons, function (x) { x.classList.remove("active"); });
			b.classList.add("active");
			bridge.post("preview.setMode", b.getAttribute("data-mode"));
		});
	});
	document.getElementById("refresh").addEventListener("click", function () {
		bridge.post("preview.refresh", {});
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
	}
	themeToggle.addEventListener("click", function () {
		var next = (document.body.className.indexOf("light") >= 0) ? "dark" : "light";
		applyTheme(next);
		bridge.post("ui.theme", next);   /* persist in the plugin config */
	});
	bridge.on("preview.theme", function (p) { if (p && p.theme) applyTheme(p.theme); });
	applyTheme("dark");                   /* default until the saved theme arrives */
})();
