#!/usr/bin/env python3
"""Privileged, fail-closed factory runner broker.

Only this root process accepts candidate bytes.  Candidate source is retained in
root-owned read-only storage; the runner receives distinct writable home/build/
output mounts.  Raw output is copied once into root-owned held files before
semantic analysis and those same bytes are described, exported, and signed.
"""
from __future__ import annotations
import base64, fcntl, grp, hashlib, io, json, os, pwd, re, resource, selectors, shutil, signal, stat, subprocess, sys, tarfile, tempfile, time
from pathlib import Path, PurePosixPath
BUNDLE_PATH = "/usr/local/libexec/factory-runner-v2.bundle"
sys.path.insert(0, BUNDLE_PATH if os.path.isdir(BUNDLE_PATH) else os.path.dirname(os.path.abspath(__file__)))
from factory_runner_policy import PolicyError, authority_pin, class_for_uid, load_policy
from factory_runner_authority import AuthorityError, load_authority
from factory_runner_artifacts import (ArtifactError, PROTOCOL, MAX_ARTIFACTS,
    MAX_ARTIFACT_FILE, MAX_ARTIFACT_BYTES, collect, descriptors_digest, hold,
    validate_descriptors)

SHA1=re.compile(r"^[0-9a-f]{40}$"); SHA256=re.compile(r"^[0-9a-f]{64}$"); NAME=re.compile(r"^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$")
MAX_HEADER=65536; MAX_ARCHIVE=128*1024*1024; MAX_FILES=10000; MAX_CONTENT=256*1024*1024; MAX_LOG=4*1024*1024
# One request receives one kernel-enforced writable pool.  The inode bound is
# intentionally far below host-risk levels; it also bounds directory entries.
WRITABLE_BYTES=768*1024*1024; WRITABLE_INODES=65536
HEADER_TIMEOUT=15; ARCHIVE_TIMEOUT=120; BROKER_ADMISSION=8; PROCESS_STOP_RESERVE=12
NONCE_TTL=900; NONCE_OUTSTANDING=32; NONCE_TOTAL=4096; NONCE_RATE=8; NONCE_RATE_WINDOW=60
ADMISSION_ROOT=Path("/run/factory-runner-admission")
SIGNER="/usr/local/libexec/factory-runner-signer"

class BrokerError(RuntimeError): pass

def fail(msg):
 print(json.dumps({"schema":"factory-runner-error/v1","result":"fail","error":msg},sort_keys=True,separators=(",",":")))
 raise SystemExit(1)
def emit(v): print(json.dumps(v,sort_keys=True,separators=(",",":")),flush=True)

def _admit():
 """Take one host-wide root slot before accepting even a header byte."""
 try:ADMISSION_ROOT.mkdir(mode=0o700,parents=True)
 except FileExistsError:pass
 _safe_chain(ADMISSION_ROOT,leaf="dir")
 ai=os.lstat(ADMISSION_ROOT)
 if ai.st_uid or stat.S_IMODE(ai.st_mode)!=0o700:raise BrokerError("unsafe broker admission directory")
 for index in range(BROKER_ADMISSION):
  fd=os.open(ADMISSION_ROOT/f"slot-{index}",os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW|os.O_CLOEXEC,0o600)
  try:
   i=os.fstat(fd)
   if i.st_uid or not stat.S_ISREG(i.st_mode) or stat.S_IMODE(i.st_mode)!=0o600:raise BrokerError("unsafe broker admission slot")
   try:fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);return fd
   except BlockingIOError:pass
  except Exception:os.close(fd);raise
  os.close(fd)
 raise BrokerError("root broker admission limit reached")

def _read_deadline(fd,limit,deadline,*,exact=False):
 """Read without allowing trickled bytes to extend a monotonic deadline."""
 out=bytearray();poll=selectors.DefaultSelector();poll.register(fd,selectors.EVENT_READ)
 try:
  while len(out)<limit:
   remaining=deadline-time.monotonic()
   if remaining<=0 or not poll.select(max(0,remaining)):raise BrokerError("request read deadline exceeded")
   chunk=os.read(fd,min(65536,limit-len(out)))
   if not chunk:break
   out.extend(chunk)
   if not exact and b"\n" in chunk:break
 finally:poll.close()
 return bytes(out)

def _header(fd):
 raw=_read_deadline(fd,MAX_HEADER+1,time.monotonic()+HEADER_TIMEOUT)
 line,sep,extra=raw.partition(b"\n")
 if not sep or len(line)>MAX_HEADER:raise BrokerError("request header is invalid")
 return json.loads(line),extra

def caller_uid():
 try: uid=int(os.environ["SUDO_UID"])
 except (KeyError,ValueError): fail("broker requires an exact sudo caller UID")
 if uid<=0 or os.getuid()!=0 or os.geteuid()!=0: fail("broker must run as root for a non-root sudo caller")
 return uid

ACCOUNT_CLASS={"devrunner":"dev-runner-vm","iprunner":"iprunner","gpurunner":"gpurunner"}
def verify_runner_groups(entry):
 account=pwd.getpwuid(entry["uid"])
 if ACCOUNT_CLASS.get(account.pw_name)!=entry["name"]:raise BrokerError("runner numeric UID does not match the exact OS-account/class map")
 actual={grp.getgrgid(g).gr_name for g in os.getgrouplist(account.pw_name,account.pw_gid)}
 approved=set(entry["approved_groups"])
 if actual!=approved or grp.getgrgid(account.pw_gid).gr_name not in approved:raise BrokerError("runner primary/supplementary groups differ from exact approved set")

def _safe_chain(path:Path, *, leaf="file"):
 """Reject every symlink, writable component, and non-root component."""
 if not path.is_absolute(): raise BrokerError("trusted path is not absolute")
 current=Path("/")
 for index,part in enumerate(path.parts[1:]):
  current/=part; info=os.lstat(current); last=index==len(path.parts)-2
  if stat.S_ISLNK(info.st_mode) or info.st_uid!=0 or info.st_mode&0o022: raise BrokerError(f"unsafe trusted path component: {current}")
  if not last and not stat.S_ISDIR(info.st_mode): raise BrokerError(f"trusted ancestor is not a directory: {current}")
  if last and leaf=="file" and (not stat.S_ISREG(info.st_mode) or not info.st_mode&0o111): raise BrokerError(f"trusted executable is unsafe: {current}")
  if last and leaf=="dir" and not stat.S_ISDIR(info.st_mode): raise BrokerError(f"trusted directory is unsafe: {current}")

class TrustedExecutable:
 def __init__(self,path:str,pin:dict|None=None):
  self.path=Path(path); _safe_chain(self.path)
  self.fd=os.open(self.path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC); self.info=os.fstat(self.fd)
  self.digest=self._digest()
  if pin is not None:
   expected={"path":str(self.path),"sha256":self.digest,"device":self.info.st_dev,"inode":self.info.st_ino}
   if pin.get("status")!="enrolled" or any(pin.get(k)!=v for k,v in expected.items()):
    self.close();raise BrokerError(f"trusted executable differs from independent enrollment: {self.path}")
 def _digest(self):
  h=hashlib.sha256(); off=0
  while True:
   b=os.pread(self.fd,65536,off)
   if not b:return h.hexdigest()
   h.update(b);off+=len(b)
 def verify(self):
  _safe_chain(self.path); now=os.lstat(self.path); opened=os.fstat(self.fd)
  if (now.st_dev,now.st_ino)!=(self.info.st_dev,self.info.st_ino) or (opened.st_dev,opened.st_ino,opened.st_size)!=(self.info.st_dev,self.info.st_ino,self.info.st_size) or self._digest()!=self.digest:
   raise BrokerError(f"trusted executable replacement detected: {self.path}")
 def close(self): os.close(self.fd)

