#!/usr/bin/env python3
"""Fixture-only adversarial broker/deployment regression checks."""
import importlib.util,io,json,os,pathlib,stat,tarfile,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('broker',ROOT/'scripts/factory-runner-broker.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)

def rejected(fn):
 try: fn()
 except SystemExit as e:
  assert e.code==1;return
 except Exception:return
 raise AssertionError('hostile fixture accepted')

# Archive extraction cannot install links, setuid modes, file capabilities, or
# mutable source bytes for a later known-good/malicious tree swap.
with tempfile.TemporaryDirectory(dir=ROOT) as td:
 root=pathlib.Path(td);source=root/'source';source.mkdir()
 stream=io.BytesIO()
 with tarfile.open(fileobj=stream,mode='w') as t:
  info=tarfile.TarInfo('tool');data=b'#!/bin/sh\n';info.size=len(data);info.mode=0o6755;t.addfile(info,io.BytesIO(data))
 b.extract(stream.getvalue(),source)
 original_chown=b.os.chown;b.os.chown=lambda *args:None
 try:b.freeze_tree(source)
 finally:b.os.chown=original_chown
 mode=stat.S_IMODE((source/'tool').stat().st_mode)
 assert mode==0o555 and not mode&0o6000
 hostile=io.BytesIO()
 with tarfile.open(fileobj=hostile,mode='w') as t:
  info=tarfile.TarInfo('home-link');info.type=tarfile.SYMTYPE;info.linkname='/root/.ssh/authorized_keys';t.addfile(info)
 rejected(lambda:b.extract(hostile.getvalue(),root/'other'))

broker=(ROOT/'scripts/factory-runner-broker.py').read_text()
# Candidate cannot see SSH/home/request siblings or credentials; all writable
# mounts are explicit and request source is read-only. Network and devices are
# denied by default, and process/session escape remains in the killed cgroup.
for marker in ('ProtectHome=yes','ProtectSystem=strict','BindReadOnlyPaths=',
 'PrivateTmp=yes','PrivateMounts=yes','PrivatePIDs=yes','PrivateIPC=yes',
 'CapabilityBoundingSet=','AmbientCapabilities=','NoNewPrivileges=yes',
 'RestrictSUIDSGID=yes','RestrictNamespaces=yes','IPAddressDeny=any',
 'RestrictAddressFamilies=AF_UNIX','DevicePolicy=closed','TasksMax=512',
 'MemoryMax=4G','CPUQuota=400%','RuntimeMaxSec=7200','LimitFSIZE=',
 'stop","kill','MainPID','cgroup.procs','populated 0'):
 assert marker in broker,marker
assert 'os.chown(request_dir,uid' not in broker and 'os.chown(product,uid' not in broker
assert broker.rindex('collect(artifacts') < broker.rindex('analyze(authority') < broker.rindex('signed=sign(')

signer=(ROOT/'scripts/factory-runner-signer.py').read_text()
assert 'FACTORY_BROKER_SIGNING' not in signer
assert '--broker-fd' in signer and 'broker_auth_sha256' in signer
policy=(ROOT/'scripts/factory_runner_policy.py').read_text()
assert 'probe_authority_status' in policy and 'pending or unapproved' in policy
routing=(ROOT/'deploy/factory-runner-authority-v1/probe-controller-production-routing.sh').read_text()
assert 'FACTORY_INPUTPLUMBER_PROVENANCE' in routing and 'root-broker-outside-private-pids' in routing
assert 'readlink -f "/proc/$pid/exe"' not in routing
installer=(ROOT/'scripts/install-factory-runner-v2.sh').read_text()
for marker in ('trap rollback EXIT INT TERM HUP','git','show',"pwd.getpwuid(c['uid'])",'mv -T','factory-runner-v2.bundle','visudo -cf','probe_authority_status'):
 assert marker in installer,marker
assert 'runner-policy-enrollment.json' not in installer
print('test: broker persistence/substitution/oracle/containment/cleanup/installer fixtures passed')
