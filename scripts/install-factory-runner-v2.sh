#!/usr/bin/env bash
# Transactional root deployment/migration for the privileged v2 broker.
# The supplied policy is independently approved authority; repository
# enrollment JSON is only a pending request and is never installed as policy.
set -euo pipefail
[[ $(/usr/bin/id -u) -eq 0 && $# -eq 1 ]] || { echo 'usage: sudo install-factory-runner-v2.sh APPROVED-POLICY.json' >&2; exit 2; }
ROOT=$(cd -- "$(/usr/bin/dirname -- "${BASH_SOURCE[0]}")/.." && /bin/pwd -P)
POLICY=$1
TXN=$(/usr/bin/mktemp -d /var/tmp/factory-runner-install.XXXXXXXX)
BACKUP="$TXN/backup"; STAGE="$TXN/stage"; /bin/mkdir -m 0700 "$BACKUP" "$STAGE"
COMMITTED=0
rollback() {
  rc=$?; trap - EXIT INT TERM HUP
  if [[ $COMMITTED -eq 0 ]]; then
    for item in authority libexec policy sudoers; do
      marker="$BACKUP/$item.present"; target=""
      case "$item" in
        authority) target=/opt/factory-runner/authority/v1 ;;
        libexec) target=/usr/local/libexec/factory-runner-v2.bundle ;;
        policy) target=/etc/factory-runner/runner-policy.json ;;
        sudoers) target=/etc/sudoers.d/factory-runner-broker ;;
      esac
      if [[ -e "$BACKUP/$item" ]]; then /bin/rm -rf -- "$target"; /bin/mv -T -- "$BACKUP/$item" "$target"; elif [[ ! -e "$marker" ]]; then /bin/rm -rf -- "$target"; fi
    done
  fi
  /bin/rm -rf -- "$TXN"; exit "$rc"
}
trap rollback EXIT INT TERM HUP

# Complete preflight happens before touching production paths.
/usr/bin/python3 - "$ROOT" "$POLICY" "$STAGE" <<'PY'
import grp,hashlib,json,os,pathlib,pwd,shutil,stat,subprocess,sys
root=pathlib.Path(sys.argv[1]); policy_path=pathlib.Path(sys.argv[2]); stage=pathlib.Path(sys.argv[3])
def die(s): raise SystemExit('installer preflight: '+s)
def chain(p,regular=False):
 if not p.is_absolute():die(f'non-absolute trusted path: {p}')
 cur=pathlib.Path('/')
 for part in p.parts[1:]:
  cur/=part;i=os.lstat(cur)
  if stat.S_ISLNK(i.st_mode) or i.st_uid!=0 or i.st_mode&0o022:die(f'unsafe root authority chain: {cur}')
 if regular and not p.is_file():die(f'not regular: {p}')
 return p
def committed(path):
 rel=path.relative_to(root).as_posix()
 expected=subprocess.check_output(['/usr/bin/git','-C',str(root),'show',f'HEAD:{rel}'])
 actual=path.read_bytes()
 if actual!=expected:die(f'deployment input is not exact committed bytes: {rel}')
 return actual
chain(policy_path,True);pi=os.lstat(policy_path)
if not stat.S_ISREG(pi.st_mode) or stat.S_ISLNK(pi.st_mode) or pi.st_uid!=0 or pi.st_nlink!=1 or pi.st_mode&0o022:die('approved policy inode unsafe')
policy=json.loads(policy_path.read_bytes())
if policy.get('schema')!='factory-runner-policy/v2':die('wrong policy schema')
# Load through the production validator, including enrolled statuses.
sys.path.insert(0,str(root/'scripts'));import factory_runner_policy as fp
fp.DEFAULT_POLICY_PATH=policy_path;fp.load_policy()
auth=root/'deploy/factory-runner-authority-v1'; raw=committed(auth/'authority.json'); digest=hashlib.sha256(raw).hexdigest()
authority_doc=json.loads(raw)
for item in auth.rglob('*'):
 if item.is_file(): committed(item)
for rel,pin in authority_doc['files'].items():
 if hashlib.sha256((auth/rel).read_bytes()).hexdigest()!=pin:die(f'authority closure pin mismatch: {rel}')
if any(c['name']=='gpurunner' for c in policy['classes']):
 licensed=json.loads((auth/'licensed-diagram-authority.json').read_bytes());oracle=json.loads((auth/'licensed-diagram-oracle.json').read_bytes())
 if licensed.get('status')!='accepted-machine-authority' or oracle.get('authority_status') not in {'approved','enrolled'}:die('gpurunner licensed authority/oracle is pending or unapproved')
for c in policy['classes']:
 if c['probe_authority_sha256']!=digest or c['probe_authority_status']!='enrolled':die(f"class {c['name']} authority is not independently enrolled")
 account=pwd.getpwuid(c['uid'])
 if account.pw_uid!=c['uid'] or account.pw_name==c['name'] and c['name']=='dev-runner-vm':die('dev class/account mapping invalid')
 actual={grp.getgrgid(g).gr_name for g in os.getgrouplist(account.pw_name,account.pw_gid)}
 if actual!=set(c['approved_groups']) or grp.getgrgid(account.pw_gid).gr_name not in actual:die(f"runner {c['name']} has dangerous/extraneous group membership")
 if any(cap in {'inputplumber-system-dbus','target-consumer','controller-production-routing'} for cap in c['allowed_capabilities']):
  proxy=chain(pathlib.Path(c['dbus_proxy']),True);pi=proxy.stat()
  if proxy!=pathlib.Path('/usr/bin/xdg-dbus-proxy') or not pi.st_mode&0o111:die('exact D-Bus proxy prerequisite absent')
 for p in (c['workspace_root'],pathlib.Path(c['nonce_ledger']).parent):
  q=pathlib.Path(p);q.mkdir(parents=True,exist_ok=True,mode=0o700);chain(q)
