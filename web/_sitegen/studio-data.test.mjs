import assert from 'node:assert/strict';
import test from 'node:test';
import { scoreSegments, waveformPeaks, trackerCells } from './assets/studio-data.mjs';

test('tracker cells respect sharp notes, row labels, comments and empty channels', () => {
  const original = '\t0A: D#4  2  40 A0F | . | off 3 // release';
  assert.deepEqual(trackerCells(original, 4), ['D#4  2  40 A0F', '.', 'off 3', '.']);
  assert.deepEqual(trackerCells('  C-4 1 20 | | . # rest', 3), ['C-4 1 20', '.', '.']);
});
test('arrangement segments preserve repeated orders and clip the last interval to duration', () => {
  const states = new Int32Array([0,0,0, 0,1,0, 1,0,0, 0,0,0]);
  assert.deepEqual(scoreSegments(states, 2, 1.75), [
    {order:0,start:0,end:1}, {order:1,start:1,end:1.5}, {order:0,start:1.5,end:1.75},
  ]);
});
test('waveform represents peaks from both actual audio channels', () => {
  assert.deepEqual([...waveformPeaks(new Float32Array([0,.5,-.25,0]),new Float32Array([-.75,0,0,1]),2)],[-.75,.5,-.25,1]);
});
