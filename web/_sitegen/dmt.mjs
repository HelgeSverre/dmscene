// Parser and syntax highlighter for dmscene .dmt source files.
// No dependencies: the site generator turns these structures into static HTML.
import { readMetadata } from './metadata.mjs';

const TOP_LEVEL = new Set([
  'title', 'song', 'order', 'pan', 'uniform', 'text', 'sample', 'texture',
  'venv', 'timeline', 'fn', 'inst', 'chan', 'pattern', 'frame', 'pixel',
  'post', 'master', 'palette',
]);

const BLOCK_KINDS = new Set([
  'venv', 'timeline', 'fn', 'inst', 'chan', 'pattern', 'frame', 'pixel',
  'post', 'master', 'palette',
]);

const KEYWORDS = new Set([
  ...TOP_LEVEL,
  'repeat', 'if', 'else', 'return',
]);

const FUNCTIONS = new Set([
  'sin', 'cos', 'tan', 'abs', 'floor', 'ceil', 'fract', 'sqrt', 'exp', 'log',
  'sign', 'tanh', 'pow', 'min', 'max', 'clamp', 'mix', 'step', 'smooth',
  'atan2', 'len', 'hash', 'osc', 'saw', 'tri', 'sqr', 'bsaw', 'bsqr', 'usaw',
  'noise', 'env', 'venv', 'lp', 'hp', 'svf', 'crush', 'fold', 'dist', 'dly',
  'verb', 'smp', 'smplen', 'wave', 'tex', 'mandel', 'julia', 'ship', 'newton',
  'kali', 'kifs', 'rot', 'polar', 'fbm', 'vor', 'glyph', 'str', 'strlen',
  'evt', 'evtl', 'scene', 'trig', 'vel', 'note', 'cenv', 'cget', 'prev', 'tap',
  'out', 'rgb', 'pal', 'mixpal', 'hsv', 'cmul', 'cadd', 'cprev', 'cprevz',
  'select',
]);

const INPUTS = new Set([
  'time', 'beat', 'lbeat', 'llen', 'bar', 'row', 'tick', 'bpm', 'sr', 'order',
  'pos', 'mp0', 'mp1', 'mp2', 'mp3', 't', 'frame', 'w', 'h', 'aspect', 'x', 'y',
  'u', 'v', 'ph', 'freq', 'rel', 'vel', 'note', 'chan', 'l', 'r', 'p0', 'p1',
  'p2', 'p3', 'prev', 'r1', 'r2', 'r3', 'pi', 'tau',
]);

const ATTRS = new Set([
  'bpm', 'speed', 'rpb', 'channels', 'loop', 'volume', 'scale', 'dither',
  'rows', 'nna', 'sustain', 'rate',
]);

const PALETTE_KINDS = new Set(['palette', 'timeline', 'pattern', 'venv']);

