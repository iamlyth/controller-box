#!/usr/bin/env python3
"""Adversarial coordinator launcher and publication boundary checks."""
import hashlib,importlib.util,json,os,pathlib,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("runner_client",ROOT/"scripts/run-factory-runners.py")
client=importlib.util.module_from_spec(spec);spec.loader.exec_module(client)

def reject(fn):
 try:fn()
 except SystemExit:return
 raise AssertionError("hostile coordinator fixture accepted")

with tempfile.TemporaryDirectory(dir=ROOT) as td:
 root=pathlib.Path(td);launcher=root/"launcher";launcher.write_bytes(b"#!/bin/sh\nexit 0\n");launcher.chmod(0o700)
 info=launcher.stat();manifest=root/"manifest.json"
 def enroll(path=launcher):
  i=path.stat();manifest.write_text(json.dumps({"schema":"factory-ssh-launcher/v1","path":str(path),
   "sha256":hashlib.sha256(path.read_bytes()).hexdigest(),"device":i.st_dev,"inode":i.st_ino}))
  manifest.chmod(0o600)
 os.environ["FACTORY_RUNNER_FIXTURE_MODE"]="1";os.environ["FACTORY_SSH_LAUNCHER_MANIFEST"]=str(manifest)
 enroll()
 with client.HeldLauncher() as held:
  assert held.executable==f"/proc/self/fd/{held.fd}"
 launcher.write_bytes(b"#!/bin/sh\nexit 1\n")
 reject(client.HeldLauncher)
 launcher.unlink();target=root/"target";target.write_bytes(b"#!/bin/sh\nexit 0\n");target.chmod(0o700);launcher.symlink_to(target);enroll(target)
 doc=json.loads(manifest.read_text());doc["path"]=str(launcher);manifest.write_text(json.dumps(doc))
 reject(client.HeldLauncher)

 # Destination and aggregate collisions are atomic no-replace.
 source=root/"source";destination=root/"destination";source.write_bytes(b"new");destination.write_bytes(b"old")
 reject(lambda:client.rename_noreplace(source,destination))
 assert source.read_bytes()==b"new" and destination.read_bytes()==b"old"
 aggregate=ROOT/".factory-state"/f".runner-client-test-{os.getpid()}.json"
 try:
  client.atomic_write(aggregate,b"one")
  reject(lambda:client.atomic_write(aggregate,b"two"));assert aggregate.read_bytes()==b"one"
 finally:aggregate.unlink(missing_ok=True)

 # A same-UID replacement after descriptors are held changes identity and is
 # rejected before publication; held descriptors still name the original.
 staging=root/"staging";staging.mkdir();item=staging/"manifest.json";item.write_bytes(b"good")
 identities,fds=client.hold_tree(staging)
 try:
  item.unlink();item.write_bytes(b"evil")
  assert client.tree_identities(staging)!=identities
 finally:
  for fd in fds:os.close(fd)
print("test: runner launcher/publication race checks passed")
