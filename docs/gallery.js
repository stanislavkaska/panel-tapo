/**
 * Panel světel — lightbox galerie obrazovek a fotek krabičky.
 * Obrazovky jsou pixelová grafika 480 × 480. Zvětšují se jen na celý násobek
 * (2× nebo 3×) s image-rendering: pixelated, aby zůstaly ostré; když se
 * násobek do okna nevejde, kreslí se originál 1:1, na malém displeji plynule.
 * Fotky (odkazy s data-photo) se zmenší plynule do okna. Šipky listují jen
 * v rámci jedné sady. Odkaz #obrazovka-N otevře N-tou obrazovku rovnou.
 */
(function () {
  'use strict';
  const lb = document.getElementById('lightbox');
  if (!lb) return;
  const img = lb.querySelector('img');
  const cap = lb.querySelector('.lb-caption');
  const NATIVE = 480;

  // Sady podle čísla v atributu; stejné číslo (třeba fotka v úvodu i v sekci
  // Fotky) = stejná položka, popisek má přednost z figcaption před alt
  const sets = { screen: [], photo: [] };
  function collect(attr, kind) {
    document.querySelectorAll('a[' + attr + ']').forEach((a) => {
      const i = parseInt(a.getAttribute(attr), 10) || 0;
      const fig = a.closest('figure');
      const fc = fig ? fig.querySelector('figcaption') : null;
      if (!sets[kind][i] || (fc && !sets[kind][i].fromFig)) {
        const caption = fc ? fc.textContent : (a.querySelector('img') || {}).alt;
        sets[kind][i] = { src: a.getAttribute('href'), caption: (caption || '').trim(), fromFig: !!fc };
      }
      a.addEventListener('click', (e) => {
        e.preventDefault();
        show(kind, i);
      });
    });
  }
  collect('data-lightbox', 'screen');
  collect('data-photo', 'photo');
  let kind = 'screen';
  let idx = 0;

  function fit() {
    if (kind === 'photo') {
      img.style.width = '';
      img.style.height = '';
      img.classList.remove('crisp');
      img.classList.add('photo');
      return;
    }
    img.classList.remove('photo');
    const maxW = window.innerWidth * 0.92;
    const maxH = window.innerHeight - 150;   // místo pro popisek a tlačítka
    const avail = Math.max(120, Math.min(maxW, maxH));
    const k = Math.min(3, Math.floor(avail / NATIVE));
    if (k >= 1) {
      img.style.width = (NATIVE * k) + 'px';
      img.style.height = (NATIVE * k) + 'px';
      img.classList.add('crisp');
    } else {
      const s = Math.floor(avail);
      img.style.width = s + 'px';
      img.style.height = s + 'px';
      img.classList.remove('crisp');
    }
  }

  function show(k, i) {
    const items = sets[k];
    if (!items.length) return;
    kind = k;
    idx = ((i % items.length) + items.length) % items.length;
    img.src = items[idx].src;
    img.alt = items[idx].caption;
    cap.textContent = items[idx].caption;
    lb.setAttribute('aria-label', kind === 'photo' ? 'Zvětšená fotka' : 'Zvětšená obrazovka');
    fit();
    lb.hidden = false;
    lb.classList.add('open');
    document.body.style.overflow = 'hidden';
  }

  function close() {
    lb.classList.remove('open');
    lb.hidden = true;
    document.body.style.overflow = '';
    if (location.hash.indexOf('#obrazovka-') === 0) history.replaceState(null, '', location.pathname + location.search);
  }

  lb.addEventListener('click', (e) => {
    if (e.target.closest('.lb-next')) { show(kind, idx + 1); return; }
    if (e.target.closest('.lb-prev')) { show(kind, idx - 1); return; }
    if (e.target.closest('.lb-close')) { close(); return; }
    if (!e.target.closest('img')) close();
  });
  window.addEventListener('keydown', (e) => {
    if (lb.hidden) return;
    if (e.key === 'Escape') close();
    else if (e.key === 'ArrowRight') show(kind, idx + 1);
    else if (e.key === 'ArrowLeft') show(kind, idx - 1);
  });
  window.addEventListener('resize', () => { if (!lb.hidden) fit(); });

  const m = /^#obrazovka-(\d+)$/.exec(location.hash);
  if (m) show('screen', parseInt(m[1], 10) - 1);
})();
