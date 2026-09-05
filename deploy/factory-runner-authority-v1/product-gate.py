#!/usr/bin/python3
"""Root-authority complete project gate in the project's pinned Nix environment."""
import os, pathlib, subprocess
root=pathlib.Path(os.environ.get("FACTORY_PRODUCT_ROOT", ""))
build=pathlib.Path(os.environ.get("FACTORY_BUILD_ROOT", ""))
if not root.is_absolute() or not (root/"scripts/verify-project.sh").is_file(): raise SystemExit("product-gate: invalid product root")
if not build.is_absolute() or build == root or root in build.parents: raise SystemExit("product-gate: invalid separate writable build root")
env={k:v for k,v in os.environ.items() if k in {"HOME","PATH","LANG","LC_ALL","NIX_REMOTE","GIT_CONFIG_NOSYSTEM","GIT_CONFIG_GLOBAL","GIT_ATTR_NOSYSTEM"}}
env["CBX_VERIFY_BUILD_DIR"]=str(build/"complete-project-gate")
env["CBX_VERIFY_INSTALL_PREFIX"]=str(build/"complete-project-install")
# verify-project is the complete pinned project verifier: authenticated Nix
# entry, full CTest discovery, packaging, visual gate, and installed smoke.
# A reduced CMake/CTest/install approximation is deliberately not accepted.
command=["/usr/bin/nix-shell","--run","./scripts/verify-project.sh"]
result=subprocess.run(command,cwd=root,env=env)
if result.returncode: raise SystemExit(result.returncode)
print("root-authority-product-gate: COMPLETE PINNED VERIFY EXECUTED NONSKIP NONSIMULATED PASS")
