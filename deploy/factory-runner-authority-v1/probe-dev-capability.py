#!/usr/bin/python3
"""Root-authority dev capability semantics; no generic gate substitution."""
import hashlib,json,os,pathlib,subprocess,sys
if len(sys.argv)!=2: raise SystemExit("usage: probe-dev-capability.py CAPABILITY")
cap=sys.argv[1]
root=pathlib.Path(os.environ["FACTORY_PRODUCT_ROOT"]);build=pathlib.Path(os.environ["FACTORY_BUILD_ROOT"])
env={k:v for k,v in os.environ.items() if k in {"HOME","PATH","LANG","LC_ALL","NIX_REMOTE","GIT_CONFIG_NOSYSTEM","GIT_CONFIG_GLOBAL","GIT_ATTR_NOSYSTEM"}}
if cap=="remote-project-gate":
 env["CBX_VERIFY_BUILD_DIR"]=str(build/"remote-complete-project-gate");env["CBX_VERIFY_INSTALL_PREFIX"]=str(build/"remote-complete-project-install")
 commands=[["/usr/bin/nix-shell","--run","./scripts/verify-project.sh"]]
elif cap=="systemd-user":
 commands=[["/usr/bin/systemd-run","--user","--quiet","--wait","--pipe","--collect","--service-type=exec","/usr/bin/printf","factory-systemd-user-ok"]]
elif cap=="kernel-uinput":
 commands=[["/usr/bin/nix-shell","--run",f"cmake -S . -B {build}/uinput -DCMAKE_BUILD_TYPE=Debug && cmake --build {build}/uinput --parallel && ctest --test-dir {build}/uinput --no-tests=error -R '^test_kernel_controller$' --output-on-failure"]]
elif cap=="installed-package":
 commands=[["/usr/bin/nix-shell","--run",f"cmake -S . -B {build}/package -DCMAKE_BUILD_TYPE=Debug && cmake --build {build}/package --parallel && CBX_REQUIRE_FLATPAK=1 ./tests/test_packaging.sh {build}/package && ctest --test-dir {build}/package --no-tests=error -R '^test_installed_smoke$' --output-on-failure"]]
else: raise SystemExit("unapproved dev capability")
for command in commands:
 r=subprocess.run(command,env=env,cwd=root)
 if r.returncode: raise SystemExit(r.returncode)
out=pathlib.Path(os.environ["FACTORY_RUNNER_ARTIFACT_DIR"]);out.mkdir(mode=0o700,parents=True,exist_ok=True)
marker={"schema":"factory-capability-semantics/v2","capability":cap,"must_execute":True,"executed":True,"must_not_skip":True,"skipped":False,"deny_simulated":True,"simulated":False,"complete_project_gate":cap=="remote-project-gate","command_sha256":hashlib.sha256(json.dumps(commands,separators=(",",":" )).encode()).hexdigest()}
(out/"authority-result.json").write_text(json.dumps(marker,sort_keys=True)+"\n");(out/"authority-result.json").chmod(0o600)
print(f"root-authority-{cap}: EXECUTED NONSKIP NONSIMULATED PASS")
