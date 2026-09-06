#!/usr/bin/env python3
"""Exhaustive launch/campaign residual-set regression fixtures."""
import importlib.util,os,pathlib,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
LOOP=ROOT/".factory/loop"
import sys;sys.path.insert(0,str(LOOP))
import launch as l
OUTCOMES=("success","findings","blocked","failed","infrastructure_failure","interrupted","timeout","crash","recovery")
with tempfile.TemporaryDirectory() as td:
 root=pathlib.Path(td);external=root/"external-evidence";external.mkdir();sentinel=external/"receipt";sentinel.write_text("signed")
 canonical=root/"campaign";canonical.mkdir();
 for name in ("factory-loop.json","factory-state-digests.jsonl","campaign-result.json"):(canonical/name).write_text(name)
 for outcome in OUTCOMES:
  owned=[]
  for prefix in ("factory-home-","factory-loop-session-","factory-exec-"):
   p=pathlib.Path(tempfile.mkdtemp(prefix=prefix+outcome+"-",dir=root));owned.append(p)
   (p/"child.lock").write_text("lock");(p/".factory-loop.json.tmp.1").write_text("atomic temp")
   nested=p/"launch";nested.mkdir();(nested/"child.pid").write_text("999999")
  # A hostile symlink in an owned tree must not permit no-follow cleanup to
  # remove external evidence.
  os.symlink(external,owned[0]/"external-link")
  l._remove_private_directories(owned)
  assert not any(p.exists() or p.is_symlink() for p in owned),outcome
  assert sentinel.read_text()=="signed",outcome
  assert sorted(p.name for p in canonical.iterdir())==["campaign-result.json","factory-loop.json","factory-state-digests.jsonl"],outcome
print("test: exhaustive campaign residual sets passed")
