#!/usr/bin/env python3
import fcntl,hashlib,json,os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];TOOL=ROOT/'scripts/archive-factory-campaign.py'
NAME_RE=re.compile(r'^campaign-a-\d{8}T\d{6}Z-[0-9a-f]{16}\.manifest\.json$')
def run(root,key,campaign_id='campaign-a',*extra):
 fd=os.open(key,os.O_RDONLY)
 try:
  env={**os.environ,'FACTORY_COORDINATOR_AUTH_FD':str(fd)}
  return subprocess.run([str(TOOL),'--root',str(root),'--campaign-id',campaign_id,*extra],capture_output=True,text=True,env=env,pass_fds=(fd,))
 finally:os.close(fd)
def make_campaign(root,cid='campaign-a'):
 state=root/'.factory-state';campaigns=state/'campaigns'
 state.mkdir(mode=0o700,exist_ok=True);state.chmod(0o700)
 campaigns.mkdir(mode=0o700,exist_ok=True);campaigns.chmod(0o700)
 a=campaigns/cid;a.mkdir(mode=0o700)
 f=a/'factory-loop.json';f.write_text(json.dumps({'outcome':'failed','campaign_id':cid}));f.chmod(0o600)
 return a
def archives(root,cid='campaign-a'):
 d=root/'.factory-state'/'campaign-archives'
 if not d.exists():return []
 pat=re.compile(r'^'+re.escape(cid)+r'-\d{8}T\d{6}Z-[0-9a-f]{16}\.manifest\.json$')
 return sorted(p for p in d.iterdir() if pat.fullmatch(p.name))
def keyfile(root):return root.parent/(root.name+'-archive-key')
def setup(root):
 (root/'.git').mkdir()
 state=root/'.factory-state';state.mkdir(mode=0o700)
 key=keyfile(root);key.write_bytes(os.urandom(32));key.chmod(0o600)
 return key
# Basic archive: campaign deleted, unrelated campaign and .ralph untouched,
# manifest binds schema/namespaces/archive digest, name is stamp+nonce.
with tempfile.TemporaryDirectory() as td:
 root=Path(td);key=setup(root)
 try:
  a=make_campaign(root)
  receipts=root/'.factory-state'/'audit-receipts';receipts.mkdir(mode=0o700)
  receipt=receipts/'campaign-a-final.json';receipt.write_text('{"campaign_id":"campaign-a","exit_code":0}\n');receipt.chmod(0o600)
  other=root/'.factory-state'/'campaigns'/'campaign-b';other.mkdir(mode=0o700)
  of=other/'factory-loop.json';of.write_text('other\n');of.chmod(0o600)
  ralph=root/'.ralph';ralph.mkdir();sentinel=ralph/'keep';sentinel.write_text('keep')
  r=run(root,key);assert r.returncode==0,r.stderr;assert not a.exists();assert of.read_text()=='other\n' and sentinel.read_text()=='keep'
  manifests=archives(root);assert len(manifests)==1 and NAME_RE.fullmatch(manifests[0].name)
  m=json.loads(manifests[0].read_text());assert m['schema']=='factory-campaign-archive/v2' and m['authentication']['namespace']=='factory-campaign-archive'
  assert m['campaign_id']=='campaign-a' and m['created_at']==manifests[0].name.split('-')[2]
  assert m['namespaces']['campaigns'][0]['path']=='campaigns/campaign-a/factory-loop.json'
  assert m['namespaces']['audit-receipts'][0]['path']=='audit-receipts/campaign-a-final.json'
  archive=manifests[0].with_name(manifests[0].name.replace('.manifest.json','.tar'));assert m['archive_sha256']==hashlib.sha256(archive.read_bytes()).hexdigest()
  # Tampered retained same-campaign archive blocks before publish/delete: the
  # campaign survives and no new archive is published (timing-independent).
  a=make_campaign(root)
  archive.write_bytes(archive.read_bytes()+b'x')
  rr=run(root,key);assert rr.returncode!=0 and a.exists() and len(archives(root))==1
  # Repairing the retained archive lets the operation complete.
  archive.write_bytes(archive.read_bytes()[:-1])
  r=run(root,key);assert r.returncode==0,r.stderr;assert not a.exists() and len(archives(root))==2
  # Unsafe mode is rejected without touching the campaign.
  a=make_campaign(root);bad=a/'factory-loop.json';bad.chmod(0o644)
  r=run(root,key);assert r.returncode!=0 and a.exists();bad.chmod(0o600)
  # Active/resumable campaign is rejected.
  bad.write_text('{"current_phase":"implementation","campaign_id":"campaign-a"}\n')
  r=run(root,key);assert r.returncode!=0 and 'active/resumable' in r.stderr and a.exists()
  bad.write_text('{"current_phase":"failed","campaign_id":"campaign-a"}\n')
  # Active campaign lock is rejected.
  fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY);fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
  try:r=run(root,key);assert r.returncode!=0 and 'active campaign lock' in r.stderr
  finally:os.close(fd)
 finally:key.unlink(missing_ok=True)
