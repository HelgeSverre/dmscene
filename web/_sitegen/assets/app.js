(() => {
  'use strict';

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  /* ------------------------------------------------------- gallery filter */

  const gallery = $('#gallery-grid');
  if (gallery) {
    const cards = $$('[data-demo]', gallery);
    const search = $('#q');
    const sort = $('#sort');
    const count = $('#count');
    const empty = $('#empty');
    const featured = $('#featured');
    const tagButtons = $$('[data-tag]');
    const total = cards.length;
    let activeTag = 'all';

    const visible = () => cards.filter((card) => !card.hidden);

    const apply = () => {
      const query = (search?.value || '').trim().toLowerCase();
      for (const card of cards) {
        const text = card.dataset.search || '';
        const tags = (card.dataset.tags || '').split(/\s+/);
        const matchesText = !query || text.includes(query);
        const matchesTag = activeTag === 'all' || tags.includes(activeTag);
        card.hidden = !(matchesText && matchesTag);
      }

      const key = sort?.value || 'az';
      const ordered = [...cards].sort((a, b) => {
        const nameA = a.dataset.title || '';
        const nameB = b.dataset.title || '';
        switch (key) {
          case 'za':
            return nameB.localeCompare(nameA);
          case 'duration':
            return (Number(a.dataset.duration) || 1e9) - (Number(b.dataset.duration) || 1e9);
          case 'bpm':
            return (Number(a.dataset.bpm) || 1e9) - (Number(b.dataset.bpm) || 1e9);
          case 'recent':
            return (Number(b.dataset.mtime) || 0) - (Number(a.dataset.mtime) || 0);
          default:
            return nameA.localeCompare(nameB);
        }
      });
      gallery.append(...ordered);

      const shown = visible().length;
      if (count) {
        count.textContent = shown === total
          ? `${total} demos.`
          : `${shown} of ${total} demos.`;
      }
      if (empty) empty.hidden = shown !== 0;
      if (featured) featured.hidden = Boolean(query) || activeTag !== 'all';
      const url = new URL(location.href);
      for (const [name, value] of [['q', search?.value.trim()], ['tag', activeTag === 'all' ? '' : activeTag], ['sort', key === 'az' ? '' : key]]) {
        if (value) url.searchParams.set(name, value);
        else url.searchParams.delete(name);
      }
      if (url.href !== location.href) history.replaceState(null, '', url);
    };

    for (const button of tagButtons) {
      button.addEventListener('click', () => {
        activeTag = button.dataset.tag || 'all';
        for (const other of tagButtons) {
          other.setAttribute('aria-pressed', String(other === button));
        }
        apply();
      });
    }

    search?.addEventListener('input', apply);
    sort?.addEventListener('change', apply);

    const restore = () => {
      const params = new URLSearchParams(location.search);
      if (search) search.value = params.get('q') || '';
      activeTag = tagButtons.some(button => button.dataset.tag === params.get('tag')) ? params.get('tag') : 'all';
      if (sort) sort.value = [...sort.options].some(option => option.value === params.get('sort')) ? params.get('sort') : 'az';
      for (const button of tagButtons) button.setAttribute('aria-pressed', String(button.dataset.tag === activeTag));
      apply();
    };
    $('[data-reset-gallery]')?.addEventListener('click', () => {
      if (search) search.value = '';
      if (sort) sort.value = 'az';
      activeTag = 'all';
      for (const button of tagButtons) button.setAttribute('aria-pressed', String(button.dataset.tag === 'all'));
      apply(); search?.focus();
    });
    window.addEventListener('popstate', restore);
    restore();

    document.addEventListener('keydown', (event) => {
      if (event.key === '/' && search && document.activeElement !== search) {
        const tag = document.activeElement?.tagName;
        if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
        event.preventDefault();
        search.focus();
      }
    });
  }

  /* -------------------------------------------------------- source filter */

  const sourceQuery = $('#source-q');
  if (sourceQuery) {
    const rows = $$('[data-source-row]');
    const sourceCount = $('#source-count');
    const filter = () => {
      const query = sourceQuery.value.trim().toLowerCase();
      let shown = 0;
      for (const row of rows) {
        const match = !query || (row.dataset.search || '').includes(query);
        row.hidden = !match;
        if (match) shown += 1;
      }
      if (sourceCount) {
        sourceCount.textContent = shown === rows.length
          ? `${rows.length} source files.`
          : `${shown} of ${rows.length} source files.`;
      }
    };
    sourceQuery.addEventListener('input', filter);
    filter();
  }

  /* --------------------------------------------------------- video dialog */

  const dialog = $('#viewer');
  const player = $('#player');
  if (dialog && typeof dialog.showModal === 'function') {
    const video = $('#viewer-video');
    const title = $('#viewer-title');
    const download = $('#viewer-download');
    const detail = $('#viewer-detail');
    let opener = null;

    document.addEventListener('click', (event) => {
      const trigger = event.target.closest('[data-watch]');
      if (!trigger) return;
      event.preventDefault();
      opener = trigger;
      const source = trigger.dataset.video;
      if (!source) return;
      if (title) title.textContent = trigger.dataset.title || 'demo';
      if (video) {
        video.src = source;
        video.poster = trigger.dataset.poster || '';
      }
      if (download) {
        download.href = source;
        download.setAttribute('download', '');
      }
      if (detail) detail.href = trigger.dataset.detail || '#';
      if (!dialog.open) dialog.showModal();
      if (video) video.play().catch(() => {});
    });

    const close = $('#viewer-close');
    close?.addEventListener('click', () => dialog.close());

    dialog.addEventListener('click', (event) => {
      if (event.target === dialog) dialog.close();
    });

    dialog.addEventListener('close', () => {
      if (video) {
        video.pause();
        video.removeAttribute('src');
        video.removeAttribute('poster');
        video.load();
      }
      opener?.focus?.();
    });
  }

  /* ----------------------------------------------------------- timeline seek */

  if (player) {
    document.addEventListener('click', (event) => {
      const target = event.target.closest('[data-seek]');
      if (!target) return;
      const time = Number(target.dataset.seek);
      if (!Number.isFinite(time)) return;
      event.preventDefault();
      player.currentTime = time;
      player.play().catch(() => {});
      player.scrollIntoView({ behavior: reduced ? 'auto' : 'smooth', block: 'center' });
    });
  }

  /* ------------------------------------------------------------- code wrap */

  const wrap = $('#wrap-code');
  if (wrap) {
    wrap.addEventListener('change', () => {
      $('#code')?.classList.toggle('wrap', wrap.checked);
    });
  }

  /* ------------------------------------------------------------- copy source */

  document.addEventListener('click', async (event) => {
    const button = event.target.closest('[data-copy-source]');
    if (!button) return;
    event.preventDefault();
    const url = button.dataset.copySource;
    const label = button.textContent;
    try {
      const response = await fetch(url);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const text = await response.text();
      await navigator.clipboard.writeText(text);
      button.textContent = 'copied';
      window.setTimeout(() => { button.textContent = label; }, 1500);
    } catch {
      button.textContent = 'copy failed';
      window.setTimeout(() => { button.textContent = label; }, 1800);
    }
  });

  /* --------------------------------------------------------------- random */

  const random = $('[data-random]');
  if (random) {
    random.addEventListener('click', async (event) => {
      event.preventDefault();
      const cards = $$('[data-demo]:not([hidden])');
      if (!cards.length) {
        if (gallery) return;
        const root = document.body.dataset.root || '';
        try {
          const response = await fetch(root + 'demos.json');
          if (!response.ok) throw new Error('Demo list unavailable');
          const demos = await response.json();
          const choices = demos.filter(demo => !window.location.pathname.endsWith('/' + demo.url));
          if (choices.length) window.location.href = root + choices[Math.floor(Math.random() * choices.length)].url;
        } catch {
          window.location.href = random.href;
        }
        return;
      }
      const card = cards[Math.floor(Math.random() * cards.length)];
      const link = card.querySelector('h3 a') || card.querySelector('a[href]');
      if (link) window.location.href = link.href;
    });
  }
})();
