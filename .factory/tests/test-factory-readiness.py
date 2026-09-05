#!/usr/bin/env python3
"""Fail-before-model conformance for the production round-zero authority."""
from __future__ import annotations
import hashlib, json, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / ".factory/loop"))
import readiness
import state

class ReadinessTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        subprocess.run(["git", "init", "-q", self.root], check=True)
        subprocess.run(["git", "-C", self.root, "config", "user.email", "fixture@test"], check=True)
        subprocess.run(["git", "-C", self.root, "config", "user.name", "fixture"], check=True)
        (self.root / "captures").mkdir()
        for name in readiness.PROTECTED_STATES:
            (self.root / "captures" / f"{name}.png").write_bytes((name + " nonblank licensed xb360").encode())
        subprocess.run(["git", "-C", self.root, "add", "."], check=True)
        subprocess.run(["git", "-C", self.root, "commit", "-qm", "candidate"], check=True)
        self.candidate = self.git("rev-parse", "HEAD")
        self.tree = self.git("rev-parse", "HEAD^{tree}")
        self.key = self.root / "human-key"
        subprocess.run([readiness.SSH_KEYGEN, "-q", "-t", "ed25519", "-N", "", "-f", self.key], check=True)
        self.public_key = (self.root / "human-key.pub").read_text().strip()
        self.trust = {"schema": readiness.TRUST_SCHEMA, "status": "active", "namespace": readiness.SIGNATURE_NAMESPACE,
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
             "capture_sha256": hashlib.sha256((name + " nonblank licensed xb360").encode()).hexdigest(),
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
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                d = json.loads(json.dumps(valid)); mutate(d)
                with self.assertRaises(readiness.HumanApprovalBlocked):
                    readiness.validate_human_approval(readiness.canonical_bytes(d),
                        accepted_commit=accepted, trust_raw=readiness.canonical_bytes(self.trust),
                        blob_at=self.blob, object_id=self.oid,
                        is_ancestor=lambda a,b: True, diff_paths=lambda a,b: [readiness.APPROVAL_PATH, valid["signature_path"]])

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

if __name__ == "__main__": unittest.main(verbosity=2)
