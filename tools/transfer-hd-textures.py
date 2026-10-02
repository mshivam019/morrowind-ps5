#!/usr/bin/env python3
"""Verify MET locally offline; --upload explicitly enables separate verified transfer."""
import argparse
import concurrent.futures
import ftplib
import hashlib
import io
import json
from pathlib import Path
import threading
from texture_pack import DEFAULT_STAGE, PACK, REMOTE_TITLE, ROOT, TITLE, load_manifest


def require_inactive(response):
    if not isinstance(response, dict) or not isinstance(response.get('procs'), list):
        raise RuntimeError('Cannot verify running-title state; refusing transfer')
    for process in response['procs']:
        if not isinstance(process, dict) or not isinstance(process.get('title_id'), str):
            raise RuntimeError('Invalid process state; refusing transfer')
        if process.get('title_id', '').startswith(('PPSA', 'CUSA')):
            raise RuntimeError('Close running game titles before transferring textures')


def upload(stage, manifest, workers):
    # Offline paths never import/contact the console client.
    from console_control import HOST, call
    require_inactive(json.loads(call(162, {})))

    def connect():
        ftp = ftplib.FTP()
        ftp.connect(HOST, 2121, timeout=60)
        ftp.login()
        return ftp

    def mkdir(ftp, path):
        try:
            ftp.mkd(path)
        except ftplib.error_perm:
            ftp.cwd(path)  # Fail unless an actual directory already exists.

    base = REMOTE_TITLE + '/assets/Mods/' + manifest['pack']
    ftp = connect()
    try:
        ftp.cwd(REMOTE_TITLE)
        marker = io.BytesIO()
        ftp.retrbinary('RETR openmw-port-receipt.json', marker.write)
        if json.loads(marker.getvalue()).get('title') != TITLE:
            raise RuntimeError('Unrecognized existing title; refusing transfer')
        mkdir(ftp, REMOTE_TITLE + '/assets')
        mkdir(ftp, REMOTE_TITLE + '/assets/Mods')
        mkdir(ftp, base)
        for name in sorted({str(Path(item['path']).parent) for item in manifest['files']}):
            current = base
            for part in name.split('/'):
                current += '/' + part
                mkdir(ftp, current)
    finally:
        ftp.quit()

    local = threading.local()
    connections = []
    lock = threading.Lock()
    done = 0

    def put(item):
        nonlocal done
        if not hasattr(local, 'ftp'):
            local.ftp = connect()
            with lock:
                connections.append(local.ftp)
        target = base + '/' + item['path']
        temporary = target + '.next'
        with (stage / item['path']).open('rb') as stream:
            local.ftp.storbinary('STOR ' + temporary, stream, blocksize=1024*1024)
        digest = hashlib.sha256()
        offset = 0
        while offset < item['bytes']:
            block = call(48, dict(path=temporary, offset=offset,
                                 limit=min(1048576, item['bytes'] - offset)))
            if not block or len(block) > item['bytes'] - offset:
                raise RuntimeError('Invalid remote read: ' + temporary)
            digest.update(block)
            offset += len(block)
        if digest.hexdigest() != item['sha256']:
            raise RuntimeError('Remote hash mismatch: ' + temporary)
        local.ftp.rename(temporary, target)
        with lock:
            done += 1
            if done % 250 == 0:
                print(f'Remote SHA-256 verified {done}/{len(manifest["files"])}', flush=True)

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(put, manifest['files']))
    finally:
        for connection in connections:
            try:
                connection.quit()
            except (OSError, ftplib.Error):
                connection.close()
    receipt = json.dumps(dict(title=TITLE, root=base, manifest=manifest), indent=2).encode()
    ftp = connect()
    try:
        temporary = base + '/transfer-receipt.json.next'
        ftp.storbinary('STOR ' + temporary, io.BytesIO(receipt))
        offset, digest = 0, hashlib.sha256()
        while offset < len(receipt):
            block = call(48, dict(path=temporary, offset=offset,
                                 limit=min(1048576, len(receipt)-offset)))
            if not block or len(block) > len(receipt)-offset:
                raise RuntimeError('Short transfer-receipt read')
            digest.update(block)
            offset += len(block)
        if digest.hexdigest() != hashlib.sha256(receipt).hexdigest():
            raise RuntimeError('Transfer-receipt hash mismatch')
        ftp.rename(temporary, base + '/transfer-receipt.json')
    finally:
        ftp.quit()
    (ROOT / 'logs').mkdir(exist_ok=True)
    (ROOT / 'logs/hd-textures-transfer.json').write_bytes(receipt)
    print('Verified separate texture transfer complete. No title launch requested.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, default=DEFAULT_STAGE)
    parser.add_argument('--upload', action='store_true', help='contact PS5 and upload (default stays offline)')
    parser.add_argument('--workers', type=int, choices=range(1, 5), default=2)
    args = parser.parse_args()
    manifest = load_manifest(args.stage, verify=True)
    print(f'Local SHA-256 verified: {len(manifest["files"])} textures, {manifest["total_bytes"]} bytes')
    print('Remote destination:', REMOTE_TITLE + '/assets/Mods/' + manifest['pack'])
    if args.upload:
        upload(args.stage.resolve(), manifest, args.workers)
    else:
        print('Offline validation complete; --upload is required to contact the console.')


if __name__ == '__main__':
    main()
