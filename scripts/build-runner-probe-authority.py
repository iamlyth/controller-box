#!/usr/bin/env python3
"""Build the exact deployable root probe-authority manifest.

Run by the release operator before root installation.  The output directory is
then copied read-only to /opt/factory-runner/authority/v1 and its printed
manifest digest enrolled in root runner policy.  It is not candidate authority
until that out-of-band enrollment occurs.
"""
import hashlib,json,pathlib
root=pathlib.Path(__file__).resolve().parents[1]/'deploy/factory-runner-authority-v1'
files={p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob('*')) if p.is_file() and p.name!='authority.json' and '__pycache__' not in p.parts and p.suffix!='.pyc'}
empty={"required":[],"files":{}}
def desc(argv,artifacts=empty,semantic=False,cap=''):
 analyzer=["/usr/bin/python3","@/validate-retained-artifacts.py","--capability",cap,"--artifacts","{artifacts}","--commit","{commit}","--tree","{tree}"] if semantic else ["/usr/bin/python3","@/validate-no-artifacts.py","--capability",cap,"--artifacts","{artifacts}","--commit","{commit}","--tree","{tree}"]
 return {"argv":argv,"artifacts":artifacts,"analyzer_argv":analyzer}
gate=desc(["/usr/bin/python3","@/product-gate.py"],cap='gate')
routing_required=["routing-results.json","observer.log","overlay.log","cleanup.log"]+[f"assignment-slot-{i}.yaml" for i in range(4)]
routing_art={"required":routing_required,"files":{n:("application/json" if n.endswith(".json") else "application/yaml" if n.endswith(".yaml") else "text/plain") for n in routing_required}}
gpu_names=['artifact-manifest.json','renderer-verdict.json','verdict.json','installed-manifest.json','probe.log','controller-box-unhighlighted.png']+[f'capture-{x}.png' for x in ('a','b','x','y','up','down','left','right','start','select','guide','l1','r1','l2','r2','l3','r3')]
gpu_art={"required":gpu_names,"files":{n:('image/png' if n.endswith('.png') else 'text/plain' if n.endswith('.log') else 'application/json') for n in gpu_names}}
classes={
 'dev-runner-vm':{"gate":gate,"capabilities":{c:desc(["/usr/bin/python3","@/product-gate.py"],cap=c) for c in ('remote-project-gate','systemd-user','kernel-uinput','installed-package')}},
 'iprunner':{"gate":gate,"capabilities":{
  'inputplumber-system-dbus':desc(['/bin/bash','@/probe-inputplumber-system-dbus.sh'],cap='inputplumber-system-dbus'),
  'physical-controller':desc(['/bin/bash','@/probe-physical-controller.sh'],cap='physical-controller'),
  'target-consumer':desc(['/bin/bash','@/probe-target-consumer.sh'],cap='target-consumer'),
  'controller-production-routing':desc(['/bin/bash','@/probe-controller-production-routing.sh'],routing_art,True,'controller-production-routing')}},
 'gpurunner':{"gate":gate,"capabilities":{
  'gpu-compositor':desc(['/bin/bash','@/probe-gpu-compositor.sh'],gpu_art,True,'gpu-compositor'),
  'installed-licensed-diagram':desc(['/bin/bash','@/probe-gpu-compositor.sh'],gpu_art,True,'installed-licensed-diagram')}}}
doc={"schema":"factory-probe-authority/v1","version":1,"files":files,"classes":classes,"trusted_path":["/usr/bin"],"licensed_oracle":{"path":"licensed-diagram-oracle.json","sha256":files['licensed-diagram-oracle.json']}}
raw=(json.dumps(doc,sort_keys=True,indent=2)+'\n').encode();(root/'authority.json').write_bytes(raw)
print(hashlib.sha256(raw).hexdigest())
