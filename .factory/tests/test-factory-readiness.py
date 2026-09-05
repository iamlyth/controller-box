#!/usr/bin/env python3
"""Fail-before-model conformance for the production round-zero authority."""
from __future__ import annotations
import hashlib, json, os, subprocess, sys, tempfile, unittest, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / ".factory/loop"))
sys.path.insert(0, str(ROOT / "scripts"))
import readiness
import launch
import state
import campaign
import factory_runner_policy

def png(label: str) -> bytes:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return len(payload).to_bytes(4, "big") + kind + payload + zlib.crc32(kind + payload).to_bytes(4, "big")
    pixel = bytes((len(label) % 255, 20, 30, 255))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", (1).to_bytes(4,"big") * 2 + b"\x08\x06\x00\x00\x00")
            + chunk(b"IDAT", zlib.compress(b"\x00" + pixel)) + chunk(b"IEND", b""))


class ReadinessTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        subprocess.run(["git", "init", "-q", self.root], check=True)
        subprocess.run(["git", "-C", self.root, "config", "user.email", "fixture@test"], check=True)
        subprocess.run(["git", "-C", self.root, "config", "user.name", "fixture"], check=True)
        (self.root / "captures").mkdir()
        for name in readiness.PROTECTED_STATES:
            (self.root / "captures" / f"{name}.png").write_bytes(png(name))
        subprocess.run(["git", "-C", self.root, "add", "."], check=True)
        subprocess.run(["git", "-C", self.root, "commit", "-qm", "candidate"], check=True)
        self.candidate = self.git("rev-parse", "HEAD")
        self.tree = self.git("rev-parse", "HEAD^{tree}")
        self.key = self.root / "human-key"
        subprocess.run([readiness.SSH_KEYGEN, "-q", "-t", "ed25519", "-N", "", "-f", self.key], check=True)
        self.public_key = " ".join((self.root / "human-key.pub").read_text().split()[:2])
        self.trust = {"schema": readiness.TRUST_SCHEMA, "status": "active", "namespace": readiness.SIGNATURE_NAMESPACE,
                      "scope": "production-human-review",
                      "keys": [{"key_id": "review-key", "reviewer": "reviewer@example", "public_key": self.public_key}]}

    def tearDown(self): self.tmp.cleanup()
    def git(self, *args):
        return subprocess.run(["git", "-C", self.root, *args], check=True, text=True, capture_output=True).stdout.strip()
    def blob(self, commit, path):
        return subprocess.run(["git", "-C", self.root, "show", f"{commit}:{path}"], check=True, capture_output=True).stdout
    def oid(self, rev): return self.git("rev-parse", rev)
    def approval(self):
        return {"schema": readiness.APPROVAL_SCHEMA, "status": "approved", "candidate_commit": self.candidate,
                "candidate_tree": self.tree, "accepted_relationship": "approval-only-descendant", "states": [
            {"id": name, "model": model, "capture": f"captures/{name}.png",
             "capture_blob": self.oid(f"{self.candidate}:captures/{name}.png"),
             "capture_sha256": hashlib.sha256(png(name)).hexdigest(),
             "assessment": {"recognizable": True, "sharp": True, "contrast": True,
                            "marker_aligned": True}}
            for name, model in zip(readiness.PROTECTED_STATES, ("xb360", "xbox-series", "ds5"))],
            "provenance": {"renderer": "AMD RADV accelerated", "renderer_accelerated": True,
                           "capture_tool": "kmsgrab", "session": "physical-console",
                           "display": "1920x1080@60", "seat": "seat0"},
            "checklist": list(readiness.REQUIRED_CHECKLIST), "decision": "approve",
            "reviewer": {"identity": "reviewer@example", "key_id": "review-key"},
            "signature_path": ".factory/production-graphics-approval.sig"}

    def test_human_approval_positive_and_negative_matrix(self):
        valid = self.approval()
        approval_path = self.root / ".factory/production-graphics-approval.json"
        approval_path.parent.mkdir()
        approval_path.write_bytes(readiness.canonical_bytes(valid))
        subprocess.run([readiness.SSH_KEYGEN, "-Y", "sign", "-f", self.key, "-n", readiness.SIGNATURE_NAMESPACE, approval_path], check=True, stdout=subprocess.DEVNULL)
        generated_signature = Path(str(approval_path) + ".sig")
        (self.root / ".factory/production-graphics-approval.sig").write_bytes(generated_signature.read_bytes())
        generated_signature.unlink()
        subprocess.run(["git", "-C", self.root, "add", ".factory"], check=True)
        subprocess.run(["git", "-C", self.root, "commit", "-qm", "human approval only"], check=True)
        accepted = self.git("rev-parse", "HEAD")
        digest = readiness.validate_human_approval(readiness.canonical_bytes(valid),
            accepted_commit=accepted, trust_raw=readiness.canonical_bytes(self.trust),
            blob_at=self.blob, object_id=self.oid,
            is_ancestor=lambda a,b: subprocess.run(["git","-C",self.root,"merge-base","--is-ancestor",a,b]).returncode == 0,
            diff_paths=lambda a,b: self.git("diff","--name-only",a,b).splitlines())
        self.assertRegex(digest, r"^[0-9a-f]{64}$")
        mutations = {
            "missing-state": lambda d: d["states"].pop(),
            "wrong-state": lambda d: d["states"][0].update(id="generic_fallback"),
            "tampered-capture": lambda d: d["states"][0].update(capture_sha256="0"*64),
            "software-renderer": lambda d: d["provenance"].update(renderer="llvmpipe software"),
            "renderer-flag": lambda d: d["provenance"].update(renderer_accelerated=False),
            "model-prose": lambda d: d["reviewer"].update(identity="model"),
            "not-approved": lambda d: d.update(decision="pending"),
            "stale-tree": lambda d: d.update(candidate_tree="0"*40),
            "self-reviewed": lambda d: d.update(candidate_commit=accepted),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                d = json.loads(json.dumps(valid)); mutate(d)
                with self.assertRaises(readiness.HumanApprovalBlocked):
                    readiness.validate_human_approval(readiness.canonical_bytes(d),
                        accepted_commit=accepted, trust_raw=readiness.canonical_bytes(self.trust),
                        blob_at=self.blob, object_id=self.oid,
                        is_ancestor=lambda a,b: True, diff_paths=lambda a,b: [readiness.APPROVAL_PATH, valid["signature_path"]])

    def test_external_trust_anchor_requires_root_owned_immutable_exact_digest(self):
        authority = self.root / "review-anchor.json"
        raw = readiness.canonical_bytes(self.trust)
        authority.write_bytes(raw); authority.chmod(0o444)
        digest = hashlib.sha256(raw).hexdigest()
        # Disposable test roots are operator-owned; production accepts only
        # root-owned ancestors (plus a root-owned sticky /tmp). The explicit
        # fixture switch never exists in production campaign environments.
        with self.assertRaises(readiness.HumanApprovalBlocked):
            readiness.read_external_authority(authority, digest, expected_uid=os.getuid())
        os.environ["FACTORY_TEST_AUTHORITY_ANCESTORS"] = "1"
        try:
            loaded, info = readiness.read_external_authority(authority, digest, expected_uid=os.getuid())
        finally:
            os.environ.pop("FACTORY_TEST_AUTHORITY_ANCESTORS", None)
        self.assertEqual(loaded, raw); self.assertEqual(info.st_uid, os.getuid())
        authority.chmod(0o644)
        with self.assertRaises(readiness.HumanApprovalBlocked):
            readiness.read_external_authority(authority, digest, expected_uid=os.getuid())
        authority.chmod(0o444)
        with self.assertRaises(readiness.HumanApprovalBlocked):
            readiness.read_external_authority(authority, "0" * 64, expected_uid=os.getuid())
        authority.unlink()

    def test_rejects_text_named_png(self):
        with self.assertRaises(readiness.HumanApprovalBlocked):
            readiness._validate_png(b"not a png")

    def test_human_tier_is_final_conformance_authority(self):
        side={"requirements":[{"id":"VRF-07","classification":"verified",
              "evidence_tier":"human","required_tier":"human",
              "evidence_commit":self.candidate,
              "artifacts":[readiness.APPROVAL_PATH]}]}
        self.assertRegex(readiness.validate_human_conformance(
            json.dumps(side).encode(), candidate_commit=self.candidate), r"^[0-9a-f]{64}$")
        side["requirements"][0]["classification"]="partial"
        with self.assertRaises(readiness.HumanApprovalBlocked):
            readiness.validate_human_conformance(json.dumps(side).encode(), candidate_commit=self.candidate)

    def test_core_mapping_requires_every_explicit_policy_row(self):
        requirements=[]; policies=[]
        for capability, ids in readiness.CORE_ROWS.items():
            for rid in ids:
                if rid not in {r["id"] for r in requirements}:
                    caps=sorted(c for c, rows in readiness.CORE_ROWS.items() if rid in rows)
                    requirements.append({"id":rid,"required_capabilities":caps,"classification":"partial"})
                    policies.append({"id":rid,"required_capabilities":caps})
        side={"requirements":requirements}; policy={"requirements":policies}
        self.assertRegex(readiness.validate_core_mapping(json.dumps(side).encode(), json.dumps(policy).encode()), r"^[0-9a-f]{64}$")
        for label, mutate in (
            ("missing", lambda d: d["requirements"].pop()),
            ("relaxed", lambda d: d["requirements"][0].update(required_capabilities=[])),
            ("malformed", lambda d: d["requirements"][0].update(classification="verified-ish")),
        ):
            with self.subTest(label=label):
                changed=json.loads(json.dumps(side)); mutate(changed)
                with self.assertRaises(readiness.ReadinessError):
                    readiness.validate_core_mapping(json.dumps(changed).encode(), json.dumps(policy).encode())

    def test_launch_authorization_requires_complete_bindings_and_is_restart_durable(self):
        bindings={"accepted_commit":self.candidate,"tree":self.tree,"environment_blob":self.candidate,
            "specification_sha256":"1"*64,"plan_sha256":"2"*64,"conformance_sha256":"3"*64,
            "policy_sha256":"4"*64,"contracts_sha256":"5"*64,"install_manifest_sha256":"6"*64,
            "command_authority_sha256":"7"*64,"human_authority_sha256":"8"*64,"trust_authority_sha256":"9"*64}
        results={"aggregate_sha256":"a"*64,"capability_result_sha256":"b"*64,"core_result_sha256":"c"*64,
            "conformance_result_sha256":"d"*64,"human_result_sha256":"e"*64}
        value=readiness.result_document(campaign_id="auth-test",nonce="f"*64,status="complete",
            terminal_outcome="pass",bindings=bindings,results=results)
        raw=json.dumps(value,sort_keys=True,separators=(",",":")).encode(); descriptor="a"*64
        # Canonical-looking caller JSON, a known nonce, and arbitrary nonzero
        # digests cannot supply authority or expected values to the mint.
        with self.assertRaises(TypeError):
            launch.authorize_readiness_launch(
                raw, expected_campaign_id="auth-test", expected_nonce="f" * 64,
                expected_bindings=bindings, expected_results=results,
                launch_descriptor_sha256=descriptor, workspace=self.root)
        import inspect
        self.assertEqual(
            set(inspect.signature(launch.authorize_readiness_launch).parameters),
            {"campaign_id", "invocation", "workspace"})

    def test_result_exact_bindings_status_and_nonzero_pass_digests(self):
        bindings = {"accepted_commit":"a"*40,"tree":"b"*40,"environment_blob":"c"*40,
                    **{k:"d"*64 for k in ("specification_sha256","plan_sha256","conformance_sha256","policy_sha256","contracts_sha256","install_manifest_sha256","command_authority_sha256","human_authority_sha256","trust_authority_sha256")}}
        results = {k:"e"*64 for k in ("aggregate_sha256","capability_result_sha256","core_result_sha256","conformance_result_sha256","human_result_sha256")}
        value = readiness.result_document(campaign_id="campaign-a", nonce="f"*64,
            status="complete", terminal_outcome="pass", bindings=bindings, results=results)
        readiness.validate_result(value, expected_campaign_id="campaign-a", expected_nonce="f"*64, expected_bindings=bindings)
        for label, mutate in (
            ("campaign-replay", lambda d: d.update(campaign_id="campaign-b")),
            ("nonce-replay", lambda d: d.update(nonce="1"*64)),
            ("binding-forgery", lambda d: d["bindings"].update(plan_sha256="2"*64)),
            ("zero-pass-digest", lambda d: d["results"].update(core_result_sha256="0"*64)),
            ("status-mismatch", lambda d: d.update(terminal_outcome="findings")),
        ):
            with self.subTest(label=label):
                changed=json.loads(json.dumps(value)); mutate(changed)
                with self.assertRaises(readiness.ReadinessError):
                    readiness.validate_result(changed, expected_campaign_id="campaign-a", expected_nonce="f"*64, expected_bindings=bindings)

    def test_state_v1_requires_explicit_offline_migration(self):
        old = json.loads((ROOT / ".factory/tests/fixtures/state-valid-initial.json").read_text())
        old["schema"] = state.LEGACY_SCHEMA_NAME
        old.pop("readiness")
        with self.assertRaises(state.StateTamperError): state.parse_state(old)
        migrated = state.migrate_offline_state(old)
        self.assertEqual(migrated["schema"], state.SCHEMA_NAME)
        self.assertFalse(migrated["readiness"]["required"])
        with self.assertRaises(state.StateTamperError): state.migrate_offline_state(old, readiness_required=True)

    def test_state_requires_published_result_before_round_one(self):
        binding=state.empty_readiness(required=True); binding["nonce"]="1"*64
        base=state.FactoryState(schema=state.SCHEMA_NAME, repository_identity="1:2", branch="develop", campaign_id="five", rounds_requested=5, current_round=0, current_phase="readiness", specification_digest="a"*64, plan_digest="b"*64, role_prompt_digests={"planner":"c"*64}, audit_objectives_digest="d"*64, pre_round_hook_configuration_digest="e"*64, pre_round_hook_commit="f"*40, pre_round_hook_results_digest="0"*64, pre_round_hook_started_round=0, pre_round_hook_completed_round=0, phase_base_commit="f"*40, selected_task_id=None, attempt_number=0, phase_started_at_monotonic=1, attempt_started_at_monotonic=0, last_outcome=None, readiness=binding)
        base.validate()
        with self.assertRaises(state.StateTransitionError): state.advance(base, "pass", now=2)
        complete=dict(binding); complete.update(cursor=6,status="complete",terminal_outcome="pass",result_sha256="2"*64,
            aggregate_sha256="3"*64, capability_result_sha256="4"*64, core_result_sha256="5"*64,
            conformance_result_sha256="6"*64, human_result_sha256="7"*64)
        advanced=state.advance(state.update_readiness(base, complete), "pass", now=2)
        self.assertEqual((advanced.current_phase, advanced.current_round), ("planning",1))
        mutated=dict(advanced.readiness); mutated["result_sha256"]="3"*64
        self.assertNotEqual(state.state_digest(advanced), state.state_digest(state.FactoryState(**{**advanced.__dict__,"readiness":mutated})))


    def test_readiness_only_cannot_impersonate_campaign_success(self):
        record=campaign.PhaseRecord(1,"audit",1,"pass","a"*40,"b"*64)
        good=campaign.CampaignResult("ready",5,0,"readiness_complete","readiness_complete","a"*40,())
        good.validate(); campaign.validate_campaign_result(good)
        with self.assertRaises(campaign.CampaignResultError):
            campaign.CampaignResult("forged",5,0,"success","pass","a"*40,()).validate()
        complete=campaign.CampaignResult("complete",1,1,"success","pass","a"*40,(record,))
        complete.validate(); campaign.validate_campaign_result(complete)


class RunnerPolicyAuthorityTests(unittest.TestCase):
    def test_gpurunner_requires_two_enrolled_exact_class_pins(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "policy.json"
            base = {"schema":"factory-runner-policy/v2", "namespace":"factory-runner-receipt",
                    "authority_pins":[], "classes":[{
                        "name":"gpurunner", "uid":os.getuid() or 1,
                        "workspace_root":"/var/lib/factory-gpurunner",
                        "allowed_capabilities":["gpu-compositor","installed-licensed-diagram"],
                        "broker_helper":"/usr/local/libexec/factory-runner-broker",
                        "probe_authority":"/opt/factory-runner/authority/v1",
                        "probe_authority_sha256":"b"*64, "probe_authority_status":"enrolled",
                        "signer_key":"/etc/factory/key", "signer_principal_file":"/etc/factory/principal",
                        "nonce_ledger":"/var/lib/factory-runner/nonces", "systemd_run":"/usr/bin/systemd-run",
                        "systemctl":"/usr/bin/systemctl", "cgroup_root":"/sys/fs/cgroup",
                        "dbus_proxy":"/usr/bin/xdg-dbus-proxy", "approved_groups":["users"]}]}
            prior = factory_runner_policy.DEFAULT_POLICY_PATH
            prior_env = os.environ.get("FACTORY_RUNNER_POLICY")
            os.environ["FACTORY_RUNNER_POLICY"] = str(path)
            factory_runner_policy.DEFAULT_POLICY_PATH = path
            try:
                for status, accepted in (("pending-human-review", False), ("enrolled", True)):
                    value=json.loads(json.dumps(base)); value["authority_pins"]=[
                        {"class":"gpurunner","scope":scope,"authority_sha256":"a"*64,"status":status}
                        for scope in sorted(factory_runner_policy.PIN_SCOPES)]
                    path.write_text(json.dumps(value)); path.chmod(0o444)
                    if accepted:
                        policy=factory_runner_policy.load_policy()
                        self.assertEqual(factory_runner_policy.authority_pin(policy,"gpurunner","installed-licensed-diagram"),"a"*64)
                    else:
                        with self.assertRaises(factory_runner_policy.PolicyError): factory_runner_policy.load_policy()
                    path.chmod(0o644)
                value=json.loads(json.dumps(base)); value["authority_pins"]=[]
                path.write_text(json.dumps(value)); path.chmod(0o444)
                with self.assertRaises(factory_runner_policy.PolicyError): factory_runner_policy.load_policy()
            finally:
                factory_runner_policy.DEFAULT_POLICY_PATH = prior
                if prior_env is None: os.environ.pop("FACTORY_RUNNER_POLICY", None)
                else: os.environ["FACTORY_RUNNER_POLICY"] = prior_env

if __name__ == "__main__": unittest.main(verbosity=2)