def _open_private_dir(path:Path,create=False):
 if create: path.mkdir(mode=0o700,parents=True,exist_ok=True)
 _safe_chain(path,leaf="dir"); fd=os.open(path,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
 i=os.fstat(fd)
 if i.st_uid!=0 or i.st_mode&0o077: os.close(fd); raise BrokerError("private root state has unsafe mode")
 return fd

def _list_regular(fd):
 names=[]
 for name in os.listdir(fd):
  if not SHA256.fullmatch(name): raise BrokerError("nonce ledger contains an invalid name")
  i=os.stat(name,dir_fd=fd,follow_symlinks=False)
  if not stat.S_ISREG(i.st_mode) or i.st_uid!=0 or i.st_nlink!=1 or stat.S_IMODE(i.st_mode)!=0o600: raise BrokerError("nonce ledger inode is unsafe")
  names.append((name,i))
 return names

def _ledger(entry):
 root=Path(entry["nonce_ledger"]); rfd=_open_private_dir(root,True)
 try:
  lockfd=os.open("ledger.lock",os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=rfd)
  li=os.fstat(lockfd)
  if li.st_uid or not stat.S_ISREG(li.st_mode) or stat.S_IMODE(li.st_mode)!=0o600:raise BrokerError("nonce ledger lock is unsafe")
  fcntl.flock(lockfd,fcntl.LOCK_EX)
  for name in ("issued","consumed"):
   try: os.mkdir(name,0o700,dir_fd=rfd)
   except FileExistsError: pass
  ifd=os.open("issued",os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=rfd)
  cfd=os.open("consumed",os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=rfd)
  for fd in (ifd,cfd):
   i=os.fstat(fd)
   if i.st_uid!=0 or i.st_mode&0o077: raise BrokerError("nonce ledger child is unsafe")
  return rfd,ifd,cfd,lockfd
 except Exception:
  for fd in (locals().get("cfd"),locals().get("ifd"),locals().get("lockfd"),rfd):
   if fd is not None:
    try:os.close(fd)
    except OSError:pass
  raise

def _gc_and_bound(ifd,cfd,uid,now):
 issued=_list_regular(ifd); consumed=_list_regular(cfd)
 # Expired issued and old consumed records are securely unlinked by descriptor.
 for name,i in issued:
  if now-int(i.st_mtime)>NONCE_TTL: os.unlink(name,dir_fd=ifd)
 for name,i in consumed:
  if now-int(i.st_mtime)>NONCE_TTL: os.unlink(name,dir_fd=cfd)
 issued=_list_regular(ifd); consumed=_list_regular(cfd)
 owned=[]; recent=0
 for name,i in issued:
  raw=os.open(name,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=ifd)
  try: rec=json.loads(os.read(raw,4097))
  finally: os.close(raw)
  if rec.get("uid")==uid:
   owned.append(name)
   if now-rec.get("issued_at",0)<NONCE_RATE_WINDOW: recent+=1
 if len(owned)>=NONCE_OUTSTANDING or len(issued)+len(consumed)>=NONCE_TOTAL: raise BrokerError("nonce ledger quota exhausted")
 if recent>=NONCE_RATE: raise BrokerError("nonce issuance rate exceeded")

def issue_nonce(entry,req,uid):
 if set(req)!={"schema","runner","campaign_id","readiness_nonce","capabilities"} or req.get("schema")!="factory-runner-nonce-request/v1": fail("nonce request is invalid")
 if req["runner"]!=entry["name"] or req["capabilities"]!=sorted(entry["allowed_capabilities"]) or not NAME.fullmatch(req["campaign_id"]) or not SHA256.fullmatch(req["readiness_nonce"]): fail("nonce request binding is invalid")
 now=int(time.time()); nonce=hashlib.sha256(os.urandom(64)).hexdigest()
 try:
  rfd,ifd,cfd,lockfd=_ledger(entry)
  try:
   _gc_and_bound(ifd,cfd,uid,now)
   fd=os.open(nonce,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=ifd)
   record={"uid":uid,"runner":entry["name"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"capabilities":req["capabilities"],"issued_at":now}
   os.write(fd,(json.dumps(record,sort_keys=True)+"\n").encode());os.fsync(fd);os.close(fd);os.fsync(ifd)
  finally: os.close(cfd);os.close(ifd);os.close(lockfd);os.close(rfd)
 except (OSError,ValueError,BrokerError) as e: fail(str(e))
 emit({"schema":"factory-runner-nonce/v1","nonce":nonce,"runner":entry["name"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"capabilities":req["capabilities"]})

def consume_nonce(entry,request,uid):
 try:
  rfd,ifd,cfd,lockfd=_ledger(entry)
  try:
   fd=os.open(request["nonce"],os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=ifd); i=os.fstat(fd); raw=os.read(fd,4097);os.close(fd)
   if not stat.S_ISREG(i.st_mode) or i.st_uid!=0 or i.st_nlink!=1 or stat.S_IMODE(i.st_mode)!=0o600: raise BrokerError("nonce inode is unsafe")
   record=json.loads(raw); now=int(time.time())
   expected={"uid":uid,"runner":entry["name"],"campaign_id":request["campaign_id"],"readiness_nonce":request["readiness_nonce"],"capabilities":request["capabilities"]}
   if any(record.get(k)!=v for k,v in expected.items()) or now-record.get("issued_at",0)>NONCE_TTL or record.get("issued_at",0)>now: raise BrokerError("nonce issuance binding is stale or mismatched")
   # renameat is the one atomic state transition; destination pre-existence is replay.
   os.rename(request["nonce"],request["nonce"],src_dir_fd=ifd,dst_dir_fd=cfd);os.fsync(ifd);os.fsync(cfd)
  finally: os.close(cfd);os.close(ifd);os.close(lockfd);os.close(rfd)
 except (OSError,ValueError,BrokerError): fail("nonce was not issued or was already consumed")

def extract(archive:bytes,dest:Path):
 count=total=0
 with tarfile.open(fileobj=io.BytesIO(archive),mode="r:") as stream:
  for member in stream:
   count+=1
   if count>MAX_FILES: fail("archive file count exceeded before extraction")
   p=PurePosixPath(member.name)
   if p.is_absolute() or not p.parts or p.parts[0]==".git" or any(x in ("",".","..") for x in p.parts) or not(member.isfile() or member.isdir()): fail("archive member is unsafe")
   if member.isfile():
    total+=member.size
    if member.size>MAX_ARCHIVE or total>MAX_CONTENT: fail("archive byte bound exceeded before extraction")
   target=dest.joinpath(*p.parts)
   if member.isdir(): target.mkdir(mode=0o755,parents=True,exist_ok=True);continue
   target.parent.mkdir(mode=0o755,parents=True,exist_ok=True);src=stream.extractfile(member)
   if src is None: fail("archive member cannot be read")
   fd=os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o755 if member.mode&0o111 else 0o644)
   try:
    remaining=member.size
    while remaining:
     chunk=src.read(min(65536,remaining))
     if not chunk: fail("archive member shortened")
     os.write(fd,chunk);remaining-=len(chunk)
   finally:os.close(fd)

def freeze_tree(root:Path):
 for parent,dirs,files in os.walk(root,topdown=False):
  for n in files:
   p=Path(parent)/n;i=p.lstat()
   if not stat.S_ISREG(i.st_mode) or stat.S_ISLNK(i.st_mode) or i.st_nlink!=1: fail("candidate source inode is unsafe")
   os.chown(p,0,0);p.chmod(0o555 if i.st_mode&0o111 else 0o444)
  for n in dirs: p=Path(parent)/n;os.chown(p,0,0);p.chmod(0o555)
 os.chown(root,0,0);root.chmod(0o555)

def source_digest(root:Path):
 h=hashlib.sha256()
 for p in sorted(root.rglob("*")):
  i=p.lstat();rel=p.relative_to(root).as_posix().encode()
  if stat.S_ISLNK(i.st_mode) or i.st_uid!=0 or i.st_mode&0o022: raise BrokerError("source tree ownership/type changed during execution")
  h.update(rel+b"\0"+str(stat.S_IMODE(i.st_mode)).encode()+b"\0")
  if stat.S_ISREG(i.st_mode): h.update(p.read_bytes())
 return h.hexdigest()


def clean_env(home,authority,product,build,artifacts):
 return {"HOME":str(home),"GIT_CONFIG_NOSYSTEM":"1","GIT_CONFIG_GLOBAL":"/dev/null","GIT_ATTR_NOSYSTEM":"1","XDG_CACHE_HOME":str(home/".cache"),"XDG_CONFIG_HOME":str(home/".config"),"XDG_DATA_HOME":str(home/".local/share"),"PATH":":".join(authority.document["trusted_path"]),"LANG":"C.UTF-8","LC_ALL":"C.UTF-8","NIX_REMOTE":"daemon","FACTORY_PRODUCT_ROOT":str(product),"FACTORY_BUILD_ROOT":str(build),"FACTORY_RUNNER_ARTIFACT_DIR":str(artifacts),"CONTROLLER_PRODUCTION_ROUTING_ARTIFACTS":str(artifacts/"controller-production-routing"),"CBX_GPU_PROBE_ARTIFACTS":str(artifacts/"gpu-compositor")}
def argv_for(authority,argv,product,artifacts,commit="",tree=""):
 out=[]
 for token in argv:
  token=token.replace("{product}",str(product)).replace("{artifacts}",str(artifacts)).replace("{commit}",commit).replace("{tree}",tree)
  if token.startswith("@/"): token=str(authority.path(token[2:]))
  out.append(token)
 return out

def exact_tree(product,request,git,env):
 def run(args,input=None):
  git.verify();r=subprocess.run([str(git.path),*args],cwd=product,env=env,input=input,capture_output=True,timeout=120)
  if r.returncode: fail("candidate tree reconstruction/revalidation failed")
  return r.stdout
 run(["init","-q"]);run(["add","-f","--all"]);tree=run(["write-tree"]).decode().strip()
 commit=run(["hash-object","-t","commit","-w","--stdin"],base64.b64decode(request["commit_object_b64"],validate=True)).decode().strip();run(["update-ref","HEAD",commit])
 if tree!=request["tree"] or commit!=request["commit"] or run(["status","--porcelain","--untracked-files=normal"]): fail("candidate product differs from exact requested tree")
 # Freeze product and broker-created Git metadata root-owned/read-only. Probes
 # may inspect/archive HEAD, but the runner cannot alter refs, index, or objects.
 freeze_tree(product)

def _device_properties(capability):
 devices={"kernel-uinput":["/dev/uinput rw"],"physical-controller":["char-input r"],"controller-production-routing":["char-input r"],"gpu-compositor":["char-drm rw"],"installed-licensed-diagram":["char-drm rw"]}.get(capability,[])
 return (["PrivateDevices=yes"] if not devices else ["PrivateDevices=no","DevicePolicy=closed",*[f"DeviceAllow={x}" for x in devices]])

def _pin(entry,name):
 try:return entry["executable_pins"][name]
 except KeyError as exc:raise BrokerError(f"missing executable enrollment: {name}") from exc

class WritablePool:
 """Root-mounted, byte/inode-bounded disposable storage for candidate writes."""
 def __init__(self,entry,runroot,uid):
  self.root=runroot/"writable";self.root.mkdir(mode=0o700)
  self.mount=TrustedExecutable("/usr/bin/mount",_pin(entry,"mount"));self.umount=TrustedExecutable("/usr/bin/umount",_pin(entry,"umount"));self.mounted=False
  opts=f"size={WRITABLE_BYTES},nr_inodes={WRITABLE_INODES},mode=0700,uid=0,gid=0,nosuid,nodev"
  self.mount.verify();r=subprocess.run([str(self.mount.path),"-t","tmpfs","-o",opts,"factory-runner-writable",str(self.root)],capture_output=True,timeout=10);self.mount.verify()
  if r.returncode:raise BrokerError("bounded writable tmpfs unavailable")
  self.mounted=True
  for name in ("build","home","output"):
   p=self.root/name;p.mkdir(mode=0o700);os.chown(p,uid,uid)
 def paths(self):return self.root/"build",self.root/"home",self.root/"output"
 def close(self):
  error=None
  if self.mounted:
   self.umount.verify()
   try:r=subprocess.run([str(self.umount.path),str(self.root)],capture_output=True,timeout=10)
   except Exception as exc:r=None;error=exc
   self.umount.verify()
   if r is None or r.returncode:error=error or BrokerError("bounded writable pool did not unmount")
   else:self.mounted=False
  # Never recursively walk candidate trees: unmount discards all attacker inodes.
  if not self.mounted:
   try:self.root.rmdir()
   except OSError as exc:error=error or exc
  self.mount.close();self.umount.close()
  if error:raise BrokerError("bounded writable backing resource cleanup not proven") from error

def _cleanup_unit(systemctl,unit,cgroup_root):
 errors=[]
 for verb in ("stop","kill"):
  try: subprocess.run([str(systemctl.path),verb,unit],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=15)
  except Exception as e: errors.append(type(e).__name__)
 deadline=time.monotonic()+15;values={}
 while time.monotonic()<deadline:
  systemctl.verify();q=subprocess.run([str(systemctl.path),"show",unit,"--property=ControlGroup","--property=ActiveState","--property=SubState","--property=MainPID"],capture_output=True,text=True,timeout=10)
  values=dict(line.split("=",1) for line in q.stdout.splitlines() if "=" in line)
  if q.returncode==0 and values.get("ActiveState") in {"inactive","failed"} and values.get("MainPID")=="0":
   cg=Path(cgroup_root)/values.get("ControlGroup","").lstrip("/")
   if not cg.exists(): return not errors
   try:
    if not (cg/"cgroup.procs").read_text().strip() and "populated 0" in (cg/"cgroup.events").read_text(): return not errors
   except OSError: pass
  time.sleep(.1)
 return False

def _bounded_process(argv,env,timeout,stdout_path,stderr_path,*,cwd="/",preexec=None,overflow=None,pass_fds=()):
 """Run, stop, drain and reap under one absolute, non-resettable deadline."""
 flags=os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC
 outfd=os.open(stdout_path,flags,0o600);errfd=os.open(stderr_path,flags,0o600)
 proc=None;poll=None;exceeded=False;deadline=time.monotonic()+timeout;counts={"stdout":0,"stderr":0};stop_sent=False
 def stop():
  nonlocal stop_sent
  if stop_sent:return
  stop_sent=True
  try:
   if overflow:overflow(max(0.0,deadline-time.monotonic()))
   elif proc:os.killpg(proc.pid,signal.SIGKILL)
  except (ProcessLookupError,OSError,subprocess.SubprocessError):pass
 try:
  # timeout includes Popen and leaves a fixed reserve for stop/drain/wait.
  proc=subprocess.Popen(argv,cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,stdin=subprocess.DEVNULL,start_new_session=True,preexec_fn=preexec,pass_fds=pass_fds)
  poll=selectors.DefaultSelector()
  for stream,name,fd in ((proc.stdout,"stdout",outfd),(proc.stderr,"stderr",errfd)):
   os.set_blocking(stream.fileno(),False);poll.register(stream,selectors.EVENT_READ,(name,fd))
  run_end=deadline-min(PROCESS_STOP_RESERVE,max(0.1,timeout/2))
  while poll.get_map() and time.monotonic()<deadline:
   now=time.monotonic()
   if now>=run_end:exceeded=True;stop()
   events=poll.select(min(.25,max(0,deadline-now)))
   for key,_ in events:
    name,fd=key.data
    try:chunk=os.read(key.fd,65536)
    except BlockingIOError:continue
    if not chunk:poll.unregister(key.fileobj);continue
    allowed=max(0,MAX_LOG-counts[name]);os.write(fd,chunk[:allowed]);counts[name]+=len(chunk)
    if counts[name]>MAX_LOG:exceeded=True;stop()
  if poll.get_map():
   exceeded=True;stop()
   # Pipe holders must not retain the broker: close our read ends at deadline.
   for key in list(poll.get_map().values()):poll.unregister(key.fileobj);key.fileobj.close()
  left=max(0,deadline-time.monotonic())
  try:proc.wait(timeout=left)
  except subprocess.TimeoutExpired:exceeded=True;stop()
  os.fsync(outfd);os.fsync(errfd)
 finally:
  if poll:poll.close()
  for stream in ((proc.stdout,proc.stderr) if proc else ()):
   try:stream.close()
   except Exception:pass
  os.close(outfd);os.close(errfd)
  if proc and proc.poll() is None:stop() # never perform an unbounded wait
 def held(path):
  fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC)
  try:return os.read(fd,MAX_LOG+1)
  finally:os.close(fd)
 if exceeded or not proc or proc.poll() is None:raise BrokerError("child output or absolute runtime bound exceeded")
 return proc.returncode,held(stdout_path),held(stderr_path)

def _analyzer_limits():
 resource.setrlimit(resource.RLIMIT_CPU,(120,120));resource.setrlimit(resource.RLIMIT_AS,(768*1024*1024,768*1024*1024))
 resource.setrlimit(resource.RLIMIT_FSIZE,(MAX_LOG,MAX_LOG));resource.setrlimit(resource.RLIMIT_NPROC,(32,32));resource.setrlimit(resource.RLIMIT_NOFILE,(64,64))

DBUS_CAPS={"inputplumber-system-dbus","target-consumer","controller-production-routing","gpu-compositor","installed-licensed-diagram"}
DBUS_READ_CALLS=(
 "org.freedesktop.DBus.ObjectManager.GetManagedObjects",
 "org.freedesktop.DBus.Properties.Get","org.freedesktop.DBus.Properties.GetAll",
 "org.freedesktop.DBus.Introspectable.Introspect")
# Synthetic injection is never delegated. Production routing receives only
# the exact mutation members used by the product; no destination-wide --talk
# grant is present. The retained proxy log is part of root cleanup/audit.
DBUS_MUTATING_CALLS=(
 "org.shadowblip.InputManager.CreateTargetDevice",
 "org.shadowblip.InputManager.StopTargetDevice",
 "org.shadowblip.Input.CompositeDevice.SetTargetDevices",
 "org.shadowblip.Input.CompositeDevice.SetInterceptActivation",
 "org.freedesktop.DBus.Properties.Set",
)
FORBIDDEN_DBUS_CALLS=("org.shadowblip.InputPlumber.Target.InputEvent",)

class DbusMonitor:
 """Root-owned bounded system-bus monitor started before the proxy.

 The raw busctl JSON stream is the audit authority.  Candidate mounts never
 include this directory.  At shutdown the broker resolves every connection
 whose Unix PID equals the held proxy PID through GetConnectionUnixProcessID;
 zero or multiple senders fail closed.
 """
 def __init__(self,entry,parent):
  self.busctl=TrustedExecutable("/usr/bin/busctl",_pin(entry,"busctl"));self.path=parent/"inputplumber-dbus-monitor.jsonl"
  self.fd=os.open(self.path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o400)
  self.busctl.verify();self.proc=subprocess.Popen([str(self.busctl.path),"--system","--json=short","monitor","org.shadowblip.InputPlumber"],cwd="/",env={"PATH":"/usr/bin:/bin","LANG":"C.UTF-8"},stdin=subprocess.DEVNULL,stdout=self.fd,stderr=self.fd,start_new_session=True,preexec_fn=_analyzer_limits)
  self.started_ns=time.monotonic_ns();time.sleep(.2)
  if self.proc.poll() is not None:self.close();raise BrokerError("root InputPlumber bus monitor failed before proxy launch")
  self.closed=False;self.digest=None;self.size=None;self.proxy_senders=[]
 def _proxy_senders(self,proxy):
  self.busctl.verify();listed=subprocess.run([str(self.busctl.path),"--system","--no-pager","--no-legend","list"],capture_output=True,text=True,timeout=10);self.busctl.verify()
  if listed.returncode:raise BrokerError("cannot enumerate proxy bus connections")
  found=[]
  for line in listed.stdout.splitlines():
   fields=line.split()
   if not fields or not re.fullmatch(r":[0-9]+\.[0-9]+",fields[0]):continue
   name=fields[0]
   self.busctl.verify();r=subprocess.run([str(self.busctl.path),"--system","call","org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","GetConnectionUnixProcessID","s",name],capture_output=True,text=True,timeout=10);self.busctl.verify()
   if r.returncode:continue
   m=re.search(r"(?:^|\s)([0-9]+)\s*$",r.stdout.strip())
   if m and int(m.group(1))==proxy.pid:found.append(name)
  if len(found)!=1:raise BrokerError("multiple, hidden, or absent proxy D-Bus senders")
  return found
 def normalize(self,proxy,owner,before,after,parent,entry,capabilities):
  """Normalize held busctl history and replay the installed root analyzer."""
  if not self.closed:raise BrokerError('D-Bus monitor must be closed before normalization')
  sender=self.proxy_senders[0] if len(self.proxy_senders)==1 else None
  if sender is None:raise BrokerError('proxy historical sender binding is absent')
  events=[];pending=set()
  for line in self.path.read_bytes().splitlines():
   try:m=json.loads(line)
   except ValueError:continue
   typ=m.get('type');payload=m.get('payload',{})
   if typ in ('method_call','call') and m.get('destination')=='org.shadowblip.InputPlumber':
    if m.get('sender')!=sender:raise BrokerError('concurrent non-mediator InputPlumber traffic observed')
    serial=m.get('cookie',m.get('serial'));pending.add(serial)
    events.append({'type':'call','seq':len(events)+1,'serial':serial,'sender':sender,'destination':m.get('destination'),'path':m.get('path'),'interface':m.get('interface'),'member':m.get('member'),'signature':payload.get('type',m.get('signature','')),'body':payload.get('data',m.get('body',[]))})
   elif typ in ('method_return','return','method_error','error') and m.get('destination')==sender:
    reply=m.get('reply_cookie',m.get('reply_serial'))
    if reply not in pending:continue
    events.append({'type':'error' if typ in ('method_error','error') else 'return','seq':len(events)+1,'reply_serial':reply,'sender':m.get('sender'),'destination':sender,'signature':payload.get('type',m.get('signature','')),'body':payload.get('data',m.get('body',[])),'error_name':m.get('error_name')})
  document={'schema':'factory-inputplumber-dbus-audit/v1','complete':True,'overflow':False,'truncated':False,'monitor_started_ns':self.started_ns,'proxy_started_ns':proxy.started_ns,'monitor_pid':self.proc.pid,'proxy_pid':proxy.pid,'proxy_starttime':proxy.starttime,'proxy_cgroup':proxy.cgroup,'inputplumber_owner':owner,'sender_pids':{sender:proxy.pid},'events':events}
  paths={n:parent/n for n in ('root-dbus-audit.json','root-dbus-before.json','root-dbus-after.json','root-dbus-contract.json','root-dbus-binding.json')}
  values=(document,before,after,{'target_count':4 if 'controller-production-routing' in capabilities else 0})
  for path,value in zip(list(paths.values())[:4],values):
   fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o400);os.write(fd,(json.dumps(value,sort_keys=True,separators=(',',':'))+'\n').encode());os.fsync(fd);os.close(fd)
  analyzer=Path(BUNDLE_PATH)/'inputplumber-dbus-audit.py';python=TrustedExecutable('/usr/bin/python3',_pin(entry,'python3'));python.verify()
  r=subprocess.run([str(python.path),str(analyzer),'--audit',str(paths['root-dbus-audit.json']),'--before',str(paths['root-dbus-before.json']),'--after',str(paths['root-dbus-after.json']),'--contract',str(paths['root-dbus-contract.json']),'--binding-out',str(paths['root-dbus-binding.json'])],capture_output=True,timeout=120);python.verify();python.close()
  if r.returncode:raise BrokerError('root InputPlumber temporal audit analyzer rejected held history')
  return paths
 def close(self,proxy=None):
  if getattr(self,"closed",False):return
  if proxy is not None:self.proxy_senders=self._proxy_senders(proxy)
  if getattr(self,"proc",None) and self.proc.poll() is None:
   try:os.killpg(self.proc.pid,signal.SIGINT);self.proc.wait(timeout=3)
   except Exception:
    try:os.killpg(self.proc.pid,signal.SIGKILL)
    except ProcessLookupError:pass
    self.proc.wait(timeout=3)
  if self.proc.returncode not in (0,-signal.SIGINT,-signal.SIGTERM):raise BrokerError("root InputPlumber monitor stopped unexpectedly (gap/overflow)")
  os.fsync(self.fd);os.close(self.fd);self.fd=None
  info=os.lstat(self.path)
  if not stat.S_ISREG(info.st_mode) or info.st_uid!=0 or info.st_size<=0 or info.st_size>MAX_LOG:raise BrokerError("root D-Bus audit is empty/truncated/overflowed")
  raw=self.path.read_bytes();self.digest=hashlib.sha256(raw).hexdigest();self.size=len(raw);self.closed=True;self.busctl.close()

