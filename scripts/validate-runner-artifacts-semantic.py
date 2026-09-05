#!/usr/bin/python3
"""Coordinator-independent validation of exact held acceptance artifacts."""
import argparse,hashlib,json,pathlib,re,struct,zlib
H=re.compile(r"^[0-9a-f]{64}$"); OBJ=re.compile(r"^/org/shadowblip/InputPlumber/[A-Za-z0-9_/]+$"); NODE=re.compile(r"^/dev/input/event[0-9]+$")
def die(x): raise SystemExit("artifact-authority: "+x)
def read(p,limit=16*1024*1024):
 p=pathlib.Path(p)
 if p.is_symlink() or not p.is_file() or p.stat().st_size>limit: die("unsafe/oversized raw artifact")
 return p.read_bytes()
def within(root,rel):
 if not isinstance(rel,str) or pathlib.PurePosixPath(rel).is_absolute() or any(x in ('','.','..') for x in pathlib.PurePosixPath(rel).parts): die('artifact path traversal')
 p=root.joinpath(*pathlib.PurePosixPath(rel).parts)
 try:p.resolve().relative_to(root.resolve())
 except ValueError:die('artifact path escapes held root')
 return p
def png(path):
 raw=read(path); 
 if raw[:8]!=b'\x89PNG\r\n\x1a\n': die("bad PNG signature")
 off=8;chunks=[];w=h=ct=bd=interlace=None;packed=b'';idat_done=False
 while off+12<=len(raw):
  n=struct.unpack('>I',raw[off:off+4])[0];typ=raw[off+4:off+8];end=off+12+n
  if n>16*1024*1024 or end>len(raw) or zlib.crc32(raw[off+4:off+8+n])&0xffffffff!=struct.unpack('>I',raw[off+8+n:end])[0]:die('bad PNG chunk/CRC')
  payload=raw[off+8:off+8+n]
  if typ==b'IHDR':
   if chunks or n!=13:die('bad PNG IHDR')
   w,h,bd,ct,_,_,interlace=struct.unpack('>IIBBBBB',payload)
  elif typ==b'IDAT':
   if idat_done:die('non-contiguous PNG IDAT');packed+=payload
  elif packed:idat_done=True
  if typ[0]&32==0 and typ not in (b'IHDR',b'IDAT',b'IEND'):die('unknown critical PNG chunk')
  chunks.append(typ);off=end
  if typ==b'IEND':break
 if off!=len(raw) or not chunks or chunks[0]!=b'IHDR' or chunks[-1]!=b'IEND' or not packed or w<1280 or h<720 or bd!=8 or ct not in (2,6) or interlace!=0:die('unsupported/incomplete/clipped PNG')
 channels=3 if ct==2 else 4;stride=w*channels
 try:data=zlib.decompress(packed)
 except zlib.error:die('PNG IDAT decode failed')
 if len(data)!=(stride+1)*h:die('PNG decoded size mismatch')
 rows=[];prev=bytearray(stride);o=0
 def paeth(a,b,c):
  p=a+b-c;pa=abs(p-a);pb=abs(p-b);pc=abs(p-c);return a if pa<=pb and pa<=pc else b if pb<=pc else c
 for _ in range(h):
  f=data[o];src=data[o+1:o+1+stride];o+=stride+1;row=bytearray(stride)
  for i,x in enumerate(src):
   a=row[i-channels] if i>=channels else 0;b=prev[i];c=prev[i-channels] if i>=channels else 0
   if f==0:v=x
   elif f==1:v=x+a
   elif f==2:v=x+b
   elif f==3:v=x+((a+b)//2)
   elif f==4:v=x+paeth(a,b,c)
   else:die('invalid PNG filter')
   row[i]=v&255
  rows.append(row);prev=row
 return hashlib.sha256(raw).hexdigest(),w,h,channels,rows
def yaml_slot(raw,pid,slot=None):
 text=raw.decode('utf-8'); ids=re.findall(r'^\s*(?:id|PersistentId):\s*["\']?([^"\'\s]+)',text,re.M);slots=[int(x) for x in re.findall(r'^\s*slot:\s*(\d+)\s*$',text,re.M)]
 if slot is None:return pid not in ids
 return ids.count(pid)==1 and slot in slots
def routing(root):
 cap=root/'controller-production-routing';d=json.loads(read(cap/'routing-results.json'));rows=d.get('targets')
 if d.get('schema')!='controller-production-routing-results/v3' or not isinstance(rows,list) or len(rows)!=4:die('routing v3/cardinality invalid')
 paths=set();nodes=set();sysfs=set();obs=set();last=-1;composite=None;source=None
 log=read(cap/'observer.log').decode('utf-8');windows=list(re.finditer(r'WINDOW baseline=(\d+) selected=(-?\d+) clear=(true|false)',log))
 if len(windows)!=5:die('raw observer must contain four selection windows and one clear window')
 overlay=read(cap/'overlay.log').decode('utf-8',errors='replace')
 created=re.findall(r'target-created slot=([0-3]) path=(/org/shadowblip/InputPlumber/[A-Za-z0-9_/]+) device-type=xb360',overlay)
 if [int(x[0]) for x in created]!=[0,1,2,3]:die('production serial target creation evidence absent')
 udev=read(cap/'udev-targets.log').decode('utf-8',errors='replace')
 udev_nodes=[]
 for node in re.findall(r'DEVNAME=(/dev/input/event[0-9]+)',udev):
  if node not in udev_nodes:udev_nodes.append(node)
 for i,r in enumerate(rows):
  if r.get('slot')!=i or not OBJ.fullmatch(str(r.get('dbus_path',''))) or not NODE.fullmatch(str(r.get('kernel_node',''))) or not OBJ.fullmatch(str(r.get('composite_path',''))):die('routing identity malformed')
  if created[i][1]!=r['dbus_path']:die('DBus path is not bound to production creation order')
  for value,seen in ((r['dbus_path'],paths),(r['kernel_node'],nodes),(r.get('sysfs_identity'),sysfs)):
   if not isinstance(value,str) or value in seen:die('routing stable identity reused')
   seen.add(value)
  if not r['sysfs_identity'].startswith('/sys/devices/') or r.get('source_vidpid')!='045e:028e' or r.get('device_type')!='xb360' or r.get('target_devices')!=[r['dbus_path']]:die('routing kernel/source/assignment binding invalid')
  composite=composite or r['composite_path'];source=source or r.get('source_path')
  if r['composite_path']!=composite or r.get('source_path')!=source:die('routing did not use one real source/composite')
  st=r.get('source_event_us');tt=r.get('target_event_us');oid=r.get('observation_id')
  if type(st)is not int or type(tt)is not int or st<=last or tt<st or tt-st>2_000_000 or oid in obs:die('routing freshness invalid')
  obs.add(oid);last=st
  segment=log[windows[i].start():windows[i+1].start()]
  if f'selected={i} clear=false' not in segment:die('selected window mismatch')
  events=re.findall(r'RAW node=(-?\d+) ts=(\d+) type=(\d+) code=(\d+) value=(-?\d+) bytes=([0-9a-f]+)',segment)
  matched=[e for e in events if (int(e[2]),int(e[3]),int(e[4]))==(1,304,1)]
  if not any(int(e[0])==-1 and int(e[1])==st for e in matched) or not any(int(e[0])==i and int(e[1])==tt for e in matched):die('held raw bytes do not prove selected correlation')
  if any(int(e[0]) not in (-1,i) for e in events):die('held raw bytes prove cross-target leakage')
  for e in events:
   b=bytes.fromhex(e[5]);
   if len(b)<24 or struct.unpack_from('HHi',b,len(b)-8)!=(int(e[2]),int(e[3]),int(e[4])):die('raw evdev bytes disagree with metadata')
  persisted=read(within(cap,r['persisted_path']))
  if hashlib.sha256(persisted).hexdigest()!=r.get('persisted_sha256') or not yaml_slot(persisted,r['persistent_id'],i):die('persisted selected assignment bytes invalid')
 clear=log[windows[4].start():]
 if 'clear=true' not in clear or re.search(r'RAW node=[0-3] ',clear):die('post-clear raw target silence absent')
 un=d.get('unassignment',{});after=read(within(cap,un.get('persisted_path','')))
 if un.get('target_devices')!=[] or not yaml_slot(after,rows[0]['persistent_id']) or un.get('production_dispatch') is not True:die('persisted unassignment invalid')
 held_order=[n for n in udev_nodes if n in nodes]
 if held_order!=[r['kernel_node'] for r in rows]:die('kernel devnodes are not bound to held udev creation order')
 cleanup=read(cap/'cleanup.log')
 if hashlib.sha256(cleanup).hexdigest()!=d.get('cleanup_log_sha256') or b'targets-absent=true kernel-nodes-absent=true' not in cleanup:die('cleanup bytes invalid')
def authority_dir():
 here=pathlib.Path(__file__).resolve().parent
 for p in (here,here.parent/'deploy/factory-runner-authority-v1'):
  if (p/'licensed-diagram-authority.json').is_file():return p
 die('licensed authority unavailable')
def gpu(root,commit,tree,capability):
 cap=root/capability;m=json.loads(read(cap/'artifact-manifest.json'))
 if m.get('candidate_commit')!=commit or m.get('candidate_tree')!=tree:die('GPU candidate mismatch')
 by={x.get('filename'):x.get('sha256') for x in m.get('artifacts',[]) if isinstance(x,dict)}
 names=['controller-box-unhighlighted.png']+[f'capture-{x}.png' for x in ('a','b','x','y','up','down','left','right','start','select','guide','l1','r1','l2','r2','l3','r3')]
 imgs=[]
 for n in names:
  d,w,h,c,pix=png(cap/n)
  if by.get(n)!=d or d in [x[0] for x in imgs]:die('GPU capture digest missing/reused')
  imgs.append((d,w,h,c,pix))
 ev=json.loads(read(cap/'device-type-evidence.json'))
 if (set(ev)!={'schema','commit','tree','source','target_object_path','target_name','device_type','icon_map_asset','profile_override'} or ev.get('schema')!='controller-box-device-type-evidence/v1' or ev.get('commit')!=commit or ev.get('tree')!=tree or ev.get('source')!='production-backend-selected-target' or not OBJ.fullmatch(str(ev.get('target_object_path',''))) or not isinstance(ev.get('target_name'),str) or not ev['target_name'] or ev.get('device_type')!='xb360' or ev.get('icon_map_asset')!='xbox-360.svg' or ev.get('profile_override') is not False):die('DeviceType production mapping absent')
 if b'provenance=profile-override' in read(cap/'manager.log') or b'provenance=supported-model' not in read(cap/'manager.log'):die('profile override substituted for DeviceType path')
 ad=authority_dir();authority_raw=read(ad/'licensed-diagram-authority.json');authority=json.loads(authority_raw);oracle_raw=read(ad/'licensed-diagram-oracle.json');oracle=json.loads(oracle_raw)['models']['xb360']
 held={'icons/svg/xbox-360.svg':'installed-xbox-360.svg','icons/svg/LICENSE.controllercons':'installed-license.controllercons','controller-icons.yaml':'installed-controller-icons.yaml','controller-layouts/xbox-360.json':'installed-layout.json','licensed-diagram-oracle.json':'installed-oracle.json'}
 for rel,name in held.items():
  raw=read(cap/name)
  if hashlib.sha256(raw).hexdigest()!=authority['files'][rel]:die('held installed licensed bytes differ from pin')
 installed=json.loads(read(cap/'installed-manifest.json'))
 if installed.get('commit')!=commit or installed.get('tree')!=tree or installed.get('fallback') is not False:die('installed provenance invalid')
 controls=oracle['required_controls'];slugs=('a','b','x','y','up','down','left','right','start','select','guide','l1','r1','l2','r2','l3','r3')
 if len(controls)!=17:die('oracle control count invalid')
 base=imgs[0];x0,y0,dw,dh=16,88,300,300
 for idx,(control,slug) in enumerate(zip(controls,slugs),1):
  _,w,h,c,pix=imgs[idx]
  points=[]
  for y in range(y0,y0+dh):
   for x in range(x0,x0+dw):
    a=base[4][y][x*base[3]:x*base[3]+3];b=pix[y][x*c:x*c+3]
    if max(abs(a[j]-b[j]) for j in range(3))>=20:points.append((x-x0,y-y0))
  spec=oracle['controls'][control]
  if len(points)<spec['minimum_pixels']:die('independent oracle difference region too small')
  cx=sum(x for x,_ in points)/len(points);cy=sum(y for _,y in points)/len(points)
  if not(spec['centroid_x'][0]<=cx<=spec['centroid_x'][1] and spec['centroid_y'][0]<=cy<=spec['centroid_y'][1]):die('independent oracle marker misaligned')
 renderer=json.loads(read(cap/'renderer-verdict.json'))
 if renderer.get('result')!='pass' or re.search(r'(?i)llvmpipe|softpipe|software|swrast',str(renderer.get('renderer',''))):die('renderer evidence invalid')
def main():
 a=argparse.ArgumentParser();a.add_argument('--capability',required=True);a.add_argument('--artifacts',required=True);a.add_argument('--commit',required=True);a.add_argument('--tree',required=True);x=a.parse_args();root=pathlib.Path(x.artifacts)
 if x.capability=='controller-production-routing':routing(root)
 elif x.capability in ('gpu-compositor','installed-licensed-diagram'):gpu(root,x.commit,x.tree,x.capability)
 print('root-authority-artifact-validation: PASS')
main()
