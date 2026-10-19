#!/usr/bin/env python3
"""Generate the Twemoji identifier manifest from the pinned upstream archive.

Normal app builds use the checked-in outputs and require no download.
Usage: python3 tools/build_emoji_manifest.py scratch/twemoji-v15.1.0.tar.gz
"""
import hashlib
import json
from pathlib import Path
import sys
import tarfile

ARCHIVE_SHA256 = 'ffa39215bb14fbedf29743c5e7b897c0a2f06070f27009c6ec716ff080032d19'
ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'frontend/src/assets/emoji'


def main():
    archive = Path(sys.argv[1])
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise SystemExit('Archive hash does not match the pinned Twemoji v15.1.0 source')
    DEST.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as source:
        files = sorted((m for m in source.getmembers()
                        if m.isfile() and m.name.startswith('twemoji-15.1.0/assets/72x72/')
                        and m.name.endswith('.png')), key=lambda m: Path(m.name).stem)
        identifiers = [Path(member.name).stem for member in files]
        (DEST / 'identifiers.js').write_text('// Generated Twemoji identifiers; see README.md and LICENSE-GRAPHICS.\nexport default ' + json.dumps(identifiers, separators=(',', ':')) + ';\n')
        (DEST / 'LICENSE-GRAPHICS').write_bytes(source.extractfile('twemoji-15.1.0/LICENSE-GRAPHICS').read())
    print(f'{len(identifiers)} emoji identifiers')


if __name__ == '__main__':
    main()
