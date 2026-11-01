/**
 * Panel světel — barevná syntaxe bloků kódu bez knihoven.
 * Blok: <pre><code data-lang="ps">…</code></pre> (PowerShell) nebo data-lang="cpp" (C++).
 * Pravidla se hledají zleva doprava, na stejném místě vyhrává první v seznamu.
 */
(function () {
  'use strict';
  const RULES = {
    ps: [
      ['com', /#[^\n]*/],
      ['str', /"[^"\n]*"|'[^'\n]*'/],
      ['par', /(?<=\s)--?[A-Za-z][\w-]*/],
      ['cmd', /(?<=^[ \t]*)(?:\.\\)?[\w.\\-]+/],
      ['num', /\b\d+(?:\.\d+)*\b/],
      ['pun', /`$/],
    ],
    cpp: [
      ['com', /\/\/[^\n]*/],
      ['pre', /^#\w+/],
      ['str', /"(?:[^"\\\n]|\\.)*"/],
      ['kw', /\b(?:static|const|struct)\b/],
      ['typ', /\b(?:WifiNet|TapoDev|char|int|bool)\b/],
      ['mac', /\b[A-Z][A-Z0-9_]{2,}\b/],
      ['num', /\b\d+(?:\.\d+)*\b/],
      ['pun', /[{}[\]();,=]/],
    ],
  };
  const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

  function highlight(text, rules) {
    // Pravidla nemají vlastní zachytávací skupiny, číslo skupiny = pořadí pravidla
    const re = new RegExp(rules.map(([, r]) => '(' + r.source + ')').join('|'), 'gm');
    let out = '';
    let last = 0;
    let m;
    while ((m = re.exec(text)) !== null) {
      if (!m[0]) { re.lastIndex++; continue; }
      const k = m.slice(1).findIndex((g) => g !== undefined);
      out += esc(text.slice(last, m.index)) + '<span class="tok-' + rules[k][0] + '">' + esc(m[0]) + '</span>';
      last = m.index + m[0].length;
    }
    return out + esc(text.slice(last));
  }

  document.querySelectorAll('pre code[data-lang]').forEach((code) => {
    const rules = RULES[code.dataset.lang];
    if (rules) code.innerHTML = highlight(code.textContent, rules);
  });
})();
