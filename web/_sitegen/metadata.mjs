// Optional authoring metadata lives in leading comments, never in bytecode.
const textFields = new Set(['author', 'model', 'description', 'music', 'visual', 'collection', 'license', 'url', 'warning']);

export function readMetadata(source) {
  const metadata = { tags: [] };
  for (const [index, line] of String(source).replace(/\r\n?/g, '\n').split('\n').entries()) {
    if (!line.trim()) continue;
    const comment = line.match(/^\s*(?:#|\/\/)(.*)$/);
    if (!comment) break;
    const field = comment[1].match(/^\s*@([a-z][a-z0-9-]*)\s+(.+?)\s*$/i);
    if (!field) continue;
    const key = field[1].toLowerCase(), value = field[2];
    if (key === 'tags') {
      const tags = value.toLowerCase().split(/[\s,]+/).filter(Boolean);
      if (tags.some(tag => !/^[a-z0-9][a-z0-9_-]*$/.test(tag))) {
        throw new Error(`metadata line ${index + 1}: tags must use letters, digits, hyphens or underscores`);
      }
      metadata.tags = [...new Set([...metadata.tags, ...tags])];
    } else if (key === 'duration') {
      if (!/^\d+(?:\.\d+)?$/.test(value) || !Number.isFinite(Number(value)) || Number(value) <= 0) {
        throw new Error(`metadata line ${index + 1}: duration must be positive seconds`);
      }
      if (metadata.duration !== undefined) throw new Error(`metadata line ${index + 1}: duplicate duration`);
      metadata.duration = Number(value);
    } else if (textFields.has(key)) {
      if (key === 'url') {
        let url;
        try { url = new URL(value); } catch { /* invalid URL below */ }
        if (!url || !['https:', 'http:'].includes(url.protocol)) {
          throw new Error(`metadata line ${index + 1}: url must be an absolute HTTP(S) address`);
        }
      }
      const separator = key === 'description' ? '\n\n' : ', ';
      metadata[key] = metadata[key] ? metadata[key] + separator + value : value;
    }
  }
  return metadata;
}
