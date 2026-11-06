import { scoreSegments, waveformPeaks, trackerCells } from './studio-data.mjs';
const host = document.querySelector('[data-live-demo]');
if (host) {
  const root = document.body.dataset.root || '';
  const buttons = [...host.querySelectorAll('[data-mode]')];
  const video = document.querySelector('#player');
  const panel = host.querySelector('.live-panel');
  const canvas = host.querySelector('canvas');
  const ctx = canvas.getContext('2d', { alpha: false });
  const status = host.querySelector('[data-live-status]');
  const play = host.querySelector('[data-live-play]');
  const restart = host.querySelector('[data-live-restart]');
  const seek = host.querySelector('[data-live-seek]');
  const clock = host.querySelector('[data-live-clock]');
  const studio = host.querySelector('.studio');
  const tracker = host.querySelector('.tracker-rows');
  const trackerHead = host.querySelector('.tracker-channel-head');
  const programSearch = host.querySelector('[data-program-search]');
  const playheadFollow = host.querySelector('[data-follow-playhead]');
  const code = host.querySelector('.clip-code');
  const ruler = host.querySelector('.studio-ruler');
  const follow = host.querySelector('[data-live-follow]');
  const workspace = host.querySelector('.studio-window');
  const dock = host.querySelector('[data-live-dock]');
  const timelineGrid = host.querySelector('.arrangement-grid');
  const timelineScroll = host.querySelector('.arrangement-scroll');
  const workspaceState = host.querySelector('[data-workspace-state]');
  const loading = host.querySelector('[data-engine-loading]');
  const loadingStage = host.querySelector('[data-engine-stage]');
  const loadingProgress = host.querySelector('[data-engine-progress]');
  const loadingPercent = host.querySelector('[data-engine-percent]');
  const loadingDetail = host.querySelector('[data-engine-detail]');
  const retry = host.querySelector('[data-engine-retry]');
  let frameImage, highlightedSource = -1;
  let timelineLayout = { label: 148, width: 720, viewport: 720 };
  let timelineZoom = 1;
  const precise = n => fmt(n) + '.' + String(Math.floor(n % 1 * 100)).padStart(2, '0');
  let mode = video ? 'video' : 'source', worker, audio, gain, source, buffer, metadata;
  let running = false, offset = 0, origin = 0, inFlight = false, ready = false;
  let states, duration = Number(host.dataset.duration), activePattern = -1, activeRow = -1;
  let raf = 0, previousFrame = -1, loadPromise, startGeneration = 0;
  const fmt = n => `${Math.floor(n / 60)}:${String(Math.floor(n % 60)).padStart(2, '0')}`;
  const position = () => running ? Math.min(duration, offset + audio.currentTime - origin) : offset;

  function pause() {
    startGeneration++;
    offset = position(); running = false;
    if (source) { source.onended = null; source.stop(); source.disconnect(); source = null; }
    play.textContent = 'Play';
    cancelAnimationFrame(raf); raf = 0;
    update();
  }
  async function start() {
    if (!ready) return;
    const generation = ++startGeneration;
    await audio.resume();
    if (generation !== startGeneration || running || (mode !== 'terminal' && mode !== 'studio')) return;
    if (offset >= duration) offset = 0;
    source = audio.createBufferSource(); source.buffer = buffer; source.connect(gain);
    origin = audio.currentTime; source.start(0, offset); running = true;
    source.onended = () => { if (running) { pause(); offset = duration; update(); } };
    play.textContent = 'Pause'; loop();
  }
  function jump(time) {
    const resume = running; pause(); offset = Math.max(0, Math.min(duration, time));
    previousFrame = -1; update(); if (resume) start();
  }
  function update() {
    const time = position(); seek.value = time; clock.textContent = `${precise(time)} / ${fmt(duration)}`;
    seek.setAttribute('aria-valuetext', `${precise(time)} of ${fmt(duration)}`);
    timelineGrid.style.setProperty('--position', time / duration);
    workspaceState.textContent = ready ? (running ? 'Playing' : 'Paused') : 'Preparing audio';
    if (running && mode === 'studio' && playheadFollow.checked) {
      const headX = timelineLayout.label + (timelineLayout.width - timelineLayout.label) * time / duration;
      if (headX > timelineScroll.scrollLeft + timelineLayout.viewport - 40 || headX < timelineScroll.scrollLeft + timelineLayout.label) {
        timelineScroll.scrollLeft = Math.max(0, headX - timelineLayout.label - 32);
      }
    }
    if (!ready || inFlight || mode === 'video' || mode === 'source') return;
    const frame = Math.floor(time * 30);
    if (frame === previousFrame) return;
    previousFrame = frame; inFlight = true;
    worker.postMessage({ type: 'frame', time, width: mode === 'studio' ? 160 : 80, height: mode === 'studio' ? 100 : 50 });
  }
  function loop() { update(); if (running) raf = requestAnimationFrame(loop); }

  function draw({ rgb, width, height }) {
    if (mode === 'studio') {
      if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
      if (!frameImage || frameImage.width !== width || frameImage.height !== height) frameImage = ctx.createImageData(width, height);
      const pixels = frameImage;
      for (let i = 0; i < width * height; i++) { pixels.data[i * 4] = rgb[i * 3]; pixels.data[i * 4 + 1] = rgb[i * 3 + 1]; pixels.data[i * 4 + 2] = rgb[i * 3 + 2]; pixels.data[i * 4 + 3] = 255; }
      ctx.putImageData(pixels, 0, 0);
    } else {
      // Same truecolour upper/lower half-cell mapping as dm_term_blit.
      canvas.width = 800; canvas.height = 500;
      for (let y = 0; y < 50; y++) for (let x = 0; x < 80; x++) {
        const i = (Math.floor(y * height / 50) * width + Math.floor(x * width / 80)) * 3;
        ctx.fillStyle = `rgb(${rgb[i]},${rgb[i + 1]},${rgb[i + 2]})`;
        ctx.fillRect(x * 10, y * 10, 10, 10);
      }
    }
  }

  function selectBlock(block) {
    if (!block) return;
    highlightedSource = -1;
    const original = document.querySelector('#code');
    code.replaceChildren(...Array.from({ length: block.end - block.start + 1 }, (_, i) => {
      const line = original.querySelector('#L' + (block.start + i + 1)).cloneNode(true);
      line.dataset.sourceLine = block.start + i + 1;
      line.removeAttribute('id'); return line;
    }));
    host.querySelector('[data-clip-title]').textContent = `${block.kind} ${block.name || ''}`.trim();
    host.querySelector('[data-clip-source]').href = '#L' + (block.start + 1);
    host.querySelectorAll('[data-block]').forEach(button => button.setAttribute('aria-pressed', String(Number(button.dataset.block) === metadata.blocks.indexOf(block))));
  }
  function syncTracker(state) {
    if (!metadata || mode !== 'studio') return;
    const [order, row, tick] = state, pattern = metadata.order[order];
    if (pattern !== activePattern) {
      activePattern = pattern; activeRow = -1;
      const block = metadata.blocks.find(b => b.kind === 'pattern' && Number(b.name) === pattern);
      const lines = metadata.patterns[String(pattern)] || [];
      tracker.replaceChildren(...lines.map((line, index) => {
        const div = document.createElement('div'); div.className = 'tracker-row';
        const number = document.createElement('span'); number.className = 'tracker-row-number'; number.textContent = index.toString(16).toUpperCase().padStart(2, '0');
        div.append(number);
        for (const raw of trackerCells(line.text, metadata.channels)) {
          const cell = document.createElement('span'); cell.className = 'tracker-cell'; cell.title = raw;
          const fields = raw.split(/\s+/);
          for (let i = 0; i < 4; i++) {
            const value = document.createElement('span'); value.className = i === 0 ? 'tracker-note' : 'tracker-field';
            value.textContent = fields[i] || (i === 0 || i === 3 ? '...' : '..'); cell.append(value);
          }
          div.append(cell);
        }
        return div;
      }));
      if (follow.checked || !code.childElementCount) selectBlock(block);
    }
    host.querySelector('[data-tracker-position]').textContent = `Order ${order} · Pattern ${pattern} · Row ${row.toString(16).toUpperCase().padStart(2, '0')} · Tick ${tick}`;
    if (row !== activeRow) {
      tracker.children[activeRow]?.classList.remove('is-current'); activeRow = row;
      const current = tracker.children[row];
      if (current) {
        current.classList.add('is-current');
        tracker.scrollTop = current.offsetTop - tracker.offsetTop - tracker.clientHeight / 2 + current.clientHeight / 2;
      }
    }
    const sourceLine = metadata.patterns[String(pattern)]?.[row]?.line;
    if (sourceLine !== highlightedSource) {
      for (const line of code.children) line.classList.toggle('is-current', Number(line.dataset.sourceLine) === sourceLine);
      highlightedSource = sourceLine;
    }
    const playingLine = code.querySelector('.is-current');
    if (playingLine && follow.checked) code.scrollTop = playingLine.offsetTop - code.clientHeight / 2;
    host.querySelectorAll('[data-order]').forEach(clip => clip.classList.toggle('is-current', position() >= Number(clip.dataset.start) && position() < Number(clip.dataset.end)));
    workspace.dataset.order = order; workspace.dataset.row = row;
  }
  function resizeTimeline() {
    const viewport = timelineScroll.clientWidth;
    const width = Math.max(viewport, 720) * timelineZoom;
    timelineGrid.style.width = width + 'px';
    timelineLayout = { width, viewport, label: parseFloat(getComputedStyle(timelineGrid).getPropertyValue('--lane-label')) || 148 };
  }
  function buildStudio() {
    metadata.channels ||= Math.max(1, ...Object.values(metadata.patterns).map(lines => trackerCells(lines[0]?.text || '').length));
    host.querySelector('.studio-tracker').style.setProperty('--tracker-channels', metadata.channels);
    const header = document.createElement('div'); header.className = 'tracker-heading-row';
    const rowLabel = document.createElement('span'); rowLabel.className = 'tracker-row-number'; rowLabel.textContent = 'ROW'; header.append(rowLabel);
    for (let i = 0; i < metadata.channels; i++) {
      const channel = document.createElement('span'); channel.className = 'tracker-column-heading';
      const title = document.createElement('b'); title.textContent = `CH ${String(i + 1).padStart(2, '0')}`;
      const fields = document.createElement('span'); fields.textContent = 'NOTE INS VOL FX'; channel.append(title, fields); header.append(channel);
    }
    trackerHead.replaceChildren(header);
    const clips = host.querySelector('.pattern-clips');
    const segments = scoreSegments(states, 30, duration);
    resizeTimeline();
    clips.replaceChildren(...segments.map(segment => {
      const button = document.createElement('button'); button.type = 'button';
      button.className = 'pattern-clip'; button.dataset.order = segment.order;
      button.dataset.start = segment.start; button.dataset.end = segment.end;
      const pattern = metadata.order[segment.order];
      button.style.left = segment.start / duration * 100 + '%';
      button.style.width = (segment.end - segment.start) / duration * 100 + '%';
      const title = document.createElement('b'); title.textContent = `Pattern ${pattern}`;
      const bars = document.createElement('span'); bars.className = 'clip-notes';
      const lines = metadata.patterns[String(pattern)] || [];
      for (const line of lines) {
        const note = document.createElement('i');
        note.style.opacity = /[A-G][#b-]\d/.test(line.text) ? '1' : '.15';
        bars.append(note);
      }
      button.append(title, bars);
      button.title = `Order ${segment.order} · pattern ${pattern} · ${precise(segment.start)}`;
      button.addEventListener('click', () => { follow.checked = true; jump(segment.start); selectBlock(metadata.blocks.find(b => b.kind === 'pattern' && Number(b.name) === pattern)); });
      return button;
    }));
    const blocks = host.querySelector('.program-clips');
    blocks.replaceChildren();
    for (const [name, kinds] of [['Sound', ['inst', 'chan', 'master']], ['Image', ['frame', 'pixel', 'post']], ['Score', ['pattern', 'timeline']], ['Functions', ['fn']]]) {
      const list = metadata.blocks.filter(block => kinds.includes(block.kind));
      if (!list.length) continue;
      const group = document.createElement('details'); group.open = true;
      const summary = document.createElement('summary'); summary.textContent = `${name} · ${list.length}`; group.append(summary);
      for (const block of list) {
        const button = document.createElement('button'); button.type = 'button'; button.className = 'program-clip';
        button.dataset.block = metadata.blocks.indexOf(block); button.setAttribute('aria-pressed', 'false');
        const kind = document.createElement('span'); kind.className = 'block-kind'; kind.textContent = block.kind;
        const label = document.createElement('span'); label.textContent = block.name || '';
        button.append(kind, label);
        button.title = `${block.kind} ${block.name || ''} · lines ${block.start + 1}–${block.end + 1}`;
        button.addEventListener('click', () => { follow.checked = false; selectBlock(block); if (innerWidth < 1000) setPane('code'); });
        group.append(button);
      }
      blocks.append(group);
    }
    filterPrograms();
    ruler.replaceChildren(...Array.from({ length: 11 }, (_, i) => {
      const tick = document.createElement('span'); tick.textContent = fmt(duration * i / 10);
      tick.style.left = i * 10 + '%'; return tick;
    }));
    const eventLanes = host.querySelector('.timeline-event-lanes');
    eventLanes.replaceChildren();
    for (const name of [...new Set(metadata.events.filter(event => event.time <= duration).map(event => event.name))]) {
      const lane = document.createElement('div'); lane.className = 'timeline-lane';
      const label = document.createElement('div'); label.className = 'lane-title'; label.textContent = name;
      const events = document.createElement('div'); events.className = 'lane-content event-clips';
      for (const event of metadata.events.filter(event => event.name === name && event.time <= duration)) {
        const button = document.createElement('button'); button.type = 'button'; button.className = 'event-clip';
        button.style.left = `${Math.min(100, event.time / duration * 100)}%`;
        button.title = `${event.name} = ${event.raw} · ${precise(event.time)}`; button.setAttribute('aria-label', button.title);
        button.addEventListener('click', () => jump(event.time)); events.append(button);
      }
      lane.append(label, events); eventLanes.append(lane);
    }
  }
  function drawWaveform(left, right) {
    const peaks = waveformPeaks(left, right);
    const wave = host.querySelector('.audio-waveform'); wave.width = peaks.length / 2; wave.height = 56;
    const paint = wave.getContext('2d'); paint.strokeStyle = '#a5acbf'; paint.lineWidth = 1;
    paint.beginPath();
    for (let x = 0; x < peaks.length / 2; x++) { paint.moveTo(x + .5, 28 + peaks[x * 2] * 26); paint.lineTo(x + .5, 28 + peaks[x * 2 + 1] * 26); }
    paint.stroke();
  }

  async function load() {
    if (loadPromise) return loadPromise;
    loading.hidden = false;
    loadingStage.textContent = 'Loading engine';
    loadingProgress.hidden = false;
    retry.hidden = true;
    loadingProgress.removeAttribute('value');
    loadingPercent.textContent = '';
    loadPromise = (async () => {
      audio = new AudioContext({ sampleRate: 44100 });
      gain = audio.createGain(); gain.gain.value = Number(host.querySelector('[data-live-volume]').value); gain.connect(audio.destination);
      const response = await fetch(host.dataset.metadata);
      if (!response.ok) throw new Error('Could not load demo metadata.');
      metadata = await response.json();
      worker = new Worker(root + 'engine-worker.js', { type: 'module' });
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error('Engine loading timed out. Try another demo or use video playback.')), 180000);
        worker.onerror = () => {
          clearTimeout(timeout);
          const error = new Error('Could not start the WebAssembly engine. Retry or use video playback.');
          if (ready) showFailure(error);
          reject(error);
        };
        worker.onmessage = ({ data }) => {
          if (data.type === 'progress') {
            const percent = Math.round(data.value * 100);
            status.textContent = `Preparing audio · ${percent}%`;
            if (loadingStage.textContent !== 'Preparing soundtrack') loadingStage.textContent = 'Preparing soundtrack';
            loadingProgress.value = data.value;
            loadingPercent.textContent = percent + '%';
            loadingDetail.textContent = `${fmt(data.value * duration)} of ${fmt(duration)} audio prepared`;
            workspaceState.textContent = `Preparing audio · ${percent}%`;
          }
          if (data.type === 'error') { clearTimeout(timeout); const error = new Error(data.message); if (ready) showFailure(error); reject(error); }
          if (data.type === 'ready') {
            clearTimeout(timeout); duration = data.duration; states = data.states;
            buffer = audio.createBuffer(2, data.left.length, 44100);
            buffer.copyToChannel(data.left, 0); buffer.copyToChannel(data.right, 1);
            drawWaveform(data.left, data.right);
            ready = true; seek.max = duration; play.disabled = restart.disabled = seek.disabled = false;
            loadingProgress.value = 1;
            loading.hidden = true;
            status.textContent = mode === 'studio' ? 'Live engine · 160 × 100 pixels' : 'Live engine · 80 × 25 cells';
            buildStudio(); resolve(); update();
          }
          if (data.type === 'frame') { inFlight = false; draw(data); syncTracker(data.state); update(); }
        };
        worker.postMessage({ type: 'load', source: new URL(host.dataset.source, location.href).href, assets: metadata.assets.map(asset => ({ ...asset, url: new URL(root + asset.url, location.href).href })), duration });
      });
    })();
    try { await loadPromise; }
    catch (error) {
      showFailure(error); throw error;
    }
  }
  function showFailure(error) {
    ready = false; inFlight = false; pause();
    play.disabled = restart.disabled = seek.disabled = true;
    status.textContent = error.message;
    loading.hidden = false;
    loadingStage.textContent = 'Could not prepare this demo'; loadingDetail.textContent = error.message;
    loadingProgress.hidden = true; retry.hidden = false;
    worker?.terminate(); worker = null; audio?.close(); loadPromise = null;
    workspaceState.textContent = 'Loading failed';
  }
  retry.addEventListener('click', () => load().then(() => { previousFrame = -1; activePattern = -1; update(); }).catch(() => {}));
  function filterPrograms() {
    const query = programSearch.value.trim().toLowerCase();
    let matches = 0;
    host.querySelectorAll('.program-clips details').forEach(group => {
      let groupMatches = 0;
      group.querySelectorAll('.program-clip').forEach(button => { button.hidden = Boolean(query) && !button.textContent.toLowerCase().includes(query); if (!button.hidden) groupMatches++; });
      group.hidden = groupMatches === 0;
      if (query && groupMatches) group.open = true;
      matches += groupMatches;
    });
    host.querySelector('[data-program-empty]').hidden = matches > 0 || !query;
  }
  programSearch.addEventListener('input', filterPrograms);
  tracker.addEventListener('scroll', () => { trackerHead.scrollLeft = tracker.scrollLeft; }, { passive: true });
  async function setMode(nextMode, updateHistory = true) {
    const previousMode = mode;
    if (updateHistory) {
      const url = new URL(location.href);
      if (nextMode === 'studio' || nextMode === 'terminal') url.searchParams.set('mode', nextMode);
      else url.searchParams.delete('mode');
      if (url.href !== location.href) history.pushState(null, '', url);
    }
    mode = nextMode; pause(); video?.pause();
    if (previousMode === 'video' && video) offset = Math.min(duration, video.currentTime);
    for (const button of buttons) button.setAttribute('aria-pressed', String(button.dataset.mode === mode));
    if (video) video.closest('.video-frame').hidden = mode !== 'video';
    panel.hidden = mode === 'video' || mode === 'source'; studio.hidden = mode !== 'studio';
    host.classList.toggle('studio-mode', mode === 'studio');
    host.querySelectorAll('[data-studio-only]').forEach(element => { element.hidden = mode !== 'studio'; });
    if (mode === 'studio') {
      workspace.append(panel);
      if (!workspace.open) workspace.showModal();
      resizeTimeline();
    } else {
      if (workspace.open) workspace.close();
      dock.after(panel);
    }
    if (mode === 'video' || mode === 'source') { if (video) video.currentTime = offset; return; }
    status.textContent = ready ? (mode === 'studio' ? '160 × 100 · WebAssembly' : '80 × 25 cells · WebAssembly') : 'Loading engine…';
    try { await load(); previousFrame = -1; activePattern = -1; update(); } catch { /* load sets visible recovery text */ }
  }
  for (const button of buttons) button.addEventListener('click', () => setMode(button.dataset.mode));
  host.querySelector('[data-studio-close]').addEventListener('click', () => workspace.close());
  workspace.addEventListener('close', () => {
    if (mode === 'studio') setMode(video ? 'video' : 'terminal');
  });
  workspace.addEventListener('click', event => {
    if (event.target.closest('a[href^="#"]')) workspace.close();
  });
  workspace.addEventListener('keydown', event => {
    if (event.target.closest('input,select,textarea,button,summary,a')) return;
    if (event.key === '/') { event.preventDefault(); if (innerWidth < 1000) setPane('browser'); programSearch.focus(); }
    if (event.code === 'Space') { event.preventDefault(); running ? pause() : start().catch(error => { status.textContent = error.message; }); }
    if (event.key === 'Home') { event.preventDefault(); jump(0); }
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') { event.preventDefault(); jump(position() + (event.key === 'ArrowLeft' ? -5 : 5)); }
  });
  function setPane(name) {
    workspace.dataset.pane = name;
    host.querySelectorAll('[data-pane]').forEach(button => { if (button.tagName === 'BUTTON') button.setAttribute('aria-pressed', String(button.dataset.pane === name)); });
  }
  host.querySelectorAll('button[data-pane]').forEach(button => button.addEventListener('click', () => setPane(button.dataset.pane)));
  host.querySelector('[data-timeline-zoom]').addEventListener('input', event => { timelineZoom = Number(event.target.value); event.target.setAttribute('aria-valuetext', timelineZoom + ' times'); resizeTimeline(); });
  host.querySelector('[data-fit-timeline]').addEventListener('click', () => {
    timelineZoom = 1;
    const zoom = host.querySelector('[data-timeline-zoom]'); zoom.value = 1; zoom.setAttribute('aria-valuetext', '1 times');
    resizeTimeline(); timelineScroll.scrollLeft = 0;
  });
  let scrubbing = false, resumeScrub = false;
  function scrub(event) { const box = ruler.getBoundingClientRect(); jump((event.clientX - box.left) / box.width * duration); }
  ruler.addEventListener('pointerdown', event => { if (!ready) return; resumeScrub = running; pause(); scrubbing = true; ruler.setPointerCapture(event.pointerId); scrub(event); });
  ruler.addEventListener('pointermove', event => { if (scrubbing) scrub(event); });
  ruler.addEventListener('pointerup', () => { scrubbing = false; if (resumeScrub) start(); });
  ruler.addEventListener('pointercancel', () => { scrubbing = false; });
  const separator = host.querySelector('.workspace-resize');
  let resizing = false;
  function timelineHeight(value) {
    const height = Math.max(180, Math.min(600, innerHeight * .65, value));
    workspace.style.setProperty('--timeline-height', height + 'px'); separator.setAttribute('aria-valuenow', Math.round(height));
  }
  separator.addEventListener('pointerdown', event => { resizing = true; separator.setPointerCapture(event.pointerId); event.preventDefault(); });
  separator.addEventListener('pointermove', event => { if (resizing) timelineHeight(innerHeight - event.clientY - 26); });
  separator.addEventListener('pointerup', () => { resizing = false; });
  separator.addEventListener('pointercancel', () => { resizing = false; });
  separator.addEventListener('keydown', event => { if (['ArrowUp','ArrowDown'].includes(event.key)) { event.preventDefault(); event.stopPropagation(); timelineHeight(Number(separator.getAttribute('aria-valuenow')) + (event.key === 'ArrowUp' ? 20 : -20)); } });
  new ResizeObserver(resizeTimeline).observe(timelineScroll);
  const urlMode = () => {
    const requested = new URLSearchParams(location.search).get('mode');
    return requested === 'studio' || requested === 'terminal' ? requested : video ? 'video' : 'source';
  };
  window.addEventListener('popstate', () => setMode(urlMode(), false));
  if (urlMode() === 'studio' || urlMode() === 'terminal') setMode(urlMode(), false);
  play.addEventListener('click', () => running ? pause() : start().catch(error => { status.textContent = error.message; }));
  restart.addEventListener('click', () => jump(0));
  follow.addEventListener('change', () => {
    if (follow.checked && metadata) {
      selectBlock(metadata.blocks.find(block => block.kind === 'pattern' && Number(block.name) === activePattern));
      previousFrame = -1; update();
    }
  });
  seek.addEventListener('input', () => jump(Number(seek.value)));
  document.addEventListener('click', event => {
    const target = event.target.closest('[data-seek]');
    if (!target || (mode !== 'terminal' && mode !== 'studio')) return;
    event.preventDefault(); event.stopImmediatePropagation();
    if (!ready) return;
    jump(Number(target.dataset.seek));
    start().catch(error => { status.textContent = error.message; });
    if (mode !== 'studio') host.scrollIntoView({ block: 'start', behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
  }, true);
  host.querySelector('[data-live-volume]').addEventListener('input', event => { if (gain) gain.gain.value = Number(event.target.value); });
  document.addEventListener('visibilitychange', () => { if (document.hidden) pause(); });
  window.addEventListener('pagehide', () => { pause(); worker?.terminate(); audio?.close(); });
}
