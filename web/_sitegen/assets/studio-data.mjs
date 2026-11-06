export function trackerCells(raw, channels) {
  const comment = raw.search(/(?:^|\s)(?:#|\/\/)/);
  const code = (comment < 0 ? raw : raw.slice(0, comment)).replace(/^\s*[\da-f]+:\s*/i, '');
  const cells = code.split('|');
  return Array.from({ length: channels || cells.length }, (_, i) => cells[i]?.trim() || '.');
}

export function scoreSegments(states, fps, duration) {
  const segments = [];
  for (let i = 0; i < states.length / 3 && i / fps < duration; i++) {
    const order = states[i * 3];
    const end = Math.min(duration, (i + 1) / fps);
    if (segments.at(-1)?.order !== order) segments.push({ order, start: i / fps, end });
    else segments.at(-1).end = end;
  }
  return segments;
}

export function waveformPeaks(left, right, bins = 1024) {
  const peaks = new Float32Array(bins * 2);
  for (let bin = 0; bin < bins; bin++) {
    const start = Math.floor(bin * left.length / bins);
    const end = Math.min(left.length, Math.max(start + 1, Math.floor((bin + 1) * left.length / bins)));
    let low = 0, high = 0;
    for (let i = start; i < end; i++) {
      low = Math.min(low, left[i], right[i]); high = Math.max(high, left[i], right[i]);
    }
    peaks[bin * 2] = low; peaks[bin * 2 + 1] = high;
  }
  return peaks;
}
