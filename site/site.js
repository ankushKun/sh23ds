// sh23ds website: the page's script (the 404 loads it too). The page works without it; each feature
// turns on only when its markup is present. The packer has its own scripts in packer/.

// Analytics: PostHog, in the same project as the rest of ankush.one and through the same first-party
// proxy (p1.ankush.one). This site shares the ankush.one origin with the others, so a visitor keeps one
// id and one replay session across all of them; the `site` property tells the events apart. Nothing
// loads on localhost or when the browser sends Global Privacy Control; window.track() and
// window.trackError() are safe to call either way.
(function () {
  !function(t,e){var o,n,p,r;e.__SV||(window.posthog && window.posthog.__loaded)||(window.posthog=e,e._i=[],e.init=function(i,s,a){function g(t,e){var o=e.split(".");2==o.length&&(t=t[o[0]],e=o[1]),t[e]=function(){t.push([e].concat(Array.prototype.slice.call(arguments,0)))}}(p=t.createElement("script")).type="text/javascript",p.crossOrigin="anonymous",p.async=!0,p.src=s.api_host.replace(".i.posthog.com","-assets.i.posthog.com")+"/static/array.js",(r=t.getElementsByTagName("script")[0]).parentNode.insertBefore(p,r);var u=e;for(void 0!==a?u=e[a]=[]:a="posthog",u.people=u.people||[],u.toString=function(t){var e="posthog";return"posthog"!==a&&(e+="."+a),t||(e+=" (stub)"),e},u.people.toString=function(){return u.toString(1)+".people (stub)"},o="ki Ci init qi Hi pr Bi zi Di capture calculateEventProperties Qi register register_once register_for_session unregister unregister_for_session Ki getFeatureFlag getFeatureFlagPayload getFeatureFlagResult getAllFeatureFlags isFeatureEnabled reloadFeatureFlags updateFlags updateEarlyAccessFeatureEnrollment getEarlyAccessFeatures on onFeatureFlags onSurveysLoaded onSessionId getSurveys getActiveMatchingSurveys renderSurvey displaySurvey cancelPendingSurvey canRenderSurvey canRenderSurveyAsync Xi identify setPersonProperties unsetPersonProperties group resetGroups setPersonPropertiesForFlags resetPersonPropertiesForFlags setGroupPropertiesForFlags resetGroupPropertiesForFlags reset shutdown setIdentity clearIdentity get_distinct_id getGroups get_session_id get_session_replay_url alias set_config startSessionRecording stopSessionRecording sessionRecordingStarted captureException addExceptionStep captureLog startExceptionAutocapture stopExceptionAutocapture loadToolbar get_property getSessionProperty Ji Gi createPersonProfile setInternalOrTestUser Yi Ai rn opt_in_capturing opt_out_capturing has_opted_in_capturing has_opted_out_capturing get_explicit_consent_status is_capturing clear_opt_in_out_capturing Vi debug mr it getPageViewId captureTraceFeedback captureTraceMetric Oi".split(" "),n=0;n<o.length;n++)g(u,o[n]);e._i.push([i,s,a])},e.__SV=1)}(document,window.posthog||[]);

  var SITE = "sh23ds";

  // The stub only gets its capture methods when init() runs, after the page has loaded, so anything
  // tracked before then waits here (with the time it happened) and is passed on by init().
  var early = [];
  function call(method, a, b) {
    if (early) early.push([method, a, b, { timestamp: new Date() }]);
    else if (typeof window.posthog[method] === "function") window.posthog[method](a, b);
  }
  // Analytics events must never break the page.
  window.track = function (name, props) {
    try { call("capture", name, props || {}); } catch (e) {}
  };
  window.trackError = function (err, context) {
    try { call("captureException", err, { context: context }); } catch (e) {}
  };

  var host = location.hostname;
  var off = host === "localhost" || host === "127.0.0.1" || host === "[::1]" || host === "" ||
    navigator.globalPrivacyControl === true;
  if (off) {
    early = null;
    return;
  }

  function init() {
    window.posthog.init("phc_nBvIMuMmaqQkAXL36Bi6eyTRga0A04klevYFyZf4cfc", {
      api_host: "https://p1.ankush.one",
      ui_host: "https://eu.posthog.com",
      defaults: "2026-05-30",
      // localStorage + cookie, so return visits join up. Every visitor gets a person profile.
      person_profiles: "always",
      capture_pageview: "history_change",
      capture_pageleave: true,
      autocapture: { capture_copied_text: true },
      capture_dead_clicks: true,
      capture_heatmaps: true,
      rageclick: true,
      capture_performance: { network_timing: true, web_vitals: true },
      capture_exceptions: true,
      // Session replay, with console logs and network calls. Headers, bodies and canvas capture also
      // need the matching switches on in the PostHog project's Session replay settings.
      disable_session_recording: false,
      enable_recording_console_log: true,
      session_recording: {
        maskAllInputs: false,
        maskInputOptions: { password: true },
        recordHeaders: true,
        recordBody: true,
        collectFonts: true
      },
      // Set per event rather than with register(): a registered property would be saved in the
      // storage this origin shares and follow the visitor onto the other sites.
      before_send: function (event) {
        if (event && event.properties) event.properties.site = SITE;
        return event;
      }
    });
    var held = early;
    early = null;
    held.forEach(function (c) {
      try { window.posthog[c[0]](c[1], c[2], c[0] === "capture" ? c[3] : undefined); } catch (e) {}
    });
  }

  // The library (~105 KB) waits until the page has loaded and the browser is idle.
  function whenIdle() {
    if (window.requestIdleCallback) requestIdleCallback(init, { timeout: 3000 });
    else setTimeout(init, 1);
  }
  if (document.readyState === "complete") whenIdle();
  else window.addEventListener("load", whenIdle);
})();