class DbusProxy:
 """Root-owned pre-forward mediator for the candidate's private system bus.

 Unlike an xdg-dbus-proxy call allowlist, the mediator decodes each complete
 message and enforces source path, argument, sequence, and learned target state
 before issuing the corresponding call on its own system-bus connection.
 """
 def __init__(self,entry,parent,host_lock,capabilities=(),monitor=None):
  if monitor is None or monitor.proc.poll() is not None:raise BrokerError("root D-Bus monitor must be live before mediator")
  if host_lock is None:raise BrokerError("mediator requires root-held dedicated topology")
  self.exe=TrustedExecutable("/usr/libexec/inputplumber-mediator",_pin(entry,"inputplumber-mediator"));self.root=parent/"dbus-mediator";self.root.mkdir(mode=0o700);os.chown(self.root,0,0)
  self.socket=self.root/"system_bus_socket";self.audit=parent/"root-dbus-preforward.jsonl"
  mutate="yes" if "controller-production-routing" in capabilities else "no"
  argv=[str(self.exe.path),"--mutations",mutate,str(self.socket),str(host_lock.snapshot_path),str(self.audit)]
  self.exe.verify();self.proc=subprocess.Popen(argv,cwd="/",env={"PATH":"/usr/bin:/bin","LANG":"C.UTF-8"},stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True,preexec_fn=_analyzer_limits)
  self.pid=self.proc.pid;self.starttime=InputPlumberProvenance._starttime(self.pid);self.started_ns=time.monotonic_ns()
  cgroups=Path(f'/proc/{self.pid}/cgroup').read_text().splitlines();unified=[x.split(':',2)[2] for x in cgroups if x.startswith('0::')]
  if len(unified)!=1 or not unified[0].startswith('/'):self.close();raise BrokerError('proxy cgroup identity is unavailable')
  self.cgroup=unified[0]
  deadline=time.monotonic()+5
  while time.monotonic()<deadline and not self.socket.exists() and self.proc.poll() is None:time.sleep(.02)
  if not self.socket.exists() or self.proc.poll() is not None:self.close();raise BrokerError("allowlisting D-Bus proxy failed closed")
  si=os.lstat(self.socket)
  if not stat.S_ISSOCK(si.st_mode) or si.st_uid!=0:self.close();raise BrokerError("allowlisting D-Bus proxy socket is unsafe")
  os.chown(self.socket,0,pwd.getpwuid(entry["uid"]).pw_gid);os.chmod(self.socket,0o660)
 def validate_decisions(self,document):
  """Replay the mediator-owned decisions and bind them to monitor calls."""
  rows=[]
  for raw in self.audit.read_bytes().splitlines():
   try:row=json.loads(raw)
   except ValueError:raise BrokerError("mediator decision stream is malformed")
   exact={"schema","seq","allow","path","interface","member","signature","arguments","reason"}
   if set(row)!=exact or row.get("schema")!="factory-inputplumber-preforward-decision/v1" or row.get("seq")!=len(rows)+1:raise BrokerError("mediator decision schema/sequence is invalid")
   if row.get("allow") is not True or row.get("member")=="InputEvent":raise BrokerError("candidate issued a denied InputPlumber request")
   rows.append(row)
  forwards=[r for r in rows if r["reason"]=="authorized before system-bus forward"]
  calls=[e for e in document["events"] if e.get("type")=="call"]
  ip_forwards=[r for r in forwards if r["path"].startswith("/org/shadowblip/InputPlumber")]
  if not rows or len(ip_forwards)!=len(calls):raise BrokerError("pre-forward decisions do not equal monitored InputPlumber calls")
  return hashlib.sha256(self.audit.read_bytes()).hexdigest(),len(rows)
 def close(self):
  if getattr(self,"proc",None) and self.proc.poll() is None:
   try:os.killpg(self.proc.pid,signal.SIGTERM);self.proc.wait(timeout=3)
   except Exception:
    try:os.killpg(self.proc.pid,signal.SIGKILL)
    except ProcessLookupError:pass
    self.proc.wait()
  if getattr(self,"audit",None) and self.audit.exists():
   info=os.lstat(self.audit)
   if not stat.S_ISREG(info.st_mode) or info.st_uid!=0 or info.st_size>MAX_LOG:raise BrokerError("mediator decision log is unsafe or overflowed")
  if getattr(self,"exe",None):self.exe.close();self.exe=None
  try:
   if getattr(self,"socket",None):self.socket.unlink(missing_ok=True)
  except OSError:raise BrokerError("mediator socket cleanup failed")

