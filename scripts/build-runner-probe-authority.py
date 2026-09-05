#!/usr/bin/env python3
"""Build the exact deployable root probe-authority manifest.

Run by the release operator before root installation.  The output directory is
then copied read-only to /opt/factory-runner/authority/v1 and its printed
manifest digest enrolled in root runner policy.  It is not candidate authority
until that out-of-band enrollment occurs.
"""
import argparse,hashlib,json,pathlib,subprocess
ap=argparse.ArgumentParser()
ap.add_argument('--source',required=True,help='trusted clean Git snapshot root')
ap.add_argument('--git',required=True,help='absolute trusted Git executable')
ap.add_argument('--expected-commit',required=True)
ap.add_argument('--expected-tree',required=True)
ap.add_argument('--output',required=True,help='authority directory inside the trusted snapshot or an exact copy')
a=ap.parse_args(); source=pathlib.Path(a.source).resolve();root=pathlib.Path(a.output).resolve();git_exe=pathlib.Path(a.git)
if not git_exe.is_absolute() or not git_exe.is_file():raise SystemExit('authority builder: --git must be an absolute executable')
def git(*args):return subprocess.check_output([str(git_exe),'-C',str(source),*args],text=True).strip()
if git('rev-parse','HEAD')!=a.expected_commit or git('rev-parse','HEAD^{tree}')!=a.expected_tree:
 raise SystemExit('authority builder: explicit commit/tree does not match source snapshot')
if subprocess.run([str(git_exe),'-C',str(source),'diff','--quiet','--ignore-submodules','HEAD','--']).returncode or subprocess.run([str(git_exe),'-C',str(source),'diff','--cached','--quiet','--ignore-submodules','HEAD','--']).returncode:
 raise SystemExit('authority builder: source snapshot is dirty')
expected=(source/'deploy/factory-runner-authority-v1').resolve()
if root!=expected:raise SystemExit('authority builder: output must be the authority directory in the exact trusted snapshot')
files={p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob('*')) if p.is_file() and p.name!='authority.json' and '__pycache__' not in p.parts and p.suffix!='.pyc'}
empty={"required":[],"files":{}}
def desc(argv,artifacts=empty,semantic=False,cap='',runner=''):
 analyzer=["/usr/bin/python3","@/validate-retained-artifacts.py","--capability",cap,"--artifacts","{artifacts}","--commit","{commit}","--tree","{tree}"] if semantic else ["/usr/bin/python3","@/validate-no-artifacts.py","--capability",cap,"--artifacts","{artifacts}","--commit","{commit}","--tree","{tree}"]
 core={"argv":argv,"artifacts":artifacts,"analyzer_argv":analyzer}
 digest=hashlib.sha256(json.dumps(core,sort_keys=True,separators=(',',':')).encode()).hexdigest()
 return {**core,"probe_id":f"factory-root-probe:{runner}:{cap}:v1","descriptor_sha256":digest}
# Gates are class-specific because probe identity is part of authority.
def gate_for(runner): return desc(["/usr/bin/python3","@/product-gate.py"],cap='gate',runner=runner)
routing_required=["routing-results.json","observer.log","overlay.log","udev-targets.log","cleanup.log","assignment-after-clear.yaml",
 "om-before.json","om-after-create.json","om-after-clear.json","om-cleanup.json",
 "dbus-unique-owner.json","dbus-owner-pid.json","provenance-before-routing.json","provenance-after-routing.json",
 "dev-input-before.txt","dev-input-after-create.txt","dev-input-after-cleanup.txt","sysfs-targets.txt","physical-source-sysfs.txt"]+[f"assignment-slot-{i}.yaml" for i in range(4)]+[f"om-assignment-{i}.json" for i in range(4)]
routing_art={"required":routing_required,"files":{n:("application/json" if n.endswith(".json") else "application/yaml" if n.endswith(".yaml") else "text/plain") for n in routing_required}}
gpu_names=['artifact-manifest.json','renderer-verdict.json','verdict.json','installed-manifest.json','installed-authority.json','device-type-evidence.json','device-type-om.json','egl-renderer-output.txt','profile-selection-evidence.json','selected-profile.yaml','probe.log','manager.log','configure.log','build.log','install.log','weston.log','egl-build.log','screenshooter.log','installed-xbox-360.svg','installed-license.controllercons','installed-controller-icons.yaml','installed-layout.json','installed-oracle.json','controller-box-unhighlighted.png']+[f'capture-{x}.png' for x in ('a','b','x','y','up','down','left','right','start','select','guide','l1','r1','l2','r2','l3','r3')]
def gpu_media(name):
 if name.endswith('.png'):return 'image/png'
 if name.endswith('.svg'):return 'image/svg+xml'
 if name.endswith('.log') or name.endswith('.controllercons'):return 'text/plain'
 if name.endswith('.yaml'):return 'application/yaml'
 return 'application/json'
gpu_art={"required":gpu_names,"files":{n:gpu_media(n) for n in gpu_names}}
dev_art={"required":["authority-result.json"],"files":{"authority-result.json":"application/json"}}
def dev_desc(cap):
 core={"argv":["/usr/bin/python3","@/probe-dev-capability.py",cap],"artifacts":dev_art,"analyzer_argv":["/usr/bin/python3","@/validate-capability-semantics.py","--capability",cap,"--artifacts","{artifacts}","--commit","{commit}","--tree","{tree}"]}
 digest=hashlib.sha256(json.dumps(core,sort_keys=True,separators=(',',':')).encode()).hexdigest()
 return {**core,"probe_id":f"factory-root-probe:dev-runner-vm:{cap}:v1","descriptor_sha256":digest}
classes={
 'dev-runner-vm':{"gate":gate_for('dev-runner-vm'),"capabilities":{c:dev_desc(c) for c in ('remote-project-gate','systemd-user','kernel-uinput','installed-package')}},
 'iprunner':{"gate":gate_for('iprunner'),"capabilities":{
  'inputplumber-system-dbus':desc(['/usr/bin/bash','@/probe-inputplumber-system-dbus.sh'],cap='inputplumber-system-dbus',runner='iprunner'),
  'physical-controller':desc(['/usr/bin/bash','@/probe-physical-controller.sh'],cap='physical-controller',runner='iprunner'),
  'target-consumer':desc(['/usr/bin/bash','@/probe-target-consumer.sh'],cap='target-consumer',runner='iprunner'),
  'controller-production-routing':desc(['/usr/bin/bash','@/probe-controller-production-routing.sh'],routing_art,True,'controller-production-routing','iprunner')}},
 'gpurunner':{"gate":gate_for('gpurunner'),"capabilities":{
  'gpu-compositor':desc(['/usr/bin/bash','@/probe-gpu-compositor.sh'],gpu_art,True,'gpu-compositor','gpurunner'),
  'installed-licensed-diagram':desc(['/usr/bin/bash','@/probe-gpu-compositor.sh'],gpu_art,True,'installed-licensed-diagram','gpurunner')}}}
doc={"schema":"factory-probe-authority/v1","version":1,"files":files,"classes":classes,"trusted_path":["/usr/bin"],"licensed_oracle":{"path":"licensed-diagram-oracle.json","sha256":files['licensed-diagram-oracle.json']}}
raw=(json.dumps(doc,sort_keys=True,indent=2)+'\n').encode();(root/'authority.json').write_bytes(raw)
print(hashlib.sha256(raw).hexdigest())
