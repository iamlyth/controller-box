#!/usr/bin/env python3
"""Authenticated, campaign-scoped terminal archive and bounded retention."""
from __future__ import annotations
import argparse,fcntl,hashlib,hmac,io,json,os,re,stat,tarfile,tempfile,time
from pathlib import Path,PurePosixPath

NAME=re.compile(r"^[a-z0-9][a-z0-9._-]{0,127}$")
TEMP=re.compile(r"(?:^|/)(?:\.[^/]*\.tmp(?:\..*)?|factory-(?:home|loop-session)-.*)$")
MAX_FILES=4096;MAX_BYTES=256*1024*1024;MAX_MEMBER=32*1024*1024;RETENTION_DEFAULT=20
ARCHIVE_SCHEMA="factory-campaign-archive/v2";AUTH_NAMESPACE="factory-campaign-archive"

def die(s): raise SystemExit("archive-factory-campaign: "+s)
def fsync_dir(p):
 fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
 try:os.fsync(fd)
 finally:os.close(fd)
def held_bytes(path:Path,uid:int):
 i=os.lstat(path)
 if not stat.S_ISREG(i.st_mode) or stat.S_ISLNK(i.st_mode) or i.st_uid!=uid or i.st_nlink!=1 or i.st_mode&0o077:die(f"unsafe archive member: {path}")
 fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC);chunks=[];h=hashlib.sha256();size=0
 try:
  before=os.fstat(fd)
  while True:
   b=os.read(fd,65536)
   if not b:break
   size+=len(b)
   if size>MAX_MEMBER:die(f"oversized archive member: {path}")
   chunks.append(b);h.update(b)
  after=os.fstat(fd)
  if (before.st_dev,before.st_ino,before.st_size)!=(after.st_dev,after.st_ino,after.st_size):die(f"member raced: {path}")
 finally:os.close(fd)
 return b"".join(chunks),h.hexdigest(),stat.S_IMODE(i.st_mode)
def inspect(root:Path,uid:int,prefix:str):
 out=[]
 for current,dirs,files in os.walk(root,topdown=True,followlinks=False):
  dirs.sort();files.sort()
  for n in dirs:
   p=Path(current)/n;rel=p.relative_to(root).as_posix()
   i=os.lstat(p)
   if TEMP.search(rel) or stat.S_ISLNK(i.st_mode) or not stat.S_ISDIR(i.st_mode) or i.st_uid!=uid or stat.S_IMODE(i.st_mode)!=0o700:die(f"unsafe campaign directory: {rel}")
  for n in files:
   p=Path(current)/n;rel=p.relative_to(root).as_posix();q=PurePosixPath(rel)
   if any(x in ("",".","..") for x in q.parts) or TEMP.search(rel):die(f"noncanonical campaign entry: {rel}")
   raw,digest,mode=held_bytes(p,uid);out.append({"path":f"{prefix}/{rel}","size":len(raw),"mode":mode,"sha256":digest,"_bytes":raw})
   if len(out)>MAX_FILES or sum(x["size"] for x in out)>MAX_BYTES:die("campaign archive bounds exceeded")
 return out
def selected_evidence(state:Path,campaign_id:str,uid:int):
 out=[]
 for namespace,base in (("runner-evidence",state/"runner-evidence"),("audit-receipts",state/"audit-receipts")):
  if not base.exists():continue
  for p in sorted(base.rglob("*")):
   if not p.is_file() or p.is_symlink():continue
   try:raw,digest,mode=held_bytes(p,uid)
   except SystemExit:raise
   # Relevant evidence is identity-bound by content or namespaced path.  Never
   # archive unrelated campaigns merely because they share a directory.
   rel=p.relative_to(base).as_posix()
   if campaign_id.encode() not in raw and campaign_id not in rel:continue
   out.append({"path":f"{namespace}/{rel}","size":len(raw),"mode":mode,"sha256":digest,"_bytes":raw})
 return out
