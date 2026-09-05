#!/usr/bin/python3
"""Independent root-authority validator for retained routing/GPU bytes."""
import argparse,hashlib,json,pathlib,re,struct,zlib
H=re.compile(r"^[0-9a-f]{64}$"); OBJ=re.compile(r"^/org/shadowblip/InputPlumber/[A-Za-z0-9_/]+$"); NODE=re.compile(r"^/dev/input/event[0-9]+$")
def die(x): raise SystemExit("artifact-authority: "+x)
def read(p,limit=8*1024*1024):
 p=pathlib.Path(p)
 if p.is_symlink() or not p.is_file() or p.stat().st_size>limit: die("unsafe/oversized raw artifact")
 return p.read_bytes()
def png(path):
 raw=read(path); 
 if raw[:8]!=b'\x89PNG\r\n\x1a\n': die("bad PNG signature")
 off=8; seen=[]; w=h=0
 while off+12<=len(raw):
  n=struct.unpack('>I',raw[off:off+4])[0]; typ=raw[off+4:off+8]; end=off+12+n
  if n>8*1024*1024 or end>len(raw) or zlib.crc32(raw[off+4:off+8+n])&0xffffffff!=struct.unpack('>I',raw[off+8+n:end])[0]: die("bad PNG chunk/CRC")
  if typ==b'IHDR':
   if n!=13 or seen: die("bad PNG IHDR")
   w,h=struct.unpack('>II',raw[off+8:off+16])
  seen.append(typ); off=end
  if typ==b'IEND': break
 if off!=len(raw) or not seen or seen[0]!=b'IHDR' or seen[-1]!=b'IEND' or b'IDAT' not in seen or w<1280 or h<720: die("incomplete/clipped PNG")
 return hashlib.sha256(raw).hexdigest(),w,h
def routing(root):
 p=root/'controller-production-routing'/'routing-results.json'; d=json.loads(read(p))
 if d.get('schema')!='controller-production-routing-results/v3': die('routing schema must include unassignment/silence v3')
 rows=d.get('targets');
 if not isinstance(rows,list) or len(rows)!=4: die('routing requires exactly four targets')
 paths=set();nodes=set();composites=set();obs=set();last=-1
 for i,r in enumerate(rows):
  if r.get('slot')!=i or not OBJ.fullmatch(str(r.get('dbus_path',''))) or not NODE.fullmatch(str(r.get('kernel_node',''))) or not OBJ.fullmatch(str(r.get('composite_path',''))): die('routing absolute identity malformed')
  for value,seen in ((r['dbus_path'],paths),(r['kernel_node'],nodes),(r['composite_path'],composites)):
   if value in seen: die('routing identity reused'); seen.add(value)
  if r.get('source_vidpid')!='045e:028e' or r.get('device_type')!='xb360' or r.get('target_devices')!=[r['dbus_path']] or r.get('concurrent_nonselected_events')!=0: die('routing assignment/cross-target silence invalid')
  st=r.get('source_event_us');tt=r.get('target_event_us');oid=r.get('observation_id')
  if type(st)is not int or type(tt)is not int or st<=last or tt<st or tt-st>2_000_000 or not isinstance(oid,str) or oid in obs: die('routing freshness/correlation invalid')
  obs.add(oid);last=st
  persisted=read(root/'controller-production-routing'/r['persisted_path'])
  if hashlib.sha256(persisted).hexdigest()!=r.get('persisted_sha256') or r.get('persisted_exact') is not True: die('routing persisted bytes not cross-bound')
 un=d.get('unassignment',{})
 if un.get('target_devices')!=[] or un.get('persisted_removed') is not True or un.get('target_events_after_clear')!=0 or un.get('production_dispatch') is not True: die('routing unassignment/no-output proof absent')
 cleanup=read(root/'controller-production-routing'/'cleanup.log')
 if hashlib.sha256(cleanup).hexdigest()!=d.get('cleanup_log_sha256') or d.get('cleanup',{}).get('targets_absent') is not True or d['cleanup'].get('kernel_nodes_absent') is not True: die('routing cleanup bytes do not prove cleanup')
def gpu(root,commit,tree,capability):
 cap=root/capability; manifest=json.loads(read(cap/'artifact-manifest.json'))
 if manifest.get('candidate_commit')!=commit or manifest.get('candidate_tree')!=tree: die('GPU installed manifest candidate mismatch')
 entries=manifest.get('artifacts'); by={x.get('filename'):x.get('sha256') for x in entries if isinstance(x,dict)} if isinstance(entries,list) else {}
 names=['controller-box-unhighlighted.png']+[f'capture-{x}.png' for x in ('a','b','x','y','up','down','left','right','start','select','guide','l1','r1','l2','r2','l3','r3')]
 if set(names)-set(by): die('GPU does not retain baseline plus all 17 controls')
 dig=[]
 for n in names:
  d,_,_=png(cap/n)
  if d!=by[n] or d in dig: die('GPU capture descriptor/digest reused or wrong')
  dig.append(d)
 verdict=json.loads(read(cap/'verdict.json')); renderer=json.loads(read(cap/'renderer-verdict.json')); installed=json.loads(read(cap/'installed-manifest.json'))
 if verdict.get('result')!='pass' or set(verdict.get('observations',{}))!=set(x.removeprefix('capture-').removesuffix('.png') for x in names[1:]): die('GPU 17-region oracle verdict incomplete')
 if renderer.get('result')!='pass' or re.search(r'(?i)llvmpipe|softpipe|software|swrast',str(renderer.get('renderer',''))): die('GPU renderer raw evidence rejected')
 if installed.get('commit')!=commit or installed.get('tree')!=tree or not isinstance(installed.get('files'),dict) or not all(H.fullmatch(str(x)) for x in installed['files'].values()): die('GPU installed product hashes not pinned')
def main():
 a=argparse.ArgumentParser();a.add_argument('--capability',required=True);a.add_argument('--artifacts',required=True);a.add_argument('--commit',required=True);a.add_argument('--tree',required=True);x=a.parse_args();root=pathlib.Path(x.artifacts)
 if x.capability=='controller-production-routing': routing(root)
 elif x.capability in ('gpu-compositor','installed-licensed-diagram'): gpu(root,x.commit,x.tree,x.capability)
 print('root-authority-artifact-validation: PASS')
main()
