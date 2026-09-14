#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify and reconstruct reviewed native-owner edits; publish blobs, never refs."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import urllib.request

BASE = '6ffc8992daeec195d182ea790358e9101457b2cf'


# Compute Git's exact content-addressed identity, independent of line endings.
def git_blob(data):
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()


# Read baseline bytes from Git objects, not a platform-transformed checkout.
def source_bytes(path, sha):
    actual = subprocess.check_output(['git', 'rev-parse', BASE + ':' + path], text=True).strip()
    if actual != sha:
        raise RuntimeError('Unexpected baseline: ' + path)
    data = subprocess.check_output(['git', 'cat-file', 'blob', sha])
    if git_blob(data) != sha:
        raise RuntimeError('Corrupt baseline object')
    return data


# Reconstruct all outputs before altering a file or issuing a remote write.
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    plan = json.loads(Path('.github/native-backend/plan.json').read_bytes())
    fixes = json.loads(Path('.github/native-backend/fixes.json').read_bytes())
    if plan['base'] != BASE or os.environ['GITHUB_REPOSITORY'] != 'sunflower2333/mesa':
        raise RuntimeError('Wrong baseline or repository')
    sources = [source_bytes(p, s).decode('utf-8').splitlines(keepends=True)
               for p, s in plan['sources']]
    outputs = {}
    for file in plan['files']:
        path = file['path']
        if not path.startswith('src/freedreno/wddm/') or '..' in Path(path).parts or path in outputs:
            raise RuntimeError('Invalid destination: ' + path)
        if file['parts'] == 'checkout':
            data = subprocess.check_output(['git', 'show', 'HEAD:' + path])
        else:
            pieces = []
            for part in file['parts']:
                if isinstance(part, str):
                    pieces.append(part)
                else:
                    source, start, count = part
                    if start < 0 or count < 1 or start + count > len(sources[source]):
                        raise RuntimeError('Copy range out of bounds')
                    pieces.extend(sources[source][start:start + count])
            data = ''.join(pieces).encode('utf-8')
        if hashlib.sha256(data).hexdigest() != file['sha256']:
            raise RuntimeError('Review hash mismatch: ' + path)
        if path in fixes:
            text = data.decode('utf-8')
            for old, new in fixes[path]['replacements']:
                if text.count(old) != 1:
                    raise RuntimeError('Correction anchor drift: ' + path)
                text = text.replace(old, new)
            data = text.encode('utf-8')
            if hashlib.sha256(data).hexdigest() != fixes[path]['sha256']:
                raise RuntimeError('Correction hash mismatch: ' + path)
        outputs[path] = data
    if set(fixes) - set(outputs):
        raise RuntimeError('Correction path was not reviewed')
    for path, data in outputs.items():
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(data)
    receipt = {'base': BASE, 'refs_updated': False, 'files': []}
    for path, data in outputs.items():
        sha = git_blob(data)
        if args.publish:
            request = urllib.request.Request(
                'https://api.github.com/repos/sunflower2333/mesa/git/blobs',
                data=json.dumps({'encoding': 'base64', 'content': base64.b64encode(data).decode()}).encode(),
                headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                         'Accept': 'application/vnd.github+json', 'Content-Type': 'application/json'},
                method='POST')
            with urllib.request.urlopen(request, timeout=60) as response:
                result = json.load(response)
            if result['sha'] != sha:
                raise RuntimeError('Published identity mismatch')
        receipt['files'].append({'path': path, 'sha': sha,
                                 'sha256': hashlib.sha256(data).hexdigest()})
    Path('native-backend-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('PASS reconstructed and hash-verified', len(outputs), 'native backend files')
    if args.publish:
        print(json.dumps(receipt))


if __name__ == '__main__':
    main()
