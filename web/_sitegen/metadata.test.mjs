import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { readMetadata } from './metadata.mjs';
import { analyzeDmt } from './dmt.mjs';
import { assetPath, assetUrl } from './assets/asset-path.mjs';

test('metadata supports comments, Unicode, paragraphs and repeated tags in the header', () => {
  const source = '# ordinary notes\n# @author Héllø & friends\n// @model Model A\n# @description First paragraph.\n# @description Second paragraph.\n# @tags ambient, FEEDBACK\n# @tags ambient texture\n# @duration 42.5\ntitle Actual title\n# @author ignored body comment';
  const meta = readMetadata(source);
  assert.deepEqual(meta.tags, ['ambient', 'feedback', 'texture']);
  assert.equal(meta.author, 'Héllø & friends');
  assert.equal(meta.model, 'Model A');
  assert.equal(meta.duration, 42.5);
  assert.equal(meta.description, 'First paragraph.\n\nSecond paragraph.');
  const demo = analyzeDmt(source, 'demo');
  assert.equal(demo.title, 'Actual title');
  assert.equal(demo.summary, 'First paragraph.');
  assert.deepEqual(demo.paragraphs, ['First paragraph.', 'Second paragraph.']);
});

test('invalid metadata fails with a useful line and cannot inject URL schemes', () => {
  for (const value of ['NaN', '-2', '0', '1e309', '30 seconds']) {
    assert.throws(() => readMetadata(`# @duration ${value}`), /line 1.*positive seconds/);
  }
  assert.throws(() => readMetadata('# @duration 2\n# @duration 3'), /line 2.*duplicate/);
  assert.throws(() => readMetadata('# @url javascript:alert(1)'), /HTTP\(S\)/);
  assert.throws(() => readMetadata('# @tags <script>'), /tags must/);
  assert.equal(readMetadata('# @url https://example.com/a?q=1').url, 'https://example.com/a?q=1');
  assert.deepEqual(readMetadata('# @future-key allowed for later readers'), { tags: [] });
});

test('asset paths preserve directories and encode URL characters', () => {
  assert.equal(assetPath('./assets/one/color.ppm'), 'assets/one/color.ppm');
  assert.notEqual(assetUrl('assets/one/color.ppm'), assetUrl('assets/two/color.ppm'));
  assert.equal(assetUrl('assets/a #1.ppm'), 'source/assets/a%20%231.ppm');
  for (const value of ['/etc/passwd', '../outside.ppm', 'assets/../../x', 'https://example.com/x', 'a\\b', '']) {
    assert.throws(() => assetPath(value));
  }
});

test('native compiler ignores metadata, including literal markup', () => {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'dm-meta-test-'));
  try {
    const source = 'title Meta\nsong channels=1\npixel {\n rgb(1, 0, 0)\n}\npattern 0 rows=1 {\n ...\n}\n';
    const compile = (name, text) => {
      const input = path.join(temp, name + '.dmt'), output = path.join(temp, name + '.dms');
      fs.writeFileSync(input, text);
      const result = spawnSync('./dm', ['compile', input, '-o', output], { encoding: 'utf8' });
      assert.equal(result.status, 0, result.stderr);
      return fs.readFileSync(output);
    };
    assert.deepEqual(compile('before', source), compile('after', '# @author <script>alert(1)</script>\n# @duration 20\n' + source));
  } finally { fs.rmSync(temp, { recursive: true, force: true }); }
});

test('site generation reads source credits, tags and duration without README or ffmpeg', () => {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'dm-site-test-'));
  try {
    const emptyPath = path.join(temp, 'bin'); fs.mkdirSync(emptyPath);
    const result = spawnSync(process.execPath, ['web/_sitegen/sitegen.mjs'], {
      encoding: 'utf8', env: { ...process.env, PATH: emptyPath, DM_WEB_OUT: temp }, maxBuffer: 4 * 1024 * 1024,
    });
    assert.equal(result.status, 0, result.stderr);
    assert.match(fs.readFileSync(path.join(temp, 'index.html'), 'utf8'), /data-tag="rave"/);
    const carnival = fs.readFileSync(path.join(temp, 'demo/carnival.html'), 'utf8');
    assert.ok(carnival.includes('DeepSeek V4.1 Flash'));
    const metadata = JSON.parse(fs.readFileSync(path.join(temp, 'demo/showcase.json'), 'utf8'));
    assert.equal(metadata.duration, 42.631836735);
    assert.equal(metadata.metadata.duration, 42.631836735);
    assert.ok(metadata.assets.some(asset => asset.path === 'assets/snare.wav' && asset.url === 'source/assets/snare.wav'));
    assert.ok(fs.existsSync(path.join(temp, 'source/assets/snare.wav')));
  } finally { fs.rmSync(temp, { recursive: true, force: true }); }
});
