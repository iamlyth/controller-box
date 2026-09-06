#!/usr/bin/env python3
import fcntl,hashlib,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];TOOL=ROOT/'scripts/archive-factory-campaign.py'
def run(root,key,*extra):
 fd=os.open(key,os.O_RDONLY)
 try:
  env={**os.environ,'FACTORY_COORDINATOR_AUTH_FD':str(fd)}
  return subprocess.run([str(TOOL),'--root',str(root),'--campaign-id','campaign-a',*extra],capture_output=True,text=True,env=env,pass_fds=(fd,))
 finally:os.close(fd)
with tempfile.TemporaryDirectory() as td:
 root=Path(td);(root/'.git').mkdir();state=root/'.factory-state';state.mkdir(mode=0o700);campaigns=state/'campaigns';campaigns.mkdir(mode=0o700)
 key=root.parent/(root.name+'-archive-key');key.write_bytes(os.urandom(32));key.chmod(0o600)
 try:
  a=campaigns/'campaign-a';a.mkdir(mode=0o700);f=a/'factory-loop.json';f.write_text('{"outcome":"failed","campaign_id":"campaign-a"}\n');f.chmod(0o600)
  receipts=state/'audit-receipts';receipts.mkdir(mode=0o700);receipt=receipts/'campaign-a-final.json';receipt.write_text('{"campaign_id":"campaign-a","exit_code":0}\n');receipt.chmod(0o600)
  other=campaigns/'campaign-b';other.mkdir(mode=0o700);of=other/'factory-loop.json';of.write_text('other\n');of.chmod(0o600)
  ralph=root/'.ralph';ralph.mkdir();sentinel=ralph/'keep';sentinel.write_text('keep')
  r=run(root,key);assert r.returncode==0,r.stderr;assert not a.exists();assert of.read_text()=='other\n' and sentinel.read_text()=='keep'
  manifests=list((state/'campaign-archives').glob('campaign-a-*.manifest.json'));assert len(manifests)==1
  m=json.loads(manifests[0].read_text());assert m['schema']=='factory-campaign-archive/v2' and m['authentication']['namespace']=='factory-campaign-archive'
  assert m['namespaces']['campaigns'][0]['path']=='campaigns/campaign-a/factory-loop.json'
  assert m['namespaces']['audit-receipts'][0]['path']=='audit-receipts/campaign-a-final.json'
  archive=manifests[0].with_name(manifests[0].name.replace('.manifest.json','.tar'));assert m['archive_sha256']==hashlib.sha256(archive.read_bytes()).hexdigest()
  # Tampered archive and missing evidence fail authentication before deletion.
  a.mkdir(mode=0o700);bad=a/'factory-loop.json';bad.write_text('{"outcome":"failed","campaign_id":"campaign-a"}\n');bad.chmod(0o600)
  archive.write_bytes(archive.read_bytes()+b'x')
  rr=run(root,key);assert rr.returncode!=0 and a.exists()
  archive.write_bytes(archive.read_bytes()[:-1]);receipt.unlink()
  # Unsafe mode is rejected without touching the campaign.
  bad.chmod(0o644);r=run(root,key);assert r.returncode!=0 and a.exists();bad.chmod(0o600)
  bad.write_text('{"current_phase":"implementation","campaign_id":"campaign-a"}\n');r=run(root,key);assert r.returncode!=0 and 'active/resumable' in r.stderr and a.exists()
  bad.write_text('{"current_phase":"failed","campaign_id":"campaign-a"}\n')
  fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY);fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
  try:r=run(root,key);assert r.returncode!=0 and 'active campaign lock' in r.stderr
  finally:os.close(fd)
 finally:key.unlink(missing_ok=True)
# Production readiness that binds an aggregate cannot be archived when that
# runner-evidence member is absent.
with tempfile.TemporaryDirectory() as td:
 root=Path(td);state=root/'.factory-state';campaigns=state/'campaigns';a=campaigns/'campaign-a'
 a.mkdir(mode=0o700,parents=True);state.chmod(0o700);campaigns.chmod(0o700)
 f=a/'factory-loop.json';f.write_text(json.dumps({'outcome':'failed','campaign_id':'campaign-a','readiness':{'required':True,'aggregate_sha256':'a'*64}}));f.chmod(0o600)
 key=root.parent/(root.name+'-archive-key');key.write_bytes(os.urandom(32));key.chmod(0o600)
 try:
  missing=run(root,key);assert missing.returncode!=0 and 'required runner evidence is missing' in missing.stderr and a.exists()
 finally:key.unlink(missing_ok=True)
print('test: authenticated campaign-scoped archive/prune safety passed')
