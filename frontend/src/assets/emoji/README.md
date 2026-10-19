Emoji artwork is from Twemoji v15.1.0 graphics by Twitter, Inc. and
other contributors, licensed under CC BY 4.0 (see LICENSE-GRAPHICS).

Source: https://github.com/jdecked/twemoji/tree/v15.1.0
Archive: https://codeload.github.com/jdecked/twemoji/tar.gz/refs/tags/v15.1.0
Archive SHA-256: ffa39215bb14fbedf29743c5e7b897c0a2f06070f27009c6ec716ff080032d19

Only the identifier manifest is bundled. Artwork is loaded on demand from
https://cdn.jsdelivr.net/gh/jdecked/twemoji@v15.1.0/assets/72x72/.
The version is pinned so cached artwork never needs periodic refreshes.

Fetched PNGs use the browser HTTP cache and a bounded localStorage cache of up
to 128 images (24 KB per data URL). Cached emoji work offline, including on the
PS5's HTTP origin where Service Worker/Cache Storage may be unavailable. Storage
failures do not prevent rendering. Artwork is decoded by the native image
loader first; fetch, Blob and FileReader are used only for optional persistent
caching. Empty Blob MIME types from older WebKit are normalized to image/png. Uncached emoji fall back to Unicode if the
CDN cannot be reached and retry when the markdown is rendered again.

Regenerate identifiers from the pinned archive (no image tools required):

    python3 tools/build_emoji_manifest.py scratch/twemoji-v15.1.0.tar.gz

Ordinary builds need no archive or network access.
