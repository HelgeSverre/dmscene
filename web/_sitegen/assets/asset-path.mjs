// Paths are relative to the demo, shared by site generation and the Wasm FS.
export function assetPath(value) {
  if (typeof value !== 'string' || !value || value.startsWith('/') || /[\\\u0000:]/.test(value)) {
    throw new Error(`Asset path must be relative to the demo: ${value}`);
  }
  const parts = value.split('/').filter(part => part && part !== '.');
  if (!parts.length || parts.includes('..')) throw new Error(`Asset path leaves the demo directory: ${value}`);
  return parts.join('/');
}

export function assetUrl(value) {
  return 'source/' + assetPath(value).split('/').map(encodeURIComponent).join('/');
}
