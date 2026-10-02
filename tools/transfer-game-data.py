#!/usr/bin/env python3
"""Stage game assets on M.2, checking every transferred file by SHA-256."""
from pathlib import Path
import concurrent.futures, ftplib, hashlib, io, json, queue, sys, threading
ROOT=Path(__file__).resolve().parents[1]
from console_control import HOST, call
remote='/mnt/ext1/etaHEN/games/PPSA99630'
data=ROOT/'game-data/Data Files'
def connect():
 f=ftplib.FTP();f.connect(HOST,2121,timeout=60);f.login();return f
f=connect()
try:
 f.cwd(remote);names={n for n,d in f.mlsd()};assert 'openmw-port-receipt.json' in names,'Unrecognized title exists'
except ftplib.error_perm:
 f.mkd(remote);f.storbinary('STOR '+remote+'/openmw-port-receipt.json',io.BytesIO(b'{"title":"PPSA99630","stage":"game data only"}'))
try:f.mkd(remote+'/assets')
except ftplib.error_perm:f.cwd(remote+'/assets')
base=remote+'/assets/Data Files'
for p in [data,*sorted(x for x in data.rglob('*') if x.is_dir())]:
 dst=base+('/'+p.relative_to(data).as_posix() if p!=data else '')
 try:f.mkd(dst)
 except ftplib.error_perm:f.cwd(dst)
f.quit()
items=queue.Queue()
for p in sorted(data.rglob('*')):
 if p.is_file():items.put(p)
results=[];lock=threading.Lock()
def worker():
 f=connect()
 try:
  while True:
   try:p=items.get_nowait()
   except queue.Empty:break
   path=base+'/'+p.relative_to(data).as_posix()
   with p.open('rb') as stream:expected=hashlib.file_digest(stream,'sha256').hexdigest()
   with p.open('rb') as stream:f.storbinary('STOR '+path+'.next',stream,blocksize=1024*1024)
   offset=0;digest=hashlib.sha256()
   while offset<p.stat().st_size:
    b=call(48,dict(path=path+'.next',offset=offset,limit=1048576))
    assert b,path
    digest.update(b);offset+=len(b)
   assert offset==p.stat().st_size and digest.hexdigest()==expected,path
   f.rename(path+'.next',path)
   with lock:
    results.append(dict(path=p.relative_to(data).as_posix(),bytes=offset,sha256=expected))
    if len(results)%500==0:print('Verified',len(results),'files',flush=True)
 finally:f.quit()
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
 for job in [pool.submit(worker) for _ in range(4)]:job.result()
receipt=json.dumps({'root':base,'files':sorted(results,key=lambda x:x['path'])},indent=2).encode();(ROOT/'logs/game-data-transfer.json').write_bytes(receipt)
f=connect();f.storbinary('STOR '+remote+'/game-data-receipt.json',io.BytesIO(receipt));f.quit()
print('Complete:',len(results),'files,',sum(x['bytes'] for x in results),'bytes verified',flush=True)
