#!/usr/bin/env python3
"""Privileged, fail-closed factory runner broker.

Only this root process accepts candidate bytes.  Candidate source is retained in
root-owned read-only storage; the runner receives distinct writable home/build/
output mounts.  Raw output is copied once into root-owned held files before
semantic analysis and those same bytes are described, exported, and signed.
"""
from __future__ import annotations
import base64, hashlib, io, json, os, re, shutil, stat, subprocess, sys, tarfile, tempfile, time
from pathlib import Path, PurePosixPath
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from factory_runner_policy import PolicyError, authority_pin, class_for_uid, load_policy
from factory_runner_authority import AuthorityError, load_authority
from factory_runner_artifacts import (ArtifactError, PROTOCOL, MAX_ARTIFACTS,
    MAX_ARTIFACT_FILE, MAX_ARTIFACT_BYTES, collect, descriptors_digest, hold)

SHA1=re.compile(r"^[0-9a-f]{40}$"); SHA256=re.compile(r"^[0-9a-f]{64}$"); NAME=re.compile(r"^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$")
MAX_HEADER=65536; MAX_ARCHIVE=128*1024*1024; MAX_FILES=10000; MAX_CONTENT=256*1024*1024; MAX_LOG=4*1024*1024
NONCE_TTL=900; NONCE_OUTSTANDING=32; NONCE_TOTAL=4096; NONCE_RATE=8; NONCE_RATE_WINDOW=60
SIGNER="/usr/local/libexec/factory-runner-signer"

class BrokerError(RuntimeError): pass

def fail(msg):
 print(json.dumps({"schema":"factory-runner-error/v1","result":"fail","error":msg},sort_keys=True,separators=(",",":")))
 raise SystemExit(1)
