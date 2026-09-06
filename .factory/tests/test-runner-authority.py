#!/usr/bin/env python3
"""Adversarial tests for the v2 runner trust boundary."""
import hashlib,json,os,pathlib,shutil,tempfile,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts'));import factory_runner_authority as auth
def reject(fn):
 try: fn()
 except (auth.AuthorityError,OSError): return
 raise AssertionError('hostile authority accepted')
source=ROOT/'deploy/factory-runner-authority-v1'
with tempfile.TemporaryDirectory(dir=ROOT) as td:
 bundle=pathlib.Path(td)/'authority';shutil.copytree(source,bundle)
 digest=hashlib.sha256((bundle/'authority.json').read_bytes()).hexdigest()
 loaded=auth.load_authority(bundle,digest,fixture=True); assert loaded.class_contract('iprunner')['capabilities'];loaded.close()
 # Candidate replacement of a probe, analyzer, contract manifest, or oracle
 # cannot preserve the root policy digest.
 for rel in ('probe-controller-production-routing.sh','validate-retained-artifacts.py','licensed-diagram-oracle.json'):
  original=(bundle/rel).read_bytes();(bundle/rel).write_bytes(original+b'\n# candidate replacement\n')
  reject(lambda:auth.load_authority(bundle,digest,fixture=True));(bundle/rel).write_bytes(original)
 # Manifest substitution is rejected by the enrolled outer digest.
 d=json.loads((bundle/'authority.json').read_text());d['classes']['iprunner']['capabilities'].pop('controller-production-routing');(bundle/'authority.json').write_text(json.dumps(d))
 reject(lambda:auth.load_authority(bundle,digest,fixture=True))
# ForcedCommand has no parser/signer and names only the fixed broker sudo path.
server=(ROOT/'.factory/runner/factory-runner-server.py').read_text()
assert 'factory-runner-v2' in server and 'factory-runner-signer' not in server and '[SUDO, "-n", BROKER]' in server
signer=(ROOT/'.factory/runner/factory-runner-signer.py').read_text();assert 'direct signing is forbidden' in signer
broker=(ROOT/'.factory/runner/factory-runner-broker.py').read_text()
for required in ('PrivatePIDs=yes','PrivateMounts=yes','NoNewPrivileges=yes','CapabilityBoundingSet=',
 'ProtectSystem=strict','ProtectHome=yes','DevicePolicy=closed','IPAddressDeny=any','SystemCallFilter=',
 'TasksMax=512','MemoryMax=4G','KillMode=control-group','cgroup.procs',
 'nonce was not issued or was already consumed','NONCE_OUTSTANDING','os.rename(',
 'exact_tree(product','freeze_tree(product)','expected_uid=uid','hold(d,p','InputPlumberProvenance','starttime','/proc/{self.pid}/exe'):
 assert required in broker,required
assert 'os.chown(product,uid' not in broker and 'FACTORY_BROKER_SIGNING' not in broker
assert 'licensed authority/oracle status is pending or unapproved' in broker
# Every capability has a reachable immutable authority executable and an exact
# delimiter; @/ paths resolve beneath the one read-only authority mount.
doc=json.loads((source/'authority.json').read_text()); caps=[]
for runner,klass in doc['classes'].items():
 for cap,contract in klass['capabilities'].items():
  caps.append(cap)
  for argv_name in ('argv','analyzer_argv'):
   argv=contract[argv_name]
   for token in argv:
    if token.startswith('@/'): assert (source/token[2:]).is_file(),(cap,token)
  probe=(source/contract['argv'][1][2:]).read_text()
  assert (f'--- {cap} capability contract ---' in probe
          or 'probe-dev-capability.py' in contract['argv'][1]
          or ('FACTORY_CAPABILITY' in probe and cap in ('gpu-compositor','installed-licensed-diagram')))
assert len(caps)==10 and len(set(caps))==10
for cap,contract in doc['classes']['dev-runner-vm']['capabilities'].items():
 assert any('probe-dev-capability.py' in x for x in contract['argv']) and contract['argv'][-1]==cap
 assert contract['artifacts']['required']==['authority-result.json']
broker=(ROOT/'.factory/runner/factory-runner-broker.py').read_text()
assert 'authority_mount=f"BindReadOnlyPaths={authority.root}:{authority.root}"' in broker
assert 'blocked_paths=["/root","/home","/run","/var/run","/etc/ssh","/etc/sudoers","/etc/sudoers.d","/etc/factory-runner","/opt/factory-runner"' in broker
print('test: v2 root probe authority/broker adversarial checks passed')
