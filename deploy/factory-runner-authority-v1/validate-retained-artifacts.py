#!/usr/bin/python3
"""Coordinator-independent validation of exact held acceptance artifacts."""
import argparse,hashlib,json,pathlib,re,struct,sys,zlib
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent/'iprunner-probes'))
from unwrap_variant import decode_method_single, decode_object_manager, decode_string_method, variant_value
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
 raw=read(path)
 if raw[:8]!=b'\x89PNG\r\n\x1a\n': die("bad PNG signature")
 off=8;chunks=[];w=h=ct=bd=interlace=None;packed=bytearray();idat_done=False
 while off+12<=len(raw):
  if len(chunks)>=4096:die('too many PNG chunks')
  n=struct.unpack('>I',raw[off:off+4])[0];typ=raw[off+4:off+8];end=off+12+n
  if n>16*1024*1024 or end>len(raw) or zlib.crc32(raw[off+4:off+8+n])&0xffffffff!=struct.unpack('>I',raw[off+8+n:end])[0]:die('bad PNG chunk/CRC')
  payload=raw[off+8:off+8+n]
  if typ==b'IHDR':
   if chunks or n!=13:die('bad PNG IHDR')
   w,h,bd,ct,_,_,interlace=struct.unpack('>IIBBBBB',payload)
   if w<1280 or h<720 or w>8192 or h>8192 or w*h>16_777_216:die('PNG dimensions/pixels out of bounds')
  elif typ==b'IDAT':
   if idat_done:die('non-contiguous PNG IDAT')
   if len(packed)+n>16*1024*1024:die('PNG IDAT bytes exceed bound')
   packed.extend(payload)
  elif packed:
   idat_done=True
  if typ[0]&32==0 and typ not in (b'IHDR',b'IDAT',b'IEND'):die('unknown critical PNG chunk')
  chunks.append(typ);off=end
  if typ==b'IEND':break
 if off!=len(raw) or not chunks or chunks[0]!=b'IHDR' or chunks[-1]!=b'IEND' or not packed or w is None or bd!=8 or ct not in (2,6) or interlace!=0:die('unsupported/incomplete/clipped PNG')
 channels=3 if ct==2 else 4;stride=w*channels;expected=(stride+1)*h
 if expected>67_125_248 or expected>len(packed)*2048:die('PNG inflate size/ratio exceeds bound')
 try:
  decoder=zlib.decompressobj();data=bytearray()
  for pos in range(0,len(packed),65536):
   chunk=memoryview(packed)[pos:pos+65536]
   while chunk:
    piece=decoder.decompress(chunk,expected+1-len(data));data.extend(piece)
    if len(data)>expected:die('PNG inflate exceeds bound')
    chunk=decoder.unconsumed_tail
  data.extend(decoder.flush(expected+1-len(data)))
 except (zlib.error,ValueError):die('PNG IDAT decode failed')
 if len(data)!=expected or not decoder.eof or decoder.unused_data:die('PNG decoded size/stream mismatch')
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
 text=raw.decode('utf-8',errors='strict');rows=[];current=None
 for line in text.splitlines():
  m=re.fullmatch(r'\s*-\s+id:\s*["\']?([^"\'\s]+)["\']?\s*',line)
  if m:
   if current is not None:rows.append(current)
   current={'id':m.group(1)};continue
  m=re.fullmatch(r'\s+(slot|profile):\s*["\']?([^"\']*?)["\']?\s*',line)
  if m and current is not None:current[m.group(1)]=m.group(2)
 if current is not None:rows.append(current)
 matches=[r for r in rows if r.get('id')==pid]
 if slot is None:return not matches
 return len(matches)==1 and matches[0].get('slot')==str(slot)