(function () {
  "use strict";

  var REPO = "ankushKun/sh23ds";
  var IMAGE = "ghcr.io/ankushkun/sh23ds-packer";
  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };
  var reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var track = window.track;

  function store(key, value) {
    try {
      if (value === undefined) return JSON.parse(localStorage.getItem(key) || "null");
      localStorage.setItem(key, JSON.stringify(value));
    } catch (e) { return null; }
  }

  function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

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
        if (!out) return;
        copyText(out.textContent.trim(), b);
        track("command_copied", { command: b.dataset.copy });
      });
    });
  }

  // A link to a folded section (#docker) opens it.
  function openTarget() {
    var el = location.hash && document.getElementById(decodeURIComponent(location.hash.slice(1)));
    if (el && el.tagName === "DETAILS") el.open = true;
  }

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

  // The opening still plays the video in its place. Nothing comes from YouTube until the click; a click
  // meant for a new tab or window goes to YouTube as a plain link.
  function tape() {
    var link = $("[data-tape]");
    if (!link) return;
    link.addEventListener("click", function (e) {
      if (e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
      e.preventDefault();
      var scene = link.closest(".scene");
      var player = document.createElement("iframe");
      player.src = "https://www.youtube-nocookie.com/embed/" + link.dataset.tape + "?autoplay=1&rel=0&playsinline=1";
      player.title = "Silent Hill 2 on a real 3DS: the install and the first fifteen minutes";
      player.allow = "autoplay; encrypted-media; picture-in-picture; fullscreen";
      player.allowFullscreen = true;
      player.referrerPolicy = "strict-origin-when-cross-origin";
      scene.classList.add("is-playing");
      scene.appendChild(player);
      player.focus();
      track("video_played", { video: link.dataset.tape });
    });
  }

  // ---------------------------------------------------------------- the Docker packer

  function detectOS() {
    var p = ((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || navigator.userAgent || "").toLowerCase();
    if (p.indexOf("win") >= 0) return "windows";
    if (p.indexOf("mac") >= 0) return "mac";
    return "linux";
  }

  // The Docker packer command for the chosen system and game folder.
  function builder() {
    var root = $("[data-builder]");
    if (!root) return;
    var ORDER = ["mac", "windows", "linux"];
    var NAMES = { mac: "macOS", windows: "Windows", linux: "Linux" };
    var SHELLS = { mac: "Terminal", windows: "PowerShell", linux: "Terminal" };
    var PLACEHOLDERS = { mac: "/Users/you/Games/Silent Hill 2", windows: "C:\\Games\\Silent Hill 2", linux: "/home/you/Games/Silent Hill 2" };
    var input = $("[data-game]", root), label = $("[data-os-label]", root), colima = $('[data-note="colima"]', root);
    var outPack = $('[data-out="pack"]');
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
      var path = clean(input.value || ""), shown = path || PLACEHOLDERS[os], pack;
      if (os === "windows") {
        var g = "<mark>" + esc(ps(shown)) + "</mark>";
        pack = "mkdir -Force sd\ndocker run --rm `\n  -v \"" + g + ":/game:ro\" `\n  -v \"${PWD}\\sd:/out\" `\n  " + IMAGE;
      } else {
        // A leading ~ doesn't expand inside quotes, so it becomes $HOME.
        var p = shown, prefix = "";
        if (/^~(\/|$)/.test(p)) { prefix = "$HOME"; p = p.slice(1); }
        var g2 = "<mark>" + prefix + esc(sh(p)) + "</mark>";
        pack = "mkdir -p sd\ndocker run --rm \\\n" + (os === "linux" ? '  --user "$(id -u):$(id -g)" \\\n' : "") +
          '  -v "' + g2 + ':/game:ro" \\\n  -v "$PWD/sd:/out" \\\n  ' + IMAGE;
      }
      outPack.innerHTML = pack;
      if (colima) colima.classList.toggle("is-on", os === "mac" && !!path && !/^(~|\/Users\/)/.test(path));
      store(KEY, { os: os, path: input.value });
    }

    $$("[data-os-step]", root).forEach(function (b) {
      b.addEventListener("click", function () {
        var i = ORDER.indexOf(os) + Number(b.dataset.osStep);
        os = ORDER[(i + ORDER.length) % ORDER.length];
        render();
        track("install_os_changed", { os: os });
      });
    });
    input.addEventListener("input", render);
    render();
  }

  release();
  copyButtons();
  openTarget();
  window.addEventListener("hashchange", openTarget);
  titleVideo();
  tape();
  builder();
})();
