// Tiq doc site JS: mobile nav, copy buttons, Ctrl+K search. No dependencies.
(function () {
  var menuBtn = document.getElementById("menu-btn");
  var navLinks = document.getElementById("nav-links");
  if (menuBtn && navLinks) {
    menuBtn.addEventListener("click", function () {
      navLinks.classList.toggle("open");
    });
  }

  document.querySelectorAll("[data-copy]").forEach(function (btn) {
    btn.addEventListener("click", function () {
      var id = btn.getAttribute("data-copy");
      var el = document.getElementById(id);
      if (!el) return;
      var text = el.innerText;
      if (navigator.clipboard) navigator.clipboard.writeText(text);
      btn.textContent = "Copied";
      setTimeout(function () { btn.textContent = "Copy"; }, 1200);
    });
  });

  var overlay = document.getElementById("search-overlay");
  var input = document.getElementById("search-input");
  var results = document.getElementById("search-results");
  var openBtn = document.getElementById("search-btn");
  var INDEX = [
    { t: "Introduction — Tiny source, explicit intent", u: "index.html#intro" },
    { t: "Quick start — build & run", u: "docs.html#getting-started" },
    { t: "Syntax at a glance", u: "index.html#syntax" },
    { t: "Functions read like definitions", u: "examples.html#functions" },
    { t: "Recurrences — fib, factorial, powers", u: "examples.html#recurrence" },
    { t: "Bracket loops — ranges, while, nesting", u: "examples.html#loops" },
    { t: "Mutation — = vs <-, &mut borrows", u: "examples.html#mutation" },
    { t: "Option & Result — ? and ??", u: "examples.html#option-result" },
    { t: "Structs, slices, views", u: "examples.html#structs" },
    { t: "CLI — check, run, build, emit-c", u: "docs.html#cli" },
    { t: "Quick reference cheat sheet", u: "docs.html#quick-reference" },
    { t: "Tiq Book", u: "docs.html#book" },
    { t: "Playground (static demo)", u: "../../editors/playground/index.html" },
    { t: "Implementation status", u: "docs.html#status" },
    { t: "Benchmarks vs C/Go/Rust/Python", u: "index.html#benchmarks" }
  ];
  function render(q) {
    if (!results) return;
    var query = (q || "").toLowerCase();
    var hits = INDEX.filter(function (x) { return x.t.toLowerCase().indexOf(query) !== -1; }).slice(0, 12);
    results.innerHTML = hits.map(function (x) {
      return '<a href="' + x.u + '">' + x.t + "</a>";
    }).join("") || '<a href="docs.html">No matches — open Docs</a>';
  }
  function open() { if (overlay) { overlay.classList.add("open"); render(""); if (input) { input.value = ""; setTimeout(function(){ input.focus(); }, 0); } } }
  function close() { if (overlay) overlay.classList.remove("open"); }
  if (openBtn) openBtn.addEventListener("click", open);
  document.addEventListener("keydown", function (e) {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") { e.preventDefault(); overlay.classList.contains("open") ? close() : open(); }
    if (e.key === "Escape") close();
  });
  if (overlay) overlay.addEventListener("click", function (e) { if (e.target === overlay) close(); });
  if (input) input.addEventListener("input", function () { render(input.value); });
  render("");
})();
