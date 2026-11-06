#!/usr/bin/env node
// Static showcase generator for dmscene.
//
//   node web/_sitegen/sitegen.mjs        (or: make site)
//
// Reads demos/*.dmt and build/video/*.mp4, copies the media into web/, and
// writes plain HTML/CSS/JS. The output folder is the site root. No npm
// dependencies; ffprobe/ffmpeg are used for video metadata and poster stills.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { assetPath, assetUrl } from './assets/asset-path.mjs';

import {
  analyzeDmt,
  attr,
  esc,
  highlightSource,
  paletteGradient,
} from './dmt.mjs';
import {
  REPO,
  badge,
  fmtBytes,
  fmtDuration,
  fmtInt,
  layout,
  sectionHeading,
  tagLabel,
  truncate,
} from './html.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const WEB = path.resolve(process.env.DM_WEB_OUT || path.join(ROOT, 'web'));
const RUNTIME = path.join(ROOT, 'web', 'runtime');
const DEMOS_DIR = path.join(ROOT, 'demos');
const BUILD_VIDEO = path.join(ROOT, 'build', 'video');
const SITE_ASSETS = path.join(HERE, 'assets');

const HAS_FFMPEG = commandExists('ffmpeg');
const HAS_FFPROBE = commandExists('ffprobe');

if (!HAS_FFPROBE) {
  console.warn('ffprobe unavailable: building source/live pages without MP4 metadata.');
}

/* ------------------------------------------------------------------ utils */