class UdevMonitor:
 """Root-held, size-limited raw kernel udev stream with bounded shutdown."""
 def __init__(self,parent,entry):
  self.udev=TrustedExecutable("/usr/bin/udevadm",_pin(entry,"udevadm"));self.stdbuf=TrustedExecutable("/usr/bin/stdbuf",_pin(entry,"stdbuf"))
  self.path=parent/"udev-snapshot.log";self.fd=os.open(self.path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o444)
  self.udev.verify();self.stdbuf.verify()
  self.proc=subprocess.Popen([str(self.stdbuf.path),"-oL",str(self.udev.path),"monitor","--kernel","--property","--subsystem-match=input"],cwd="/",env={"PATH":"/usr/bin:/bin","LANG":"C.UTF-8"},stdin=subprocess.DEVNULL,stdout=self.fd,stderr=self.fd,start_new_session=True,preexec_fn=_analyzer_limits)
  time.sleep(.2)
  if self.proc.poll() is not None:self.close();raise BrokerError("root-held udev monitor failed")
 def close(self):
  deadline=time.monotonic()+3
  if getattr(self,"proc",None) and self.proc.poll() is None:
   try:os.killpg(self.proc.pid,signal.SIGTERM);self.proc.wait(timeout=max(0.0,deadline-time.monotonic()))
   except Exception:
    try:os.killpg(self.proc.pid,signal.SIGKILL)
    except ProcessLookupError:pass
    try:self.proc.wait(timeout=max(0.0,deadline-time.monotonic()))
    except subprocess.TimeoutExpired:raise BrokerError("udev monitor exceeded absolute shutdown deadline")
  if getattr(self,"fd",None) is not None:os.fsync(self.fd);os.close(self.fd);self.fd=None
  for name in ("udev","stdbuf"):
   exe=getattr(self,name,None)
   if exe:exe.close();setattr(self,name,None)

