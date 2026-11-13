/* RecLink guides — small progressive enhancements. The pages work without JavaScript. */
(function () {
  'use strict';
  var root = document.documentElement;
  var $ = function (s, c) { return (c || document).querySelector(s); };
  var $$ = function (s, c) { return Array.prototype.slice.call((c || document).querySelectorAll(s)); };
  var store = {
    get: function (k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set: function (k, v) { try { localStorage.setItem(k, v); } catch (e) {} }
  };

  /* ---- theme ---- */
  function effectiveTheme() {
    var t = root.dataset.theme;
    if (t === 'light' || t === 'dark') return t;
    return window.matchMedia && matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
  }
  function syncThemeColor() {
    var m = $('meta[name="theme-color"]');
    if (m) m.setAttribute('content', effectiveTheme() === 'light' ? '#f6f8fa' : '#0f1216');
  }
  var themeBtn = $('.theme-btn');
  if (themeBtn) themeBtn.addEventListener('click', function () {
    var next = effectiveTheme() === 'light' ? 'dark' : 'light';
    root.dataset.theme = next;
    store.set('rl-theme', next);
    syncThemeColor();
  });
  syncThemeColor();

  /* ---- language: remember the choice, keep the section when switching ---- */
  $$('.seg a[data-lang]').forEach(function (a) {
    var base = a.getAttribute('href');
    function update() { if (base && base !== '#') a.setAttribute('href', base.split('#')[0] + location.hash); }
    update();
    window.addEventListener('hashchange', update);
    a.addEventListener('click', function () { store.set('rl-lang', a.dataset.lang); });
  });
  var cur = $('.seg a[aria-current="true"]');
  /* first visit (no saved choice): Italian browsers get the Italian pages, everyone else English */
  if (cur && !store.get('rl-lang')) {
    var want = (function(){var a=navigator.languages&&navigator.languages.length?navigator.languages:[navigator.language||''];for(var i=0;i<a.length;i++){if(/^it\b/i.test(a[i]))return 'it';if(/^en\b/i.test(a[i]))return 'en';}return 'en';})();
    var alt = $('.seg a[data-lang="' + want + '"]');
    if (want !== cur.dataset.lang && alt && alt.getAttribute('href') !== '#') {
      store.set('rl-lang', want);
      location.replace(alt.href);
    }
  }
  if (cur) store.set('rl-lang', cur.dataset.lang);

  /* ---- PDF menu: close on outside click / Escape ---- */
  var menus = $$('details.menu');
  document.addEventListener('click', function (e) {
    menus.forEach(function (d) { if (d.open && !d.contains(e.target)) d.open = false; });
  });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') { menus.forEach(function (d) { d.open = false; }); closeToc(); closeLightbox(); }
  });

  /* ---- table of contents (mobile drawer + scroll spy) ---- */
  var toc = $('#toc');
  var tocBtn = $('.toc-btn');
  function closeToc() {
    if (!toc) return;
    toc.classList.remove('open');
    if (tocBtn) tocBtn.setAttribute('aria-expanded', 'false');
  }
  if (toc && tocBtn) {
    tocBtn.addEventListener('click', function (e) {
      e.stopPropagation();
      var open = toc.classList.toggle('open');
      tocBtn.setAttribute('aria-expanded', open ? 'true' : 'false');
    });
    toc.addEventListener('click', function (e) { if (e.target.closest('a')) closeToc(); });
    document.addEventListener('click', function (e) {
      if (toc.classList.contains('open') && !toc.contains(e.target) && !tocBtn.contains(e.target)) closeToc();
    });
  }

  var progress = $('#progress');
  var items = toc ? $$('#toc > .side-in > ol > li') : [];
  var subLinks = toc ? $$('#toc ol.sub li') : [];
  var heads = [];
  if (items.length) {
    heads = $$('.content section.chapter[id], .content section.chapter h2[id]');
  }
  var ticking = false;
  function onScroll() {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(function () {
      ticking = false;
      var y = window.scrollY, h = document.documentElement.scrollHeight - window.innerHeight;
      if (progress) progress.style.transform = 'scaleX(' + (h > 0 ? Math.min(1, y / h) : 0) + ')';
      if (!heads.length) return;
      var line = Math.round(window.innerHeight * 0.38), activeSec = null, activeSub = null;
      for (var i = 0; i < heads.length; i++) {
        var r = heads[i].getBoundingClientRect();
        if (r.top - line > 0) break;
        if (heads[i].tagName === 'SECTION') { activeSec = heads[i]; activeSub = null; } else { activeSub = heads[i]; }
      }
      var sid = activeSec ? activeSec.id : null;
      items.forEach(function (li) {
        var on = li.dataset.id === sid || (activeSub && li.dataset.id === activeSub.id);
        li.classList.toggle('active', !!on);
      });
      subLinks.forEach(function (li) {
        var a = li.firstElementChild;
        li.classList.toggle('on', !!(activeSub && a && a.getAttribute('href') === '#' + activeSub.id));
      });
      var act = $('#toc li.active > a');
      var box = $('#toc .side-in');
      if (act && box && !box.contains(document.activeElement)) {
        var ar = act.getBoundingClientRect(), br = box.getBoundingClientRect();
        if (ar.top < br.top + 8 || ar.bottom > br.bottom - 8) box.scrollTop += ar.top - br.top - br.height / 3;
      }
    });
  }
  window.addEventListener('scroll', onScroll, { passive: true });
  window.addEventListener('resize', onScroll);
  onScroll();

  /* ---- screenshot lightbox ---- */
  var lb = null;
  function closeLightbox() { if (lb) { lb.remove(); lb = null; } }
  document.addEventListener('click', function (e) {
    var a = e.target.closest && e.target.closest('a.zoom');
    if (!a) return;
    if (e.metaKey || e.ctrlKey || e.shiftKey) return;
    e.preventDefault();
    closeLightbox();
    lb = document.createElement('div');
    lb.className = 'lb';
    lb.setAttribute('role', 'dialog');
    lb.setAttribute('aria-modal', 'true');
    var img = document.createElement('img');
    img.src = a.getAttribute('href');
    img.alt = a.dataset.caption || '';
    lb.appendChild(img);
    if (a.dataset.caption) { var p = document.createElement('p'); p.textContent = a.dataset.caption; lb.appendChild(p); }
    lb.addEventListener('click', closeLightbox);
    document.body.appendChild(lb);
  });
})();
