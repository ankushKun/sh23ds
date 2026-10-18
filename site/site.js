// sh23ds website: one script for every page. Every page works without it; each feature turns on only
// when its markup is present.
(function () {
  "use strict";

  var REPO = "ankushKun/sh23ds";
  var IMAGE = "ghcr.io/ankushkun/sh23ds-packer";
  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };
  var reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  function store(key, value) {
    try {
      if (value === undefined) return JSON.parse(localStorage.getItem(key) || "null");
      localStorage.setItem(key, JSON.stringify(value));
    } catch (e) { return null; }
  }

  function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

  // On a phone the step and memo menus are rows that scroll sideways: bring the chosen one to the middle.
  function reveal(strip, el) {
    if (!strip || !el || strip.scrollWidth <= strip.clientWidth + 1) return;
    var r = el.getBoundingClientRect(), b = strip.getBoundingClientRect();
    strip.scrollBy({ left: r.left - b.left - (b.width - r.width) / 2, behavior: reduceMotion ? "auto" : "smooth" });
  }

  // Arrow keys move between the buttons of a tab list, as on a menu screen.
  function arrowKeys(items, select) {
    items.forEach(function (b, i) {
      b.addEventListener("keydown", function (e) {
        var k = e.key, n = null;
        if (k === "ArrowDown" || k === "ArrowRight") n = i + 1;
        else if (k === "ArrowUp" || k === "ArrowLeft") n = i - 1;
        else if (k === "Home") n = 0;
        else if (k === "End") n = items.length - 1;
        if (n === null) return;
        e.preventDefault();
        select(items[(n + items.length) % items.length], true);
      });
    });
  }

  // ---------------------------------------------------------------- every page

  // The latest release's tag and file sizes. Until it answers, the pages show the values they were written with.
  function release() {
    if (!window.fetch || !$("[data-version], [data-size]")) return;
    fetch("https://api.github.com/repos/" + REPO + "/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) {
        if (!data || !data.tag_name) return;
        $$("[data-version]").forEach(function (el) { el.textContent = data.tag_name; });
        (data.assets || []).forEach(function (a) {
          var kb = a.size / 1024;
          var t = kb > 1024 ? (kb / 1024).toFixed(1) + " MB" : Math.round(kb) + " KB";
          $$('[data-size="' + a.name + '"]').forEach(function (el) { el.textContent = t; });
        });
      })
      .catch(function () {});
  }

  function tabs() {
    $$("[data-tabs]").forEach(function (box) {
      var buttons = $$('[role="tab"]', box);
      function select(b, focus) {
        buttons.forEach(function (x) {
          var on = x === b;
          x.setAttribute("aria-selected", on ? "true" : "false");
          x.tabIndex = on ? 0 : -1;
          var p = document.getElementById(x.getAttribute("aria-controls"));
          if (p) p.hidden = !on;
        });
        if (focus) b.focus();
      }
      buttons.forEach(function (b) { b.addEventListener("click", function () { select(b, false); }); });
      arrowKeys(buttons, select);
    });
  }

  function copyText(text, button) {
    var done = function () {
      button.textContent = "Copied";
      button.classList.add("is-done");
      setTimeout(function () { button.textContent = "Copy"; button.classList.remove("is-done"); }, 1500);
    };
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(text).then(done, function () {});
    else {
      var area = document.createElement("textarea");
      area.value = text;
      area.setAttribute("readonly", "");
      area.style.position = "fixed";
      area.style.opacity = "0";
      document.body.appendChild(area);
      area.select();
      try { document.execCommand("copy"); done(); } catch (e) {}
      area.remove();
    }
  }

  function copyButtons() {
    $$("[data-copy]").forEach(function (b) {
      b.addEventListener("click", function () {
        var out = $('[data-out="' + b.dataset.copy + '"]');
        if (out) copyText(out.textContent.trim(), b);
      });
    });
    $$("[data-copy-text]").forEach(function (b) {
      b.addEventListener("click", function () { copyText(b.dataset.copyText, b); });
    });
  }

  // The side menu's red bar follows the section on screen.
  function scrollspy() {
    var box = $("[data-spy]");
    if (!box || !("IntersectionObserver" in window)) return;
    var links = $$("a", box), byId = {};
    if (links[0]) links[0].setAttribute("aria-current", "true");
    links.forEach(function (a) { byId[decodeURIComponent(a.hash.slice(1))] = a; });
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (!e.isIntersecting) return;
        var link = byId[e.target.id];
        if (!link) return;
        if (link.hasAttribute("aria-current")) return;
        links.forEach(function (a) { a.removeAttribute("aria-current"); });
        link.setAttribute("aria-current", "true");
        reveal(box, link);
      });
    }, { rootMargin: "-20% 0px -70% 0px" });
    Object.keys(byId).forEach(function (id) { var el = document.getElementById(id); if (el) io.observe(el); });
  }

  // ---------------------------------------------------------------- home

  // The movie loop behind the title: still for anyone who asks for less motion, and paused while off screen.
  function titleVideo() {
    var v = $("[data-title-video]");
    if (!v) return;
    var saveData = navigator.connection && navigator.connection.saveData;
    if (reduceMotion || saveData) {
      v.removeAttribute("autoplay");
      v.pause();
      // With data saver on, don't download the movie at all; the poster stays.
      if (saveData) { $$("source", v).forEach(function (src) { src.remove(); }); v.load(); }
      return;
    }
    var play = function () { var p = v.play(); if (p && p.catch) p.catch(function () {}); };
    if ("IntersectionObserver" in window) {
      new IntersectionObserver(function (entries) {
        if (entries[0].isIntersecting) play(); else v.pause();
      }).observe(v);
    }
  }

  // Up and down move through the title menu, like the game's.
  function titleMenu() {
    var menu = $(".menu");
    if (!menu) return;
    var items = $$("a", menu);
    arrowKeys(items, function (a) { a.focus(); });
  }

  function memos() {
    var box = $("[data-memos]");
    if (!box) return;
    var tabsList = $$('[role="tab"]', box), count = $("[data-memo-count]", box);
    function select(b, focus) {
      tabsList.forEach(function (x, i) {
        var on = x === b;
        x.setAttribute("aria-selected", on ? "true" : "false");
        x.tabIndex = on ? 0 : -1;
        document.getElementById(x.getAttribute("aria-controls")).hidden = !on;
        if (on && count) count.textContent = (i + 1) + " / " + tabsList.length;
      });
      reveal(b.parentNode, b);
      if (focus) b.focus({ preventScroll: true });
    }
    tabsList.forEach(function (b) { b.addEventListener("click", function () { select(b, false); }); });
    arrowKeys(tabsList, select);
    select(tabsList[0], false);
  }

  // ---------------------------------------------------------------- install guide

  function detectOS() {
    var p = ((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || navigator.userAgent || "").toLowerCase();
    if (p.indexOf("win") >= 0) return "windows";
    if (p.indexOf("mac") >= 0) return "mac";
    return "linux";
  }

  // The packer commands for the chosen system and game folder.
  function builder() {
    var root = $("[data-builder]");
    if (!root) return;
    var ORDER = ["mac", "windows", "linux"];
    var NAMES = { mac: "macOS", windows: "Windows", linux: "Linux" };
    var SHELLS = { mac: "Terminal", windows: "PowerShell", linux: "Terminal" };
    var PLACEHOLDERS = { mac: "/Users/you/Games/Silent Hill 2", windows: "C:\\Games\\Silent Hill 2", linux: "/home/you/Games/Silent Hill 2" };
    var input = $("[data-game]", root), label = $("[data-os-label]", root), colima = $('[data-note="colima"]', root);
    var outPack = $('[data-out="pack"]'), outHash = $('[data-out="md5"]');
    var KEY = "sh23ds.builder.v2";
    var saved = store(KEY) || {};
    var os = ORDER.indexOf(saved.os) >= 0 ? saved.os : detectOS();
    if (saved.path) input.value = saved.path;

    // A path that points at sh2pc.exe or data/ means the folder above it.
    function clean(raw) {
      var path = raw.trim().replace(/^["']|["']$/g, "");
      var m = path.match(/^(.*?)[\\/]+(sh2pc\.exe|data)[\\/]*$/i);
      if (m) path = m[1];
      return path.replace(/[\\/]+$/, "");
    }
    var sh = function (s) { return s.replace(/(["\\$`])/g, "\\$1"); };
    var ps = function (s) { return s.replace(/([`$"])/g, "`$1"); };

    function render() {
      label.textContent = NAMES[os];
      input.placeholder = PLACEHOLDERS[os];
      $$("[data-shell]").forEach(function (el) { el.textContent = SHELLS[os]; });
      var path = clean(input.value || ""), shown = path || PLACEHOLDERS[os], pack, hash;
      if (os === "windows") {
        var g = "<mark>" + esc(ps(shown)) + "</mark>";
        hash = 'Get-FileHash -Algorithm MD5 "' + g + '\\sh2pc.exe"';
        pack = "mkdir -Force sd\ndocker run --rm `\n  -v \"" + g + ":/game:ro\" `\n  -v \"${PWD}\\sd:/out\" `\n  " + IMAGE;
      } else {
        // A leading ~ doesn't expand inside quotes, so it becomes $HOME.
        var p = shown, prefix = "";
        if (/^~(\/|$)/.test(p)) { prefix = "$HOME"; p = p.slice(1); }
        var g2 = "<mark>" + prefix + esc(sh(p)) + "</mark>";
        hash = (os === "mac" ? 'md5 -q "' : 'md5sum "') + g2 + '/sh2pc.exe"';
        pack = "mkdir -p sd\ndocker run --rm \\\n" + (os === "linux" ? '  --user "$(id -u):$(id -g)" \\\n' : "") +
          '  -v "' + g2 + ':/game:ro" \\\n  -v "$PWD/sd:/out" \\\n  ' + IMAGE;
      }
      outPack.innerHTML = pack;
      outHash.innerHTML = hash;
      if (colima) colima.classList.toggle("is-on", os === "mac" && !!path && !/^(~|\/Users\/)/.test(path));
      store(KEY, { os: os, path: input.value });
    }

    $$("[data-os-step]", root).forEach(function (b) {
      b.addEventListener("click", function () {
        var i = ORDER.indexOf(os) + Number(b.dataset.osStep);
        os = ORDER[(i + ORDER.length) % ORDER.length];
        render();
      });
    });
    input.addEventListener("input", render);
    render();
  }

  // ---------------------------------------------------------------- roadmap

  // Releases newer than the ones written into the page, from GitHub, with the first sentence of their notes.
  function releases() {
    var list = $("[data-releases]");
    if (!list || !window.fetch) return;
    var known = {};
    $$("[data-tag]", list).forEach(function (a) { known[a.dataset.tag] = true; });
    var months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    fetch("https://api.github.com/repos/" + REPO + "/releases?per_page=6", { headers: { Accept: "application/vnd.github+json" } })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) {
        if (!data || !data.length) return;
        data.filter(function (r) { return !r.draft && !r.prerelease && !known[r.tag_name]; }).reverse().forEach(function (r) {
          var d = new Date(r.published_at);
          var body = (r.body || "").replace(/[*_`#>]/g, "").trim();
          var first = (body.match(/^[^\n]*?[.!?](\s|$)/) || [body.split("\n")[0] || ""])[0].trim();
          var a = document.createElement("a");
          a.className = "rel";
          a.href = r.html_url;
          a.dataset.tag = r.tag_name;
          var tag = document.createElement("span");
          tag.className = "rel-tag";
          var name = document.createElement("b");
          name.textContent = r.tag_name;
          var date = document.createElement("span");
          date.textContent = d.getUTCDate() + " " + months[d.getUTCMonth()] + " " + d.getUTCFullYear();
          tag.appendChild(name);
          tag.appendChild(date);
          var text = document.createElement("span");
          text.className = "rel-text";
          text.textContent = first;
          a.appendChild(tag);
          a.appendChild(text);
          list.insertBefore(a, list.firstChild);
        });
      })
      .catch(function () {});
  }

  release();
  tabs();
  copyButtons();
  scrollspy();
  titleMenu();
  titleVideo();
  memos();
  builder();
  releases();
})();
