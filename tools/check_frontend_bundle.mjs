import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'

const html = await readFile(new URL('../frontend/dist/index.html', import.meta.url), 'utf8')
assert.ok(html.includes('name="emoji-attribution"'), 'emoji artwork attribution must survive the build')
assert.ok(html.includes('https://cdn.jsdelivr.net/gh/jdecked/twemoji@v15.1.0/assets/72x72/'), 'emoji artwork must use the pinned CDN version')
assert.ok(html.length < 1200000, 'frontend must remain below the pre-CDN bundle size')
console.log('Production bundle: pinned Twemoji CDN, attribution and compact HTML verified')