function commandExists(command) {
  const result = spawnSync(command, ['-version'], { stdio: 'ignore' });
  return !result.error && result.status === 0;
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function writeGenerated(file, content) {
  ensureDir(path.dirname(file));
  const temp = `${file}.${process.pid}.tmp`;
  try {
    fs.writeFileSync(temp, content);
    fs.renameSync(temp, file);
  } finally { fs.rmSync(temp, { force: true }); }
}

function copyIfChanged(source, destination) {
  const from = fs.statSync(source);
  let to = null;
  try {
    to = fs.statSync(destination);
  } catch {
    // destination does not exist yet
  }
  if (to && to.size === from.size && Math.abs(to.mtimeMs - from.mtimeMs) < 2) return false;

  ensureDir(path.dirname(destination));
  try {
    fs.copyFileSync(source, destination, fs.constants.COPYFILE_FICLONE);
  } catch {
    fs.copyFileSync(source, destination);
  }
  fs.utimesSync(destination, from.atime, from.mtime);
  return true;
}

function copyDirectory(source, destination) {
  ensureDir(destination);
  let count = 0;
  for (const entry of fs.readdirSync(source, { withFileTypes: true })) {
    if (entry.name === '.DS_Store') continue;
    const from = path.join(source, entry.name);
    const to = path.join(destination, entry.name);
    if (entry.isDirectory()) count += copyDirectory(from, to);
    else if (entry.isFile() && copyIfChanged(from, to)) count += 1;
  }
  return count;
}

function parseFps(value) {
  if (!value) return 0;
  const [num, den] = String(value).split('/').map(Number);
  if (Number.isFinite(num) && Number.isFinite(den) && den) return num / den;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : 0;
}

function probeVideo(file) {
  if (!HAS_FFPROBE) return null;
  const result = spawnSync('ffprobe', [
    '-v', 'error',
    '-print_format', 'json',
    '-show_format',
    '-show_streams',
    file,
  ], { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });

  if (result.error || result.status !== 0 || !result.stdout) return null;

  let data;
  try {
    data = JSON.parse(result.stdout);
  } catch {
    return null;
  }

  const streams = Array.isArray(data.streams) ? data.streams : [];
  const videoStream = streams.find((stream) => stream.codec_type === 'video');
  const audioStream = streams.find((stream) => stream.codec_type === 'audio');
  if (!videoStream) return null;

  const duration = Number.parseFloat(data.format?.duration || videoStream.duration || '0');
  if (!Number.isFinite(duration) || duration <= 0) return null;

  const stat = fs.statSync(file);
  return {
    file,
    duration,
    width: Number(videoStream.width) || 0,
    height: Number(videoStream.height) || 0,
    fps: parseFps(videoStream.r_frame_rate),
    codec: videoStream.codec_name || '',
    audio: Boolean(audioStream),
    size: Number(data.format?.size) || stat.size,
    mtimeMs: stat.mtimeMs,
  };
}

function ensurePoster(demoName, video) {
  const destination = path.join(WEB, 'posters', `${demoName}.jpg`);
  let existing = null;
  try {
    existing = fs.statSync(destination);
  } catch {
    // not generated yet
  }
  const source = fs.statSync(video.file);
  if (existing && existing.mtimeMs >= source.mtimeMs - 2 && existing.size > 0) return true;
  if (!HAS_FFMPEG) return false;

  const at = Math.min(
    Math.max(video.duration * 0.28, 0.4),
    15,
    Math.max(0.4, video.duration * 0.9),
  );
  const result = spawnSync('ffmpeg', [
    '-y', '-hide_banner', '-loglevel', 'error',
    '-ss', at.toFixed(3),
    '-i', video.file,
    '-frames:v', '1',
    '-an',
    '-vf', 'scale=720:-2:flags=lanczos',
    '-q:v', '4',
    destination,
  ], { encoding: 'utf8' });

  if (result.error || result.status !== 0) {
    console.warn(`  ! poster failed for ${demoName}: ${(result.stderr || '').trim()}`);
    return false;
  }
  try {
    return fs.statSync(destination).size > 0;
  } catch {
    return false;
  }
}

/* --------------------------------------------------------------- load data */

ensureDir(WEB);
ensureDir(path.join(WEB, 'videos'));
ensureDir(path.join(WEB, 'posters'));
ensureDir(path.join(WEB, 'source'));
ensureDir(path.join(WEB, 'assets', 'demo'));
ensureDir(path.join(WEB, 'demo'));

const demoFiles = fs.readdirSync(DEMOS_DIR)
  .filter((file) => file.endsWith('.dmt'))
  .sort();

const demoSet = new Set(demoFiles.map((file) => path.basename(file, '.dmt')));
if (fs.existsSync(BUILD_VIDEO)) {
  const extras = fs.readdirSync(BUILD_VIDEO)
    .filter((file) => file.endsWith('.mp4'))
    .map((file) => path.basename(file, '.mp4'))
    .filter((name) => !demoSet.has(name));
  if (extras.length) {
    console.log(`ignoring non-demo videos: ${extras.join(', ')}`);
  }
}

let copiedVideos = 0;
let generatedPosters = 0;
let sourceCopies = 0;

const demos = demoFiles.map((file) => {
  const name = path.basename(file, '.dmt');
  const sourcePath = path.join(DEMOS_DIR, file);
  const source = fs.readFileSync(sourcePath, 'utf8');
  const analysis = analyzeDmt(source, name);
  const meta = analysis.metadata;

  const sourceCopy = path.join(WEB, 'source', file);
  if (copyIfChanged(sourcePath, sourceCopy)) sourceCopies += 1;

  const videoPath = path.join(BUILD_VIDEO, `${name}.mp4`);
  let video = null;
  if (fs.existsSync(videoPath)) {
    video = probeVideo(videoPath);
    if (!video) {
      console.warn(`  ! ${name}.mp4 is not a complete MP4 yet (skipped)`);
    } else {
      const destination = path.join(WEB, 'videos', `${name}.mp4`);
      if (copyIfChanged(videoPath, destination)) copiedVideos += 1;
      video.copiedPath = destination;
    }
  }

  let poster = false;
  if (video) {
    poster = ensurePoster(name, video);
    if (poster) generatedPosters += 1;
  }

  const duration = video?.duration
    || meta.duration
    || analysis.songLength
    || 0;
  const bpm = analysis.song.bpm || Number(meta.bpm) || 0;

  const tags = new Set(meta.tags);
  if (meta.model) tags.add('model');
  const homageText = `${analysis.description} ${meta.visual || ''} ${meta.reading || ''}`;
  if (/homage|study (?:after|of direction)|after fr-|after "|port of/i.test(homageText)) {
    tags.add('homage');
  }
  if (analysis.song.loop && Number(analysis.song.loop) !== 0) tags.add('loops');
  if (duration >= 45) tags.add('long');
  if (analysis.warning) tags.add('flashing');
  if (!video) tags.add('unrendered');

  const summary = analysis.summary
    || meta.reading
    || meta.visual
    || meta.music
    || '';

  const search = [
    name,
    analysis.title,
    analysis.description,
    summary,
    meta.model,
    meta.author,
    meta.collection,
    meta.music,
    meta.visual,
    meta.reading,
    [...tags].join(' '),
  ].join(' ').toLowerCase();

  return {
    name,
    file,
    sourcePath,
    analysis,
    meta,
    video,
    poster,
    duration,
    bpm,
    tags: [...tags],
    summary,
    search,
    mtime: video?.mtimeMs || fs.statSync(sourcePath).mtimeMs,
  };
});

let assetCopies = 0;
const assetPaths = new Set(demos.flatMap(demo => [...demo.analysis.samples, ...demo.analysis.textures]).map(resource => assetPath(resource.path)));
for (const relative of assetPaths) {
  const source = path.join(DEMOS_DIR, relative);
  if (!fs.realpathSync(source).startsWith(fs.realpathSync(DEMOS_DIR) + path.sep)) throw new Error(`Asset outside demos/: ${relative}`);
  if (copyIfChanged(source, path.join(WEB, 'source', relative))) assetCopies++;
}

/* ------------------------------------------------------------------- stats */

const rendered = demos.filter((demo) => demo.video);
const totalRuntime = rendered.reduce((sum, demo) => sum + demo.video.duration, 0);
const totalLines = demos.reduce((sum, demo) => sum + demo.analysis.stats.lineCount, 0);
const totalPrograms = demos.reduce((sum, demo) => sum + demo.analysis.programCount, 0);
const models = new Map();
for (const demo of demos) {
  if (!demo.meta.model) continue;
  if (!models.has(demo.meta.model)) models.set(demo.meta.model, []);
  models.get(demo.meta.model).push(demo);
}

const stats = {
  demos: demos.length,
  rendered: rendered.length,
  runtime: totalRuntime,
  lines: totalLines,
  programs: totalPrograms,
  unrendered: demos.length - rendered.length,
};

console.log(`demos: ${stats.demos}, rendered: ${stats.rendered}, runtime: ${fmtDuration(stats.runtime, 'clock')}`);
console.log(`media: ${copiedVideos} video(s) copied, ${generatedPosters} poster(s) checked, ${sourceCopies} source(s) copied, ${assetCopies} asset(s) copied`);

/* ------------------------------------------------------------------ helpers */

const TAG_ORDER = [...new Set(['model', 'homage', 'brief', 'collab', 'loops', 'long', 'flashing', 'unrendered', ...demos.flatMap(demo => demo.tags).sort()])];

const KIND_COLORS = {
  title: '#17233f',
  song: '#2d3d63',
  order: '#4d5b80',
  pan: '#68749a',
  uniform: '#8a5b00',
  text: '#8b3a52',
  sample: '#7a4b8f',
  texture: '#3f6b3a',
  venv: '#2f6f6b',
  timeline: '#9d3d21',
  fn: '#6b3fa0',
  inst: '#194d86',
  chan: '#2f6f6b',
  pattern: '#c7b38c',
  frame: '#3f6b3a',
  pixel: '#93b3c6',
  post: '#8a5b00',
  master: '#8b2f18',
  palette: '#9fb89c',
};

function demoUrl(demo, root = '') {
  return `${root}demo/${demo.name}.html`;
}

function sourceUrl(demo, root = '') {
  return `${root}source/${demo.name}.dmt`;
}

function videoUrl(demo, root = '') {
  return `${root}videos/${demo.name}.mp4`;
}

function posterUrl(demo, root = '') {
  return `${root}posters/${demo.name}.jpg`;
}

function githubUrl(demo) {
  return `${REPO}/blob/main/demos/${demo.name}.dmt`;
}

function tagBadges(tags) {
  return TAG_ORDER
    .filter((tag) => tags.includes(tag))
    .map((tag) => badge(tagLabel(tag), tag === 'flashing' ? 'hot' : tag === 'unrendered' ? '' : 'ok'))
    .join('');
}

function metaLine(demo) {
  const bits = [];
  if (demo.bpm) bits.push(`${demo.bpm} BPM`);
  if (demo.duration) bits.push(fmtDuration(demo.duration));
  if (demo.video) bits.push(`${demo.video.width}×${demo.video.height}`);
  else bits.push('render pending');
  return bits.join(' · ');
}

function demoCard(demo, root = '') {
  const detail = demoUrl(demo, root);
  const watch = demo.video ? videoUrl(demo, root) : '';
  const poster = demo.poster ? posterUrl(demo, root) : '';
  const desc = truncate(demo.summary || 'No description.', 190);
  const palette = demo.analysis.palettes.find(palette => palette.keys.length);
  const paletteStrip = palette
    ? `<div class="card-palette" style="background:${paletteGradient(palette.keys)}" aria-hidden="true"></div>`
    : '';

  let thumb;
  if (demo.video && poster) {
    thumb = `<a class="thumb" href="${attr(watch)}" data-watch data-video="${attr(watch)}" data-title="${attr(demo.analysis.title)}" data-poster="${attr(poster)}" data-detail="${attr(detail)}">`
      + `<img src="${attr(poster)}" alt="${attr(demo.analysis.title)} — video still" loading="lazy" width="720" height="450">`
      + '<span class="play">watch</span></a>';
  } else if (demo.video) {
    thumb = `<a class="thumb pending" href="${attr(watch)}" data-watch data-video="${attr(watch)}" data-title="${attr(demo.analysis.title)}" data-detail="${attr(detail)}">`
      + '<span>watch mp4</span></a>';
  } else {
    thumb = `<a class="thumb pending" href="${attr(detail)}"><span>render pending</span></a>`;
  }

  const links = [];
  if (demo.video) links.push(`<a href="${attr(watch)}" download>MP4 ${fmtBytes(demo.video.size)}</a>`);
  links.push(`<a href="${attr(detail)}?mode=studio">Studio ↗</a>`);
  links.push(`<a href="${attr(detail)}#source">Source</a>`);
  links.push(`<span class="fname">${esc(demo.name)}.dmt</span>`);

  return `<article class="card" data-demo data-title="${attr(demo.analysis.title)}" data-tags="${attr(demo.tags.join(' '))}" data-search="${attr(demo.search)}" data-duration="${attr(demo.duration || 0)}" data-bpm="${attr(demo.bpm || 0)}" data-mtime="${attr(demo.mtime || 0)}">
  ${thumb}
  ${paletteStrip}
  <h3><a href="${attr(detail)}">${esc(demo.analysis.title)}</a></h3>
  <p class="meta">${esc(metaLine(demo))}${demo.meta.model ? ` · <span class="model">${esc(demo.meta.model)}</span>` : ''}</p>
  <p class="desc">${esc(desc)}</p>
  <p class="links">${links.join(' · ')}</p>
</article>`;
}

function featuredBlock(demo) {
  const detail = demoUrl(demo);
  const watch = demo.video ? videoUrl(demo) : '';
  const poster = demo.poster ? posterUrl(demo) : '';
  const media = poster
    ? `<a class="featured-media" href="${attr(watch)}" data-watch data-video="${attr(watch)}" data-title="${attr(demo.analysis.title)}" data-poster="${attr(poster)}" data-detail="${attr(detail)}"><img src="${attr(poster)}" alt="${attr(demo.analysis.title)} — video still" width="720" height="450"></a>`
    : `<a class="featured-media" href="${attr(detail)}"><span class="thumb pending" style="aspect-ratio:16/10"><span>${demo.video ? 'watch mp4' : 'render pending'}</span></span></a>`;

  const actions = [];
  if (demo.video && watch) {
    actions.push(`<a class="btn primary" href="${attr(watch)}" data-watch data-video="${attr(watch)}" data-title="${attr(demo.analysis.title)}" data-poster="${attr(poster)}" data-detail="${attr(detail)}">watch</a>`);
  }
  actions.push(`<a class="btn" href="${attr(detail)}?mode=studio">Open Studio ↗</a>`);
  if (demo.video) actions.push(`<a class="btn" href="${attr(watch)}" download>download ${fmtBytes(demo.video.size)}</a>`);

  const badges = tagBadges(demo.tags);
  return `<section class="featured" id="featured">
  ${media}
  <div class="featured-copy">
    <h2>${esc(demo.analysis.title)}</h2>
    <p class="meta">${esc(metaLine(demo))} · ${esc(demo.name)}.dmt</p>
    <p>${esc(truncate(demo.summary || demo.meta.visual || '', 180))}</p>
    ${badges ? `<p class="badges">${badges}</p>` : ''}
    <div class="actions">${actions.join(' ')}</div>
    <dl class="featured-anatomy"><div><dt>Patterns</dt><dd>${demo.analysis.counts.pattern}</dd></div><div><dt>Programs</dt><dd>${demo.analysis.programCount}</dd></div><div><dt>Events</dt><dd>${demo.analysis.timeline.events.length}</dd></div></dl>
  </div>
</section>`;
}

function timelineSvg(demo) {
  const events = demo.analysis.timeline.events;
  if (!events.length) return '';
  const names = demo.analysis.timeline.names;
  const maxTime = Math.max(
    demo.video?.duration || 0,
    demo.analysis.timeline.length,
    demo.analysis.songLength,
    0.1,
  );
  const width = 760;
  const left = 116;
  const right = 18;
  const top = 22;
  const laneHeight = 21;
  const chartWidth = width - left - right;
  const height = top + names.length * laneHeight + 27;
  const colors = ['#1d4f86', '#9d3d21', '#3f6b3a', '#7a4b8f', '#8a5b00', '#2f6f6b', '#8b3a52', '#4c5b8a'];

  const x = (time) => left + (Math.max(0, Math.min(maxTime, time)) / maxTime) * chartWidth;
  const lines = [];

  lines.push(`<rect x="0" y="0" width="${width}" height="${height}" fill="var(--surface)"/>`);
  for (const fraction of [0, 0.25, 0.5, 0.75, 1]) {
    const px = left + fraction * chartWidth;
    lines.push(`<line class="tl-grid" x1="${px.toFixed(1)}" y1="${top - 5}" x2="${px.toFixed(1)}" y2="${height - 21}"/>`);
    lines.push(`<text class="tl-time" x="${px.toFixed(1)}" y="${height - 8}" text-anchor="${fraction === 1 ? 'end' : fraction === 0 ? 'start' : 'middle'}">${esc(fmtDuration(maxTime * fraction, 'clock'))}</text>`);
  }

  names.forEach((name, index) => {
    const laneEvents = events
      .filter((event) => event.name === name)
      .sort((a, b) => a.time - b.time);
    const numeric = laneEvents.filter((event) => event.value !== null);
    const values = numeric.map((event) => event.value);
    const min = values.length ? Math.min(...values) : 0;
    const max = values.length ? Math.max(...values) : 0;
    const yBase = top + index * laneHeight + laneHeight / 2;
    const y = (value) => {
      if (!values.length || min === max) return yBase;
      return yBase + 7 - ((value - min) / (max - min)) * 14;
    };
    const color = colors[index % colors.length];

    lines.push(`<text class="tl-label" x="${left - 8}" y="${(yBase + 3).toFixed(1)}" text-anchor="end">${esc(name)}</text>`);
    lines.push(`<line class="tl-axis" x1="${left}" y1="${yBase.toFixed(1)}" x2="${left + chartWidth}" y2="${yBase.toFixed(1)}" opacity="0.35"/>`);

    if (numeric.length > 1 && min !== max) {
      let d = `M ${x(0).toFixed(1)} ${y(numeric[0].value).toFixed(1)}`;
      for (const event of numeric) {
        d += ` H ${x(event.time).toFixed(1)} V ${y(event.value).toFixed(1)}`;
      }
      d += ` H ${x(maxTime).toFixed(1)}`;
      lines.push(`<path class="tl-line" d="${d}" stroke="${color}"/>`);
    }

    for (const event of laneEvents) {
      const cx = x(event.time);
      const cy = y(event.value ?? yBase);
      const seek = demo.video ? ` data-seek="${event.time.toFixed(3)}"` : '';
      const value = event.value === null ? event.raw : event.value;
      lines.push(`<circle class="tl-dot" cx="${cx.toFixed(1)}" cy="${cy.toFixed(1)}" r="2.7" fill="${color}"${seek}><title>${esc(`${name}=${value} at ${fmtDuration(event.time, 'clock')}`)}</title></circle>`);
    }
  });

  return `<figure class="timeline-figure">
  <svg viewBox="0 0 ${width} ${height}" role="img" aria-label="Timeline event lanes for ${attr(demo.analysis.title)}">
    ${lines.join('\n    ')}
  </svg>
</figure>`;
}

function eventTable(demo) {
  const events = demo.analysis.timeline.events
    .slice()
    .sort((a, b) => a.time - b.time);
  if (!events.length) return '';
  const rows = events.map((event) => {
    const seek = demo.video
      ? `<button class="linkbtn" type="button" data-seek="${event.time.toFixed(3)}">seek</button>`
      : '';
    return `<tr>
      <td>${esc(fmtDuration(event.time, 'clock'))}</td>
      <td>${esc(event.name)}</td>
      <td>${esc(event.raw)}</td>
      <td><a href="#L${event.line}">L${event.line}</a></td>
      <td>${seek}</td>
    </tr>`;
  }).join('');
  return `<table class="event-table">
  <thead><tr><th>at</th><th>event</th><th>value</th><th>source</th><th></th></tr></thead>
  <tbody>${rows}</tbody>
</table>`;
}

function paletteBlock(palette) {
  const gradient = paletteGradient(palette.keys);
  const keys = palette.keys.map((key) => {
    const r = Math.round(key.r * 255);
    const g = Math.round(key.g * 255);
    const b = Math.round(key.b * 255);
    return `<span class="key" style="background:rgb(${r},${g},${b})" title="${key.pos.toFixed(3)}: rgb(${r},${g},${b})"></span>`;
  }).join('');
  return `<div class="palette">
  <div class="palette-head"><b>${esc(palette.name)}</b><span>${palette.keys.length} keys · lines ${palette.start}–${palette.end}</span></div>
  <div class="gradient" style="background:${gradient}"></div>
  <div class="keys">${keys}</div>
</div>`;
}

function compositionBar(demo) {
  const entries = Object.entries(demo.analysis.lineCounts)
    .filter(([, count]) => count > 0)
    .sort((a, b) => b[1] - a[1]);
  const total = entries.reduce((sum, [, count]) => sum + count, 0);
  if (!total) return '';
  const segments = entries.map(([kind, count]) => {
    const pct = (count / total) * 100;
    return `<span class="comp-seg" style="flex:${count};background:${KIND_COLORS[kind] || '#999'}" title="${esc(`${kind}: ${count} lines (${pct.toFixed(1)}%)`)}"></span>`;
  }).join('');
  const legend = entries.map(([kind, count]) => {
    const pct = (count / total) * 100;
    return `<span><i style="background:${KIND_COLORS[kind] || '#999'}"></i>${esc(kind)} ${pct.toFixed(0)}%</span>`;
  }).join('');
  return `<div class="composition">${segments}</div>
<p class="legend">${legend}</p>`;
}

function orderStrip(demo) {
  const order = demo.analysis.order;
  if (!order.length) return '';
  const loop = Number(demo.analysis.song.loop) || 0;
  const chips = order.map((id, index) => {
    const isLoop = loop > 0 && index === loop;
    return `<span class="order-chip${isLoop ? ' loop' : ''}" title="${isLoop ? 'loop point' : `order ${index}`}">${esc(String(id))}</span>`;
  }).join('');
  return `<div class="order-strip">${chips}</div>
<p class="note">order list${loop > 0 ? ` · loop returns to order ${loop}` : ''} · ${order.length} entries</p>`;
}

/* -------------------------------------------------------------------- index */

function buildIndex() {
  // Feature the rendered demo with the most timeline events; break ties by name.
  const featured = rendered.slice().sort((a, b) =>
    b.analysis.timeline.events.length - a.analysis.timeline.events.length
    || a.name.localeCompare(b.name))[0];
  const tagCounts = new Map();
  for (const demo of demos) {
    for (const tag of demo.tags) {
      tagCounts.set(tag, (tagCounts.get(tag) || 0) + 1);
    }
  }

  const tagButtons = [`<button class="tag" type="button" data-tag="all" aria-pressed="true">all (${demos.length})</button>`]
    .concat(TAG_ORDER
      .filter((tag) => tagCounts.get(tag))
      .map((tag) => `<button class="tag" type="button" data-tag="${attr(tag)}" aria-pressed="false">${esc(tagLabel(tag))} (${tagCounts.get(tag)})</button>`))
    .join('\n        ');

  const cards = demos
    .slice()
    .sort((a, b) => a.analysis.title.localeCompare(b.analysis.title))
    .map((demo) => demoCard(demo))
    .join('\n');

  const unrenderedNote = stats.unrendered
    ? `<p class="note">${stats.unrendered} demo${stats.unrendered === 1 ? '' : 's'} without MP4 renders.</p>`
    : '';

  const body = `
<section class="intro">
  <h1>Demo archive</h1>
  <p class="lead">Watch the videos, run demos in your browser, or open Studio to follow the tracker and inspect the code.</p>
  ${unrenderedNote}
</section>

<p class="archive-summary">${stats.demos} demos · ${stats.rendered} videos · ${fmtDuration(stats.runtime, 'clock')} total runtime</p>

${featured ? featuredBlock(featured) : ''}

<h2 id="gallery">gallery</h2>
<div class="controls" role="search" aria-label="Filter the gallery">
  <div class="control-row">
    <label class="control"><span>search</span><input id="q" type="search" placeholder="title, model, description, music…" autocomplete="off"></label>
    <label class="control"><span>sort</span>
      <select id="sort">
        <option value="az">title A–Z</option>
        <option value="za">title Z–A</option>
        <option value="duration">runtime, short first</option>
        <option value="bpm">BPM, slow first</option>
        <option value="recent">recently rendered</option>
      </select>
    </label>
  </div>
  <div class="tag-row" role="group" aria-label="Filter by tag">
        ${tagButtons}
  </div>
  <p class="result-count" id="count" role="status">${demos.length} demos.</p>
</div>

<div class="grid" id="gallery-grid">
${cards}
</div>
<div class="empty" id="empty" hidden><p>No demos match these filters.</p><button class="btn" type="button" data-reset-gallery>Clear filters</button></div>

<dialog class="viewer" id="viewer" aria-label="Demo video player">
  <div class="viewer-head">
    <strong id="viewer-title">demo</strong>
    <button class="btn" type="button" id="viewer-close">close</button>
  </div>
  <video id="viewer-video" controls playsinline preload="none"></video>
  <div class="viewer-foot"><a id="viewer-download" href="#" download>download mp4</a> · <a id="viewer-detail" href="#">Details & source</a></div>
</dialog>
`;

  return layout({
    title: 'dmscene',
    description: `A static archive of ${stats.demos} dmscene demos: MP4 renders, full .dmt sources, palettes, timelines and VM anatomy.`,
    body,
    root: '',
    active: 'gallery',
  });
}

/* --------------------------------------------------------------- demo page */

function buildDemoPage(demo, previous, next) {
  const a = demo.analysis;
  const detail = demoUrl(demo);
  const sec = sectionHeading;
  const hasRuntime = ['engine.mjs', 'engine.wasm'].every(file => fs.existsSync(path.join(RUNTIME, file)));
  const assets = [...a.samples, ...a.textures].map(resource => ({
    path: assetPath(resource.path), url: assetUrl(resource.path),
  }));
  const metadata = {
    schema_version: 1, title: a.title, metadata: a.metadata, duration: demo.duration,
    order: a.order, channels: a.song.channels,
    blocks: a.blocks.map(({ kind, name, start, end }) => ({ kind, name, start, end })),
    patterns: Object.fromEntries(a.blocks.filter(block => block.kind === 'pattern').map(block => [block.name,
      a.lines.slice(block.start + 1, block.end).flatMap((text, index) =>
        a.lineInfo[block.start + 1 + index].type === 'patternRow' ? [{ text, line: block.start + index + 2 }] : []),
    ])),
    events: a.timeline.events, assets,
  };
  writeGenerated(path.join(WEB, 'demo', `${demo.name}.json`), JSON.stringify(metadata));
  const live = hasRuntime ? `<section class="live-demo" data-live-demo data-title="${attr(a.title)}" data-duration="${demo.duration || a.songLength || 30}" data-source="${attr(sourceUrl(demo, '../'))}" data-metadata="${demo.name}.json" aria-label="Demo playback">
  <div class="playback-modes" role="group" aria-label="Playback mode">
    ${demo.video ? '<button type="button" data-mode="video" aria-pressed="true">Video</button>' : ''}
    <button type="button" data-mode="terminal" aria-pressed="false">Live terminal</button>
    <button type="button" data-mode="studio" aria-pressed="false">Open Studio <span aria-hidden="true">↗</span></button>
  </div>
  <div data-live-dock></div>
  <div class="live-panel" hidden>
    <header class="workspace-header" data-studio-only hidden>
      <a class="workspace-brand" href="../index.html">dm<span>scene</span></a>
      <div class="workspace-project"><h1 id="workspace-title" tabindex="-1" autofocus>${esc(a.title)}</h1><span>${esc(demo.file)}</span></div>
      <span class="workspace-label">STUDIO</span>
      <button class="btn" type="button" data-studio-close>Back to demo <kbd>Esc</kbd></button>
    </header>
    <nav class="workspace-tabs" data-studio-only hidden aria-label="Studio panels">
      <button type="button" data-pane="preview" aria-pressed="true">Monitor</button>
      <button type="button" data-pane="tracker" aria-pressed="false">Tracker</button>
      <button type="button" data-pane="code" aria-pressed="false">Code</button>
      <button type="button" data-pane="browser" aria-pressed="false">Programs</button>
    </nav>
    <aside class="studio-browser" data-studio-only hidden aria-label="Program browser">
      <div class="panel-heading"><h2>Programs</h2><span>${a.programCount}</span></div>
      <label class="program-search"><span class="visually-hidden">Filter programs</span><input type="search" name="program-filter" data-program-search placeholder="Filter programs" autocomplete="off"></label>
      <div class="program-clips"></div>
      <p class="program-empty" data-program-empty hidden>No matching programs.</p>
      <a class="browser-download" href="${attr(sourceUrl(demo, '../'))}" download>Download .dmt</a>
    </aside>
    <section class="terminal-preview" aria-label="Program monitor">
      <div class="terminal-bar"><span>Program monitor</span><span data-live-status role="status">Loading engine…</span></div>
      <div class="monitor-screen"><canvas width="800" height="500" role="img" aria-label="Live render of ${attr(a.title)}">Live demo preview. The source and timeline are available below.</canvas>
        <div class="engine-loading" data-engine-loading>
          <div class="engine-loading-label"><strong data-engine-stage role="status">Loading engine</strong><span data-engine-percent aria-hidden="true"></span></div>
          <progress data-engine-progress max="1" aria-label="Studio loading progress"></progress>
          <p data-engine-detail>Compiling ${esc(demo.file)} and preparing audio.</p>
          <button class="btn" type="button" data-engine-retry hidden>Retry loading</button>
        </div>
      </div>
      <div class="monitor-caption" data-studio-only hidden><span>Live render</span><span>160 × 100 · 30 fps target</span></div>
    </section>
    <div class="live-transport">
      <div class="transport-buttons"><button class="btn primary" type="button" data-live-play disabled>Play</button>
      <button class="btn" type="button" data-live-restart disabled>Restart</button></div>
      <output data-live-clock>0:00.00</output>
      <label class="live-scrub"><span class="visually-hidden">Playback position</span><input type="range" data-live-seek min="0" max="${demo.duration || 30}" step="0.01" value="0" disabled></label>
      <span class="transport-tempo" data-studio-only hidden>${demo.bpm} <small>BPM</small><span>${a.song.channels} CH</span></span>
      <label class="live-volume">Volume <input type="range" data-live-volume min="0" max="1" step="0.05" value="0.8"></label>
    </div>
    <div class="studio" hidden>
      <section class="studio-tracker" aria-label="Live tracker">
        <div class="panel-heading"><h2>Tracker</h2><span data-tracker-position>Preparing score…</span></div>
        <div class="tracker-channel-head" aria-hidden="true"></div>
        <div class="tracker-rows" tabindex="0" role="region" aria-label="Pattern rows; highlighted row is playing"></div>
      </section>
      <div class="workspace-resize" role="separator" aria-label="Timeline height" aria-orientation="horizontal" aria-valuemin="180" aria-valuemax="600" aria-valuenow="280" tabindex="0"></div>
      <section class="studio-timeline" aria-label="Song arrangement">
        <div class="arrangement-toolbar"><h2>Arrangement</h2><label class="playhead-follow"><input type="checkbox" data-follow-playhead checked> Follow playhead</label><button class="btn fit-timeline" type="button" data-fit-timeline>Fit</button><label>Zoom <input type="range" data-timeline-zoom min="1" max="8" step="0.25" value="1" aria-valuetext="1 times"></label></div>
        <div class="arrangement-scroll" tabindex="0" role="region" aria-label="Score timeline"><div class="arrangement-grid">
          <div class="timeline-ruler-row"><span class="lane-title">${esc(demo.name)}.dmt</span><div class="studio-ruler"></div></div>
          <div class="timeline-lane"><div class="lane-title"><b>Master</b><span>STEREO AUDIO</span></div><div class="lane-content audio-lane"><canvas class="audio-waveform" aria-label="Synthesized stereo waveform" role="img"></canvas></div></div>
          <div class="timeline-lane"><div class="lane-title"><b>Patterns</b><span>TRACKER SCORE</span></div><div class="lane-content pattern-clips"></div></div>
          <div class="timeline-event-lanes"></div>
          <div class="timeline-playhead" aria-hidden="true"></div>
        </div></div>
      </section>
      <section class="studio-code" aria-label="Selected clip source">
        <div class="panel-heading"><h2 data-clip-title>Clip source</h2><label><input type="checkbox" data-live-follow checked> Follow</label><a data-clip-source href="#source">Source ↗</a></div>
        <div class="clip-code code" tabindex="0" role="region" aria-label="Selected source block"></div>
      </section>
    </div>
    <footer class="workspace-footer" data-studio-only hidden><span><i class="engine-light"></i> C engine / WebAssembly</span><span><kbd>Space</kbd> Play / pause <kbd>←</kbd><kbd>→</kbd> Seek 5s <kbd>Home</kbd> Restart</span><span data-workspace-state>Preparing audio</span></footer>
  </div>
  <dialog class="studio-window" aria-labelledby="workspace-title" data-pane="preview"></dialog>
</section>` : '';

  const warnings = [];
  if (a.warning) warnings.push(`<p class="warning"><strong>warning</strong> ${esc(a.warning)}</p>`);
  if (!demo.video) {
    const seconds = demo.duration ? demo.duration.toFixed(2) : '30';
    warnings.push(`<div class="notice"><strong>MP4 unavailable</strong> ${hasRuntime ? 'Use live playback, download the source, or render locally:' : 'Download the source or render locally:'}
<pre class="snippet">./dm render demos/${esc(demo.name)}.dmt --mp4 build/video/${esc(demo.name)}.mp4 --wav build/audio/${esc(demo.name)}.wav --seconds ${esc(seconds)} --size 960x600 --fps 30</pre></div>`);
  }

  const poster = demo.poster ? posterUrl(demo, '../') : '';
  const watch = demo.video ? videoUrl(demo, '../') : '';
  const player = demo.video
    ? `<div class="video-frame"><video id="player" controls playsinline preload="metadata"${poster ? ` poster="${attr(poster)}"` : ''}>
  <source src="${attr(watch)}" type="video/mp4">
  Your browser does not support the video element. <a href="${attr(watch)}">Download the MP4</a>.
</video></div>`
    : '';

  const actions = [];
  if (demo.video) actions.push(`<a class="btn" href="${attr(watch)}" download>download MP4 · ${fmtBytes(demo.video.size)}</a>`);
  actions.push(`<a class="btn" href="${attr(sourceUrl(demo, '../'))}" download>download .dmt</a>`);
  actions.push(`<a class="btn" href="${attr(githubUrl(demo))}" rel="external">view on GitHub</a>`);
  actions.push('<a class="btn" href="../index.html">back to gallery</a>');

  const paragraphs = a.paragraphs.length
    ? a.paragraphs.map((paragraph) => `<p>${esc(paragraph)}</p>`).join('\n')
    : `<p>${esc(demo.summary || 'No notes for this demo yet.')}</p>`;

  const metaRows = [];
  metaRows.push(['file', `<code>${esc(demo.file)}</code>`]);
  metaRows.push(['duration', esc(fmtDuration(demo.duration, 'clock'))]);
  if (demo.video) {
    metaRows.push(['video', `${demo.video.width}×${demo.video.height} · ${Math.round(demo.video.fps)} fps · ${esc(demo.video.codec)}${demo.video.audio ? ' + audio' : ''} · ${fmtBytes(demo.video.size)}`]);
  } else {
    metaRows.push(['video', 'not rendered yet']);
  }
  if (a.song.bpm) metaRows.push(['song', `${a.song.bpm} BPM · speed ${a.song.speed} · rpb ${a.song.rpb} · ${a.song.channels} channels`]);
  if (a.song.loop !== null) metaRows.push(['loop', String(a.song.loop)]);
  metaRows.push(['render', `scale ${a.song.scale}${a.song.dither ? ' · dither on' : ''}${a.song.volume ? ` · master ${esc(a.song.volume)}` : ''}`]);
  if (a.uniformNames.length) metaRows.push(['uniforms', a.uniformNames.map((name) => `<code>${esc(name)}</code>`).join(' ')]);
  if (demo.meta.model) metaRows.push(['model', esc(demo.meta.model)]);
  if (demo.meta.author) metaRows.push(['author', esc(demo.meta.author)]);
  if (demo.meta.collection) metaRows.push(['collection', esc(demo.meta.collection)]);
  if (demo.meta.license) metaRows.push(['license', esc(demo.meta.license)]);
  if (demo.meta.url) metaRows.push(['link', `<a href="${attr(demo.meta.url)}" rel="external">${esc(demo.meta.url)}</a>`]);
  if (demo.tags.length) metaRows.push(['tags', tagBadges(demo.tags)]);
  metaRows.push(['source', `${fmtInt(a.stats.lineCount)} lines · ${fmtBytes(a.stats.sizeBytes)} · <a href="${attr(sourceUrl(demo, '../'))}" download>download</a>`]);

  const metaTable = `<table class="meta-table"><tbody>${metaRows.map(([key, value]) => `<tr><th>${esc(key)}</th><td>${value}</td></tr>`).join('')}</tbody></table>`;

  const readmeRows = [];
  if (demo.meta.music) readmeRows.push(['music', demo.meta.music]);
  if (demo.meta.visual) readmeRows.push(['visual', demo.meta.visual]);
  if (demo.meta.reading) readmeRows.push(['reading', demo.meta.reading]);
  const readmeTable = readmeRows.length
    ? `<table class="meta-table"><tbody>${readmeRows.map(([key, value]) => `<tr><th>${esc(key)}</th><td>${esc(value)}</td></tr>`).join('')}</tbody></table>`
    : '';

  const programKinds = ['inst', 'chan', 'master', 'frame', 'pixel', 'post', 'fn'];
  const programGrid = programKinds.map((kind) => {
    const count = a.counts[kind] || 0;
    return `<div class="program${count ? '' : ' off'}"><b>${count}</b><span>${esc(kind)}</span></div>`;
  }).join('');

  const anatomy = `<div class="stat-grid">
  <div class="stat-box"><b>${a.programCount}</b><span>VM programs</span></div>
  <div class="stat-box"><b>${a.counts.pattern}</b><span>patterns</span></div>
  <div class="stat-box"><b>${a.counts.palette}</b><span>palettes</span></div>
  <div class="stat-box"><b>${a.timeline.events.length}</b><span>timeline events</span></div>
  <div class="stat-box"><b>${a.counts.text + a.counts.sample + a.counts.texture}</b><span>text / sample / texture</span></div>
  <div class="stat-box"><b>${fmtInt(a.stats.lineCount)}</b><span>source lines</span></div>
</div>
<div class="program-grid">${programGrid}</div>
<h3>line composition</h3>
${compositionBar(demo)}
<h3>play order</h3>
${orderStrip(demo)}`;

  const palettes = a.palettes.length
    ? `<div class="palettes">${a.palettes.map(paletteBlock).join('')}</div>`
    : '';

  const timeline = a.timeline.events.length
    ? `${timelineSvg(demo)}
${eventTable(demo)}
<p class="note">Event times are derived from the order list, pattern lengths, BPM and RPB.${demo.video && Math.abs(demo.video.duration - a.songLength) > 1 ? ` The rendered video is ${fmtDuration(demo.video.duration, 'clock')}; the computed song length is ${fmtDuration(a.songLength, 'clock')}.` : ''}${a.song.loop ? ' The timeline loops with the song, so this is the first pass.' : ''}</p>`
    : '<p class="note">This demo has no timeline events; it is driven by the tracker clock and per-frame programs.</p>';

  const resourceRows = [];
  for (const text of a.texts) {
    resourceRows.push(`<tr><td>text</td><td>${esc(text.name)}</td><td>${text.length}</td><td class="text">${esc(truncate(text.path, 220))}</td></tr>`);
  }
  for (const sample of a.samples) {
    const target = `../${assetUrl(sample.path)}`;
    resourceRows.push(`<tr><td>sample</td><td>${esc(sample.name)}</td><td>${esc(sample.attrs.rate || '')}</td><td><a href="${attr(target)}">${esc(sample.path)}</a></td></tr>`);
  }
  for (const texture of a.textures) {
    const target = `../${assetUrl(texture.path)}`;
    resourceRows.push(`<tr><td>texture</td><td>${esc(texture.name)}</td><td></td><td><a href="${attr(target)}">${esc(texture.path)}</a></td></tr>`);
  }
  for (const venv of a.venvs) {
    resourceRows.push(`<tr><td>venv</td><td>${esc(venv.name)}</td><td>${venv.points.length} pts</td><td class="text">drawn volume envelope</td></tr>`);
  }
  const resources = resourceRows.length
    ? `<table class="resource-table"><thead><tr><th>kind</th><th>name</th><th>info</th><th>value</th></tr></thead><tbody>${resourceRows.join('')}</tbody></table>`
    : '<p class="note">No external text, samples, textures or envelopes.</p>';

  const blockNav = a.blocks.map((block) => {
    const label = `${block.kind}${block.name ? ` ${block.name}` : ''}`;
    const line = block.start + 1;
    return `<li><span class="lnum">${line}</span><a href="#L${line}" title="${attr(label)}">${esc(label)}</a></li>`;
  }).join('');

  const code = highlightSource(a);

  const prevNext = `<div class="prevnext">
  <div>${previous ? `← <a href="${attr(`${previous.name}.html`)}">${esc(previous.analysis.title)}</a>` : ''}</div>
  <div class="next">${next ? `<a href="${attr(`${next.name}.html`)}">${esc(next.analysis.title)}</a> →` : ''}</div>
</div>`;

  const body = `
<p class="breadcrumbs"><a href="../index.html">gallery</a> / ${esc(demo.name)}.dmt</p>

<div class="page-head">
  <h1>${esc(a.title)}</h1>
  <p class="subtitle">${esc(demo.name)}.dmt · ${esc(metaLine(demo))}</p>
  <p class="badges">${tagBadges(demo.tags)}</p>
</div>

${warnings.join('\n')}
${live}
${player}
<div class="actions">${actions.join(' ')}</div>

${sec('notes', 'notes')}
<div class="prose">${paragraphs}</div>
${readmeTable}
${metaTable}

${sec('anatomy', 'anatomy')}
${anatomy}

${a.palettes.length ? `${sec('palettes', 'palettes')}\n${palettes}` : ''}

${sec('timeline', 'timeline')}
${timeline}

${sec('resources', 'resources')}
${resources}

${sec('source', 'source')}
<div class="source-layout">
  <aside class="block-nav" aria-label="Source sections">
    <h3>source sections</h3>
    <ul>${blockNav}</ul>
  </aside>
  <div class="code-wrap">
    <div class="code-toolbar">
      <button class="btn" type="button" data-copy-source="${attr(sourceUrl(demo, '../'))}">copy source</button>
      <a class="btn" href="${attr(sourceUrl(demo, '../'))}" download>download .dmt</a>
      <span class="spacer"></span>
      <label><input type="checkbox" id="wrap-code"> wrap code (patterns stay aligned)</label>
    </div>
    <div class="code" id="code">${code}</div>
  </div>
</div>
<p class="note">Click a line number to link to that line.</p>

${prevNext}
`;

  return layout({
    title: a.title,
    description: truncate(demo.summary || `${a.title} — a dmscene demo.`, 180),
    body,
    root: '../',
    active: 'gallery',
  });
}

/* ------------------------------------------------------------------ sources */

function buildSourcesPage() {
  const rows = demos
    .slice()
    .sort((a, b) => a.analysis.title.localeCompare(b.analysis.title))
    .map((demo) => {
      const a = demo.analysis;
      const search = [demo.name, a.title, demo.summary, demo.meta.model].join(' ').toLowerCase();
      return `<tr data-source-row data-search="${attr(search)}">
  <td class="title"><a href="${attr(demoUrl(demo))}">${esc(a.title)}</a><br><span class="muted">${esc(demo.name)}.dmt</span></td>
  <td class="num">${fmtInt(a.stats.lineCount)}</td>
  <td class="num">${a.programCount}</td>
  <td class="num">${a.counts.pattern}</td>
  <td class="num">${a.counts.palette}</td>
  <td class="num">${a.timeline.events.length}</td>
  <td class="num">${fmtBytes(a.stats.sizeBytes)}</td>
  <td><a href="${attr(sourceUrl(demo))}" download>.dmt</a> · <a href="${attr(demoUrl(demo))}#source">annotated</a></td>
</tr>`;
    }).join('\n');

  const body = `
<div class="page-head">
  <h1>Source files</h1>
  <p class="subtitle">${demos.length} .dmt files · ${fmtInt(stats.lines)} lines · ${stats.programs} VM programs</p>
</div>

<p class="lead">Download a .dmt file or open its highlighted source.</p>

<div class="source-tools">
  <label class="control"><span>filter</span><input id="source-q" type="search" placeholder="title, file, model…" autocomplete="off"></label>
  <p class="result-count" id="source-count" role="status">${demos.length} source files.</p>
</div>

<div class="table-scroll" tabindex="0" role="region" aria-label="Source files"><table class="source-table">
  <thead><tr><th>demo</th><th>lines</th><th>programs</th><th>patterns</th><th>palettes</th><th>events</th><th>size</th><th>download</th></tr></thead>
  <tbody>
${rows}
  </tbody>
</table></div>

<p class="note">Sample and texture downloads are linked under Resources on each demo page.</p>
`;

  return layout({
    title: 'Source files',
    description: 'Download or annotate every .dmt source file in the dmscene archive.',
    body,
    root: '',
    active: 'sources',
  });
}

/* -------------------------------------------------------------------- about */

function buildAboutPage() {
  const modelRows = [...models.entries()]
    .sort((a, b) => b[1].length - a[1].length || a[0].localeCompare(b[0]))
    .map(([model, list]) => {
      const width = Math.max(4, Math.round((list.length / Math.max(...[...models.values()].map((items) => items.length))) * 100));
      return `<div class="model-row">
  <a href="index.html?q=${encodeURIComponent(model)}">${esc(model)}</a>
  <div class="bar"><span style="width:${width}%"></span></div>
  <b>${list.length}</b>
</div>`;
    }).join('');

  const attributed = demos.filter((demo) => demo.meta.model).length;
  const body = `
<div class="page-head">
  <h1>About this archive</h1>
  <p class="subtitle">${stats.demos} demos · ${fmtInt(stats.lines)} lines of source</p>
</div>

<p class="lead">dmscene compiles .dmt files into audiovisual demos. Each file contains the instruments, tracker score, visual programs, and effects. The C99 engine runs both the command-line player and this site’s live playback.</p>

<h2>Model credits</h2>
<p>${attributed} of ${stats.demos} demos have model credits in the repository.</p>
<div class="model-chart">${modelRows || '<p>No model credits found.</p>'}</div>

<h2>Source files</h2>
<p>Each demo’s palettes, timeline, instruments and patterns come from its .dmt file. Use the timeline’s seek buttons to jump to an event in the video, or follow a line number into the source.</p>
<p><a href="sources.html">Browse source files</a> · <a href="${REPO}#readme" rel="external">Engine documentation</a></p>

<h2>Run locally</h2>
<pre class="snippet">make videos   # render the demos
make site     # build the archive
make serve    # preview at localhost:8000</pre>
<p class="note">Code uses <a href="https://typographic.systems/obel-mono" rel="external">Obel Mono</a> by Typographic Systems, licensed under SIL OFL 1.1.</p>
`;

  return layout({
    title: 'About',
    description: 'How the dmscene demo archive is generated, and who made the demos.',
    body,
    root: '',
    active: 'about',
  });
}

/* ---------------------------------------------------------------------- 404 */

function build404Page() {
  const body = `
<div class="page-head">
  <h1>404 — no such demo</h1>
  <p class="subtitle">This page could not be found.</p>
</div>
<p class="lead">Try the <a href="index.html">gallery</a>, the <a href="sources.html">source index</a>, or <a href="about.html">about the archive</a>.</p>
`;
  return layout({
    title: 'Not found',
    description: 'No such page in the dmscene demo archive.',
    body,
    root: '',
    active: '',
  });
}

/* -------------------------------------------------------------------- write */

function writePage(relative, html) {
  const target = path.join(WEB, relative);
  ensureDir(path.dirname(target));
  writeGenerated(target, html);
}

writeGenerated(path.join(WEB, 'demos.json'), JSON.stringify(demos.map(demo => ({ name: demo.name, url: demoUrl(demo) }))));
writePage('index.html', buildIndex());
writePage('sources.html', buildSourcesPage());
writePage('about.html', buildAboutPage());
writePage('404.html', build404Page());

const ordered = demos.slice().sort((a, b) => a.analysis.title.localeCompare(b.analysis.title));
for (let i = 0; i < ordered.length; i += 1) {
  const demo = ordered[i];
  const previous = ordered[i - 1] || null;
  const next = ordered[i + 1] || null;
  writePage(`demo/${demo.name}.html`, buildDemoPage(demo, previous, next));
}

for (const asset of ['style.css', 'app.js', 'favicon.svg', 'live.js', 'engine-worker.js', 'studio-data.mjs', 'asset-path.mjs']) {
  const source = path.join(SITE_ASSETS, asset);
  const destination = path.join(WEB, asset);
  if (copyIfChanged(source, destination)) {
    console.log(`copied ${asset}`);
  }
}

if (path.resolve(RUNTIME) !== path.join(WEB, 'runtime')) {
  for (const file of ['engine.mjs', 'engine.wasm']) {
    if (fs.existsSync(path.join(RUNTIME, file))) copyIfChanged(path.join(RUNTIME, file), path.join(WEB, 'runtime', file));
  }
}

const fontSource = path.join(SITE_ASSETS, 'fonts');
if (fs.existsSync(fontSource)) {
  const copiedFonts = copyDirectory(fontSource, path.join(WEB, 'fonts'));
  if (copiedFonts) console.log(`copied ${copiedFonts} font file(s)`);
}



/* ----------------------------------------------------------------- validate */

function* walk(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) yield* walk(full);
    else if (entry.isFile()) yield full;
  }
}

