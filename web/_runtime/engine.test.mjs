import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import test from 'node:test';

// The same ES module supports browser workers and Node; no mocked browser globals.
const { default: createEngine } = await import('../runtime/engine.mjs');
const engine = await createEngine();
const demos = new URL('../../demos/', import.meta.url);
engine.FS.mkdir('/assets');
for (const file of await fs.readdir(new URL('assets/', demos))) {
  engine.FS.writeFile('/assets/' + file, new Uint8Array(await fs.readFile(new URL('assets/' + file, demos))));
}

test('the shipped WebAssembly compiles every demo, synthesizes PCM and renders pixels', async () => {
  const env = engine._malloc(32);
  let audible = 0;
  try {
    for (const file of (await fs.readdir(demos)).filter(file => file.endsWith('.dmt'))) {
      const source = await fs.readFile(new URL(file, demos), 'utf8');
      assert.equal(engine.ccall('web_open', 'number', ['string'], [source]), 0,
        `${file}: ${engine.UTF8ToString(engine._web_error())}`);
      const pointer = engine._web_audio(1470);
      assert.ok(pointer, file);
      const pcm = engine.HEAPF32.subarray(pointer / 4, pointer / 4 + 2940);
      assert.ok(pcm.every(Number.isFinite), file);
      if (pcm.some(value => Math.abs(value) > .0001)) audible++;
      engine.HEAPF32.set(engine.HEAPF32.subarray(engine._web_env() / 4, engine._web_env() / 4 + 8), env / 4);
      const pixels = engine._web_frame(1 / 30, 80, 50, env, 1);
      assert.ok(pixels, file);
      assert.ok(engine._web_width() > 0 && engine._web_height() > 0, file);
      assert.equal(engine._web_frame(.01, 80, 50, env, 1) > 0, true, `${file}: rewind`);
    }
    assert.ok(audible > 30, `Only ${audible} demos produced audio`);
  } finally { engine._free(env); engine._web_close(); }
});

test('a failed compile can be followed by a valid demo', async () => {
  assert.equal(engine.ccall('web_open', 'number', ['string'], ['not a demo']), -1);
  const source = await fs.readFile(new URL('acid.dmt', demos), 'utf8');
  assert.equal(engine.ccall('web_open', 'number', ['string'], [source]), 0);
  assert.ok(engine._web_audio(1470));
  engine._web_close();
});

test('nested texture paths with matching basenames retain separate data', () => {
  engine.FS.mkdirTree('/assets/red'); engine.FS.mkdirTree('/assets/blue');
  const ppm = rgb => new Uint8Array([...Buffer.from('P6\n1 1\n255\n'), ...rgb]);
  engine.FS.writeFile('/assets/red/color.ppm', ppm([255, 0, 0]));
  engine.FS.writeFile('/assets/blue/color.ppm', ppm([0, 0, 255]));
  const source = '# @description These comments are ignored by the C compiler.\n' +
    'song channels=1\ntexture red "assets/red/color.ppm"\ntexture blue "assets/blue/color.ppm"\n' +
    'pixel {\n a = tex(red, 0, 0)\n b = tex(blue, 0, 0)\n rgb(a, 0, r2)\n}\npattern 0 rows=1 {\n ...\n}\n';
  assert.equal(engine.ccall('web_open', 'number', ['string'], [source]), 0, engine.UTF8ToString(engine._web_error()));
  const pixels = engine._web_frame(0, 4, 4, 0, 1);
  assert.ok(pixels);
  assert.deepEqual(Array.from(engine.HEAPU8.subarray(pixels, pixels + 3)), [255, 0, 255]);
  engine._web_close();
});