def authority(fdno:int,uid:int):
 try:i=os.fstat(fdno)
 except OSError:die("FACTORY_COORDINATOR_AUTH_FD is not open")
 if not stat.S_ISREG(i.st_mode) or i.st_uid not in {0,uid} or stat.S_IMODE(i.st_mode)!=0o600 or i.st_nlink!=1:die("coordinator archive authority descriptor is unsafe")
 try:raw=os.pread(fdno,129,0)
 except OSError:die("cannot read coordinator archive authority")
 if len(raw)<32 or len(raw)>128:die("coordinator archive authority length is invalid")
 return raw
def canonical(doc):return (json.dumps(doc,sort_keys=True,separators=(",",":"))+"\n").encode()
def remove_tree(parent_fd:int,name:str):
 fd=os.open(name,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=parent_fd)
 try:
  for entry in os.listdir(fd):
   i=os.stat(entry,dir_fd=fd,follow_symlinks=False)
   if stat.S_ISDIR(i.st_mode):remove_tree(fd,entry)
   elif stat.S_ISREG(i.st_mode):os.unlink(entry,dir_fd=fd)
   else:die(f"member changed type during prune: {entry}")
  os.fsync(fd)
 finally:os.close(fd)
 os.rmdir(name,dir_fd=parent_fd);os.fsync(parent_fd)
def verify_published(archive:Path,manifest_path:Path,key:bytes):
 raw,digest,_=held_bytes(archive,os.getuid());doc=json.loads(manifest_path.read_bytes())
 mac=doc.pop("authentication",None)
 if not isinstance(mac,dict) or mac.get("namespace")!=AUTH_NAMESPACE or not hmac.compare_digest(str(mac.get("hmac_sha256","")),hmac.new(key,canonical(doc),hashlib.sha256).hexdigest()):die("published archive authentication failed")
 if doc.get("archive_sha256")!=digest:die("published archive digest mismatch")
 with tarfile.open(fileobj=io.BytesIO(raw),mode="r:") as tar:
  actual={m.name:hashlib.sha256(tar.extractfile(m).read()).hexdigest() for m in tar.getmembers() if m.isfile()}
 expected={m["path"]:m["sha256"] for group in doc["namespaces"].values() for m in group}
 if actual!=expected:die("published archive member authentication failed")
