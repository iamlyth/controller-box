#!/usr/bin/env python3
import fcntl,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];TOOL=ROOT/'scripts/archive-factory-campaign.py'
def run(root,*extra):return subprocess.run([str(TOOL),'--root',str(root),'--campaign-id','campaign-a',*extra],capture_output=True,text=True)
with tempfile.TemporaryDirectory() as td:
 root=Path(td);(root/'.git').mkdir();state=root/'.factory-state';state.mkdir(mode=0o700);campaigns=state/'campaigns';campaigns.mkdir(mode=0o700)
 a=campaigns/'campaign-a';a.mkdir(mode=0o700);f=a/'factory-loop.json';f.write_text('{"outcome":"failed"}\n');f.chmod(0o600)
 other=campaigns/'campaign-b';other.mkdir(mode=0o700);of=other/'factory-loop.json';of.write_text('other\n');of.chmod(0o600)
 ralph=root/'.ralph';ralph.mkdir();sentinel=ralph/'keep';sentinel.write_text('keep')
 r=run(root);assert r.returncode==0,r.stderr;assert not a.exists();assert of.read_text()=='other\n' and sentinel.read_text()=='keep'
 manifests=list((state/'campaign-archives').glob('campaign-a-*.manifest.json'));assert len(manifests)==1
 m=json.loads(manifests[0].read_text());assert m['schema']=='factory-campaign-archive/v1' and m['members'][0]['path']=='factory-loop.json'
 archive=manifests[0].with_name(manifests[0].name.replace('.manifest.json','.tar'))
 import hashlib
 assert m['archive_sha256']==hashlib.sha256(archive.read_bytes()).hexdigest()
 # Unsafe mode is rejected without touching the campaign.
 a.mkdir(mode=0o700);bad=a/'factory-loop.json';bad.write_text('x');bad.chmod(0o644)
 r=run(root);assert r.returncode!=0 and a.exists();bad.chmod(0o600)
 # An unlocked but resumable campaign is still not terminal and cannot be
 # archived or pruned.
 bad.write_text('{"current_phase":"implementation","campaign_id":"campaign-a"}\n');bad.chmod(0o600)
 r=run(root);assert r.returncode!=0 and 'active/resumable' in r.stderr and a.exists()
 bad.write_text('{"current_phase":"failed","campaign_id":"campaign-a"}\n')
 # The repository-directory writer lock makes active campaigns unarchivable.
 fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY);fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
 try:r=run(root);assert r.returncode!=0 and 'active campaign lock' in r.stderr
 finally:os.close(fd)
print('test: campaign-scoped archive/prune safety passed')
