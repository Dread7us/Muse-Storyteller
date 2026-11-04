"""Refresh reviewed overlay hashes using the LF bytes Git publishes.

Run after reviewing source changes. Upstream commit/base hashes remain unchanged.
"""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def update(root=ROOT):
    manifest_path = root / 'firmware/upstream.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    overlay = (root / 'firmware/overlay').resolve()
    prepared = []
    for name, entry in manifest['files'].items():
        path = (overlay / name).resolve()
        if not path.is_relative_to(overlay):
            raise ValueError('Overlay path escapes the source directory')
        raw = path.read_bytes()
        raw.decode('utf-8')  # The overlay contains source text, never binary assets.
        canonical = raw.replace(b'\r\n', b'\n')
        prepared.append((path, raw, canonical, entry))
    # Validate every source before making any changes.
    normalized = 0
    for path, raw, canonical, entry in prepared:
        if raw != canonical:
            path.write_bytes(canonical)
            normalized += 1
        entry['overlay_sha256'] = hashlib.sha256(canonical).hexdigest()
    manifest_path.write_bytes((json.dumps(manifest, indent=2) + '\n').encode('utf-8'))
    return len(prepared), normalized


if __name__ == '__main__':
    files, normalized = update()
    print(f'Updated {files} overlay hashes; normalized {normalized} source files to LF. Upstream base hashes unchanged.')
