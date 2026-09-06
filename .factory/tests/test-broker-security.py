#!/usr/bin/env python3
"""Fixture-only adversarial broker/deployment regression checks."""
import importlib.util,io,json,os,pathlib,stat,subprocess,sys,tarfile,tempfile,time
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('broker',ROOT/'.factory/runner/factory-runner-broker.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)

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

broker=(ROOT/'.factory/runner/factory-runner-broker.py').read_text()
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
assert broker.rindex('collect(artifacts') < broker.rindex('analyze(entry,authority') < broker.rindex('signed=sign(')
# Slow trickles do not refresh the monotonic deadline, and output is streamed
# into held bounded files rather than accumulated by subprocess.run.
r,w=os.pipe()
try:
 started=time.monotonic()
 try:b._read_deadline(r,4,started+.03,exact=True)
 except b.BrokerError:pass
 else:raise AssertionError('slow request held a broker past its deadline')
 assert time.monotonic()-started<.5
finally:os.close(r);os.close(w)
with tempfile.TemporaryDirectory(dir=ROOT) as td:
 out=pathlib.Path(td)/'out';err=pathlib.Path(td)/'err'
 try:b._bounded_process([sys.executable,'-c',f'import os;os.write(1,b"x"*{b.MAX_LOG+1})'],{},5,out,err)
 except b.BrokerError:pass
 else:raise AssertionError('huge candidate output accepted')
 assert out.stat().st_size==b.MAX_LOG
for marker in ('HEADER_TIMEOUT=15','ARCHIVE_TIMEOUT=120','BROKER_ADMISSION=8','fcntl.flock',
 'aggregate contained output exceeds bound','RLIMIT_AS','RLIMIT_CPU','/var/run',
 '/usr/local/libexec/inputplumber-mediator','--mutations','root-dbus-preforward.jsonl','DBUS_SYSTEM_BUS_ADDRESS',
 'nr_inodes={WRITABLE_INODES}','bounded writable backing resource cleanup not proven',
 'runner primary/supplementary groups differ from exact approved set'):
 assert marker in broker,marker
assert 'capture_output=True,timeout=7300' not in broker
assert 'org.shadowblip.InputPlumber.Target.InputEvent' in broker
assert '--talk=' not in broker
for method in ('CreateTargetDevice','StopTargetDevice','SetTargetDevices','SetInterceptActivation','Properties.Set'):
 assert any(entry.endswith(method) for entry in b.DBUS_MUTATING_CALLS)
assert all('InputEvent' not in method for method in b.DBUS_MUTATING_CALLS)
assert 'cannot snapshot InputPlumber objects for cleanup authority' in broker
for marker in ('class DbusMonitor','GetConnectionUnixProcessID','root InputPlumber bus monitor failed before proxy launch','validate_decisions',
 'target_consumer_operation','consumer-only','candidate_callable":False','physical_capability":False','routing_capability":False'):
 assert marker in broker,marker
assert broker.index('dbus_monitor=DbusMonitor') < broker.index('proxy=DbusProxy')
# A descendant retaining stdout cannot retain the broker beyond one absolute deadline.
with tempfile.TemporaryDirectory(dir=ROOT) as td:
 out=pathlib.Path(td)/'pipe-out';err=pathlib.Path(td)/'pipe-err';started=time.monotonic()
 rejected(lambda:b._bounded_process([sys.executable,'-c','import os,time; p=os.fork(); (time.sleep(30) if p==0 else None)'],{},.25,out,err))
 assert time.monotonic()-started<1.5
assert 'zlib.decompress(' not in (ROOT/'.factory/runner/validate-runner-artifacts-semantic.py').read_text()

client=(ROOT/'.factory/runner/run-factory-runners.py').read_text()
for marker in ('factory-ssh-launcher/v1','/proc/self/fd/','os.O_NOFOLLOW','pass_fds=(launcher.fd,)',
 'tree_identities(evidence_dir)','renameat2(sfd','runner aggregate publication collision'):
 assert marker in client,marker
assert 'Path.home() / ".ssh/factory-ssh"' not in client

signer=(ROOT/'.factory/runner/factory-runner-signer.py').read_text()
assert 'FACTORY_BROKER_SIGNING' not in signer
assert '--broker-fd' in signer and 'broker_auth_sha256' in signer
policy=(ROOT/'.factory/runner/factory_runner_policy.py').read_text()
assert 'probe_authority_status' in policy and 'pending or unapproved' in policy
routing=(ROOT/'deploy/factory-runner-authority-v1/probe-controller-production-routing.sh').read_text()
assert 'FACTORY_INPUTPLUMBER_PROVENANCE' in routing
assert 'readlink -f "/proc/$pid/exe"' not in routing
assert 'factory-host-inputplumber-provenance/v2' in routing
assert 'unix:path=/run/factory/dbus/system_bus_socket' in routing
input_probe=(ROOT/'deploy/factory-runner-authority-v1/probe-inputplumber-system-dbus.sh').read_text()
assert 'systemctl show' not in input_probe and 'dpkg-query -W' not in input_probe
assert 'exact broker D-Bus proxy is required' in input_probe
gpu=(ROOT/'deploy/factory-runner-authority-v1/probe-gpu-compositor.sh').read_text()
assert 'device-type-om.json' in gpu and 'egl-renderer-output.txt' in gpu
assert 'first-run skipped' not in gpu
installer=(ROOT/'.factory/runner/install-factory-runner-v2.sh').read_text()
assert "{'schema','path','sha256','device','inode'}" in installer
for marker in ('trap rollback_signal EXIT INT TERM HUP','commit_object_b64','mutable source race',"pwd.getpwuid(c['uid'])",'RENAME_NOREPLACE','factory-runner-v2.bundle','visudo -cf','probe_authority_status','before-backup','old-durable'):
 assert marker in installer,marker
assert 'runner-policy-enrollment.json' not in installer
print('test: broker persistence/substitution/oracle/containment/cleanup/installer fixtures passed')
