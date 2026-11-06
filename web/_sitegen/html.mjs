import { attr, esc } from './dmt.mjs';

export const REPO = 'https://github.com/HelgeSverre/dmscene';

const NAV = [
  { id: 'gallery', label: 'gallery', href: 'index.html' },
  { id: 'sources', label: 'sources', href: 'sources.html' },
  { id: 'about', label: 'about', href: 'about.html' },
  { id: 'repo', label: 'github', href: REPO, external: true },
  { id: 'random', label: 'random', href: 'index.html', random: true },
];

export function layout({
  title,
  description,
  body,
  root = '',
  active = '',
}) {
  const pageTitle = title === 'dmscene' ? 'dmscene demo archive' : `${title} — dmscene`;
  const nav = NAV.map((item) => {
    const attrs = [
      `href="${attr(item.external ? item.href : root + item.href)}"`,
      item.external ? 'rel="external"' : '',
      item.random ? 'data-random' : '',
      active === item.id ? 'aria-current="page"' : '',
    ].filter(Boolean).join(' ');
    return `<a ${attrs}>${esc(item.label)}</a>`;
  }).join('\n        ');

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${esc(pageTitle)}</title>
<meta name="description" content="${attr(description)}">
<meta name="generator" content="dmscene sitegen">
<link rel="icon" href="${root}favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="${root}style.css">
</head>
<body data-root="${root}">
<a class="skip" href="#main">skip to content</a>
<div class="shell">
  <header class="masthead">
    <div class="masthead-inner">
      <a class="brand" href="${root}index.html">dmscene</a>
      <span class="brand-description">Demos &amp; source</span>
    </div>
  <nav class="nav" aria-label="Site">
        ${nav}
  </nav>
  </header>
  <main id="main">
${body}
  </main>
  <footer class="footer">
    <p>dmscene · <a href="${REPO}" rel="external">GitHub</a> · <a href="${root}sources.html">Source files</a></p>
  </footer>
</div>
<script src="${root}app.js" defer></script>
${body.includes('data-live-demo') ? `<script type="module" src="${root}live.js"></script>` : ''}
</body>
</html>
`;
}

export function sectionHeading(title, id = '') {
  const anchor = id ? ` id="${attr(id)}"` : '';
  return `<h2${anchor}>${esc(title)}</h2>`;
}

export function fmtInt(value) {
  const n = Number(value) || 0;
  return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

export function fmtDuration(seconds, style = 'short') {
  if (!Number.isFinite(seconds) || seconds < 0) return '—';
  if (seconds === 0) return style === 'clock' ? '0:00' : '—';
  const total = Math.round(seconds);
  if (style === 'clock' || total >= 60) {
    const minutes = Math.floor(total / 60);
    const rest = total % 60;
    return `${minutes}:${String(rest).padStart(2, '0')}`;
  }
  return `${total} s`;
}

export function fmtBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return '—';
  const mb = bytes / (1024 * 1024);
  if (mb >= 1) return `${mb >= 10 ? Math.round(mb) : mb.toFixed(1)} MB`;
  const kb = bytes / 1024;
  return `${Math.max(1, Math.round(kb))} KB`;
}

export function fmtDate(ms) {
  if (!Number.isFinite(ms) || ms <= 0) return '';
  const d = new Date(ms);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

export function truncate(text, max = 180) {
  const clean = String(text || '').replace(/\s+/g, ' ').trim();
  if (clean.length <= max) return clean;
  const cut = clean.slice(0, max);
  const at = cut.lastIndexOf(' ');
  return `${(at > max * 0.6 ? cut.slice(0, at) : cut).trim()}…`;
}

export function tagLabel(tag) {
  return {
    all: 'all',
    model: 'model-made',
    homage: 'homage',
    brief: 'same brief',
    collab: 'collab',
    loops: 'loops',
    long: 'long',
    flashing: 'flashing',
    unrendered: 'unrendered',
  }[tag] || tag;
}

export function badge(text, cls = '') {
  return `<span class="badge ${attr(cls)}">${esc(text)}</span>`;
}

export function linkButton(href, label, attrs = '') {
  return `<a class="btn" href="${attr(href)}"${attrs ? ` ${attrs}` : ''}>${esc(label)}</a>`;
}