def emit(v): print(json.dumps(v,sort_keys=True,separators=(",",":")),flush=True)
def caller_uid():
 try: uid=int(os.environ["SUDO_UID"])
 except (KeyError,ValueError): fail("broker requires an exact sudo caller UID")
 if uid<=0 or os.getuid()!=0 or os.geteuid()!=0: fail("broker must run as root for a non-root sudo caller")
 return uid

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
 def __init__(self,path:str):
  self.path=Path(path); _safe_chain(self.path)
  self.fd=os.open(self.path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC); self.info=os.fstat(self.fd)
  self.digest=self._digest()
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
  for name in ("issued","consumed"):
   try: os.mkdir(name,0o700,dir_fd=rfd)
   except FileExistsError: pass
  ifd=os.open("issued",os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=rfd)
  cfd=os.open("consumed",os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=rfd)
  for fd in (ifd,cfd):
   i=os.fstat(fd)
   if i.st_uid!=0 or i.st_mode&0o077: raise BrokerError("nonce ledger child is unsafe")
  return rfd,ifd,cfd
 except Exception: os.close(rfd); raise

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
 if set(req)!={"schema","runner","campaign_id","readiness_nonce"} or req.get("schema")!="factory-runner-nonce-request/v1": fail("nonce request is invalid")
 if req["runner"]!=entry["name"] or not NAME.fullmatch(req["campaign_id"]) or not SHA256.fullmatch(req["readiness_nonce"]): fail("nonce request binding is invalid")
 now=int(time.time()); nonce=hashlib.sha256(os.urandom(64)).hexdigest()
 try:
  rfd,ifd,cfd=_ledger(entry)
  try:
   _gc_and_bound(ifd,cfd,uid,now)
   fd=os.open(nonce,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=ifd)
   record={"uid":uid,"runner":entry["name"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"issued_at":now}
   os.write(fd,(json.dumps(record,sort_keys=True)+"\n").encode());os.fsync(fd);os.close(fd);os.fsync(ifd)
  finally: os.close(cfd);os.close(ifd);os.close(rfd)
 except (OSError,ValueError,BrokerError) as e: fail(str(e))
 emit({"schema":"factory-runner-nonce/v1","nonce":nonce,"runner":entry["name"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"]})

def consume_nonce(entry,request,uid):
 try:
  rfd,ifd,cfd=_ledger(entry)
  try:
   fd=os.open(request["nonce"],os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=ifd); i=os.fstat(fd); raw=os.read(fd,4097);os.close(fd)
   if not stat.S_ISREG(i.st_mode) or i.st_uid!=0 or i.st_nlink!=1 or stat.S_IMODE(i.st_mode)!=0o600: raise BrokerError("nonce inode is unsafe")
   record=json.loads(raw); now=int(time.time())
   expected={"uid":uid,"runner":entry["name"],"campaign_id":request["campaign_id"],"readiness_nonce":request["readiness_nonce"]}
   if any(record.get(k)!=v for k,v in expected.items()) or now-record.get("issued_at",0)>NONCE_TTL or record.get("issued_at",0)>now: raise BrokerError("nonce issuance binding is stale or mismatched")
   # renameat is the one atomic state transition; destination pre-existence is replay.
   os.rename(request["nonce"],request["nonce"],src_dir_fd=ifd,dst_dir_fd=cfd);os.fsync(ifd);os.fsync(cfd)
  finally: os.close(cfd);os.close(ifd);os.close(rfd)
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
 devices={"kernel-uinput":["/dev/uinput rw"],"physical-controller":["char-input r"],"target-consumer":["char-input r"],"controller-production-routing":["char-input rw","/dev/uinput rw"],"gpu-compositor":["char-drm rw"],"installed-licensed-diagram":["char-drm rw"]}.get(capability,[])
 return (["PrivateDevices=yes"] if not devices else ["PrivateDevices=no","DevicePolicy=closed",*[f"DeviceAllow={x}" for x in devices]])

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

def run_contained(entry,authority,descriptor,host_product,host_build,host_home,host_artifacts,env,unit,capability,provenance=None):
 systemd=TrustedExecutable(entry["systemd_run"]);systemctl=TrustedExecutable(entry["systemctl"])
 sandbox_product=Path("/run/factory/product");sandbox_build=Path("/run/factory/build");sandbox_home=Path("/run/factory/home");sandbox_output=Path("/run/factory/output")
 senv=clean_env(sandbox_home,authority,sandbox_product,sandbox_build,sandbox_output)
 if capability!="gate":
  senv["FACTORY_RUNNER_ARTIFACT_DIR"]=str(sandbox_output/capability)
  senv["CONTROLLER_PRODUCTION_ROUTING_ARTIFACTS"]=str(sandbox_output/capability)
  senv["CBX_GPU_PROBE_ARTIFACTS"]=str(sandbox_output/capability)
 argv=argv_for(authority,descriptor["argv"],sandbox_product,sandbox_output);command_exe=TrustedExecutable(argv[0])
 blocked_paths=["/root","/home","/run/user","/run/credentials","/run/systemd/private","/run/systemd/users","/run/systemd/seats","/run/systemd/sessions","/etc/ssh","/etc/sudoers","/etc/sudoers.d","/etc/factory-runner","/opt/factory-runner","/workspace"]
 if capability not in {"inputplumber-system-dbus","target-consumer","controller-production-routing"}: blocked_paths.append("/run/dbus")
 blocked=" ".join(x for x in blocked_paths if Path(x).exists())
 props=["NoNewPrivileges=yes","CapabilityBoundingSet=","AmbientCapabilities=","ProtectSystem=strict","ProtectHome=yes",f"InaccessiblePaths={blocked}","PrivateTmp=yes","PrivateMounts=yes","PrivatePIDs=yes","PrivateIPC=yes","ProtectKernelTunables=yes","ProtectKernelModules=yes","ProtectKernelLogs=yes","ProtectControlGroups=yes","ProtectClock=yes","LockPersonality=yes","RestrictSUIDSGID=yes","RestrictRealtime=yes","RestrictNamespaces=yes","SystemCallArchitectures=native","SystemCallFilter=@system-service @resources","SystemCallErrorNumber=EPERM","RestrictAddressFamilies=AF_UNIX","IPAddressDeny=any","KillMode=control-group","TasksMax=512","MemoryMax=4G","CPUQuota=400%","RuntimeMaxSec=7200","TimeoutStopSec=10","LimitFSIZE=50331648",f"BindReadOnlyPaths={host_product}:{sandbox_product}",f"BindPaths={host_build}:{sandbox_build}",f"BindPaths={host_home}:{sandbox_home}",f"BindPaths={host_artifacts}:{sandbox_output}",f"WorkingDirectory={sandbox_product}",*_device_properties(capability)]
 if provenance: props.append(f"BindReadOnlyPaths={provenance}:/run/factory/inputplumber-provenance.json");senv["FACTORY_INPUTPLUMBER_PROVENANCE"]="/run/factory/inputplumber-provenance.json"
 cmd=[str(systemd.path),"--quiet","--wait","--pipe","--collect","--unit",unit,"--service-type=exec",f"--uid={entry['uid']}",*[f"--property={p}" for p in props],*[f"--setenv={k}={v}" for k,v in sorted(senv.items())],*argv]
 # systemd-run receives a scrubbed environment; candidate-controlled variables never cross.
 try:
  systemd.verify();command_exe.verify();r=subprocess.run(cmd,cwd="/",env=senv,capture_output=True,timeout=7300);command_exe.verify()
 except (OSError,subprocess.TimeoutExpired) as e:
  r=subprocess.CompletedProcess(cmd,124,b"",str(e).encode())
 finally:
  clean=_cleanup_unit(systemctl,unit,entry["cgroup_root"])
  systemd.close();systemctl.close();command_exe.close()
 if not clean: raise BrokerError("cannot prove unconditional request unit/cgroup cleanup")
 if len(r.stdout)>MAX_LOG or len(r.stderr)>MAX_LOG: raise BrokerError("contained output exceeds bound")
 return r.returncode,r.stdout,r.stderr

def analyze(authority,descriptor,held,env,commit,tree):
 authority.revalidate();argv=argv_for(authority,descriptor["analyzer_argv"],Path("/noncandidate"),held.root,commit,tree);exe=TrustedExecutable(argv[0])
 try:
  exe.verify();r=subprocess.run(argv,cwd="/",env=env,capture_output=True,timeout=300);exe.verify()
 finally:exe.close()
 authority.revalidate()
 if r.returncode or len(r.stdout)>MAX_LOG or len(r.stderr)>MAX_LOG: raise BrokerError("root authority semantic analyzer rejected held artifact bytes")

def verify_authority_pins(policy,entry,authority):
 if entry.get("probe_authority_status")!="enrolled": raise BrokerError("probe authority is pending or unapproved")
 if hashlib.sha256((json.dumps(authority.document,sort_keys=True,indent=2)+"\n").encode()).hexdigest()!=entry["probe_authority_sha256"]: raise BrokerError("held probe authority bytes differ from enrolled pin")
 if entry["name"]=="gpurunner":
  licensed_bytes=authority.bytes("licensed-diagram-authority.json");oracle_bytes=authority.bytes(authority.document["licensed_oracle"]["path"])
  licensed=hashlib.sha256(licensed_bytes).hexdigest();oracle=hashlib.sha256(oracle_bytes).hexdigest()
  if licensed!=authority_pin(policy,entry["name"],"installed-licensed-diagram") or oracle!=authority_pin(policy,entry["name"],"gpu-compositor-layout-oracle"): raise BrokerError("licensed authority/oracle exact bytes differ from root enrollment")
  try: licensed_doc=json.loads(licensed_bytes);oracle_doc=json.loads(oracle_bytes)
  except ValueError: raise BrokerError("licensed authority/oracle is malformed")
  if licensed_doc.get("status")!="accepted-machine-authority" or oracle_doc.get("authority_status") not in {"approved","enrolled"}:
   raise BrokerError("licensed authority/oracle status is pending or unapproved")

def host_cleanup_snapshot(capability):
 """Independent capability-specific host state used as cleanup postcondition."""
 fact={"capability":capability}
 if capability in {"inputplumber-system-dbus","physical-controller","target-consumer","controller-production-routing","kernel-uinput"}:
  fact["input_nodes"]=sorted(p.name for p in Path("/sys/class/input").glob("event*") if re.fullmatch(r"event[0-9]+",p.name))
 if capability in {"gpu-compositor","installed-licensed-diagram"}:
  fact["dri_nodes"]=sorted(p.name for p in Path("/dev/dri").glob("*")) if Path("/dev/dri").exists() else []
 if capability in {"inputplumber-system-dbus","target-consumer","controller-production-routing"}:
  try:
   r=subprocess.run(["/usr/bin/busctl","--system","--json=short","call","org.shadowblip.InputPlumber","/org/shadowblip/InputPlumber","org.freedesktop.DBus.ObjectManager","GetManagedObjects"],capture_output=True,timeout=10)
   fact["inputplumber_objects_sha256"]=hashlib.sha256(r.stdout).hexdigest() if r.returncode==0 else None
  except Exception: fact["inputplumber_objects_sha256"]=None
 return fact


def inputplumber_provenance(parent):
 """Privileged host-PID fact, gathered outside PrivatePIDs."""
 busctl=shutil.which("busctl",path="/usr/bin:/bin")
 if not busctl:return None
 try:
  r=subprocess.run([busctl,"--system","call","org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","GetConnectionUnixProcessID","s","org.shadowblip.InputPlumber"],capture_output=True,text=True,timeout=10)
  pid=int(r.stdout.split()[-1]);exe=os.readlink(f"/proc/{pid}/exe");st=os.stat(exe)
  fact={"schema":"factory-host-inputplumber-provenance/v1","pid":pid,"exe":exe,"exe_dev":st.st_dev,"exe_ino":st.st_ino,"exe_sha256":hashlib.sha256(Path(exe).read_bytes()).hexdigest(),"verified_by":"root-broker-outside-private-pids"}
 except Exception:return None
 p=parent/"inputplumber-provenance.json";p.write_text(json.dumps(fact,sort_keys=True)+"\n");p.chmod(0o400);return p

def sign(evidence,entry):
 token=os.urandom(32);rfd,wfd=os.pipe();os.write(wfd,token);os.close(wfd)
 payload=(json.dumps({"schema":"factory-runner-sign-request/v1","broker_auth_sha256":hashlib.sha256(token).hexdigest(),"manifest":evidence},separators=(",",":"))+"\n").encode()
 try:r=subprocess.run([SIGNER,"--broker-fd",str(rfd)],input=payload,capture_output=True,timeout=120,pass_fds=(rfd,),env={"SUDO_UID":str(entry["uid"]),"PATH":"/usr/bin:/bin","LANG":"C.UTF-8"})
 finally:os.close(rfd)
 if r.returncode: raise BrokerError("broker-owned authenticated signing failed")
 try:return json.loads(r.stdout)
 except ValueError: raise BrokerError("broker-owned signer response malformed")

def main():
 uid=caller_uid()
 try: policy=load_policy();entry=class_for_uid(policy,uid)
 except PolicyError as e: fail(f"root runner policy rejected caller: {e}")
 try:req=json.loads(sys.stdin.buffer.readline(MAX_HEADER+1))
 except Exception: fail("request header is invalid")
 if req.get("schema")=="factory-runner-nonce-request/v1":issue_nonce(entry,req,uid);return 0
 fields={"schema","runner","class","commit","commit_object_b64","tree","environment_blob","archive_sha256","archive_size","capabilities","campaign_id","readiness_nonce","nonce","authority_sha256"}
 if not isinstance(req,dict) or set(req)!=fields or req.get("schema")!="factory-runner-request/v2":fail("request fields/schema invalid")
 if req["runner"]!=entry["name"] or req["class"]!=entry["name"] or req["capabilities"]!=sorted(entry["allowed_capabilities"]):fail("request does not equal root class policy")
 if req["authority_sha256"]!=entry["probe_authority_sha256"]:fail("request authority binding mismatch")
 if not all(SHA1.fullmatch(req[x]) for x in ("commit","tree","environment_blob")) or not NAME.fullmatch(req["campaign_id"]) or not all(SHA256.fullmatch(req[x]) for x in ("archive_sha256","readiness_nonce","nonce","authority_sha256")):fail("request identity binding invalid")
 consume_nonce(entry,req,uid);size=req["archive_size"]
 if type(size)is not int or not 0<size<=MAX_ARCHIVE:fail("archive size invalid")
 archive=sys.stdin.buffer.read(size+1)
 if len(archive)!=size or hashlib.sha256(archive).hexdigest()!=req["archive_sha256"]:fail("archive framing/digest mismatch")
 authority=None;request_dir=None;held_all=[]
 try:
  authority=load_authority(Path(entry["probe_authority"]),entry["probe_authority_sha256"],fixture=bool(os.environ.get("FACTORY_BROKER_TEST_MODE")));verify_authority_pins(policy,entry,authority)
  contract=authority.class_contract(entry["name"])
  if sorted(contract["capabilities"])!=req["capabilities"]:raise BrokerError("probe authority capability set differs from root policy")
  workroot=Path(entry["workspace_root"]);_safe_chain(workroot,leaf="dir");request_dir=Path(tempfile.mkdtemp(prefix=f"request-{req['nonce']}-",dir=workroot));request_dir.chmod(0o700)
  provenance=inputplumber_provenance(request_dir) if entry["name"]=="iprunner" else None
  started=int(time.time());allout=b"";allerr=b"";payload=[];descriptors=[]
  for index,(name,descriptor) in enumerate([("gate",contract["gate"]),*sorted(contract["capabilities"].items())]):
   runroot=request_dir/f"run-{index}";product=runroot/"source";build=runroot/"build";home=runroot/"home";artifacts=runroot/"output"
   runroot.mkdir(mode=0o711);product.mkdir(mode=0o700)
   for p in (build,home,artifacts):p.mkdir(mode=0o700);os.chown(p,uid,uid)
   extract(archive,product)
   gitpath=next((str(Path(x)/"git") for x in authority.document["trusted_path"] if (Path(x)/"git").is_file()),"")
   if not gitpath:raise BrokerError("trusted control closure has no pinned git")
   git=TrustedExecutable(gitpath);env=clean_env(home,authority,product,build,artifacts)
   try:exact_tree(product,req,git,env)
   finally:git.close()
   source_before=source_digest(product)
   unit=f"factory-runner-{entry['name']}-{req['nonce'][:20]}-{index}.service";host_before=host_cleanup_snapshot(name)
   rc,out,err=run_contained(entry,authority,descriptor,product,build,home,artifacts,env,unit,name,provenance);allout+=out;allerr+=err
   if host_cleanup_snapshot(name)!=host_before:raise BrokerError(f"{name} capability-specific host cleanup not proven")
   # Root-owned source bytes/inodes/modes are revalidated after execution.
   if source_digest(product)!=source_before:raise BrokerError("candidate source changed during execution")
   if rc:raise BrokerError(f"{name} candidate execution failed")
   if name!="gate":
    d,p=collect(artifacts,[name],{name:descriptor["artifacts"]},expected_uid=uid);held=hold(d,p,request_dir);held_all.append(held)
    analyze(authority,descriptor,held,env,req["commit"],req["tree"]);descriptors.extend(d);payload.extend(held.payload)
   shutil.rmtree(runroot)
  descriptors.sort(key=lambda x:x["path"]);payload.sort(key=lambda x:x["path"]);total=sum(x["size"] for x in descriptors);digest=descriptors_digest(descriptors)
  scope=hashlib.sha256(json.dumps({"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"runner":entry["name"],"commit":req["commit"],"nonce":req["nonce"],"artifact_manifest_sha256":digest},sort_keys=True,separators=(",",":")).encode()).hexdigest()
  evidence={"schema":"factory-runner-receipt/v3","result":"pass","runner":entry["name"],"commit":req["commit"],"tree":req["tree"],"environment_blob":req["environment_blob"],"archive_sha256":req["archive_sha256"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"nonce":req["nonce"],"authority_sha256":authority.digest,"capabilities":req["capabilities"],"exit_code":0,"timed_out":False,"started_at":started,"finished_at":int(time.time()),"cleanup":True,"stdout_sha256":hashlib.sha256(allout).hexdigest(),"stderr_sha256":hashlib.sha256(allerr).hexdigest(),"artifact_protocol":PROTOCOL,"artifact_limits":{"count":MAX_ARTIFACTS,"file_bytes":MAX_ARTIFACT_FILE,"aggregate_bytes":MAX_ARTIFACT_BYTES},"artifact_count":len(descriptors),"artifact_bytes":total,"artifact_manifest_sha256":digest,"artifact_scope_sha256":scope,"artifacts":descriptors}
  signed=sign(evidence,entry)
  emit({**evidence,"stdout_b64":base64.b64encode(allout).decode(),"stderr_b64":base64.b64encode(allerr).decode(),**{k:signed[k] for k in ("manifest_b64","signature_b64","signer_principal","signer_key_sha256","signature_algorithm","namespace","signature_sha256")},"artifact_payload":payload})
 except (AuthorityError,ArtifactError,PolicyError,BrokerError,OSError,subprocess.SubprocessError,ValueError) as e:fail(str(e))
 finally:
  for held in held_all:held.close()
  if authority:authority.close()
  if request_dir:shutil.rmtree(request_dir,ignore_errors=True)
 return 0
if __name__=="__main__":raise SystemExit(main())