class UserManagerProxy(DbusProxy):
 """Capability-private endpoint for exactly the caller's user-manager bus."""
 def __init__(self,entry,parent):
  self.exe=TrustedExecutable(entry["dbus_proxy"],_pin(entry,"xdg-dbus-proxy"));self.root=parent/"user-dbus-proxy";self.root.mkdir(mode=0o700);os.chown(self.root,0,0)
  upstream=f"unix:path=/run/user/{entry['uid']}/bus";self.socket=self.root/"user_bus_socket"
  calls=(
   "org.freedesktop.systemd1.Manager.StartTransientUnit",
   "org.freedesktop.systemd1.Manager.GetUnit",
   "org.freedesktop.systemd1.Manager.Subscribe",
   "org.freedesktop.DBus.Properties.Get",
   "org.freedesktop.DBus.Properties.GetAll",
   "org.freedesktop.DBus.GetNameOwner",
  )
  argv=[str(self.exe.path),upstream,str(self.socket),"--filter",*[f"--call=org.freedesktop.systemd1={m}" for m in calls[:5]],f"--call=org.freedesktop.DBus={calls[5]}"]
  self.exe.verify();self.proc=subprocess.Popen(argv,cwd="/",env={"PATH":"/usr/bin:/bin","LANG":"C.UTF-8"},stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True,preexec_fn=_analyzer_limits)
  deadline=time.monotonic()+5
  while time.monotonic()<deadline and not self.socket.exists() and self.proc.poll() is None:time.sleep(.02)
  if not self.socket.exists() or self.proc.poll() is not None:self.close();raise BrokerError("user-manager D-Bus proxy failed closed")
  si=os.lstat(self.socket)
  if not stat.S_ISSOCK(si.st_mode) or si.st_uid!=0:self.close();raise BrokerError("user-manager proxy socket is unsafe")
  os.chown(self.socket,0,pwd.getpwuid(entry["uid"]).pw_gid);os.chmod(self.socket,0o660)

def run_contained(entry,authority,descriptor,host_product,host_build,host_home,host_artifacts,env,unit,capability,provenance=None,proxy=None,udev=None):
 systemd=TrustedExecutable(entry["systemd_run"],_pin(entry,"systemd-run"));systemctl=TrustedExecutable(entry["systemctl"],_pin(entry,"systemctl"))
 sandbox_product=Path("/run/factory/product");sandbox_build=Path("/run/factory/build");sandbox_home=Path("/run/factory/home");sandbox_output=Path("/run/factory/output")
 senv=clean_env(sandbox_home,authority,sandbox_product,sandbox_build,sandbox_output)
 if capability!="gate":
  senv["FACTORY_CAPABILITY"]=capability
  senv["FACTORY_RUNNER_ARTIFACT_DIR"]=str(sandbox_output/capability)
  senv["CONTROLLER_PRODUCTION_ROUTING_ARTIFACTS"]=str(sandbox_output/capability)
  senv["CBX_GPU_PROBE_ARTIFACTS"]=str(sandbox_output/capability)
 argv=argv_for(authority,descriptor["argv"],sandbox_product,sandbox_output)
 command_name=Path(argv[0]).name
 if command_name not in {"python3","bash"}:raise BrokerError("capability interpreter has no exact external policy pin")
 command_exe=TrustedExecutable(argv[0],_pin(entry,command_name))
 blocked_paths=["/root","/home","/run","/var/run","/etc/ssh","/etc/sudoers","/etc/sudoers.d","/etc/factory-runner","/opt/factory-runner","/workspace"]
 blocked=" ".join(x for x in blocked_paths if Path(x).exists())
 # The parent stays hidden.  This one immutable root-owned authority is
 # over-mounted read-only at the exact path used by every @/ argv.
 authority_mount=f"BindReadOnlyPaths={authority.root}:{authority.root}"
 props=["NoNewPrivileges=yes","CapabilityBoundingSet=","AmbientCapabilities=","ProtectSystem=strict","ProtectHome=yes",f"InaccessiblePaths={blocked}","PrivateTmp=yes","PrivateMounts=yes","PrivatePIDs=yes","PrivateIPC=yes","ProtectKernelTunables=yes","ProtectKernelModules=yes","ProtectKernelLogs=yes","ProtectControlGroups=yes","ProtectClock=yes","LockPersonality=yes","RestrictSUIDSGID=yes","RestrictRealtime=yes","RestrictNamespaces=yes","SystemCallArchitectures=native","SystemCallFilter=@system-service @resources","SystemCallErrorNumber=EPERM","RestrictAddressFamilies=AF_UNIX","IPAddressDeny=any","KillMode=control-group","TasksMax=512","MemoryMax=4G","CPUQuota=400%","RuntimeMaxSec=7200","TimeoutStopSec=10","LimitFSIZE=50331648",f"BindReadOnlyPaths={host_product}:{sandbox_product}",f"BindPaths={host_build}:{sandbox_build}",f"BindPaths={host_home}:{sandbox_home}",f"BindPaths={host_artifacts}:{sandbox_output}",f"WorkingDirectory={sandbox_product}",authority_mount,*_device_properties(capability)]
 if provenance: props.append(f"BindReadOnlyPaths={provenance}:/run/factory/inputplumber-provenance.json");senv["FACTORY_INPUTPLUMBER_PROVENANCE"]="/run/factory/inputplumber-provenance.json"
 if capability=="controller-production-routing":
  if udev is None:raise BrokerError("routing requires root-held udev facts")
  props.append(f"BindReadOnlyPaths={udev.path}:/run/factory/udev-snapshot.log");senv["FACTORY_UDEV_SNAPSHOT"]="/run/factory/udev-snapshot.log"
 if capability in DBUS_CAPS:
  if proxy is None:raise BrokerError("InputPlumber authority requires a private allowlisting D-Bus proxy")
  props.append(f"BindReadOnlyPaths={proxy.socket}:/run/factory/dbus/system_bus_socket")
  senv["DBUS_SYSTEM_BUS_ADDRESS"]="unix:path=/run/factory/dbus/system_bus_socket"
 if capability=="systemd-user":
  if proxy is None:raise BrokerError("user-manager authority requires a private D-Bus proxy")
  props.append(f"BindReadOnlyPaths={proxy.socket}:/run/factory/user/bus")
  senv["XDG_RUNTIME_DIR"]="/run/factory/user"
  senv["DBUS_SESSION_BUS_ADDRESS"]="unix:path=/run/factory/user/bus"
 cmd=[str(systemd.path),"--quiet","--wait","--pipe","--collect","--unit",unit,"--service-type=exec",f"--uid={entry['uid']}",*[f"--property={p}" for p in props],*[f"--setenv={k}={v}" for k,v in sorted(senv.items())],*argv]
 # systemd-run receives a scrubbed environment; candidate-controlled variables never cross.
 logroot=host_artifacts.parent;stdout_path=logroot/"candidate.stdout";stderr_path=logroot/"candidate.stderr"
 def abort(remaining):
  # All cleanup attempts share _bounded_process's absolute budget.
  for verb in ("stop","kill"):
   if remaining<=0:return
   began=time.monotonic()
   try:subprocess.run([str(systemctl.path),verb,unit],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=min(remaining,2))
   except Exception:pass
   remaining-=time.monotonic()-began
 try:
  systemd.verify();command_exe.verify();rc,out,err=_bounded_process(cmd,senv,7300,stdout_path,stderr_path,overflow=abort);command_exe.verify()
 finally:
  clean=_cleanup_unit(systemctl,unit,entry["cgroup_root"])
  systemd.close();systemctl.close();command_exe.close()
 if not clean: raise BrokerError("cannot prove unconditional request unit/cgroup cleanup")
 return rc,out,err

def analyze(entry,authority,descriptor,held,env,commit,tree):
 authority.revalidate();argv=argv_for(authority,descriptor["analyzer_argv"],Path("/noncandidate"),held.root,commit,tree)
 analyzer_name=Path(argv[0]).name
 if analyzer_name not in {"python3","bash"}:raise BrokerError("analyzer interpreter has no exact external policy pin")
 exe=TrustedExecutable(argv[0],_pin(entry,analyzer_name))
 out=held.root.parent/f"{held.root.name}.analyzer.stdout";err=held.root.parent/f"{held.root.name}.analyzer.stderr"
 try:
  exe.verify();rc,_,_=_bounded_process(argv,env,180,out,err,preexec=_analyzer_limits);exe.verify()
 finally:exe.close()
 authority.revalidate()
 if rc: raise BrokerError("root authority semantic analyzer rejected held artifact bytes")

def verify_authority_pins(policy,entry,authority):
 if entry.get("probe_authority_status")!="enrolled": raise BrokerError("probe authority is pending or unapproved")
 if hashlib.sha256((json.dumps(authority.document,sort_keys=True,indent=2)+"\n").encode()).hexdigest()!=entry["probe_authority_sha256"]: raise BrokerError("held probe authority bytes differ from enrolled pin")
 if entry["name"]=="gpurunner":
  licensed_bytes=authority.bytes("licensed-diagram-authority.json");oracle_bytes=authority.bytes(authority.document["licensed_oracle"]["path"])
  licensed=hashlib.sha256(licensed_bytes).hexdigest();oracle=hashlib.sha256(oracle_bytes).hexdigest()
  if licensed!=authority_pin(policy,entry["name"],"installed-licensed-diagram") or oracle!=authority_pin(policy,entry["name"],"gpu-compositor-layout-oracle"): raise BrokerError("licensed authority/oracle exact bytes differ from root enrollment")
  try: licensed_doc=json.loads(licensed_bytes);oracle_doc=json.loads(oracle_bytes)
  except ValueError: raise BrokerError("licensed authority/oracle is malformed")
  # machine-enforced is the immutable geometric oracle, not human approval.
  # Its exact digest must still be independently enrolled above; final human
  # graphics approval remains a separate external campaign gate.
  if licensed_doc.get("status")!="accepted-machine-authority" or oracle_doc.get("authority_status") not in {"machine-enforced","approved","enrolled"}:
   raise BrokerError("licensed authority/oracle status is pending or unapproved")