for exe in ('/usr/bin/systemd-run','/usr/bin/systemctl','/usr/bin/git','/usr/bin/bash','/usr/bin/python3','/usr/bin/ssh-keygen','/usr/bin/sudo','/usr/sbin/visudo'):
 p=chain(pathlib.Path(exe),True);i=p.stat()
 if not i.st_mode&0o111:die(f'non-executable host prerequisite: {exe}')
# PrivatePIDs is mandatory; reject old systemd instead of silently weakening.
help=subprocess.check_output(['/usr/bin/systemd-run','--help'],text=True,errors='replace')
if '--property' not in help:die('systemd-run transient properties unsupported')
# Stage exact committed authority without nesting an existing v1 directory.
auth_stage=stage/'authority';shutil.copytree(auth,auth_stage,symlinks=False)
for p in auth_stage.rglob('*'):
 if p.is_symlink():die('authority contains symlink')
 os.chown(p,0,0);os.chmod(p,0o555 if p.is_dir() or os.access(p,os.X_OK) else 0o444)
lib=stage/'libexec';lib.mkdir()
for name in ('factory-runner-broker.py','factory-runner-signer.py','factory-runner-server.py','factory_runner_policy.py','factory_runner_artifacts.py','factory_runner_authority.py'):
 data=committed(root/'scripts'/name);dst=lib/name;dst.write_bytes(data);os.chown(dst,0,0);os.chmod(dst,0o700 if '-' in name and name!='factory-runner-server.py' else 0o755)
(stage/'policy').write_bytes(policy_path.read_bytes());os.chown(stage/'policy',0,0);os.chmod(stage/'policy',0o640)
# Sudoers identities are NSS account names, never class names (devrunner maps
# to dev-runner-vm by numeric UID inside the broker).
lines=[]
for c in policy['classes']:
 account=pwd.getpwuid(c['uid']).pw_name
 lines.append(f'{account} ALL=(root) NOPASSWD: /usr/local/libexec/factory-runner-broker')
(stage/'sudoers').write_text('\n'.join(lines)+'\n');os.chmod(stage/'sudoers',0o440)
subprocess.run(['/usr/sbin/visudo','-cf',str(stage/'sudoers')],check=True)
# authorized_keys keeps the key bytes and fixed server command; reject any
# direct signer/broker oracle or non-v2 original-command installation.
for c in policy['classes']:
 home=pathlib.Path(pwd.getpwuid(c['uid']).pw_dir);ak=home/'.ssh/authorized_keys'
 if ak.exists():
  text=ak.read_text()
  if 'factory-runner-signer' in text or '/factory-runner-broker' in text:die(f'direct privileged forced command in {ak}')
  if 'factory-runner-server' not in text:die(f'authorized_keys lacks fixed unprivileged server command: {ak}')
PY

/bin/mkdir -p /opt/factory-runner/authority /usr/local/libexec /etc/factory-runner /etc/sudoers.d
# Atomic exchange avoids an absent-path window when replacing a live bundle.
# After RENAME_EXCHANGE the old object occupies the staging name and is moved
# to the transaction backup; fresh installs use one same-filesystem rename.
atomic_cutover() {
  name=$1; staged=$2; target=$3
  /usr/bin/python3 - "$staged" "$target" <<'PY'
import ctypes,os,sys
src,dst=sys.argv[1:]
if os.path.lexists(dst):
 libc=ctypes.CDLL(None,use_errno=True)
 if libc.renameat2(-100,src.encode(),-100,dst.encode(),2)!=0:
  raise OSError(ctypes.get_errno(),'RENAME_EXCHANGE failed')
else: os.rename(src,dst)
PY
  if [[ -e "$staged" ]]; then /bin/mv -T -- "$staged" "$BACKUP/$name"; /usr/bin/touch "$BACKUP/$name.present"; fi
}
# Install helper bundle, authority and policy, then sudoers; compatibility
# entry points switch only after all authority exists.
atomic_cutover libexec "$STAGE/libexec" /usr/local/libexec/factory-runner-v2.bundle
atomic_cutover authority "$STAGE/authority" /opt/factory-runner/authority/v1
atomic_cutover policy "$STAGE/policy" /etc/factory-runner/runner-policy.json
atomic_cutover sudoers "$STAGE/sudoers" /etc/sudoers.d/factory-runner-broker
for name in factory-runner-broker factory-runner-signer factory-runner-server; do
 /bin/ln -sfn "factory-runner-v2.bundle/$name.py" "/usr/local/libexec/$name.new"
 /bin/mv -Tf "/usr/local/libexec/$name.new" "/usr/local/libexec/$name"
done
# Remove obsolete direct signer grant only after the broker grant is live.
/bin/rm -f /etc/sudoers.d/factory-runner-signer
/usr/sbin/visudo -cf /etc/sudoers.d/factory-runner-broker
COMMITTED=1
trap - EXIT INT TERM HUP
/bin/rm -rf -- "$TXN"
echo 'factory runner v2 transaction committed; enrollment/evidence unchanged'
