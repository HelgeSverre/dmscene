#!/usr/bin/env node
// Incremental, isolated browser build. Native objects/binary are untouched.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';

const root = fileURLToPath(new URL('../../', import.meta.url));
const out = path.join(root, 'web/runtime');
const build = path.join(root, 'build/wasm');
const emcc = process.env.EMCC || 'emcc';
const supported = fs.readFileSync(path.join(root, '.emscripten-version'), 'utf8').trim();
const sources = ['web/_runtime/engine.c', 'src/dm_vm.c', 'src/dm_font.c', 'src/dm_seq.c', 'src/dm_synth.c', 'src/dm_module.c', 'src/dm_compile.c', 'src/dm_scene.c'];
const flags = ['-Iinclude', '-O3', '-flto', '-std=c99', '-Wall', '-Wextra', '-Wno-unused-parameter', '-D_POSIX_C_SOURCE=200809L', '-DDM_NO_THREADS'];
const exports = ['web_open', 'web_close', 'web_error', 'web_audio', 'web_env', 'web_order', 'web_row', 'web_tick', 'web_frame', 'web_width', 'web_height', 'malloc', 'free'];
const linkFlags = ['-O3', '-flto', '-sSTRICT', '-sEXPORT_ES6', '-sENVIRONMENT=worker,node', '-sALLOW_MEMORY_GROWTH',
  '-sSTACK_SIZE=4MB', '-sINITIAL_MEMORY=32MB',
  '-sEXPORTED_FUNCTIONS=' + exports.map(name => '_' + name).join(','),
  '-sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,FS,HEAPU8,HEAPF32'];
const hash = value => createHash('sha256').update(value).digest('hex');
const read = file => fs.readFileSync(path.join(root, file));
const run = args => {
  const result = spawnSync(emcc, args, { cwd: root, stdio: 'inherit' });
  if (result.error || result.status !== 0) throw new Error(`Emscripten failed${result.error ? ': ' + result.error.message : ' (exit ' + result.status + ')'}`);
};

try {
  const version = spawnSync(emcc, ['--version'], { encoding: 'utf8' });
  if (version.error || version.status !== 0) {
    throw new Error(`Cannot run ${emcc}. Activate Emscripten ${supported} (source /path/to/emsdk/emsdk_env.sh),\nthen run make wasm; or set EMCC=/path/to/emcc. See docs/wasm.md.`);
  }
  fs.mkdirSync(build, { recursive: true }); fs.mkdirSync(out, { recursive: true });
  const cacheFile = path.join(build, 'build.json');
  let previous = {};
  try { previous = JSON.parse(fs.readFileSync(cacheFile, 'utf8')); } catch { /* first build */ }
  const shared = hash(version.stdout + JSON.stringify(flags) + read('include/dm.h') + read('include/dm_ops.h'));
  const cache = { objects: {} }, objects = [];
  let changed = false;
  for (const source of sources) {
    const object = path.join(build, source.replace(/\.c$/, '.o'));
    objects.push(object);
    const digest = hash(shared + read(source));
    cache.objects[source] = digest;
    if (previous.objects?.[source] === digest && fs.existsSync(object)) continue;
    fs.mkdirSync(path.dirname(object), { recursive: true });
    console.log(`wasm: ${source}`);
    run([...flags, '-c', source, '-o', object]); changed = true;
  }
  cache.link = hash(JSON.stringify(cache.objects) + JSON.stringify(linkFlags) + version.stdout);
  if (changed || previous.link !== cache.link || !['engine.mjs', 'engine.wasm'].every(file => fs.existsSync(path.join(out, file)))) {
    const temp = fs.mkdtempSync(path.join(out, '.build-'));
    try {
      run([...objects, ...linkFlags, '-o', path.join(temp, 'engine.mjs')]);
      for (const file of ['engine.mjs', 'engine.wasm']) fs.renameSync(path.join(temp, file), path.join(out, file));
    } finally { fs.rmSync(temp, { recursive: true, force: true }); }
    fs.writeFileSync(cacheFile, JSON.stringify(cache));
    console.log('wasm: wrote web/runtime/engine.mjs and engine.wasm');
  } else console.log('wasm: up to date');
} catch (error) { console.error(`wasm: ${error.message}`); process.exitCode = 1; }