def host_cleanup_snapshot(capability,entry):
 """Independent capability-specific host state used as cleanup postcondition."""
 fact={"capability":capability}
 if capability in {"inputplumber-system-dbus","physical-controller","target-consumer","controller-production-routing","kernel-uinput"}:
  fact["input_nodes"]=sorted(p.name for p in Path("/sys/class/input").glob("event*") if re.fullmatch(r"event[0-9]+",p.name))
 if capability in {"gpu-compositor","installed-licensed-diagram"}:
  fact["dri_nodes"]=sorted(p.name for p in Path("/dev/dri").glob("*")) if Path("/dev/dri").exists() else []
 if capability in {"inputplumber-system-dbus","target-consumer","controller-production-routing"}:
  busctl=TrustedExecutable("/usr/bin/busctl",_pin(entry,"busctl"));busctl.verify()
  try:r=subprocess.run([str(busctl.path),"--system","--json=short","call","org.shadowblip.InputPlumber","/org/shadowblip/InputPlumber","org.freedesktop.DBus.ObjectManager","GetManagedObjects"],capture_output=True,timeout=10);busctl.verify()
  finally:busctl.close()
  if r.returncode!=0:raise BrokerError("cannot snapshot InputPlumber objects for cleanup authority")
  fact["inputplumber_objects_sha256"]=hashlib.sha256(r.stdout).hexdigest()
 return fact


class InputPlumberHostLock:
 """Exclusive root host lock plus pre-proxy dedicated-topology authority."""
 def __init__(self,parent,entry):
  lock_path=Path('/run/lock/controller-box-inputplumber.lock')
  self.fd=os.open(lock_path,os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW|os.O_CLOEXEC,0o600)
  i=os.fstat(self.fd)
  if i.st_uid!=0 or not stat.S_ISREG(i.st_mode) or stat.S_IMODE(i.st_mode)!=0o600:raise BrokerError('InputPlumber host lock inode is unsafe')
  try:fcntl.flock(self.fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
  except BlockingIOError:raise BrokerError('another InputPlumber capability operation holds the host lock')
  self.busctl=TrustedExecutable('/usr/bin/busctl',_pin(entry,'busctl'));self.busctl.verify()
  sudo=TrustedExecutable('/usr/bin/sudo',_pin(entry,'sudo'))
  try:
   sudo.verify();direct=subprocess.run([str(sudo.path),'-n','-u',f"#{entry['uid']}",str(self.busctl.path),'--system','call','org.shadowblip.InputPlumber','/org/shadowblip/InputPlumber','org.freedesktop.DBus.ObjectManager','GetManagedObjects'],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=10);sudo.verify();self.busctl.verify()
  finally:sudo.close()
  if direct.returncode==0 or b'AccessDenied' not in direct.stderr:raise BrokerError('runner account can connect directly to the real InputPlumber system bus')
  r=subprocess.run([str(self.busctl.path),'--system','--json=short','call','org.shadowblip.InputPlumber','/org/shadowblip/InputPlumber','org.freedesktop.DBus.ObjectManager','GetManagedObjects'],capture_output=True,timeout=10);self.busctl.verify()
  if r.returncode:raise BrokerError('cannot prove dedicated InputPlumber topology')
  try:
   doc=json.loads(r.stdout);objects=doc['data'][0]
  except (ValueError,KeyError,IndexError,TypeError):raise BrokerError('root ObjectManager topology reply is malformed')
  if doc.get('type')!='a{oa{sa{sv}}}' or not isinstance(objects,dict):raise BrokerError('root ObjectManager topology signature is wrong')
  self.objects=objects
  composites=[p for p,v in objects.items() if isinstance(v,dict) and 'org.shadowblip.Input.CompositeDevice' in v]
  targets=[p for p,v in objects.items() if isinstance(v,dict) and 'org.shadowblip.Input.Target' in v]
  approved=[]
  for vendor in Path('/sys/bus/usb/devices').glob('*/idVendor'):
   try:
    if vendor.read_text().strip().lower()=='045e' and (vendor.parent/'idProduct').read_text().strip().lower()=='028e':approved.append(str(vendor.parent.resolve()))
   except OSError:pass
  if len(composites)!=1 or targets or len(set(approved))!=1:raise BrokerError('dedicated topology requires exactly one 045e:028e physical composite and zero targets')
  self.snapshot_path=parent/'inputplumber-dedicated-topology.json';raw=json.dumps({'schema':'factory-inputplumber-dedicated-topology/v1','composite':composites[0],'targets':[],'usb_sysfs':approved[0]},sort_keys=True,separators=(',',':')).encode()+b'\n'
  out=os.open(self.snapshot_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o400);os.write(out,raw);os.fsync(out);os.close(out)
 def snapshot(self):
  self.busctl.verify();r=subprocess.run([str(self.busctl.path),'--system','--json=short','call','org.shadowblip.InputPlumber','/org/shadowblip/InputPlumber','org.freedesktop.DBus.ObjectManager','GetManagedObjects'],capture_output=True,timeout=10);self.busctl.verify()
  try:doc=json.loads(r.stdout);objects=doc['data'][0]
  except (ValueError,KeyError,IndexError,TypeError):raise BrokerError('root ObjectManager postcondition is malformed')
  if r.returncode or doc.get('type')!='a{oa{sa{sv}}}' or not isinstance(objects,dict):raise BrokerError('root ObjectManager postcondition is invalid')
  return objects
 def close(self):
  if getattr(self,'busctl',None):self.busctl.close();self.busctl=None
  if getattr(self,'fd',None) is not None:fcntl.flock(self.fd,fcntl.LOCK_UN);os.close(self.fd);self.fd=None

class InputPlumberProvenance:
 """Held host-namespace identity for the enrolled service across routing."""
 def __init__(self,parent,entry=None):
  self.busctl=TrustedExecutable("/usr/bin/busctl",_pin(entry,"busctl"))
  self.systemctl=TrustedExecutable("/usr/bin/systemctl",_pin(entry,"systemctl"))
  self.dpkg=TrustedExecutable("/usr/bin/dpkg-query",_pin(entry,"dpkg-query"))
  self.owner,self.pid=self._resolve()
  self.starttime=self._starttime(self.pid)
  self.exe_link=os.readlink(f"/proc/{self.pid}/exe")
  self.fd=os.open(f"/proc/{self.pid}/exe",os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC)
  st=os.fstat(self.fd)
  if not stat.S_ISREG(st.st_mode):raise BrokerError("InputPlumber owner executable is not regular")
  self.dev,self.ino,self.size=st.st_dev,st.st_ino,st.st_size
  self.digest=self._digest()
  package=self._host_command(self.dpkg,"-W","-f=${Package}\t${Version}\t${Status}","inputplumber").split("\t")
  service=self._host_command(self.systemctl,"show","inputplumber.service","--property=Id","--property=Type","--property=ActiveState","--property=ExecStart","--value").splitlines()
  owned=self._host_command(self.dpkg,"-S",self.exe_link)
  if len(package)!=3 or package[0]!="inputplumber" or package[2]!="install ok installed" or len(service)!=4 or service[:3]!=["inputplumber.service","dbus","active"] or self.exe_link not in service[3]:
   raise BrokerError("InputPlumber package/service provenance is not exact")
  pin=(entry or {}).get("inputplumber_pin")
  enrolled={"path":self.exe_link,"sha256":self.digest,"device":self.dev,"inode":self.ino}
  if not pin or pin.get("status")!="enrolled" or any(pin.get(k)!=v for k,v in enrolled.items()) or pin.get("package_version")!=package[1] or pin.get("service_exec_start")!=service[3]:
   raise BrokerError("InputPlumber executable/package differs from independent enrollment")
  self.package_version=package[1];self.service_exec_start=service[3]
  self.fact={"schema":"factory-host-inputplumber-provenance/v2","unique_owner":self.owner,"pid":self.pid,"starttime":self.starttime,"exe":self.exe_link,"exe_dev":self.dev,"exe_ino":self.ino,"exe_size":self.size,"exe_sha256":self.digest,"package_name":package[0],"package_version":package[1],"package_installed":True,"service_unit":service[0],"service_type":service[1],"service_active":True,"exe_owned_by_package":owned.startswith("inputplumber: "),"verified_by":"root-broker-held-proc-exe-outside-private-pids"}
  self.path=parent/"inputplumber-provenance.json"
  fd=os.open(self.path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o444)
  try:os.write(fd,(json.dumps(self.fact,sort_keys=True,separators=(",",":"))+"\n").encode());os.fsync(fd)
  finally:os.close(fd)
  self.verify()
 def _host_command(self,exe,*args):
  exe.verify();r=subprocess.run([str(exe.path),*args],capture_output=True,text=True,timeout=10);exe.verify()
  if r.returncode:raise BrokerError("cannot gather held InputPlumber host provenance")
  return r.stdout.strip()
 def _call(self,*args):
  self.busctl.verify();r=subprocess.run([str(self.busctl.path),"--system","call",*args],capture_output=True,text=True,timeout=10)
  self.busctl.verify()
  if r.returncode:raise BrokerError("cannot resolve live InputPlumber D-Bus owner")
  fields=r.stdout.strip().split(maxsplit=1)
  if len(fields)!=2:return ""
  value=fields[1].strip()
  return value[1:-1] if len(value)>=2 and value[0]=='"' and value[-1]=='"' else value
 def _resolve(self):
  owner=self._call("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","GetNameOwner","s","org.shadowblip.InputPlumber")
  if not re.fullmatch(r":[0-9]+\.[0-9]+",owner):raise BrokerError("InputPlumber has no valid unique D-Bus owner")
  raw=self._call("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","GetConnectionUnixProcessID","s",owner)
  try:pid=int(raw)
  except ValueError:raise BrokerError("InputPlumber owner PID is invalid")
  return owner,pid
 @staticmethod
 def _starttime(pid):
  raw=Path(f"/proc/{pid}/stat").read_text()
  end=raw.rfind(")")
  if end<0:raise BrokerError("InputPlumber proc stat is malformed")
  fields=raw[end+2:].split()
  if len(fields)<20:return 0
  value=int(fields[19])
  if value<=0:raise BrokerError("InputPlumber starttime is invalid")
  return value
 def _digest(self):
  h=hashlib.sha256();off=0
  while True:
   b=os.pread(self.fd,65536,off)
   if not b:return h.hexdigest()
   h.update(b);off+=len(b)
 def verify(self):
  owner,pid=self._resolve()
  try:start=self._starttime(pid);link=os.readlink(f"/proc/{pid}/exe");live=os.stat(f"/proc/{pid}/exe");held=os.fstat(self.fd)
  except OSError as exc:raise BrokerError("InputPlumber exited during provenance verification") from exc
  if (owner,pid,start)!=(self.owner,self.pid,self.starttime):raise BrokerError("InputPlumber D-Bus owner restarted or changed during routing")
  if link!=self.exe_link or (live.st_dev,live.st_ino)!=(self.dev,self.ino) or (held.st_dev,held.st_ino,held.st_size)!=(self.dev,self.ino,self.size) or self._digest()!=self.digest:
   raise BrokerError("InputPlumber executable identity changed during routing")
  package=self._host_command(self.dpkg,"-W","-f=${Version}","inputplumber")
  exec_start=self._host_command(self.systemctl,"show","inputplumber.service","--property=ExecStart","--value")
  if package!=self.package_version or exec_start!=self.service_exec_start:
   raise BrokerError("InputPlumber package or service ExecStart changed during routing")
 def close(self):
  if getattr(self,"fd",None) is not None:os.close(self.fd);self.fd=None
  self.busctl.close();self.systemctl.close();self.dpkg.close()

def target_consumer_operation(entry,parent):
 """Root-only fixed consumer check; never exposed as a runner command.

 The candidate proxy has no InputEvent rule.  This operation creates exactly
 one xb360 target, opens the independently discovered kernel consumer before
 injection, sends one pinned report, observes BTN_SOUTH, and unconditionally
 stops only the returned request target.  Its complete output is signed as
 consumer-only and is never promoted to physical/routing evidence.
 """
 busctl=TrustedExecutable("/usr/bin/busctl",_pin(entry,"busctl"));before={p.name for p in Path("/dev/input").glob("event*") if re.fullmatch(r"event[0-9]+",p.name)}
 target=None;fd=None;observed=False;started=time.monotonic_ns()
 def run(*args):
  busctl.verify();r=subprocess.run([str(busctl.path),"--system",*args],capture_output=True,text=True,timeout=10);busctl.verify()
  if r.returncode:raise BrokerError("root target-consumer operation D-Bus call failed")
  return r.stdout.strip()
 try:
  raw=run("call","org.shadowblip.InputPlumber","/org/shadowblip/InputPlumber/Manager","org.shadowblip.InputManager","CreateTargetDevice","s","xb360")
  m=re.search(r'"(/org/shadowblip/InputPlumber/[A-Za-z0-9_/]+)"',raw)
  if not m:raise BrokerError("root target-consumer CreateTargetDevice reply malformed")
  target=m.group(1);deadline=time.monotonic()+20;node=None
  while time.monotonic()<deadline:
   new=[p for p in Path("/dev/input").glob("event*") if p.name not in before and re.fullmatch(r"event[0-9]+",p.name)]
   if len(new)==1:node=new[0];break
   if len(new)>1:raise BrokerError("root target-consumer kernel target identity ambiguous")
   time.sleep(.05)
  if node is None:raise BrokerError("root target-consumer kernel node did not appear")
  fd=os.open(node,os.O_RDONLY|os.O_NONBLOCK|os.O_NOFOLLOW|os.O_CLOEXEC)
  run("call","org.shadowblip.InputPlumber",target,"org.shadowblip.Input.Target","InputEvent","say","report","20","0","0","16",*["0"]*17)
  deadline=time.monotonic()+15
  while time.monotonic()<deadline and not observed:
   try:data=os.read(fd,24*64)
   except BlockingIOError:time.sleep(.01);continue
   for off in range(0,len(data)-23,24):
    typ=int.from_bytes(data[off+16:off+18],sys.byteorder);code=int.from_bytes(data[off+18:off+20],sys.byteorder);value=int.from_bytes(data[off+20:off+24],sys.byteorder,signed=True)
    if (typ,code,value)==(1,304,1):observed=True
  if not observed:raise BrokerError("root target-consumer fixed event was not consumed")
 finally:
  if fd is not None:os.close(fd)
  if target:
   try:run("call","org.shadowblip.InputPlumber","/org/shadowblip/InputPlumber/Manager","org.shadowblip.InputManager","StopTargetDevice","s",target)
   except Exception:observed=False
  busctl.close()
 if not observed:raise BrokerError("root target-consumer cleanup or observation failed")
 record={"schema":"factory-target-consumer-operation/v1","label":"consumer-only","physical_capability":False,"routing_capability":False,"candidate_callable":False,"target":target,"device_type":"xb360","input_event":{"action":"report","data":[0,0,16]+[0]*17},"observed":{"type":1,"code":304,"value":1},"started_ns":started,"finished_ns":time.monotonic_ns(),"cleanup":True}
 raw=(json.dumps(record,sort_keys=True,separators=(",",":"))+"\n").encode();path=parent/"target-consumer-operation.json";out=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o400);os.write(out,raw);os.fsync(out);os.close(out)
 return {"path":path.name,"sha256":hashlib.sha256(raw).hexdigest(),"size":len(raw),"label":"consumer-only","candidate_callable":False,"physical_capability":False,"routing_capability":False}