export function esc(value) {
  return String(value ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;');
}

export function attr(value) {
  return esc(value).replaceAll('"', '&quot;');
}

export function stripComment(line) {
  let inString = false;
  let escaped = false;
  for (let i = 0; i < line.length; i += 1) {
    const c = line[i];
    if (inString) {
      if (escaped) escaped = false;
      else if (c === '\\') escaped = true;
      else if (c === '"') inString = false;
      continue;
    }
    if (c === '"') {
      inString = true;
      continue;
    }
    const atBoundary = i === 0 || /\s/.test(line[i - 1]);
    if (c === '#' && atBoundary) return line.slice(0, i);
    if (c === '/' && line[i + 1] === '/' && atBoundary) return line.slice(0, i);
  }
  return line;
}

function maskStrings(line) {
  let out = '';
  let inString = false;
  let escaped = false;
  for (const c of line) {
    if (inString) {
      if (escaped) escaped = false;
      else if (c === '\\') escaped = true;
      else if (c === '"') inString = false;
      out += ' ';
    } else {
      if (c === '"') {
        inString = true;
        out += ' ';
      } else {
        out += c;
      }
    }
  }
  return out;
}

function braceBalance(line) {
  const masked = maskStrings(stripComment(line));
  let depth = 0;
  for (const c of masked) {
    if (c === '{') depth += 1;
    else if (c === '}') depth -= 1;
  }
  return depth;
}

function hasBrace(line) {
  return maskStrings(stripComment(line)).includes('{');
}

function bodyLineIsData(line) {
  const t = stripComment(line).trim();
  return Boolean(t) && t !== '}';
}

function blockName(kind, code) {
  const rest = code.replace(new RegExp(`^${kind}\\b\\s*`), '');
  if (kind === 'fn') return (rest.match(/^([A-Za-z_]\w*)\s*\(/) || [])[1] || '';
  if (kind === 'chan' || kind === 'pattern') return (rest.match(/^(\d+)/) || [])[1] || '';
  if (kind === 'frame' || kind === 'master' || kind === 'post' || kind === 'timeline') return '';
  return (rest.match(/^([A-Za-z_]\w*)/) || [])[1] || '';
}

function directiveBody(code, kind) {
  return code.replace(new RegExp(`^${kind}\\b\\s*`), '').trim();
}

function parseAttributes(text) {
  const attrs = {};
  for (const match of text.matchAll(/([A-Za-z_]\w*)\s*=\s*([^\s]+)/g)) {
    attrs[match[1]] = match[2];
  }
  return attrs;
}

function parseResourceLine(text) {
  const match = text.match(/^([A-Za-z_]\w*)\s+"((?:\\.|[^"\\])*)"(.*)$/);
  if (!match) return null;
  return {
    name: match[1],
    path: match[2].replace(/\\(["\\])/g, '$1'),
    attrs: parseAttributes(match[3] || ''),
  };
}

function cleanString(value) {
  const text = value.trim();
  if (text.length >= 2 && text.startsWith('"') && text.endsWith('"')) {
    return text.slice(1, -1).replace(/\\(["\\])/g, '$1');
  }
  return text;
}

function leadingCommentParagraphs(lines) {
  const paragraphs = [];
  let current = [];
  const flush = () => {
    if (current.length) {
      paragraphs.push(current.join(' ').replace(/\s+/g, ' ').trim());
      current = [];
    }
  };
  for (const raw of lines) {
    const trimmed = raw.trim();
    if (!trimmed) {
      flush();
      continue;
    }
    if (!trimmed.startsWith('#')) break;
    let text = trimmed.replace(/^#+\s?/, '').trim();
    if (text.startsWith('@')) { flush(); continue; }
    if (/^[=\-*_]{3,}$/.test(text)) {
      flush();
      continue;
    }
    text = text.replace(/^[=\-*_]{2,}\s*/, '').replace(/\s*[=\-*_]{2,}$/, '').trim();
    if (!text) {
      flush();
      continue;
    }
    current.push(text);
  }
  flush();
  return paragraphs;
}

function parsePaletteBody(lines, block) {
  const keys = [];
  for (let i = block.start + 1; i < block.end; i += 1) {
    if (!bodyLineIsData(lines[i])) continue;
    const nums = stripComment(lines[i]).trim().split(/\s+/).map(Number);
    if (nums.length < 4 || nums.some((n) => !Number.isFinite(n))) continue;
    keys.push({ pos: nums[0], r: nums[1], g: nums[2], b: nums[3] });
  }
  return keys;
}

function parseVenvBody(lines, block) {
  const points = [];
  for (let i = block.start + 1; i < block.end; i += 1) {
    if (!bodyLineIsData(lines[i])) continue;
    const nums = stripComment(lines[i]).trim().split(/\s+/).map(Number);
    if (nums.length < 2 || nums.some((n) => !Number.isFinite(n))) continue;
    points.push({ t: nums[0], v: nums[1] });
  }
  return points;
}

function parseTimelineBody(lines, block) {
  const events = [];
  for (let i = block.start + 1; i < block.end; i += 1) {
    if (!bodyLineIsData(lines[i])) continue;
    const text = stripComment(lines[i]).trim();
    const match = text.match(/^(\d+):(\d+)\s+(.+)$/);
    if (!match) continue;
    const order = Number(match[1]);
    const row = Number(match[2]);
    for (const pair of match[3].matchAll(/([A-Za-z_]\w*)\s*=\s*([^\s]+)/g)) {
      const raw = pair[2];
      const value = Number(raw);
      events.push({
        name: pair[1],
        raw,
        value: Number.isFinite(value) ? value : null,
        order,
        row,
        line: i + 1,
      });
    }
  }
  return events;
}

export function analyzeDmt(source, name) {
  const metadata = readMetadata(source);
  const normalized = String(source).replace(/\r\n?/g, '\n');
  const lines = normalized.split('\n');
  if (lines.length && lines[lines.length - 1] === '') lines.pop();

  const lineInfo = lines.map(() => ({ type: 'blank', kind: null }));
  const blocks = [];
  const singles = [];

  let i = 0;
  while (i < lines.length) {
    const raw = lines[i];
    const trimmed = raw.trim();
    if (!trimmed) {
      lineInfo[i] = { type: 'blank' };
      i += 1;
      continue;
    }
    if (trimmed.startsWith('#') || trimmed.startsWith('//')) {
      lineInfo[i] = { type: 'comment' };
      i += 1;
      continue;
    }

    const code = stripComment(raw).trim();
    const head = code.match(/^([a-z]+)\b/);
    if (!head || !TOP_LEVEL.has(head[1])) {
      lineInfo[i] = { type: 'body' };
      i += 1;
      continue;
    }

    const kind = head[1];
    if (BLOCK_KINDS.has(kind) && hasBrace(raw)) {
      let depth = braceBalance(raw);
      let end = i;
      if (depth > 0) {
        let j = i + 1;
        while (j < lines.length && depth > 0) {
          depth += braceBalance(lines[j]);
          end = j;
          j += 1;
        }
      }
      const block = {
        kind,
        name: blockName(kind, code),
        start: i,
        end,
        header: code,
      };
      blocks.push(block);
      lineInfo[i] = { type: 'header', kind, name: block.name };

      for (let k = i + 1; k <= end; k += 1) {
        const t = lines[k].trim();
        if (!t) lineInfo[k] = { type: 'blank', kind };
        else if (t.startsWith('#') || t.startsWith('//')) lineInfo[k] = { type: 'comment', kind };
        else lineInfo[k] = { type: 'body', kind, name: block.name };
      }
      i = end + 1;
      continue;
    }

    singles.push({ kind, line: i, text: code });
    lineInfo[i] = { type: 'directive', kind };
    i += 1;
  }

  for (const block of blocks) {
    if (!PALETTE_KINDS.has(block.kind)) continue;
    const rowType = `${block.kind}Row`;
    for (let k = block.start + 1; k < block.end; k += 1) {
      if (lineInfo[k].type === 'body') {
        lineInfo[k] = { type: rowType, kind: block.kind, name: block.name };
      }
    }
  }
  for (const block of blocks) {
    if (block.end <= block.start) continue;
    const closing = stripComment(lines[block.end]).trim();
    if (closing === '}' || closing.startsWith('}')) {
      lineInfo[block.end] = { type: 'close', kind: block.kind, name: block.name };
    }
  }

  const byKind = (kind) => singles.filter((item) => item.kind === kind);

  const titleLine = byKind('title')[0];
  const title = titleLine ? cleanString(directiveBody(titleLine.text, 'title')) : name.toUpperCase();

  const songLine = byKind('song')[0];
  const songAttrs = songLine ? parseAttributes(directiveBody(songLine.text, 'song')) : {};
  const song = {
    bpm: Number(songAttrs.bpm) || 0,
    speed: Number(songAttrs.speed) || 0,
    rpb: Number(songAttrs.rpb) || 0,
    channels: Number(songAttrs.channels) || 0,
    loop: songAttrs.loop === undefined ? null : Number(songAttrs.loop),
    volume: songAttrs.volume || '',
    scale: Number(songAttrs.scale) || 1,
    dither: Number(songAttrs.dither) || 0,
    rowSeconds: 0,
  };

  const orderLine = byKind('order')[0];
  const order = orderLine
    ? directiveBody(orderLine.text, 'order').split(/\s+/).map(Number).filter(Number.isFinite)
    : [];

  const panLine = byKind('pan')[0];
  const pan = panLine
    ? directiveBody(panLine.text, 'pan').split(/\s+/).map(Number).filter(Number.isFinite)
    : [];

  const uniformLine = byKind('uniform')[0];
  const uniforms = uniformLine
    ? directiveBody(uniformLine.text, 'uniform').split(/\s+/).filter(Boolean)
    : [];

  const texts = byKind('text').map((item) => {
    const parsed = parseResourceLine(directiveBody(item.text, 'text'));
    return parsed ? { ...parsed, line: item.line + 1, length: parsed.path.length } : null;
  }).filter(Boolean);

  const samples = byKind('sample').map((item) => {
    const parsed = parseResourceLine(directiveBody(item.text, 'sample'));
    return parsed ? { ...parsed, line: item.line + 1 } : null;
  }).filter(Boolean);

  const textures = byKind('texture').map((item) => {
    const parsed = parseResourceLine(directiveBody(item.text, 'texture'));
    return parsed ? { ...parsed, line: item.line + 1 } : null;
  }).filter(Boolean);

  const palettes = blocks.filter((b) => b.kind === 'palette').map((block) => ({
    name: block.name,
    start: block.start + 1,
    end: block.end + 1,
    keys: parsePaletteBody(lines, block),
  }));

  const venvs = blocks.filter((b) => b.kind === 'venv').map((block) => ({
    name: block.name,
    start: block.start + 1,
    end: block.end + 1,
    points: parseVenvBody(lines, block),
  }));

  const timelineBlocks = blocks.filter((b) => b.kind === 'timeline');
  const timelineEvents = timelineBlocks.flatMap((block) => parseTimelineBody(lines, block));

  const patterns = new Map();
  for (const block of blocks.filter((b) => b.kind === 'pattern')) {
    const rowsMatch = block.header.match(/\brows\s*=\s*(\d+)/);
    let rows = rowsMatch ? Number(rowsMatch[1]) : 0;
    if (!rows) {
      rows = 0;
      for (let k = block.start + 1; k < block.end; k += 1) {
        if (bodyLineIsData(lines[k])) rows += 1;
      }
    }
    patterns.set(String(Number(block.name)), { id: Number(block.name), rows, block });
  }

  const rpb = song.rpb || 4;
  const bpm = song.bpm || 120;
  song.rowSeconds = 60 / (bpm * rpb);
  song.speed = song.speed || 6;
  song.channels = song.channels || pan.length || 1;
  song.loop = song.loop ?? 0;

  const playOrder = order.length
    ? order
    : [...patterns.keys()].map(Number).sort((a, b) => a - b);

  let elapsed = 0;
  const orderTimes = [];
  for (const id of playOrder) {
    orderTimes.push(elapsed);
    const pattern = patterns.get(String(id));
    elapsed += (pattern ? pattern.rows : 64) * song.rowSeconds;
  }
  const songLength = elapsed;

  for (const event of timelineEvents) {
    const base = orderTimes[event.order] === undefined ? songLength : orderTimes[event.order];
    event.time = base + event.row * song.rowSeconds;
  }

  const timelineNames = [];
  for (const event of timelineEvents) {
    if (!timelineNames.includes(event.name)) timelineNames.push(event.name);
  }

  const counts = {};
  for (const kind of TOP_LEVEL) counts[kind] = 0;
  for (const block of blocks) counts[block.kind] += 1;
  for (const item of singles) counts[item.kind] += 1;

  const paragraphs = metadata.description ? metadata.description.split('\n\n') : leadingCommentParagraphs(lines);
  const description = metadata.description || paragraphs.join('\n\n');
  const summary = paragraphs[0] || '';
  const allComments = lines
    .filter((line) => line.trim().startsWith('#'))
    .map((line) => line.trim().replace(/^#+\s?/, ''))
    .join(' ');

  const lineCounts = {};
  for (const kind of TOP_LEVEL) lineCounts[kind] = 0;
  for (const block of blocks) lineCounts[block.kind] += block.end - block.start + 1;
  for (const item of singles) lineCounts[item.kind] += 1;

  let codeLines = 0;
  let commentLines = 0;
  let blankLines = 0;
  for (const line of lines) {
    const t = line.trim();
    if (!t) blankLines += 1;
    else if (t.startsWith('#') || t.startsWith('//')) commentLines += 1;
    else codeLines += 1;
  }

  return {
    name,
    metadata,
    file: `${name}.dmt`,
    source: normalized,
    lines,
    lineInfo,
    blocks,
    singles,
    title,
    description,
    paragraphs,
    summary,
    warning: metadata.warning || (/strobe|flashing|epilep/i.test(allComments)
      ? 'This piece contains strobe or flashing effects.'
      : ''),
    song,
    order: playOrder,
    orderTimes,
    patterns,
    songLength,
    counts,
    lineCounts,
    programCount: counts.inst + counts.chan + counts.master + counts.frame
      + counts.pixel + counts.post,
    palettes,
    venvs,
    timeline: {
      events: timelineEvents,
      names: timelineNames,
      length: Math.max(songLength, ...timelineEvents.map((event) => event.time), 0),
    },
    texts,
    samples,
    textures,
    uniformNames: uniforms,
    pan,
    stats: {
      lineCount: lines.length,
      codeLines,
      commentLines,
      blankLines,
      sizeBytes: Buffer.byteLength(normalized, 'utf8'),
    },
  };
}

/* ------------------------------------------------------------------ syntax */

function tokenClass(word, opts, state) {
  if (state.nextDef) {
    state.nextDef = false;
    return 'def';
  }
  if (KEYWORDS.has(word)) return 'kw';
  if (FUNCTIONS.has(word)) return 'fn';
  if (ATTRS.has(word)) return 'attr';
  if (INPUTS.has(word)) return 'in';
  return null;
}

function lexCode(text, opts = {}) {
  let out = '';
  let plain = '';
  let i = 0;
  const state = { nextDef: Boolean(opts.header) };
  const flush = () => {
    if (plain) {
      out += esc(plain);
      plain = '';
    }
  };
  const push = (cls, value) => {
    flush();
    if (cls) out += `<span class="tok-${cls}">${esc(value)}</span>`;
    else out += esc(value);
  };

  while (i < text.length) {
    const c = text[i];
    if (c === '"') {
      let j = i + 1;
      let escaped = false;
      while (j < text.length) {
        if (escaped) escaped = false;
        else if (text[j] === '\\') escaped = true;
        else if (text[j] === '"') {
          j += 1;
          break;
        }
        j += 1;
      }
      push('str', text.slice(i, j));
      i = j;
      continue;
    }
    const atBoundary = i === 0 || /\s/.test(text[i - 1]);
    if (c === '#' && atBoundary) {
      push('com', text.slice(i));
      break;
    }
    if (c === '/' && text[i + 1] === '/' && atBoundary) {
      push('com', text.slice(i));
      break;
    }
    if (c === '$') {
      let j = i + 1;
      while (j < text.length && /[A-Za-z0-9_]/.test(text[j])) j += 1;
      push('state', text.slice(i, j));
      i = j;
      continue;
    }
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(text[i + 1] || ''))) {
      const match = text.slice(i).match(/^(?:\d+\.\d*|\.\d+|\d+)/);
      const value = match ? match[0] : c;
      push('num', value);
      i += value.length;
      continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      let j = i + 1;
      while (j < text.length && /[A-Za-z0-9_]/.test(text[j])) j += 1;
      const word = text.slice(i, j);
      push(tokenClass(word, opts, state), word);
      i = j;
      continue;
    }
    plain += c;
    i += 1;
  }
  flush();
  return out;
}

function highlightDirective(raw, info) {
  const match = raw.match(/^(\s*)([A-Za-z_]\w*)([\s\S]*)$/);
  if (!match) return lexCode(raw);
  let out = esc(match[1]) + `<span class="tok-kw">${esc(match[2])}</span>`;
  const namesFirst = ['fn', 'inst', 'pixel', 'palette', 'venv'];
  out += lexCode(match[3], {
    header: info.type === 'header' && namesFirst.includes(info.kind),
  });
  return out;
}

function highlightPatternRow(raw) {
  let out = '';
  let plain = '';
  let i = 0;
  const flush = () => {
    if (plain) {
      out += esc(plain);
      plain = '';
    }
  };
  while (i < raw.length) {
    const c = raw[i];
    if (c === '#' && (i === 0 || /\s/.test(raw[i - 1]))) {
      flush();
      out += `<span class="tok-com">${esc(raw.slice(i))}</span>`;
      break;
    }
    if (/\s/.test(c) || c === '|') {
      plain += c;
      i += 1;
      continue;
    }
    let j = i;
    while (j < raw.length && !/\s/.test(raw[j]) && raw[j] !== '|') j += 1;
    const token = raw.slice(i, j);
    let cls = null;
    if (/^(?:off|===)$/.test(token)) cls = 'note';
    else if (/^[A-G][-#]?[0-9]$/.test(token)) cls = 'note';
    else if (/^(?:\.{1,3}|-{3})$/.test(token)) cls = 'empty';
    else if (/^(?:[0-9A-F]{2}:?|[0-9A-F]{2}|[0-9]+)$/.test(token)) cls = 'num';
    else if (/^[0-9A-Fa-f]{3}$/.test(token)) cls = 'fx';
    flush();
    if (cls) out += `<span class="tok-${cls}">${esc(token)}</span>`;
    else out += esc(token);
    i = j;
  }
  flush();
  return out;
}

function highlightNumberRow(raw) {
  let out = '';
  let plain = '';
  let i = 0;
  const flush = () => {
    if (plain) {
      out += esc(plain);
      plain = '';
    }
  };
  while (i < raw.length) {
    const c = raw[i];
    if (c === '#' && (i === 0 || /\s/.test(raw[i - 1]))) {
      flush();
      out += `<span class="tok-com">${esc(raw.slice(i))}</span>`;
      break;
    }
    if (/[0-9+\-]/.test(c)) {
      const match = raw.slice(i).match(/^[+-]?(?:\d+\.\d*|\.\d+|\d+)/);
      if (match) {
        flush();
        out += `<span class="tok-num">${esc(match[0])}</span>`;
        i += match[0].length;
        continue;
      }
    }
    plain += c;
    i += 1;
  }
  flush();
  return out;
}

function highlightTimelineRow(raw) {
  const match = raw.match(/^(\s*)(\d+:\d+)([\s\S]*)$/);
  if (!match) return highlightNumberRow(raw);
  let rest = match[3];
  let out = esc(match[1]) + `<span class="tok-num">${esc(match[2])}</span>`;
  let plain = '';
  let i = 0;
  const flush = () => {
    if (plain) {
      out += esc(plain);
      plain = '';
    }
  };
  while (i < rest.length) {
    const c = rest[i];
    if (c === '#' && (i === 0 || /\s/.test(rest[i - 1]))) {
      flush();
      out += `<span class="tok-com">${esc(rest.slice(i))}</span>`;
      break;
    }
    if (/[A-Za-z_]/.test(c)) {
      let j = i + 1;
      while (j < rest.length && /[A-Za-z0-9_]/.test(rest[j])) j += 1;
      flush();
      out += `<span class="tok-attr">${esc(rest.slice(i, j))}</span>`;
      i = j;
      continue;
    }
    if (c === '=') {
      plain += c;
      i += 1;
      continue;
    }
    if (/[0-9+\-.]/.test(c)) {
      const match2 = rest.slice(i).match(/^[+-]?(?:\d+\.\d*|\.\d+|\d+)/);
      if (match2) {
        flush();
        out += `<span class="tok-num">${esc(match2[0])}</span>`;
        i += match2[0].length;
        continue;
      }
    }
    plain += c;
    i += 1;
  }
  flush();
  return out;
}

function paletteSwatch(raw) {
  const nums = stripComment(raw).trim().split(/\s+/).map(Number);
  if (nums.length < 4 || nums.slice(1, 4).some((n) => !Number.isFinite(n))) return '';
  const rgb = nums.slice(1, 4).map((n) => Math.max(0, Math.min(255, Math.round(n * 255))));
  return `<span class="swatch" style="background:rgb(${rgb.join(',')})" aria-hidden="true"></span>`;
}

export function paletteGradient(keys) {
  if (!keys.length) return 'transparent';
  const stops = keys.map((key) => {
    const r = Math.max(0, Math.min(255, Math.round(key.r * 255)));
    const g = Math.max(0, Math.min(255, Math.round(key.g * 255)));
    const b = Math.max(0, Math.min(255, Math.round(key.b * 255)));
    const pos = Math.max(0, Math.min(1, key.pos));
    return `rgb(${r},${g},${b}) ${(pos * 100).toFixed(2)}%`;
  });
  return `linear-gradient(90deg, ${stops.join(', ')})`;
}

export function highlightSource(analysis) {
  const rows = [];
  for (let i = 0; i < analysis.lines.length; i += 1) {
    const raw = analysis.lines[i];
    const info = analysis.lineInfo[i] || { type: 'body' };
    let code = '';
    let extra = '';
    if (info.type === 'blank') {
      code = esc(raw);
    } else if (info.type === 'comment') {
      code = `<span class="tok-com">${esc(raw)}</span>`;
    } else if (info.type === 'paletteRow') {
      extra = paletteSwatch(raw);
      code = highlightNumberRow(raw);
    } else if (info.type === 'timelineRow') {
      code = highlightTimelineRow(raw);
    } else if (info.type === 'patternRow') {
      code = highlightPatternRow(raw);
    } else if (info.type === 'venvRow') {
      code = highlightNumberRow(raw);
    } else if (info.type === 'directive' || info.type === 'header') {
      code = highlightDirective(raw, info);
    } else {
      code = lexCode(raw);
    }
    const kind = info.kind ? ` data-kind="${attr(info.kind)}"` : '';
    rows.push(
      `<div class="line ${info.type}"${kind} id="L${i + 1}">`
        + `<a class="ln" href="#L${i + 1}" aria-label="Line ${i + 1}">${i + 1}</a>`
        + `<span class="lc">${extra}${code}</span>`
        + '</div>',
    );
  }
  return rows.join('');
}
