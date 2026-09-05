#!/usr/bin/env python3
"""Rootless source-manifest/bootstrap regression fixtures; never touches host paths."""
import json,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
GEN=ROOT/'scripts/generate-runner-install-manifest.py'
required=(
 'scripts/install-factory-runner-v2.sh','scripts/factory-runner-root-bootstrap',
 'scripts/factory-runner-broker.py','scripts/factory-runner-signer.py',
 'scripts/factory-runner-server.py','scripts/factory_runner_policy.py',
 'scripts/factory_runner_artifacts.py','scripts/factory_runner_authority.py',
 'scripts/build-runner-probe-authority.py','deploy/factory-runner-authority-v1/authority.json')
def run(*a,check=True):return subprocess.run(a,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,check=check)
with tempfile.TemporaryDirectory() as td:
 r=pathlib.Path(td)/'repo';r.mkdir();run('git','-C',str(r),'init','-q')
 for rel in required:
  p=r/rel;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((ROOT/rel).read_bytes())
  p.chmod((ROOT/rel).stat().st_mode & 0o777)
 run('git','-C',str(r),'add','.')
 run('git','-C',str(r),'-c','user.name=fixture','-c','user.email=fixture.invalid','commit','-qm','fixture')
 out=pathlib.Path(td)/'manifest.json';run(str(GEN),'--source',str(r),'--output',str(out))
 doc=json.loads(out.read_bytes());assert set(doc['files'])==set(required)
 assert doc['files']['scripts/install-factory-runner-v2.sh']['mode']==0o755
 # An untracked authority candidate must fail before output creation.
 (r/'scripts/attack.py').write_text('raise SystemExit("root import attack")\n')
 bad=pathlib.Path(td)/'bad.json';p=run(str(GEN),'--source',str(r),'--output',str(bad),check=False)
 assert p.returncode and not bad.exists() and 'untracked' in p.stderr
 (r/'scripts/attack.py').unlink()
 # Git tree symlinks and submodules are explicitly outside the install policy.
 (r/'link').symlink_to('/tmp/attacker');run('git','-C',str(r),'add','link')
 run('git','-C',str(r),'-c','user.name=fixture','-c','user.email=fixture.invalid','commit','-qm','link')
 p=run(str(GEN),'--source',str(r),'--output',str(bad),check=False)
 assert p.returncode and 'symlink' in p.stderr
print('test: signed source manifest exact-tree/root-import fixtures passed')