def sign(evidence,entry):
 token=os.urandom(32);rfd,wfd=os.pipe();os.write(wfd,token);os.close(wfd)
 payload=(json.dumps({"schema":"factory-runner-sign-request/v1","broker_auth_sha256":hashlib.sha256(token).hexdigest(),"manifest":evidence},separators=(",",":"))+"\n").encode()
 try:r=subprocess.run([SIGNER,"--broker-fd",str(rfd)],input=payload,capture_output=True,timeout=120,pass_fds=(rfd,),env={"SUDO_UID":str(entry["uid"]),"PATH":"/usr/bin:/bin","LANG":"C.UTF-8"})
 finally:os.close(rfd)
 if r.returncode: raise BrokerError("broker-owned authenticated signing failed")
 try:return json.loads(r.stdout)
 except ValueError: raise BrokerError("broker-owned signer response malformed")

def main():
 uid=caller_uid();admission=None
 try:admission=_admit()
 except (OSError,BrokerError) as e:fail(str(e))
 try: policy=load_policy();entry=class_for_uid(policy,uid);verify_runner_groups(entry)
 except (PolicyError,BrokerError,KeyError) as e: fail(f"root runner policy rejected caller: {e}")
 try:req,prefetched=_header(sys.stdin.fileno())
 except Exception: fail("request header is invalid")
 if req.get("schema")=="factory-runner-nonce-request/v1":
  if prefetched:fail("nonce request has trailing bytes")
  try:issue_nonce(entry,req,uid);return 0
  finally:os.close(admission);admission=None
 fields={"schema","runner","class","commit","commit_object_b64","tree","environment_blob","archive_sha256","archive_size","capabilities","campaign_id","readiness_nonce","nonce","authority_sha256"}
 if not isinstance(req,dict) or set(req)!=fields or req.get("schema")!="factory-runner-request/v2":fail("request fields/schema invalid")
 if req["runner"]!=entry["name"] or req["class"]!=entry["name"] or req["capabilities"]!=sorted(entry["allowed_capabilities"]):fail("request does not equal root class policy")
 if req["authority_sha256"]!=entry["probe_authority_sha256"]:fail("request authority binding mismatch")
 if not all(SHA1.fullmatch(req[x]) for x in ("commit","tree","environment_blob")) or not NAME.fullmatch(req["campaign_id"]) or not all(SHA256.fullmatch(req[x]) for x in ("archive_sha256","readiness_nonce","nonce","authority_sha256")):fail("request identity binding invalid")
 consume_nonce(entry,req,uid);size=req["archive_size"]
 if type(size)is not int or not 0<size<=MAX_ARCHIVE:fail("archive size invalid")
 if len(prefetched)>size:fail("archive framing/digest mismatch")
 try:archive=prefetched+_read_deadline(sys.stdin.fileno(),size-len(prefetched),time.monotonic()+ARCHIVE_TIMEOUT,exact=True)
 except BrokerError as e:fail(str(e))
 if len(archive)!=size or hashlib.sha256(archive).hexdigest()!=req["archive_sha256"]:fail("archive framing/digest mismatch")
 authority=None;request_dir=None;held_all=[];host_input_lock=None
 try:
  authority=load_authority(Path(entry["probe_authority"]),entry["probe_authority_sha256"],fixture=bool(os.environ.get("FACTORY_BROKER_TEST_MODE")));verify_authority_pins(policy,entry,authority)
  contract=authority.class_contract(entry["name"])
  if sorted(contract["capabilities"])!=req["capabilities"]:raise BrokerError("probe authority capability set differs from root policy")
  workroot=Path(entry["workspace_root"]);_safe_chain(workroot,leaf="dir");request_dir=Path(tempfile.mkdtemp(prefix=f"request-{req['nonce']}-",dir=workroot));request_dir.chmod(0o700)
  host_input_lock=InputPlumberHostLock(request_dir,entry) if any(c in DBUS_CAPS for c in req["capabilities"]) else None
  # Every class that receives an InputPlumber read proxy is bound to the exact
  # root-held service identity.  In particular this avoids a provenance=None
  # GPU path while DeviceType is read from ObjectManager.
  provenance_identity=InputPlumberProvenance(request_dir,entry) if any(c in DBUS_CAPS for c in req["capabilities"]) else None
  provenance=provenance_identity.path if provenance_identity else None
  dbus_monitor=DbusMonitor(entry,request_dir) if any(c in DBUS_CAPS for c in req["capabilities"]) else None
  proxy=DbusProxy(entry,request_dir,host_input_lock,req["capabilities"],dbus_monitor) if dbus_monitor else None
  user_proxy=UserManagerProxy(entry,request_dir) if "systemd-user" in req["capabilities"] else None
  started=int(time.time());allout=b"";allerr=b"";payload=[];descriptors=[];cleanup_states=[];target_consumer_audit=None
  for index,(name,descriptor) in enumerate([("gate",contract["gate"]),*sorted(contract["capabilities"].items())]):
   runroot=request_dir/f"run-{index}";product=runroot/"source";pool=None
   runroot.mkdir(mode=0o711);product.mkdir(mode=0o700)
   pool=WritablePool(entry,runroot,uid);build,home,artifacts=pool.paths()
   extract(archive,product)
   gitpath=next((str(Path(x)/"git") for x in authority.document["trusted_path"] if (Path(x)/"git").is_file()),"")
   if not gitpath:raise BrokerError("trusted control closure has no pinned git")
   git=TrustedExecutable(gitpath,_pin(entry,"git"));env=clean_env(home,authority,product,build,artifacts)
   try:exact_tree(product,req,git,env)
   finally:git.close()
   source_before=source_digest(product)
   unit=f"factory-runner-{entry['name']}-{req['nonce'][:20]}-{index}.service";host_before=host_cleanup_snapshot(name,entry)
   if name in DBUS_CAPS:provenance_identity.verify()
   capability_proxy=user_proxy if name=="systemd-user" else proxy
   udev=UdevMonitor(request_dir,entry) if name=="controller-production-routing" else None
   try:
    if name=="target-consumer":
     target_consumer_audit=target_consumer_operation(entry,request_dir);rc,out,err=0,b"--- target-consumer capability contract ---\nPASS: broker-generated consumer-only operation semantically verified\n",b""
    else:rc,out,err=run_contained(entry,authority,descriptor,product,build,home,artifacts,env,unit,name,provenance,capability_proxy,udev)
   finally:
    if udev:udev.close()
   if name in DBUS_CAPS:provenance_identity.verify()
   if len(allout)+len(out)>MAX_LOG or len(allerr)+len(err)>MAX_LOG:raise BrokerError("aggregate contained output exceeds bound")
   allout+=out;allerr+=err
   host_after=host_cleanup_snapshot(name,entry)
   if host_after!=host_before:raise BrokerError(f"{name} capability-specific host cleanup not proven")
   cleanup_states.append({"capability":name,"before":host_before,"after":host_after})
   # Root-owned source bytes/inodes/modes are revalidated after execution.
   if source_digest(product)!=source_before:raise BrokerError("candidate source changed during execution")
   if rc:raise BrokerError(f"{name} candidate execution failed")
   if name!="gate":
    d,p=collect(artifacts,[name],{name:descriptor["artifacts"]},expected_uid=uid);held=hold(d,p,request_dir);held_all.append(held)
    analyze(entry,authority,descriptor,held,env,req["commit"],req["tree"]);descriptors.extend(d);payload.extend(held.payload)
   pool.close();pool=None
   shutil.rmtree(runroot)
  pins={name:{k:pin[k] for k in ("path","sha256","device","inode")} for name,pin in sorted(entry["executable_pins"].items())}
  dbus_audit=None
  if proxy:
   dbus_monitor.proxy_senders=dbus_monitor._proxy_senders(proxy);proxy.close();dbus_monitor.close()
   audit_paths=dbus_monitor.normalize(proxy,provenance_identity.owner,host_input_lock.objects,host_input_lock.snapshot(),request_dir,entry,req["capabilities"])
   audit_raw=audit_paths['root-dbus-audit.json'].read_bytes();audit_document=json.loads(audit_raw)
   decision_sha256,decision_count=proxy.validate_decisions(audit_document)
   decision_raw=proxy.audit.read_bytes()
   dbus_audit={"path":"root-dbus-audit.json","sha256":hashlib.sha256(audit_raw).hexdigest(),"size":len(audit_raw),"monitor_started_ns":dbus_monitor.started_ns,"proxy_started_ns":proxy.started_ns,"proxy_pid":proxy.pid,"proxy_starttime":proxy.starttime,"proxy_senders":dbus_monitor.proxy_senders,"complete":True,"overflow":False,"candidate_generated":False,"preforward_path":"root-dbus-preforward.jsonl","preforward_sha256":decision_sha256,"preforward_size":len(decision_raw),"preforward_count":decision_count}
   audit_cap='controller-production-routing' if 'controller-production-routing' in req['capabilities'] else req['capabilities'][0]
   for key in ('root-dbus-audit.json','root-dbus-before.json','root-dbus-after.json','root-dbus-contract.json','root-dbus-binding.json'):
    raw=audit_paths[key].read_bytes();rel=f"{audit_cap}/{key}";descriptors.append({"path":rel,"capability":audit_cap,"media_type":"application/json","type":"file","mode":0o600,"size":len(raw),"sha256":hashlib.sha256(raw).hexdigest()});payload.append({"path":rel,"data_b64":base64.b64encode(raw).decode()})
   rel=f"{audit_cap}/root-dbus-preforward.jsonl";descriptors.append({"path":rel,"capability":audit_cap,"media_type":"text/plain","type":"file","mode":0o600,"size":len(decision_raw),"sha256":decision_sha256});payload.append({"path":rel,"data_b64":base64.b64encode(decision_raw).decode()})
  if target_consumer_audit:
   raw=(request_dir/target_consumer_audit["path"]).read_bytes();rel="target-consumer/root-operation.json";descriptors.append({"path":rel,"capability":"target-consumer","media_type":"application/json","type":"file","mode":0o600,"size":len(raw),"sha256":hashlib.sha256(raw).hexdigest()});payload.append({"path":rel,"data_b64":base64.b64encode(raw).decode()})
  descriptors.sort(key=lambda x:x["path"]);payload.sort(key=lambda x:x["path"]);total,digest=validate_descriptors(descriptors,req["capabilities"])
  scope=hashlib.sha256(json.dumps({"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"runner":entry["name"],"commit":req["commit"],"nonce":req["nonce"],"artifact_manifest_sha256":digest},sort_keys=True,separators=(",",":")).encode()).hexdigest()
  host_authority={"executable_pins":pins,"writable_limits":{"bytes":WRITABLE_BYTES,"inodes":WRITABLE_INODES},"inputplumber_pin":entry.get("inputplumber_pin"),"dbus_audit_sha256":dbus_audit["sha256"] if dbus_audit else None,"dbus_audit_descriptor":dbus_audit,"target_consumer_operation":target_consumer_audit,"cleanup_states":cleanup_states}
  evidence={"schema":"factory-runner-receipt/v3","host_authority":host_authority,"result":"pass","runner":entry["name"],"commit":req["commit"],"tree":req["tree"],"environment_blob":req["environment_blob"],"archive_sha256":req["archive_sha256"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"nonce":req["nonce"],"authority_sha256":authority.digest,"capabilities":req["capabilities"],"exit_code":0,"timed_out":False,"started_at":started,"finished_at":int(time.time()),"cleanup":True,"stdout_sha256":hashlib.sha256(allout).hexdigest(),"stderr_sha256":hashlib.sha256(allerr).hexdigest(),"artifact_protocol":PROTOCOL,"artifact_limits":{"count":MAX_ARTIFACTS,"file_bytes":MAX_ARTIFACT_FILE,"aggregate_bytes":MAX_ARTIFACT_BYTES},"artifact_count":len(descriptors),"artifact_bytes":total,"artifact_manifest_sha256":digest,"artifact_scope_sha256":scope,"artifacts":descriptors}
  signed=sign(evidence,entry)
  emit({**evidence,"stdout_b64":base64.b64encode(allout).decode(),"stderr_b64":base64.b64encode(allerr).decode(),**{k:signed[k] for k in ("manifest_b64","signature_b64","signer_principal","signer_key_sha256","signature_algorithm","namespace","signature_sha256")},"artifact_payload":payload})
 except (AuthorityError,ArtifactError,PolicyError,BrokerError,OSError,subprocess.SubprocessError,ValueError) as e:fail(str(e))
 finally:
  if 'pool' in locals() and pool:
   try:pool.close()
   except Exception:pass
  for held in held_all:held.close()
  if authority:authority.close()
  if 'provenance_identity' in locals() and provenance_identity:provenance_identity.close()
  if 'proxy' in locals() and proxy:proxy.close()
  if 'dbus_monitor' in locals() and dbus_monitor:
   try:dbus_monitor.close(proxy if 'proxy' in locals() else None)
   except Exception:pass
  if 'user_proxy' in locals() and user_proxy:user_proxy.close()
  if host_input_lock:host_input_lock.close()
  if request_dir:shutil.rmtree(request_dir,ignore_errors=True)
  if admission is not None:os.close(admission)
 return 0
if __name__=="__main__":raise SystemExit(main())
