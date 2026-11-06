import assert from 'node:assert/strict';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { analyzeDmt, highlightSource } from './dmt.mjs';

const decode = text => text.replace(/<[^>]*>/g, '').replaceAll('&lt;', '<').replaceAll('&gt;', '>').replaceAll('&amp;', '&');
function highlightedLines(source) {
  return [...highlightSource(analyzeDmt(source, 'spacing')).matchAll(/<span class="lc">([\s\S]*?)<\/span><\/div>/g)]
    .map(match => decode(match[1].replace(/<span class="swatch"[^>]*><\/span>/g, '')));
}

test('source viewer preserves aligned pattern rows, tabs, comments and blank-line whitespace', () => {
  const lines = [
    'title Spacing',
    'song bpm=120 speed=6 rpb=4 channels=2',
    'pattern 0 rows=2 {',
    '\t00: C-4  1  40 ...  | E-4  2  20 A0F  # aligned <channels>',
    '\t01: ...  .  .. ...  | off  .  .. ...  ',
    '\t  ',
    '',
    '}',
  ];
  assert.deepEqual(highlightedLines(lines.join('\n')), lines);
});

test('all demo source lines survive syntax highlighting unchanged', () => {
  const dir = fileURLToPath(new URL('../../demos/', import.meta.url));
  for (const file of fs.readdirSync(dir).filter(file => file.endsWith('.dmt'))) {
    const source = fs.readFileSync(dir + file, 'utf8');
    const analysis = analyzeDmt(source, file);
    assert.deepEqual(highlightedLines(source), analysis.lines, file);
  }
});
