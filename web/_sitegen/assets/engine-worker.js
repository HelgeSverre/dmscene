import createEngine from './runtime/engine.mjs';
import { assetPath } from './asset-path.mjs';
let engine, envelopes, states, duration, envPointer, previous = -1;
const FPS = 30, SR = 44100, BLOCK = SR / FPS;

self.onmessage = async ({ data }) => {
  try {
    if (data.type === 'load') {
      engine = await createEngine();
      await Promise.all(data.assets.map(async asset => {
        const response = await fetch(asset.url);
        if (!response.ok) throw new Error('Could not load ' + asset.path);
        const file = '/' + assetPath(asset.path);
        engine.FS.mkdirTree(file.slice(0, file.lastIndexOf('/')) || '/');
        engine.FS.writeFile(file, new Uint8Array(await response.arrayBuffer()));
      }));
      const response = await fetch(data.source);
      if (!response.ok) throw new Error('Could not load the demo source.');
      if (engine.ccall('web_open', 'number', ['string'], [await response.text()])) {
        throw new Error(engine.UTF8ToString(engine._web_error()));
      }
      duration = Math.min(300, Math.max(1, data.duration));
      const length = Math.ceil(duration * SR), frames = Math.ceil(length / BLOCK);
      const left = new Float32Array(length), right = new Float32Array(length);
      envelopes = new Float32Array(frames * 8);
      states = new Int32Array(frames * 3);
      envPointer = engine._malloc(8 * 4);
      for (let i = 0; i < frames; i++) {
        const n = Math.min(BLOCK, length - i * BLOCK);
        states.set([engine._web_order(), engine._web_row(), engine._web_tick()], i * 3);
        const pointer = engine._web_audio(n);
        if (!pointer) throw new Error('The synth could not prepare audio.');
        const pcm = engine.HEAPF32.subarray(pointer / 4, pointer / 4 + n * 2);
        for (let j = 0; j < n; j++) { left[i * BLOCK + j] = pcm[j * 2]; right[i * BLOCK + j] = pcm[j * 2 + 1]; }
        envelopes.set(engine.HEAPF32.subarray(engine._web_env() / 4, engine._web_env() / 4 + 8), i * 8);
        if (i % 15 === 0) {
          self.postMessage({ type: 'progress', value: i / frames });
          await new Promise(resolve => setTimeout(resolve, 0));
        }
      }
      self.postMessage({ type: 'ready', left, right, states, duration, fps: FPS }, [left.buffer, right.buffer]);
    } else if (data.type === 'frame' && engine && envelopes) {
      const at = Math.min(duration - 1 / SR, Math.max(0, data.time));
      const index = Math.min(envelopes.length / 8 - 1, Math.floor(at * FPS));
      engine.HEAPF32.set(envelopes.subarray(index * 8, index * 8 + 8), envPointer / 4);
      const pointer = engine._web_frame(at, data.width, data.height, envPointer, previous < 0 || at < previous || at - previous > .25);
      previous = at;
      if (!pointer) throw new Error('The scene could not render a frame.');
      const width = engine._web_width(), height = engine._web_height();
      const rgb = engine.HEAPU8.slice(pointer, pointer + width * height * 3);
      self.postMessage({ type: 'frame', rgb, width, height, time: at, state: Array.from(states.subarray(index * 3, index * 3 + 3)) }, [rgb.buffer]);
    }
  } catch (error) {
    self.postMessage({ type: 'error', message: error.message || String(error) });
  }
};
