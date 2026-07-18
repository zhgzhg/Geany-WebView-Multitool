/*
 * hello view — exercises the injected window.bridge (no bridge code of its own).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
(function () {
	"use strict";
	var status = document.getElementById("status");

	bridge.on("sys.pong", function (p) {
		status.textContent = "bridge: round-trip OK" + (p && p.v ? " (" + p.v + ")" : "");
		status.style.color = "#7c7";
	});

	document.getElementById("ping").addEventListener("click", function () {
		bridge.post("sys.ping", { t: Date.now() });
	});

	bridge.ready.then(function () {
		status.textContent = "bridge: ready — pinging native…";
		bridge.post("sys.ping", { t: Date.now() });
	});
})();