def main():
 ap=argparse.ArgumentParser();ap.add_argument("--root",default=".");ap.add_argument("--campaign-id",required=True);ap.add_argument("--retention",type=int,default=RETENTION_DEFAULT);ap.add_argument("--archive-only",action="store_true");a=ap.parse_args()
 if not NAME.fullmatch(a.campaign_id):die("invalid campaign ID")
 if not 1<=a.retention<=100:die("retention must be between 1 and 100")
 try:authfd=int(os.environ["FACTORY_COORDINATOR_AUTH_FD"])
 except (KeyError,ValueError):die("FACTORY_COORDINATOR_AUTH_FD is required")
 root=Path(a.root).resolve();uid=os.getuid();key=authority(authfd,uid);ri=os.lstat(root)
 if not stat.S_ISDIR(ri.st_mode) or ri.st_uid!=uid:die("repository root owner/type is unsafe")
 lock=os.open(root,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
 try:
  try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
  except BlockingIOError:die("active campaign lock is held")
  state=root/".factory-state";campaigns=state/"campaigns";campaign=campaigns/a.campaign_id
  for p in (state,campaigns,campaign):
   i=os.lstat(p)
   if not stat.S_ISDIR(i.st_mode) or stat.S_ISLNK(i.st_mode) or i.st_uid!=uid or stat.S_IMODE(i.st_mode)!=0o700:die(f"unsafe campaign path: {p}")
  members=inspect(campaign,uid,f"campaigns/{a.campaign_id}")
  state_member=next((x for x in members if x["path"]==f"campaigns/{a.campaign_id}/factory-loop.json"),None)
  if state_member is None:die("campaign has no canonical resumable/final state")
  try:control=json.loads(state_member["_bytes"])
  except (ValueError,UnicodeError):die("campaign state is malformed")
  terminal={"success","findings","blocked","failed","interrupted","infrastructure_failure"};phase=control.get("current_phase",control.get("outcome")) if isinstance(control,dict) else None
  if phase not in terminal:die("active/resumable campaign cannot be archived or pruned")
  if control.get("campaign_id",a.campaign_id)!=a.campaign_id:die("campaign state identity mismatch")
  evidence=selected_evidence(state,a.campaign_id,uid)
  readiness=control.get('readiness',{}) if isinstance(control,dict) else {}
  if readiness.get('required') is True:
   aggregate=readiness.get('aggregate_sha256')
   runner_members=[x for x in evidence if x['path'].startswith('runner-evidence/')]
   if not isinstance(aggregate,str) or not re.fullmatch(r'[0-9a-f]{64}',aggregate) or not runner_members or not any(x['sha256']==aggregate for x in runner_members):
    die('required runner evidence is missing from archive namespace')
  all_members=members+evidence
  namespaces={n:[{k:v for k,v in x.items() if k!="_bytes"} for x in all_members if x["path"].startswith(n+"/")] for n in ("campaigns","runner-evidence","audit-receipts")}
  archives=state/"campaign-archives";archives.mkdir(mode=0o700,exist_ok=True);ai=os.lstat(archives)
  if ai.st_uid!=uid or stat.S_IMODE(ai.st_mode)!=0o700 or stat.S_ISLNK(ai.st_mode):die("unsafe archive directory")
  stamp=time.strftime("%Y%m%dT%H%M%SZ",time.gmtime());base=f"{a.campaign_id}-{stamp}"
  with tempfile.NamedTemporaryFile(dir=archives,prefix=".archive.",delete=False) as tf:tmp=Path(tf.name)
  try:
   with tarfile.open(tmp,"w") as tar:
    for item in all_members:
     info=tarfile.TarInfo(item["path"]);info.size=item["size"];info.mode=item["mode"];info.uid=uid;info.gid=os.getgid();info.mtime=0;tar.addfile(info,io.BytesIO(item["_bytes"]))
   archive_raw=tmp.read_bytes();archive_sha256=hashlib.sha256(archive_raw).hexdigest()
   doc={"schema":ARCHIVE_SCHEMA,"campaign_id":a.campaign_id,"created_at":stamp,"archive_sha256":archive_sha256,"runner_evidence_namespace":"factory-runner-receipt","namespaces":namespaces}
   doc["authentication"]={"namespace":AUTH_NAMESPACE,"hmac_sha256":hmac.new(key,canonical(doc),hashlib.sha256).hexdigest()}
   archive=archives/(base+".tar");os.link(tmp,archive);os.unlink(tmp)
   manifest_path=archives/(base+".manifest.json");fd=os.open(manifest_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600);os.write(fd,canonical(doc));os.fsync(fd);os.close(fd);fsync_dir(archives)
  finally:
   try:tmp.unlink()
   except FileNotFoundError:pass
  # Authenticate exact held archive bytes before any no-follow deletion.
  verify_published(archive,manifest_path,key)
  if not a.archive_only:
   pfd=os.open(campaigns,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
   try:remove_tree(pfd,a.campaign_id)
   finally:os.close(pfd)
  existing=sorted(archives.glob(a.campaign_id+"-*.manifest.json"))
  for old_manifest in existing[:-a.retention]:
   old_archive=old_manifest.with_name(old_manifest.name.replace(".manifest.json",".tar"));verify_published(old_archive,old_manifest,key);old_archive.unlink();old_manifest.unlink();fsync_dir(archives)
  print(archive)
 finally:os.close(lock)
if __name__=="__main__":main()