const missing = [];
const htmlFiles = [...walk(WEB)].filter((file) => file.endsWith('.html'));
for (const file of htmlFiles) {
  const html = fs.readFileSync(file, 'utf8');
  for (const match of html.matchAll(/(?:href|src)="([^"]+)"/g)) {
    const raw = match[1];
    if (/^(?:https?:|mailto:|data:|#)/.test(raw)) continue;
    const withoutFragment = raw.split('#')[0].split('?')[0];
    if (!withoutFragment) continue;
    let decoded = withoutFragment;
    try {
      decoded = decodeURIComponent(withoutFragment);
    } catch {
      // leave it encoded
    }
    const resolved = path.resolve(path.dirname(file), decoded);
    if (!fs.existsSync(resolved)) {
      missing.push(`${path.relative(WEB, file)} -> ${raw}`);
    }
  }
}

if (missing.length) {
  console.error(`\n${missing.length} broken local link(s):`);
  for (const item of missing.slice(0, 30)) console.error(`  ${item}`);
  if (missing.length > 30) console.error(`  … and ${missing.length - 30} more`);
  process.exitCode = 1;
}

console.log(`wrote ${htmlFiles.length} HTML page(s) to ${path.relative(ROOT, WEB)}/`);
if (stats.unrendered) {
  console.log(`note: ${stats.unrendered} demo(s) have no complete MP4 yet; they are marked render pending.`);
}
