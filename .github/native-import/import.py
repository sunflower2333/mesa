#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reconstruct the delivered foundation byte-for-byte; publish Git objects, not refs."""
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import urllib.request

BASE = 'ebe4a6652a1e07e5a91d9b47d57ce30069d32136'
TREE = '7f836bc327c050290fe4878ba6bb7a5daca44161'
REPO = 'sunflower2333/mesa'


# Hash exactly as Git does, including the length-prefixed blob header.
def blob_hash(data):
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()


# Use the scoped Actions token only to publish content-addressed Git objects.
def publish(endpoint, payload):
    request = urllib.request.Request(
        'https://api.github.com/repos/' + REPO + '/git/' + endpoint,
        data=json.dumps(payload).encode(),
        headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                 'Accept': 'application/vnd.github+json',
                 'Content-Type': 'application/json',
                 'X-GitHub-Api-Version': '2022-11-28'},
        method='POST')
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


# Verify all source and destination bytes before testing or uploading anything.
def main():
    if os.environ['GITHUB_REPOSITORY'] != REPO:
        raise RuntimeError('Wrong repository')
    tree = subprocess.check_output(['git', 'rev-parse', BASE + '^{tree}'], text=True).strip()
    if tree != TREE:
        raise RuntimeError('Baseline tree mismatch')
    outputs = {}
    for file in sorted(Path('.github/native-import').glob('recipe-*.json')):
        recipe = json.loads(file.read_text(encoding='utf-8'))
        if recipe['base'] != BASE or recipe['base_tree'] != TREE:
            raise RuntimeError('Recipe baseline mismatch')
        sources = []
        for path, sha in recipe['sources']:
            actual = subprocess.check_output(['git', 'rev-parse', BASE + ':' + path], text=True).strip()
            if actual != sha:
                raise RuntimeError('Source identity mismatch: ' + path)
            data = subprocess.check_output(['git', 'cat-file', 'blob', sha])
            if blob_hash(data) != sha:
                raise RuntimeError('Source bytes mismatch: ' + path)
            sources.append(data.decode('utf-8').splitlines(keepends=True))
        for entry in recipe['files']:
            path = entry['path']
            if not path.startswith(('src/freedreno/', '.github/workflows/')) or '..' in Path(path).parts:
                raise RuntimeError('Unexpected destination: ' + path)
            if path in outputs:
                raise RuntimeError('Duplicate destination: ' + path)
            if entry['parts'] is None:
                outputs[path] = None
                continue
            chunks = []
            for part in entry['parts']:
                if isinstance(part, str):
                    chunks.append(part)
                else:
                    source, start, count = part
                    if start < 0 or count <= 0 or start + count > len(sources[source]):
                        raise RuntimeError('Copy span out of bounds')
                    chunks.extend(sources[source][start:start + count])
            text = ''.join(chunks)
            # Repair the single JSON transport escape; the original delivered
            # SHA256 below remains authoritative, not this normalization.
            if path == 'src/freedreno/vulkan/tests/check_tu_wddm_policy.py':
                text = text.replace('wddm_source = "\n".join', 'wddm_source = "\\n".join')
            data = text.encode('utf-8')
            if hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise RuntimeError('Delivered SHA256 mismatch: ' + path)
            outputs[path] = data
    if len(outputs) != 31:
        raise RuntimeError('Incomplete foundation')
    for path, data in outputs.items():
        destination = Path(path)
        if data is None:
            destination.unlink()
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
    for script in ('src/freedreno/wddm/tests/check_native_split.py',
                   'src/freedreno/wddm/tests/run_packet.py',
                   'src/freedreno/vulkan/tests/check_tu_wddm_policy.py',
                   'src/freedreno/vulkan/tests/lifetime-perf/run.py'):
        subprocess.run(['python3', script], check=True, timeout=180)
    elements = []
    hashes = {}
    for path, data in outputs.items():
        old = subprocess.check_output(['git', 'ls-tree', BASE, '--', path], text=True)
        mode = old.split()[0] if old else '100644'
        sha = None
        if data is not None:
            sha = publish('blobs', {'encoding': 'base64',
                                   'content': base64.b64encode(data).decode()})['sha']
            if sha != blob_hash(data):
                raise RuntimeError('GitHub blob identity mismatch: ' + path)
            hashes[path] = hashlib.sha256(data).hexdigest()
        elements.append({'path': path, 'mode': mode, 'type': 'blob', 'sha': sha})
    result = publish('trees', {'base_tree': TREE, 'tree': elements})
    receipt = {'base': BASE, 'tree': result['sha'], 'files': hashes,
               'deleted': [p for p, d in outputs.items() if d is None],
               'refs_updated': False, 'delivered_file_hashes_verified': True}
    Path('native-import-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('NATIVE_FOUNDATION_TREE=' + result['sha'], flush=True)
    print('All 31 changes match the delivered patch; no branch was changed by this job.')


if __name__ == '__main__':
    main()
