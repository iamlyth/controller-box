"""factory-runner-policy.py — root-configured runner class policy (shared).

The runner protocol is class-based. A class is a root-configured mapping of a
dedicated unprivileged account to its workspace root, its approved project
verifier argv, and the exact capability allowlist it may claim. The root
endpoint (factory-runner-server.py) and the root-owned signer
(factory-runner-signer.py) both derive every authorization decision from this
policy; neither hardcodes product names, verifier paths, capability names, or
workspace roots.

The policy file lives out-of-tree at /etc/factory-runner/runner-policy.json
and is root-owned and runner-unreadable-for-write. Environment overrides exist
only for the disposable test harness; sudo resets the environment in
production, so the installed defaults always apply there. A runner can never
invent a class, widen its allowlist, or swap its verifier: the executing UID
is bound to exactly one class and the requested capability set must equal the
class allowlist exactly.
"""

from __future__ import annotations

import json
import os
import re
import stat
from pathlib import Path

DEFAULT_POLICY_PATH = Path(
    os.environ.get("FACTORY_RUNNER_POLICY", "/etc/factory-runner/runner-policy.json")
)
NAME = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
TOKEN = re.compile(r"^[^\x00-\x1f\x7f]{1,128}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
PIN_SCOPES = {"installed-licensed-diagram", "gpu-compositor-layout-oracle"}
POLICY_SCHEMA = "factory-runner-policy/v3"
PIN_FIELDS = {"path", "sha256", "device", "inode", "status"}
REQUIRED_EXECUTABLES = {"systemd-run", "systemctl", "xdg-dbus-proxy", "git", "bash", "python3", "ssh-keygen", "sudo", "busctl", "mount", "umount", "udevadm", "stdbuf", "dpkg-query", "inputplumber-mediator"}
REQUIRED_CLASSES = {"dev-runner-vm", "iprunner", "gpurunner"}


class PolicyError(Exception):
    """The runner policy is absent, unsafe, or structurally invalid."""


def policy_path() -> Path:
    return DEFAULT_POLICY_PATH


def validate_argv(argv: object, where: str) -> None:
    if (
        not isinstance(argv, list) or not argv
        or not all(isinstance(item, str) and item for item in argv)
    ):
        raise PolicyError(f"{where} verify_argv must be a non-empty array of strings")
    if any(any(ord(char) < 32 for char in item) for item in argv):
        raise PolicyError(f"{where} verify_argv must be control-character-free")
    for item in argv:
        if not TOKEN.fullmatch(item):
            raise PolicyError(f"{where} verify_argv contains an invalid token")
    first = argv[0]
    if "/" in first:
        if first.startswith("/"):
            parts = Path(first).parts
            if len(parts) < 2 or parts[1] not in {"bin", "usr"} or ".." in parts:
                raise PolicyError(f"{where} verify_argv[0] must be a /bin|/usr binary")
        else:
            parts = Path(first).parts
            if ".." in parts or len(parts) != 2 or parts[0] not in {"scripts", "tests"}:
                raise PolicyError(f"{where} verify_argv[0] must be a scripts/tests path")
    elif not NAME.fullmatch(first):
        raise PolicyError(f"{where} verify_argv[0] is not a bare command name")


def validate_capabilities(capabilities: object, where: str) -> None:
    if (
        not isinstance(capabilities, list) or not capabilities
        or len(capabilities) != len(set(capabilities))
        or not all(isinstance(item, str) and NAME.fullmatch(item) for item in capabilities)
    ):
        raise PolicyError(f"{where} allowed_capabilities must be a non-empty unique name list")


def load_policy() -> dict:
    """Load and strictly validate the root-configured runner policy.

    Returns the validated policy dict. Every structural property is checked so
    a malformed or tampered policy fails closed everywhere it is consumed.
    """
    path = policy_path()
    try:
        fixture = "FACTORY_RUNNER_POLICY" in os.environ
        expected_owner = os.getuid() if fixture else 0
        boundary = path.parent if fixture else Path("/")
        current = boundary
        components = [boundary] if fixture else [Path("/")]
        relative = path.relative_to(boundary)
        components += [boundary.joinpath(*relative.parts[:i]) for i in range(1, len(relative.parts)+1)]
        for component in components:
            ci = os.lstat(component)
            if stat.S_ISLNK(ci.st_mode) or ci.st_uid != expected_owner or ci.st_mode & 0o022:
                raise PolicyError(f"runner policy ancestor ownership/type/mode is unsafe: {component}")
        named = os.lstat(path)
        fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
        opened = os.fstat(fd)
        fixture_owner = expected_owner
        if ((named.st_dev, named.st_ino) != (opened.st_dev, opened.st_ino)
                or not stat.S_ISREG(opened.st_mode) or opened.st_uid != fixture_owner
                or opened.st_nlink != 1 or stat.S_IMODE(opened.st_mode) not in (0o400, 0o440, 0o444, 0o600, 0o640, 0o644)
                or (opened.st_mode & 0o022)):
            os.close(fd)
            raise PolicyError(f"runner policy ownership/mode/inode is unsafe: {path}")
        with os.fdopen(fd, "r", encoding="utf-8") as stream:
            raw = stream.read(1024 * 1024 + 1)
        final = os.lstat(path)
        if len(raw) > 1024 * 1024 or (final.st_dev, final.st_ino) != (opened.st_dev, opened.st_ino):
            raise PolicyError(f"runner policy changed or exceeded bounds: {path}")
        data = json.loads(raw)
    except OSError as exc:
        raise PolicyError(f"cannot read runner policy {path}: {type(exc).__name__}") from exc
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise PolicyError(f"runner policy is invalid JSON: {path}") from exc
    expected = {"schema", "namespace", "classes", "authority_pins"}
    if not isinstance(data, dict) or set(data) != expected:
        raise PolicyError("runner policy top-level fields are invalid")
    if data.get("schema") != POLICY_SCHEMA:
        raise PolicyError(f"runner policy schema is not {POLICY_SCHEMA}")
    namespace = data.get("namespace")
    if namespace != "factory-runner-receipt":
        raise PolicyError("runner policy namespace is not the canonical signature namespace")
    classes = data.get("classes")
    if not isinstance(classes, list) or not classes:
        raise PolicyError("runner policy must declare at least one class")
    pins = data.get("authority_pins")
    if not isinstance(pins, list):
        raise PolicyError("runner policy authority_pins must be an array")
    seen_pins: set[tuple[str, str]] = set()
    for index, pin in enumerate(pins):
        if (not isinstance(pin, dict) or set(pin) != {"class", "scope", "authority_sha256", "status"}
                or not isinstance(pin.get("class"), str) or not NAME.fullmatch(pin["class"])
                or pin.get("scope") not in PIN_SCOPES
                or not isinstance(pin.get("authority_sha256"), str) or not SHA256.fullmatch(pin["authority_sha256"])
                or pin.get("status") not in {"enrolled", "pending-human-review"}):
            raise PolicyError(f"runner policy authority_pins[{index}] is invalid")
        identity = (pin["class"], pin["scope"])
        if identity in seen_pins:
            raise PolicyError("runner policy authority pin is duplicated")
        seen_pins.add(identity)
    seen_names: set[str] = set()
    seen_uids: set[int] = set()
    for index, entry in enumerate(classes):
        if not isinstance(entry, dict):
            raise PolicyError(f"runner policy classes[{index}] is not an object")
        fields = {
            "name", "uid", "workspace_root", "allowed_capabilities",
            "broker_helper", "probe_authority", "probe_authority_sha256", "probe_authority_status",
            "signer_key", "signer_principal_file", "nonce_ledger",
            "systemd_run", "systemctl", "cgroup_root", "dbus_proxy", "approved_groups",
            "executable_pins", "inputplumber_pin",
        }
        if set(entry) != fields:
            raise PolicyError(f"runner policy classes[{index}] fields are invalid")
        name = entry["name"]
        if not isinstance(name, str) or not NAME.fullmatch(name):
            raise PolicyError(f"runner policy classes[{index}].name is invalid")
        if name in seen_names:
            raise PolicyError(f"runner policy declares duplicate class {name}")
        uid = entry["uid"]
        if not isinstance(uid, int) or isinstance(uid, bool) or uid <= 0:
            raise PolicyError(f"runner policy classes[{index}].uid is invalid")
        if uid in seen_uids:
            raise PolicyError(f"runner policy declares duplicate uid {uid}")
        workspace_root = entry["workspace_root"]
        if (
            not isinstance(workspace_root, str) or not workspace_root.startswith("/")
            or ".." in Path(workspace_root).parts
            or workspace_root == "/"
        ):
            raise PolicyError(f"runner policy classes[{index}].workspace_root is invalid")
        validate_capabilities(entry["allowed_capabilities"], f"classes[{index}]")
        if not isinstance(entry["probe_authority_sha256"], str) or not SHA256.fullmatch(entry["probe_authority_sha256"]):
            raise PolicyError(f"runner policy classes[{index}].probe_authority_sha256 is invalid")
        if entry["probe_authority_status"] != "enrolled":
            raise PolicyError(f"runner policy classes[{index}] probe authority is pending or unapproved")
        epins=entry["executable_pins"]
        if not isinstance(epins,dict) or set(epins)!=REQUIRED_EXECUTABLES:
            raise PolicyError(f"runner policy classes[{index}].executable_pins is incomplete")
        for key,pin in epins.items():
            if (not isinstance(pin,dict) or set(pin)!=PIN_FIELDS or pin.get("status")!="enrolled"
                    or not isinstance(pin.get("path"),str) or not pin["path"].startswith("/")
                    or not SHA256.fullmatch(str(pin.get("sha256","")))
                    or type(pin.get("device")) is not int or type(pin.get("inode")) is not int):
                raise PolicyError(f"runner policy executable pin {key} is invalid or pending")
        ipin=entry["inputplumber_pin"]
        ipin_fields=PIN_FIELDS|{"package_version","service_exec_start"}
        needs_inputplumber = any(cap in {"inputplumber-system-dbus","target-consumer","controller-production-routing","gpu-compositor","installed-licensed-diagram"} for cap in entry["allowed_capabilities"])
        if needs_inputplumber and (not isinstance(ipin,dict) or set(ipin)!=ipin_fields or ipin.get("status")!="enrolled" or not isinstance(ipin.get("package_version"),str) or not isinstance(ipin.get("service_exec_start"),str)):
            raise PolicyError("InputPlumber package/binary enrollment is absent or pending")
        if not needs_inputplumber and ipin is not None:
            raise PolicyError("InputPlumber pin belongs only to a service-consuming class")
        groups=entry["approved_groups"]
        if (not isinstance(groups,list) or not groups or len(groups)!=len(set(groups))
                or not all(isinstance(g,str) and NAME.fullmatch(g) for g in groups)):
            raise PolicyError(f"runner policy classes[{index}].approved_groups is invalid")
        for field in ("broker_helper", "probe_authority", "signer_key", "signer_principal_file", "nonce_ledger", "systemd_run", "systemctl", "cgroup_root", "dbus_proxy"):
            value = entry[field]
            if (
                not isinstance(value, str) or not value.startswith("/")
                or ".." in Path(value).parts
            ):
                raise PolicyError(f"runner policy classes[{index}].{field} is invalid")
        if any(cap in {"inputplumber-system-dbus","target-consumer","controller-production-routing"} for cap in entry["allowed_capabilities"]) and entry["dbus_proxy"] != "/usr/bin/xdg-dbus-proxy":
            raise PolicyError(f"runner policy classes[{index}] lacks the canonical D-Bus proxy")
        if entry["broker_helper"] != "/usr/local/libexec/factory-runner-broker":
            raise PolicyError(f"runner policy classes[{index}] does not use the canonical broker")
        if name == "gpurunner" or any(cap in {"gpu-compositor", "installed-licensed-diagram"} for cap in entry["allowed_capabilities"]):
            required = {(name, scope) for scope in PIN_SCOPES}
            present = {identity for identity in seen_pins if identity[0] == name}
            if present != required:
                raise PolicyError(f"runner class {name} lacks exact licensed authority pins")
            class_pins = [pin for pin in pins if pin["class"] == name]
            if any(pin["status"] != "enrolled" for pin in class_pins):
                raise PolicyError(f"runner class {name} licensed authority enrollment is pending human review")
        seen_names.add(name)
        seen_uids.add(uid)
    if seen_names != REQUIRED_CLASSES:
        raise PolicyError("runner policy must declare exactly dev-runner-vm, iprunner, and gpurunner")
    if any(class_name not in seen_names for class_name, _ in seen_pins):
        raise PolicyError("runner policy authority pin references an undeclared class")
    return data


def authority_pin(policy: dict, class_name: str, scope: str) -> str:
    matches = [p for p in policy["authority_pins"] if p["class"] == class_name and p["scope"] == scope and p["status"] == "enrolled"]
    if len(matches) != 1:
        raise PolicyError(f"no unique enrolled {scope} authority pin for {class_name}")
    return matches[0]["authority_sha256"]


def class_for_uid(policy: dict, uid: int) -> dict:
    matches = [entry for entry in policy["classes"] if entry["uid"] == uid]
    if len(matches) != 1:
        raise PolicyError(f"executing uid {uid} is not bound to exactly one runner class")
    return matches[0]


def class_for_name(policy: dict, name: str) -> dict:
    matches = [entry for entry in policy["classes"] if entry["name"] == name]
    if len(matches) != 1:
        raise PolicyError(f"runner class {name!r} is not declared in the runner policy")
    return matches[0]
