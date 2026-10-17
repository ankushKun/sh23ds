// sh23ds website. The page works without this script: it maps the screenshots onto the console photo,
// switches the tabs, copies commands, opens screenshots with both screens, shows the latest release's
// version and file sizes, and fades sections in.
(function () {
  "use strict";

  var REPO = "ankushKun/sh23ds";
  var reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  // --k = rendered width / photo width, which the screen matrices are scaled by.
  function mapScreens() {
    document.querySelectorAll(".device-stage").forEach(function (stage) {
      var photoWidth = parseFloat(getComputedStyle(stage).getPropertyValue("--photo-w"));
      if (!photoWidth) return;
      function update() {
        if (stage.offsetWidth > 0) stage.style.setProperty("--k", (stage.offsetWidth / photoWidth).toFixed(5));
      }
      update();
      if ("ResizeObserver" in window) new ResizeObserver(update).observe(stage);
      else window.addEventListener("resize", update);
      stage.classList.add("is-mapped");
    });
  }

  // A slight tilt of the console towards the pointer (fine pointers only, never with reduced motion).
  function tilt() {
    var device = document.querySelector("[data-tilt]");
    var stage = device && device.querySelector(".device-stage");
    if (!stage || reduceMotion || !window.matchMedia("(hover: hover) and (pointer: fine)").matches) return;
    var frame = null;
    device.addEventListener("pointermove", function (e) {
      var box = device.getBoundingClientRect();
      var x = (e.clientX - box.left) / box.width - 0.5;
      var y = (e.clientY - box.top) / box.height - 0.5;
      if (frame) cancelAnimationFrame(frame);
      frame = requestAnimationFrame(function () {
        stage.style.transform = "perspective(1600px) rotateX(" + (y * -3).toFixed(2) + "deg) rotateY(" + (x * 5).toFixed(2) + "deg)";
      });
    });
    device.addEventListener("pointerleave", function () {
      if (frame) cancelAnimationFrame(frame);
      stage.style.transform = "";
    });
  }

  function tabs() {
    document.querySelectorAll("[data-tabs]").forEach(function (box) {
      var buttons = Array.prototype.slice.call(box.querySelectorAll('[role="tab"]'));
      function select(button, focus) {
        buttons.forEach(function (b) {
          var on = b === button;
          b.setAttribute("aria-selected", on ? "true" : "false");
          b.tabIndex = on ? 0 : -1;
          document.getElementById(b.getAttribute("aria-controls")).hidden = !on;
        });
        if (focus) button.focus();
      }
      buttons.forEach(function (b, i) {
        b.addEventListener("click", function () { select(b, false); });
        b.addEventListener("keydown", function (e) {
          var next = e.key === "ArrowRight" ? i + 1 : e.key === "ArrowLeft" ? i - 1 : null;
          if (next === null) return;
          e.preventDefault();
          select(buttons[(next + buttons.length) % buttons.length], true);
        });
      });
      // Windows visitors see the Windows command first.
      if (/Windows/.test(navigator.userAgent)) {
        var win = box.querySelector("#tab-win");
        if (win) select(win, false);
      }
    });
  }

  function copyButtons() {
    if (!navigator.clipboard) return;
    document.querySelectorAll(".code").forEach(function (block) {
      if (block.classList.contains("tree")) return;
      var button = document.createElement("button");
      button.type = "button";
      button.className = "copy";
      button.textContent = "Copy";
      button.addEventListener("click", function () {
        // Copy the commands without prompts and comments.
        var text = block.querySelector("code").innerText.split("\n").map(function (line) {
          return line.replace(/^(\$ |PS> )/, "");
        }).filter(function (line) { return !/^\s*#/.test(line); }).join("\n").trim();
        navigator.clipboard.writeText(text).then(function () {
          button.textContent = "Copied";
          button.classList.add("is-done");
          setTimeout(function () {
            button.textContent = "Copy";
            button.classList.remove("is-done");
          }, 1600);
        });
      });
      block.appendChild(button);
    });
  }

  function viewer() {
    var dialog = document.querySelector("[data-viewer]");
    if (!dialog || typeof dialog.showModal !== "function") return;
    var top = dialog.querySelector("[data-viewer-top]");
    var bottom = dialog.querySelector("[data-viewer-bottom]");
    var caption = dialog.querySelector("[data-viewer-caption]");
    document.querySelectorAll(".shot").forEach(function (shot) {
      shot.addEventListener("click", function () {
        top.src = shot.dataset.top;
        top.alt = shot.querySelector("img").alt;
        bottom.src = shot.dataset.bottom;
        bottom.alt = "The touch screen at the same moment.";
        caption.textContent = shot.dataset.caption;
        dialog.showModal();
      });
    });
    dialog.querySelector("[data-viewer-close]").addEventListener("click", function () { dialog.close(); });
    dialog.addEventListener("click", function (e) {
      if (e.target === dialog) dialog.close();
    });
  }

  function nav() {
    var bar = document.querySelector("[data-nav]");
    var menu = document.querySelector(".nav-menu");
    function solid() {
      bar.classList.toggle("is-solid", window.scrollY > 40);
    }
    solid();
    window.addEventListener("scroll", solid, { passive: true });
    if (menu) {
      menu.querySelectorAll("a").forEach(function (a) {
        a.addEventListener("click", function () { menu.open = false; });
      });
    }
    if (!("IntersectionObserver" in window)) return;
    var links = {};
    document.querySelectorAll(".nav-links a").forEach(function (a) {
      links[a.getAttribute("href").slice(1)] = a;
    });
    var seen = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        var link = links[entry.target.id];
        if (!link || !entry.isIntersecting) return;
        Object.keys(links).forEach(function (id) { links[id].removeAttribute("aria-current"); });
        link.setAttribute("aria-current", "true");
      });
    }, { rootMargin: "-45% 0px -50% 0px" });
    Object.keys(links).forEach(function (id) {
      var section = document.getElementById(id);
      if (section) seen.observe(section);
    });
  }

  function reveal() {
    var items = document.querySelectorAll(".reveal");
    if (reduceMotion || !("IntersectionObserver" in window)) {
      items.forEach(function (el) { el.classList.add("is-in"); });
      return;
    }
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (!entry.isIntersecting) return;
        entry.target.classList.add("is-in");
        io.unobserve(entry.target);
      });
    }, { rootMargin: "0px 0px -8% 0px" });
    items.forEach(function (el) { io.observe(el); });
  }

  // The latest release's tag and file sizes. Before the first release the page keeps its defaults.
  function release() {
    if (!window.fetch) return;
    fetch("https://api.github.com/repos/" + REPO + "/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) {
        if (!data || !data.tag_name) return;
        document.querySelectorAll("[data-version]").forEach(function (el) { el.textContent = data.tag_name; });
        (data.assets || []).forEach(function (asset) {
          document.querySelectorAll('[data-size="' + asset.name + '"]').forEach(function (el) {
            el.textContent = (asset.size / 1048576).toFixed(2) + " MB";
          });
        });
      })
      .catch(function () {});
  }

  mapScreens();
  tilt();
  tabs();
  copyButtons();
  viewer();
  nav();
  reveal();
  release();
})();
