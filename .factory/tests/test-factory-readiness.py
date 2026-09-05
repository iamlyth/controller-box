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

    def tearDown(self): self.tmp.cleanup()
    def git(self, *args):
        return subprocess.run(["git", "-C", self.root, *args], check=True, text=True, capture_output=True).stdout.strip()
    def blob(self, commit, path):
        return subprocess.run(["git", "-C", self.root, "show", f"{commit}:{path}"], check=True, capture_output=True).stdout
    def oid(self, rev): return self.git("rev-parse", rev)
    def approval(self):
        return {"schema": readiness.APPROVAL_SCHEMA, "status": "approved", "candidate_commit": self.candidate,
                "candidate_tree": self.tree, "states": [
            {"id": name, "capture": f"captures/{name}.png",
             "capture_sha256": hashlib.sha256((name + " nonblank licensed xb360").encode()).hexdigest(),
             "renderer": "AMD RADV accelerated", "renderer_accelerated": True,
             "decision": "approve", "reviewer": "human reviewer", "human": True}
            for name in readiness.PROTECTED_STATES]}

    def test_human_approval_positive_and_negative_matrix(self):
        valid = self.approval()
        digest = readiness.validate_human_approval(json.dumps(valid).encode(),
            accepted_commit=self.candidate, blob_at=self.blob, object_id=self.oid)
        self.assertRegex(digest, r"^[0-9a-f]{64}$")
        mutations = {
            "missing-state": lambda d: d["states"].pop(),
            "wrong-state": lambda d: d["states"][0].update(id="generic_fallback"),
            "tampered-capture": lambda d: d["states"][0].update(capture_sha256="0"*64),
            "software-renderer": lambda d: d["states"][0].update(renderer="llvmpipe software"),
            "renderer-flag": lambda d: d["states"][0].update(renderer_accelerated=False),
            "model-prose": lambda d: d["states"][0].update(human=False),
            "not-approved": lambda d: d["states"][0].update(decision="pending"),
            "stale-tree": lambda d: d.update(candidate_tree="0"*40),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                d = json.loads(json.dumps(valid)); mutate(d)
                with self.assertRaises(readiness.HumanApprovalBlocked):
                    readiness.validate_human_approval(json.dumps(d).encode(),
                        accepted_commit=self.candidate, blob_at=self.blob, object_id=self.oid)

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

    def test_state_requires_published_result_before_round_one(self):
        binding=state.empty_readiness(required=True); binding["nonce"]="1"*64
        base=state.FactoryState(schema=state.SCHEMA_NAME, repository_identity="1:2", branch="develop", campaign_id="five", rounds_requested=5, current_round=0, current_phase="readiness", specification_digest="a"*64, plan_digest="b"*64, role_prompt_digests={"planner":"c"*64}, audit_objectives_digest="d"*64, pre_round_hook_configuration_digest="e"*64, pre_round_hook_commit="f"*40, pre_round_hook_results_digest="0"*64, pre_round_hook_started_round=0, pre_round_hook_completed_round=0, phase_base_commit="f"*40, selected_task_id=None, attempt_number=0, phase_started_at_monotonic=1, attempt_started_at_monotonic=0, last_outcome=None, readiness=binding)
        base.validate()
        with self.assertRaises(state.StateTransitionError): state.advance(base, "pass", now=2)
        complete=dict(binding); complete.update(cursor=6,status="complete",terminal_outcome="pass",result_sha256="2"*64)
        advanced=state.advance(state.update_readiness(base, complete), "pass", now=2)
        self.assertEqual((advanced.current_phase, advanced.current_round), ("planning",1))
        mutated=dict(advanced.readiness); mutated["result_sha256"]="3"*64
        self.assertNotEqual(state.state_digest(advanced), state.state_digest(state.FactoryState(**{**advanced.__dict__,"readiness":mutated})))

if __name__ == "__main__": unittest.main(verbosity=2)