def routing(root):
 cap=root/'controller-production-routing';d=json.loads(read(cap/'routing-results.json'));rows=d.get('targets')
 if d.get('schema')!='controller-production-routing-results/v3' or not isinstance(rows,list) or len(rows)!=4:die('routing v3/cardinality invalid')
 paths=set();nodes=set();sysfs=set();obs=set();last=-1;composite=None;source=None
 phase_names=['om-before.json','om-after-create.json','om-after-clear.json','om-cleanup.json']+[f'om-assignment-{i}.json' for i in range(4)]
 phases={n:decode_object_manager(json.loads(read(cap/n)),require_nonempty=True) for n in phase_names}
 provenance_before=read(cap/'provenance-before-routing.json');provenance_after=read(cap/'provenance-after-routing.json')
 try: provenance=json.loads(provenance_before)
 except ValueError:die('InputPlumber provenance fact is malformed')
 expected_provenance={'schema','unique_owner','pid','starttime','exe','exe_dev','exe_ino','exe_size','exe_sha256','package_name','package_version','package_installed','service_unit','service_type','service_active','exe_owned_by_package','verified_by'}
 if (provenance_before!=provenance_after or set(provenance)!=expected_provenance
     or provenance.get('schema')!='factory-host-inputplumber-provenance/v2'
     or not re.fullmatch(r':[0-9]+\.[0-9]+',str(provenance.get('unique_owner','')))
     or any(type(provenance.get(k)) is not int or provenance[k]<=0 for k in ('pid','starttime','exe_dev','exe_ino','exe_size'))
     or provenance.get('exe')!='/usr/bin/inputplumber' or not H.fullmatch(str(provenance.get('exe_sha256','')))
     or provenance.get('package_name')!='inputplumber' or not isinstance(provenance.get('package_version'),str)
     or provenance.get('service_unit')!='inputplumber.service' or provenance.get('service_type')!='dbus'
     or any(provenance.get(k) is not True for k in ('package_installed','service_active','exe_owned_by_package'))
     or provenance.get('verified_by')!='root-broker-held-proc-exe-outside-private-pids'):
  die('InputPlumber held executable provenance boundary changed or is invalid')
 try:
  raw_owner=decode_string_method(json.loads(read(cap/'dbus-unique-owner.json')))
  raw_pid=decode_method_single(json.loads(read(cap/'dbus-owner-pid.json')),'u')
 except (ValueError,TypeError):die('raw owner/PID replies are malformed')
 if raw_owner!=provenance['unique_owner'] or type(raw_pid) is not int or raw_pid!=provenance['pid']:die('raw owner/PID replies differ from root-held provenance')
 before_nodes=set(read(cap/'dev-input-before.txt').decode().splitlines());created_nodes=set(read(cap/'dev-input-after-create.txt').decode().splitlines());cleanup_nodes=set(read(cap/'dev-input-after-cleanup.txt').decode().splitlines())
 sysfs_facts=read(cap/'sysfs-targets.txt').decode('utf-8',errors='strict')
 physical=read(cap/'physical-source-sysfs.txt').decode('utf-8',errors='strict')
 physical_sysfs=re.search(r'(?m)^sysfs=(/sys/devices/.*usb\S*)$',physical)
 if (not physical_sysfs or not re.search(r'(?m)^vendor=(?:0x)?045e$',physical)
     or not re.search(r'(?m)^product=(?:0x)?028e$',physical)):die('retained physical USB 045e:028e sysfs identity invalid')
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
  target_iface='org.shadowblip.Input.Target';composite_iface='org.shadowblip.Input.CompositeDevice'
  created_props=phases['om-after-create.json'].get(r['dbus_path'],{}).get(target_iface)
  assigned_props=phases[f'om-assignment-{i}.json'].get(r['dbus_path'],{}).get(target_iface)
  composite_props=phases[f'om-assignment-{i}.json'].get(r['composite_path'],{}).get(composite_iface)
  if (not isinstance(created_props,dict) or not isinstance(assigned_props,dict)
      or variant_value(created_props.get('DeviceType'))!='xb360'
      or variant_value(assigned_props.get('DeviceType'))!='xb360'
      or variant_value(composite_props.get('TargetDevices') if isinstance(composite_props,dict) else None)!=[r['dbus_path']]):
   die('same ObjectManager rows do not bind xb360 target and exact TargetDevices')
  for value,seen in ((r['dbus_path'],paths),(r['kernel_node'],nodes),(r.get('sysfs_identity'),sysfs)):
   if not isinstance(value,str) or value in seen:die('routing stable identity reused')
   seen.add(value)
  if not r['sysfs_identity'].startswith('/sys/devices/') or r.get('source_vidpid')!='045e:028e' or r.get('device_type')!='xb360' or r.get('target_devices')!=[r['dbus_path']]:die('routing kernel/source/assignment binding invalid')
  if r['kernel_node'] in before_nodes or r['kernel_node'] not in created_nodes or r['kernel_node'] in cleanup_nodes or r['kernel_node'] not in sysfs_facts or r['sysfs_identity'] not in sysfs_facts:die('devnode/sysfs before-create-cleanup identity invalid')
  composite=composite or r['composite_path'];source=source or r.get('source_path')
  if r['composite_path']!=composite or r.get('source_path')!=source:die('routing did not use one real source/composite')
  st=r.get('source_event_us');tt=r.get('target_event_us');oid=r.get('observation_id')
  if type(st)is not int or type(tt)is not int or st<=last or tt<st or tt-st>2_000_000 or oid in obs:die('routing freshness invalid')
  obs.add(oid);last=st
  segment=log[windows[i].start():windows[i+1].start()]
  if (f'selected={i} clear=false' not in segment or f'QUIET-TAIL selected={i} ' not in segment
      or 'quiet_tail_ms=1000' not in segment):die('selected window/quiet-tail mismatch')
  events=re.findall(r'RAW node=(-?\d+) ts=(\d+) type=(\d+) code=(\d+) value=(-?\d+) bytes=([0-9a-f]+)',segment)
  matched=[e for e in events if (int(e[2]),int(e[3]),int(e[4]))==(1,304,1)]
  if not any(int(e[0])==-1 and int(e[1])==st for e in matched) or not any(int(e[0])==i and int(e[1])==tt for e in matched):die('held raw bytes do not prove selected correlation')
  if any(int(e[0]) not in (-1,i) for e in events):die('held raw bytes prove cross-target leakage')
  for e in events:
   b=bytes.fromhex(e[5]);
   if len(b)<24 or struct.unpack_from('HHi',b,len(b)-8)!=(int(e[2]),int(e[3]),int(e[4])):die('raw evdev bytes disagree with metadata')
  persisted=read(within(cap,r['persisted_path']))
  if hashlib.sha256(persisted).hexdigest()!=r.get('persisted_sha256') or not yaml_slot(persisted,r['persistent_id'],i):die('persisted selected assignment bytes invalid')
 if (source is None or source not in phases['om-before.json']
     or source.rsplit('/',1)[-1]!=pathlib.PurePosixPath(physical_sysfs.group(1)).name):die('raw USB/sysfs identity is not bound to the ObjectManager source row')
 if composite is None or variant_value(phases['om-after-clear.json'].get(composite,{}).get('org.shadowblip.Input.CompositeDevice',{}).get('TargetDevices'))!=[]:
  die('ObjectManager clear row does not prove exact empty TargetDevices')
 if any(path in phases['om-cleanup.json'] for path in paths):die('ObjectManager cleanup retains request target')
 clear=log[windows[4].start():]
 if ('clear=true' not in clear or 'full_window=true' not in clear
     or re.search(r'RAW node=[0-3] ',clear)):die('post-clear full-window raw target silence absent')
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
 selection=json.loads(read(cap/'profile-selection-evidence.json'));profile_raw=read(cap/'selected-profile.yaml')
 if (set(selection)!={'schema','commit','tree','filename','name','mapping_count','selection_route','selected_profile_sha256','display_order_only','icon_override'}
  or selection.get('schema')!='controller-box-profile-selection/v1' or selection.get('commit')!=commit or selection.get('tree')!=tree
  or selection.get('filename')!='gpu-xbox-360-oracle.yaml' or selection.get('name')!='GPU Xbox 360 Oracle'
  or selection.get('mapping_count')!=18 or selection.get('selection_route')!='production-ui-pointer'
  or selection.get('selected_profile_sha256')!=hashlib.sha256(profile_raw).hexdigest()
  or selection.get('display_order_only') is not True or selection.get('icon_override') is not False
  or profile_raw.count(b'\n  - ' )!=18 or profile_raw.count(b'target_events:')!=18):die('exact GPU oracle profile selection absent')
 ev=json.loads(read(cap/'device-type-evidence.json'))
 if (set(ev)!={'schema','commit','tree','source','target_object_path','target_name','device_type','icon_map_asset','profile_override'} or ev.get('schema')!='controller-box-device-type-evidence/v1' or ev.get('commit')!=commit or ev.get('tree')!=tree or ev.get('source')!='production-backend-selected-target' or not OBJ.fullmatch(str(ev.get('target_object_path',''))) or not isinstance(ev.get('target_name'),str) or not ev['target_name'] or ev.get('device_type')!='xb360' or ev.get('icon_map_asset')!='xbox-360.svg' or ev.get('profile_override') is not False):die('DeviceType production mapping absent')
 manager=read(cap/'manager.log')
 expected_selection=b'profile-selection: filename=gpu-xbox-360-oracle.yaml name=GPU Xbox 360 Oracle mappings=18 icon_override=false device_type=xb360 source=production-ui'
 if expected_selection not in manager or b'provenance=profile-override' in manager or b'provenance=supported-model' not in manager:die('profile/DeviceType production selection path invalid')
 ad=authority_dir();authority_raw=read(ad/'licensed-diagram-authority.json');authority=json.loads(authority_raw);oracle_raw=read(ad/'licensed-diagram-oracle.json');oracle=json.loads(oracle_raw)['models']['xb360']
 held={'icons/svg/xbox-360.svg':'installed-xbox-360.svg','icons/svg/LICENSE.controllercons':'installed-license.controllercons','controller-icons.yaml':'installed-controller-icons.yaml','controller-layouts/xbox-360.json':'installed-layout.json','licensed-diagram-oracle.json':'installed-oracle.json'}
 for rel,name in held.items():
  raw=read(cap/name)
  if hashlib.sha256(raw).hexdigest()!=authority['files'][rel]:die('held installed licensed bytes differ from pin')
 if read(cap/'installed-authority.json')!=authority_raw:die('held installed authority differs from canonical authority bytes')
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
 raw_renderer=read(cap/'egl-renderer-output.txt').decode('utf-8',errors='strict').splitlines()
 renderer_lines=[line[12:] for line in raw_renderer if line.startswith('GL_RENDERER=')]
 accelerated=re.compile(r'(?i)(virgl|virtio|nvidia|amd|radeon|intel|iris|nouveau)')
 if (renderer.get('result')!='pass' or len(renderer_lines)!=1 or renderer_lines[0]!=renderer.get('renderer')
     or not accelerated.search(renderer_lines[0]) or re.search(r'(?i)llvmpipe|softpipe|software|swrast',renderer_lines[0])):die('raw accelerated renderer evidence invalid')
 try: om=decode_object_manager(json.loads(read(cap/'device-type-om.json').decode('utf-8')),require_nonempty=True)
 except (ValueError,UnicodeError):die('raw ObjectManager DeviceType reply malformed')
 target_props=om.get(ev['target_object_path'],{}).get('org.shadowblip.Input.Target')
 if not isinstance(target_props,dict) or variant_value(target_props.get('DeviceType'))!='xb360':die('raw ObjectManager target row does not bind selected xb360 DeviceType')
def main():
 a=argparse.ArgumentParser();a.add_argument('--capability',required=True);a.add_argument('--artifacts',required=True);a.add_argument('--commit',required=True);a.add_argument('--tree',required=True);x=a.parse_args();root=pathlib.Path(x.artifacts)
 if x.capability=='controller-production-routing':routing(root)
 elif x.capability in ('gpu-compositor','installed-licensed-diagram'):gpu(root,x.commit,x.tree,x.capability)
 print('root-authority-artifact-validation: PASS')
main()
