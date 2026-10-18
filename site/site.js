// sh23ds website: one script for every page. Every page works without it; each feature turns on only
// when its markup is present.
(function () {
  "use strict";

  var REPO = "ankushKun/sh23ds";
  var IMAGE = "ghcr.io/ankushkun/sh23ds-packer";
  var reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };

  function store(key, value) {
    try {
      if (value === undefined) return JSON.parse(localStorage.getItem(key) || "null");
      localStorage.setItem(key, JSON.stringify(value));
    } catch (e) { return null; }
  }

  var toastTimer = null;
  function toast(text) {
    var el = $(".toast");
    if (!el) {
      el = document.createElement("div");
      el.className = "toast";
      el.setAttribute("role", "status");
      document.body.appendChild(el);
    }
    el.textContent = text;
    el.classList.add("is-on");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { el.classList.remove("is-on"); }, 1700);
  }

  // The status bar's clock, as on the HOME Menu.
  function clock() {
    var el = $(".clock");
    if (!el) return;
    var tick = function () {
      var d = new Date();
      el.textContent = String(d.getHours()).padStart(2, "0") + ":" + String(d.getMinutes()).padStart(2, "0");
    };
    tick();
    setInterval(tick, 15000);
  }

  function menu() {
    var m = $(".menu");
    if (!m) return;
    $$("a", m).forEach(function (a) { a.addEventListener("click", function () { m.open = false; }); });
    document.addEventListener("click", function (e) { if (m.open && !m.contains(e.target)) m.open = false; });
  }

  // The console photo: --k is (rendered width / photo width), which the screen matrices scale by. Until
  // it is set the photo's own blank screens show.
  function device() {
    $$(".device-stage").forEach(function (stage) {
      var w = parseFloat(getComputedStyle(stage).getPropertyValue("--photo-w"));
      if (!w) return;
      var fit = function () { if (stage.offsetWidth) stage.style.setProperty("--k", (stage.offsetWidth / w).toFixed(5)); };
      fit();
      if ("ResizeObserver" in window) new ResizeObserver(fit).observe(stage);
      else window.addEventListener("resize", fit);
      stage.classList.add("is-mapped");
    });
    // the top and bottom screens step through their pictures together
    var tops = $$(".screen-top img"), bottoms = $$(".screen-bottom img");
    if (tops.length < 2 || reduceMotion) return;
    var i = 0;
    setInterval(function () {
      if (document.hidden) return;
      var j = (i + 1) % tops.length;
      tops[i].classList.add("is-off");
      tops[j].classList.remove("is-off");
      if (bottoms[i] && bottoms[j]) {
        bottoms[i].classList.add("is-off");
        bottoms[j].classList.remove("is-off");
      }
      i = j;
    }, 4200);
  }

  // The app dock picks what the feature screen shows.
  function dock() {
    var box = $("[data-dock]");
    if (!box) return;
    var apps = $$(".app", box);
    var img = $("[data-viewer-img]"), title = $("[data-viewer-title]"), text = $("[data-viewer-text]");
    apps.forEach(function (a, i) {
      a.addEventListener("click", function () {
        apps.forEach(function (b) { b.setAttribute("aria-selected", b === a ? "true" : "false"); b.tabIndex = b === a ? 0 : -1; });
        img.src = a.dataset.shot;
        img.alt = a.dataset.alt || "";
        title.textContent = a.dataset.title;
        text.textContent = a.dataset.text;
      });
      a.addEventListener("keydown", function (e) {
        var n = e.key === "ArrowRight" ? i + 1 : e.key === "ArrowLeft" ? i - 1 : null;
        if (n === null) return;
        e.preventDefault();
        var t = apps[(n + apps.length) % apps.length];
        t.click();
        t.focus();
      });
    });
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
      buttons.forEach(function (b, i) {
        b.addEventListener("click", function () { select(b, false); });
        b.addEventListener("keydown", function (e) {
          var n = e.key === "ArrowRight" ? i + 1 : e.key === "ArrowLeft" ? i - 1 : null;
          if (n === null) return;
          e.preventDefault();
          select(buttons[(n + buttons.length) % buttons.length], true);
        });
      });
    });
  }

  function copyButtons() {
    $$(".code").forEach(function (block) {
      if (block.hasAttribute("data-nocopy")) return;
      var button = $(".copy", block);
      if (!button) {
        button = document.createElement("button");
        button.type = "button";
        button.className = "copy floating";
        button.textContent = "Copy";
        block.appendChild(button);
      }
      button.addEventListener("click", function () {
        var code = $("code", block).cloneNode(true);
        $$(".c", code).forEach(function (c) { if (c.dataset.keep === undefined) c.remove(); });
        var text = code.textContent.replace(/\n{2,}/g, "\n").trim();
        var done = function () {
          button.textContent = "Copied";
          button.classList.add("is-done");
          toast("Copied");
          setTimeout(function () { button.textContent = "Copy"; button.classList.remove("is-done"); }, 1500);
        };
        if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(text).then(done);
        else {
          var area = document.createElement("textarea");
          area.value = text;
          document.body.appendChild(area);
          area.select();
          try { document.execCommand("copy"); done(); } catch (e) {}
          area.remove();
        }
      });
    });
  }

  // The latest release's tag and file sizes. Before the first release the page keeps its defaults.
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

  function scrollspy(links, attr, value) {
    if (!links.length || !("IntersectionObserver" in window)) return;
    var byId = {};
    links.forEach(function (a) { byId[decodeURIComponent(a.hash.slice(1))] = a; });
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (!e.isIntersecting) return;
        var link = byId[e.target.id];
        if (!link) return;
        links.forEach(function (a) { a.removeAttribute(attr); });
        link.setAttribute(attr, value);
      });
    }, { rootMargin: "-25% 0px -65% 0px" });
    Object.keys(byId).forEach(function (id) { var el = document.getElementById(id); if (el) io.observe(el); });
  }

  // ---------------------------------------------------------------- install guide

  function installProgress() {
    var steps = $$("[data-step]");
    if (!steps.length) return;
    var KEY = "sh23ds.install.v1";
    var state = store(KEY) || { done: {}, checks: {} };
    var bar = $(".progress-bar span"), label = $(".progress-label");
    function render() {
      var n = 0;
      steps.forEach(function (s) {
        var done = !!state.done[s.id];
        if (done) n++;
        s.classList.toggle("is-done", done);
        var b = $(".done-btn", s);
        if (b) {
          b.setAttribute("aria-pressed", done ? "true" : "false");
          $(".t", b).textContent = done ? "Done" : "Mark as done";
        }
        var link = $('.steps-nav a[href="#' + s.id + '"]');
        if (link) link.classList.toggle("is-done", done);
      });
      if (bar) bar.style.setProperty("--p", (n / steps.length) * 100 + "%");
      if (label) label.textContent = n + " of " + steps.length + " done";
    }
    steps.forEach(function (s) {
      var b = $(".done-btn", s);
      if (!b) return;
      b.addEventListener("click", function () {
        state.done[s.id] = !state.done[s.id];
        store(KEY, state);
        render();
        var next = steps[steps.indexOf(s) + 1];
        if (state.done[s.id] && next) next.scrollIntoView({ behavior: reduceMotion ? "auto" : "smooth" });
      });
    });
    $$(".check input[data-key]").forEach(function (box) {
      box.checked = !!state.checks[box.dataset.key];
      box.addEventListener("change", function () { state.checks[box.dataset.key] = box.checked; store(KEY, state); });
    });
    var reset = $("[data-reset]");
    if (reset) reset.addEventListener("click", function () {
      state = { done: {}, checks: {} };
      store(KEY, state);
      $$(".check input[data-key]").forEach(function (box) { box.checked = false; });
      render();
      toast("Cleared");
    });
    render();
    scrollspy($$(".steps-nav a"), "aria-current", "step");
  }

  function detectOS() {
    var p = ((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || navigator.userAgent || "").toLowerCase();
    if (p.indexOf("win") >= 0) return "windows";
    if (p.indexOf("mac") >= 0) return "mac";
    return "linux";
  }

  function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

  function builder() {
    var root = $("[data-builder]");
    if (!root) return;
    var input = $("[data-game]", root);
    var osButtons = $$("[data-os]", root);
    var outPack = $('[data-out="pack"]'), outHash = $('[data-out="md5"]');
    var notes = { colima: $('[data-note="colima"]'), exe: $('[data-note="exe"]'), tilde: $('[data-note="tilde"]') };
    var KEY = "sh23ds.builder.v1";
    var saved = store(KEY) || {};
    var os = saved.os || detectOS();
    var placeholders = { mac: "/Users/you/Games/Silent Hill 2", linux: "/home/you/Games/Silent Hill 2", windows: "C:\\Games\\Silent Hill 2" };
    if (saved.path) input.value = saved.path;
    function clean(raw) {
      var path = raw.trim().replace(/^["']|["']$/g, ""), stripped = false;
      var m = path.match(/^(.*?)[\\/]+(sh2pc\.exe|data)[\\/]*$/i);
      if (m) { path = m[1]; stripped = true; }
      return { path: path.replace(/[\\/]+$/, ""), stripped: stripped };
    }
    var sh = function (s) { return s.replace(/(["\\$`])/g, "\\$1"); };
    var ps = function (s) { return s.replace(/([`$"])/g, "`$1"); };
    function render() {
      osButtons.forEach(function (b) { b.setAttribute("aria-pressed", b.dataset.os === os ? "true" : "false"); });
      input.placeholder = placeholders[os];
      var c = clean(input.value || ""), shown = c.path || placeholders[os], tilde = false, pack, hash;
      if (os === "windows") {
        var g = "<mark>" + esc(ps(shown)) + "</mark>";
        hash = 'Get-FileHash -Algorithm MD5 "' + g + '\\sh2pc.exe"';
        pack = "mkdir -Force sd\ndocker run --rm `\n  -v \"" + g + ":/game:ro\" `\n  -v \"${PWD}\\sd:/out\" `\n  " + IMAGE;
      } else {
        var p = shown, prefix = "";
        if (/^~(\/|$)/.test(p)) { tilde = true; prefix = "$HOME"; p = p.slice(1); }
        var g2 = "<mark>" + prefix + esc(sh(p)) + "</mark>";
        hash = (os === "mac" ? 'md5 -q "' : 'md5sum "') + g2 + '/sh2pc.exe"';
        pack = "mkdir -p sd\ndocker run --rm \\\n" + (os === "linux" ? '  --user "$(id -u):$(id -g)" \\\n' : "") +
          '  -v "' + g2 + ':/game:ro" \\\n  -v "$PWD/sd:/out" \\\n  ' + IMAGE;
      }
      outPack.innerHTML = pack;
      outHash.innerHTML = hash;
      if (notes.exe) notes.exe.classList.toggle("is-on", c.stripped);
      if (notes.tilde) notes.tilde.classList.toggle("is-on", tilde);
      if (notes.colima) notes.colima.classList.toggle("is-on", os === "mac" && !!c.path && !/^(~|\/Users\/)/.test(c.path));
      $$("[data-os-only]").forEach(function (el) { el.hidden = el.dataset.osOnly.split(" ").indexOf(os) < 0; });
      store(KEY, { os: os, path: input.value });
    }
    osButtons.forEach(function (b) { b.addEventListener("click", function () { os = b.dataset.os; render(); }); });
    input.addEventListener("input", render);
    render();
  }

  // ---------------------------------------------------------------- docs filter

  function docs() {
    var toc = $(".toc");
    if (!toc) return;
    var links = $$("ul a", toc);
    scrollspy(links, "aria-current", "true");
    var filter = $("[data-filter]", toc), empty = $(".empty", toc);
    if (!filter) return;
    var index = links.map(function (a) {
      var s = document.getElementById(decodeURIComponent(a.hash.slice(1)));
      return { a: a, text: ((s && s.textContent) || a.textContent).toLowerCase() };
    });
    filter.addEventListener("input", function () {
      var q = filter.value.trim().toLowerCase(), shown = 0;
      index.forEach(function (it) {
        var on = !q || it.text.indexOf(q) >= 0;
        it.a.parentElement.hidden = !on;
        if (on) shown++;
      });
      $$(".group", toc).forEach(function (g) {
        var list = g.nextElementSibling;
        g.hidden = list && !$$("li:not([hidden])", list).length;
      });
      if (empty) empty.hidden = shown > 0;
    });
    filter.addEventListener("keydown", function (e) {
      if (e.key !== "Enter") return;
      var first = index.filter(function (i) { return !i.a.parentElement.hidden; })[0];
      if (first) { first.a.click(); filter.blur(); }
    });
    document.addEventListener("keydown", function (e) {
      if (e.key === "/" && document.activeElement !== filter && !/input|textarea/i.test(document.activeElement.tagName)) {
        e.preventDefault();
        filter.focus();
      }
    });
  }

  clock();
  menu();
  device();
  dock();
  tabs();
  copyButtons();
  release();
  installProgress();
  builder();
  docs();
})();
