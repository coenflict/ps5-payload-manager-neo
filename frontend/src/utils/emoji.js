import identifiers from '../assets/emoji/identifiers.js';

// Match the longest known sequence, including flags, skin tones and ZWJ/tag
// sequences. Variation selectors don't change which Twemoji artwork is used.
const root = new Map();
const sheets = new WeakMap();
export const EMOJI_BASE_URL = 'https://cdn.jsdelivr.net/gh/jdecked/twemoji@v15.1.0/assets/72x72/';
const CACHE_KEY = 'twemoji-v15.1.0-png';
const MAX_CACHED_IMAGES = 128;
const MAX_IMAGE_SIZE = 24000;
const isImage = value => typeof value === 'string' && value.length <= MAX_IMAGE_SIZE && /^data:image\/png;base64,[A-Za-z0-9+/=]+$/.test(value);

function artworkCache() {
  try {
    const saved = JSON.parse(localStorage.getItem(CACHE_KEY) || '{}');
    return Object.fromEntries(Object.entries(saved).filter(([key, value]) => /^[a-f0-9-]+$/.test(key) && isImage(value)).slice(-MAX_CACHED_IMAGES));
  } catch { return {}; }
}

function cacheArtwork(identifier, entry) {
  // Display is already handled by the native image loader. These optional APIs
  // must never determine whether an emoji can be rendered on older PS5 WebKit.
  if (typeof fetch !== 'function' || typeof FileReader !== 'function') return;
  Promise.resolve().then(() => fetch(`${EMOJI_BASE_URL}${identifier}.png`, { cache: 'force-cache' }))
    .then(response => { if (!response.ok) throw Error('Emoji unavailable'); return response.blob(); })
    .then(blob => new Promise((resolve, reject) => {
      if ((blob.type && blob.type !== 'image/png') || blob.size > MAX_IMAGE_SIZE * 0.75) { reject(); return; }
      const reader = new FileReader();
      reader.onload = () => resolve(reader.result);
      reader.onerror = reject;
      // Older WebKit can omit the MIME type on Response.blob(). Normalize it
      // before FileReader creates the data URL used by the persistent cache.
      reader.readAsDataURL(blob.type ? blob : new Blob([blob], { type: 'image/png' }));
    }))
    .then(image => {
      if (!isImage(image)) return;
      entry.cache[identifier] = image;
      const keys = Object.keys(entry.cache);
      if (keys.length > MAX_CACHED_IMAGES) delete entry.cache[keys[0]];
      try { localStorage.setItem(CACHE_KEY, JSON.stringify(entry.cache)); } catch { /* HTTP cache remains available */ }
    })
    .catch(() => {}); // Failed persistence leaves the displayed CDN image intact.
}

function ensureEmojiRule(identifier) {
  if (typeof document === 'undefined') return;
  let entry = sheets.get(document);
  if (!entry) {
    const style = document.createElement('style');
    style.setAttribute('data-emoji-images', '');
    document.head.appendChild(style);
    entry = { sheet: style.sheet, seen: new Set(), rules: new Map(), cache: artworkCache() };
    sheets.set(document, entry);
  }
  if (entry.seen.has(identifier)) return;
  entry.seen.add(identifier);
  // Only trusted manifest identifiers reach these rules. Do not depend on
  // insertRule's return value; some older WebKit builds omit it.
  let rule = entry.rules.get(identifier);
  if (!rule) {
    const index = entry.sheet.cssRules.length;
    entry.sheet.insertRule(`.markdown-body .emoji-${identifier}{}`, index);
    rule = entry.sheet.cssRules[index];
    entry.rules.set(identifier, rule);
  }
  const show = url => {
    rule.style.backgroundImage = `url("${url}")`;
    rule.style.textIndent = '-9999px';
  };
  const load = (url, onLoad, onError) => {
    const image = new Image();
    image.onload = onLoad;
    image.onerror = onError;
    image.src = url;
  };
  const loadCDN = () => {
    const url = `${EMOJI_BASE_URL}${identifier}.png`;
    load(url, () => { show(url); cacheArtwork(identifier, entry); }, () => { entry.seen.delete(identifier); });
  };
  if (entry.cache[identifier]) {
    const saved = entry.cache[identifier];
    load(saved, () => show(saved), () => { delete entry.cache[identifier]; loadCDN(); });
  } else loadCDN();
}
identifiers.forEach(identifier => {
  let branch = root;
  for (const point of identifier.split('-').filter(hex => hex !== 'fe0f')) {
    const character = String.fromCodePoint(parseInt(point, 16));
    if (!branch.has(character)) branch.set(character, new Map());
    branch = branch.get(character);
  }
  branch.set('', identifier);
});

export function emojiSegments(text) {
  const characters = Array.from(text), segments = [];
  let plain = '';
  for (let start = 0; start < characters.length;) {
    let branch = root, end = start, matchEnd = start, identifier;
    while (end < characters.length) {
      const character = characters[end];
      if (character !== '\uFE0F') {
        branch = branch.get(character);
        if (!branch) break;
      } else if (end === start) break;
      end++;
      if (branch.has('')) { identifier = branch.get(''); matchEnd = end; }
    }
    if (identifier === undefined) { plain += characters[start++]; continue; }
    if (plain) { segments.push({ text: plain }); plain = ''; }
    segments.push({ text: characters.slice(start, matchEnd).join(''), identifier });
    start = matchEnd;
  }
  if (plain) segments.push({ text: plain });
  return segments;
}

export function replaceEmojiText(element) {
  const document = element.ownerDocument;
  const walker = document.createTreeWalker(element, 4); // SHOW_TEXT
  const nodes = [];
  while (walker.nextNode()) nodes.push(walker.currentNode);
  for (const node of nodes) {
    const segments = emojiSegments(node.nodeValue);
    if (!segments.some(segment => segment.identifier !== undefined)) continue;
    const fragment = document.createDocumentFragment();
    for (const segment of segments) {
      if (segment.identifier === undefined) fragment.appendChild(document.createTextNode(segment.text));
      else {
        ensureEmojiRule(segment.identifier);
        const span = document.createElement('span');
        span.className = `emoji emoji-${segment.identifier}`;
        span.setAttribute('role', 'img');
        span.setAttribute('aria-label', segment.text);
        // Keep copyable/accessible text; CSS hides the unsupported native glyph.
        span.textContent = segment.text;
        fragment.appendChild(span);
      }
    }
    node.parentNode.replaceChild(fragment, node);
  }
}