# A tampered archive for an unrelated campaign never blocks and is untouched.
with tempfile.TemporaryDirectory() as td:
 root=Path(td);key=setup(root)
 try:
  a=make_campaign(root,'campaign-a');r=run(root,key,'campaign-a');assert r.returncode==0,r.stderr
  b=make_campaign(root,'campaign-b');r=run(root,key,'campaign-b');assert r.returncode==0,r.stderr
  bm=archives(root,'campaign-b')[0];bt=bm.with_name(bm.name.replace('.manifest.json','.tar'))
  bt.write_bytes(bt.read_bytes()+b'x')
  a=make_campaign(root,'campaign-a')
  r=run(root,key,'campaign-a');assert r.returncode==0,r.stderr;assert not a.exists()
  assert bt.read_bytes().endswith(b'x') and len(archives(root,'campaign-b'))==1
 finally:key.unlink(missing_ok=True)
# Prefix-overlapping campaign IDs are isolated: retention for `campaign` never
# prunes `campaign-a` archives.
with tempfile.TemporaryDirectory() as td:
 root=Path(td);key=setup(root)
 try:
  a=make_campaign(root,'campaign');r=run(root,key,'campaign');assert r.returncode==0,r.stderr
  b=make_campaign(root,'campaign-a');r=run(root,key,'campaign-a');assert r.returncode==0,r.stderr
  a=make_campaign(root,'campaign')
  r=run(root,key,'campaign','--retention','1');assert r.returncode==0,r.stderr
  assert len(archives(root,'campaign'))==1 and len(archives(root,'campaign-a'))==1
 finally:key.unlink(missing_ok=True)
# Same-second archives get unique stamp+nonce names (no collision reliance).
with tempfile.TemporaryDirectory() as td:
 root=Path(td);key=setup(root)
 try:
  a=make_campaign(root);r=run(root,key);assert r.returncode==0,r.stderr
  a=make_campaign(root);r=run(root,key);assert r.returncode==0,r.stderr
  ms=archives(root);assert len(ms)==2 and len({m.name for m in ms})==2
  for m in ms:assert NAME_RE.fullmatch(m.name)
 finally:key.unlink(missing_ok=True)
# Retention keeps only the newest bound, pruning oldest after verification.
with tempfile.TemporaryDirectory() as td:
 root=Path(td);key=setup(root)
 try:
  for _ in range(3):
   make_campaign(root)
   r=run(root,key,'campaign-a','--retention','2');assert r.returncode==0,r.stderr
  assert len(archives(root))==2
 finally:key.unlink(missing_ok=True)
# An archive larger than MAX_MEMBER (32 MiB) still verifies: the archive read
# bound is MAX_BYTES, not the per-member bound.
with tempfile.TemporaryDirectory() as td:
 root=Path(td);key=setup(root)
 try:
  a=make_campaign(root)
  (a/'big1.bin').write_bytes(b'x'*(17*1024*1024));(a/'big1.bin').chmod(0o600)
  (a/'big2.bin').write_bytes(b'y'*(17*1024*1024));(a/'big2.bin').chmod(0o600)
  r=run(root,key);assert r.returncode==0,r.stderr;assert not a.exists()
  assert len(archives(root))==1
 finally:key.unlink(missing_ok=True)
# Unsafe symlink/mode in the archive directory or campaign tree fails closed
# without publishing or deleting.
with tempfile.TemporaryDirectory() as td:
 root=Path(td);key=setup(root)
 try:
  a=make_campaign(root);r=run(root,key);assert r.returncode==0,r.stderr
  m=archives(root)[0];t=m.with_name(m.name.replace('.manifest.json','.tar'))
  # Unsafe mode on a retained tar fails closed before publish/delete.
  t.chmod(0o644)
  a=make_campaign(root)
  r=run(root,key);assert r.returncode!=0 and a.exists() and len(archives(root))==1
  t.chmod(0o600)
  # Symlinked manifest in the archive directory fails closed.
  m.unlink();(m.parent/m.name).symlink_to('/dev/null')
  r=run(root,key);assert r.returncode!=0 and a.exists() and len(archives(root))==1
  (m.parent/m.name).unlink()
  # Symlink inside the campaign tree fails closed.
  (root/'.factory-state'/'campaigns'/'campaign-a'/'evil').symlink_to('/etc/passwd')
  r=run(root,key);assert r.returncode!=0 and a.exists()
 finally:key.unlink(missing_ok=True)
# Production readiness that binds an aggregate cannot be archived when that
# runner-evidence member is absent.
with tempfile.TemporaryDirectory() as td:
 root=Path(td);key=setup(root)
 try:
  a=make_campaign(root)
  f=a/'factory-loop.json';f.write_text(json.dumps({'outcome':'failed','campaign_id':'campaign-a','readiness':{'required':True,'aggregate_sha256':'a'*64}}));f.chmod(0o600)
  missing=run(root,key);assert missing.returncode!=0 and 'required runner evidence is missing' in missing.stderr and a.exists()
 finally:key.unlink(missing_ok=True)
print('test: authenticated campaign-scoped archive/prune safety passed')
