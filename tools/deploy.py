#!/usr/bin/env python3
"""Deploy a built native title to M.2, preserving all other titles and game saves."""
from pathlib import Path
import argparse,ftplib,hashlib,io,json,sys
ROOT=Path(__file__).resolve().parents[1]
(ROOT / "logs").mkdir(exist_ok=True)
from console_control import HOST, call
p=argparse.ArgumentParser()
p.add_argument('--probe',action='store_true')
p.add_argument('--data',action='store_true')
p.add_argument('--launch',action='store_true')
p.add_argument('--verify-hashes',action='store_true',help='also read back every uploaded byte (default: transfer and size checks)')
p.add_argument('--package-root',type=Path,help='pack-openmw.py output directory to deploy')
a=p.parse_args()
title='PPSA99631' if a.probe else 'PPSA99630'
package_root = a.package_root if a.package_root is not None else ROOT/('build/graphics-probe-package' if a.probe else 'build/package')
local=package_root.resolve()/'dist'/title
assert (local/'eboot.bin').is_file(),local
procs=json.loads(call(162,{}))['procs']
active=[(v.get('pid'),v.get('title_id')) for v in procs if v.get('title_id','').startswith('PPSA')]
assert not active,('Close running title before deploying',active)
f=ftplib.FTP();f.connect(HOST,2121,timeout=60);f.login()
def mkdir(path):
 cur=''
 for bit in path.split('/'):
  if not bit:continue
  cur+='/'+bit
  try:f.mkd(cur)
  except ftplib.error_perm:
   f.cwd(cur)
def put(source,target):
 mkdir(target.rsplit('/',1)[0])
 with source.open('rb') as stream:f.storbinary('STOR '+target+'.next',stream,blocksize=1024*1024)
 # FTP transfer completion plus stored size are sufficient for routine copies.
 expected_size=source.stat().st_size
 actual_size=f.size(target+'.next')
 if actual_size != expected_size:
  # FTP reports decoded ELF size for SELF executables; control metadata
  # reports the stored size without reading the file contents.
  parent,name=(target+'.next').rsplit('/',1)
  listing=json.loads(call(36,dict(path=parent)))
  entry=next((v for v in listing.get('entries',[]) if v.get('name')==name and v.get('kind')=='file'),None)
  actual_size=entry.get('size') if entry else None
 if actual_size != expected_size:
  raise RuntimeError(f'Size mismatch: {target}: {actual_size} != {expected_size}')
 item=dict(path=target,bytes=expected_size,verification='size')
 if a.verify_hashes:
  # Raw control reads avoid FTP's SELF decoding when full verification is requested.
  digest=hashlib.sha256();offset=0
  while offset<expected_size:
   b=call(48,dict(path=target+'.next',offset=offset,limit=1048576))
   if not b:raise RuntimeError('short read: '+target)
   digest.update(b);offset+=len(b)
  with source.open('rb') as stream:expected=hashlib.file_digest(stream,'sha256').hexdigest()
  assert offset==expected_size and digest.hexdigest()==expected,target
  item.update(sha256=expected,verification='sha256')
 f.rename(target+'.next',target)
 return item
remote='/mnt/ext1/etaHEN/games/'+title
try:
 f.cwd(remote)
 names={name for name,facts in f.mlsd()}
 assert 'openmw-port-receipt.json' in names,'Refusing to replace an unrecognized existing title'
except ftplib.error_perm:pass
receipt=[]
for source in sorted(local.rglob('*')):
 if source.is_file():receipt.append(put(source,remote+'/'+source.relative_to(local).as_posix()))
if a.data:
 data=ROOT/'game-data/Data Files'
 for source in sorted(data.rglob('*')):
  if source.is_file():receipt.append(put(source,remote+'/assets/Data Files/'+source.relative_to(data).as_posix()))
marker=json.dumps({'title':title,'files':receipt},indent=2).encode();f.storbinary('STOR '+remote+'/openmw-port-receipt.json',io.BytesIO(marker));(ROOT/'logs'/f'deploy-{title}.json').write_bytes(marker)
f.quit();print('Hash-verified deployment' if a.verify_hashes else 'Size-checked deployment',title,len(receipt),'files',flush=True)
print('Register:',call(56,dict(src_path=remote)),flush=True)
if a.launch:print('Launch:',call(60,dict(title_id=title)),flush=True)
