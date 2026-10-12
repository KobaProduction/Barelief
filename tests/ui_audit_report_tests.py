import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import report_ui_audit as audit


def report(group):
    return {"schema": 1, "group": group, "summary": {}, "evidence": {},
            "coverage": [{"area": group, "status": "partial", "reason": "Fixture"}],
            "findings": [], "transitions": [{"id": group + ".action", "intent": "Fixture",
                "status": "pass", "dispatch": "public_api", "before": {}, "after": {}, "elapsed_ms": 0}]}


class EvidenceTests(unittest.TestCase):
    def write_reports(self, directory, change=lambda item: None):
        for group in ("workspace", "graphs", "application"):
            item = report(group)
            change(item)
            (directory / f"{group}.json").write_text(json.dumps(item), encoding="utf-8")

    def test_unnamed_axis_and_action_changes_are_visible(self):
        before = {"quickItems": [{"path": "root/0", "id": "", "enabled": False}],
                  "actions": [{"id": "undo", "enabled": False}]}
        after = {"quickItems": [{"path": "root/0", "id": "", "enabled": True}],
                 "actions": [{"id": "undo", "enabled": True}]}
        changes = audit.changed_controls(before, after)
        self.assertEqual({item["tree"] for item in changes}, {"quickItems", "actions"})

    def test_aborted_group_survives_as_partial_error_evidence(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            def abort(item):
                if item["group"] == "application":
                    item["transitions"] = []
                    item["findings"] = [{"id": "aborted", "severity": "error", "category": "harness"}]
            self.write_reports(directory, abort)
            result = audit.merge(audit.load(directory))
            self.assertEqual(result["summary"]["executionErrors"], 1)
            self.assertEqual(result["summary"]["transitions"], 2)

    def test_duplicate_transition_ids_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.write_reports(directory, lambda item: item["transitions"].append(item["transitions"][0].copy()))
            with self.assertRaisesRegex(ValueError, "Duplicate transition"):
                audit.load(directory)

    def run_main(self, directory, strict):
        with patch.object(sys, "argv", ["report_ui_audit.py", str(directory)] + (["--fail-on-findings"] if strict else [])):
            with contextlib.redirect_stdout(io.StringIO()):
                return audit.main()

    def test_strict_mode_catches_error_without_failed_transition(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.write_reports(directory, lambda item: item["findings"].append({"id": "missing.command", "severity": "error", "category": "runtime_contract"}))
            self.assertEqual(self.run_main(directory, True), 1)

    def test_review_flags_do_not_masquerade_as_behavior_failures(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            self.write_reports(directory, lambda item: item["findings"].append({"id": "shortcut.convention", "severity": "review", "category": "usability"}))
            self.assertEqual(self.run_main(directory, True), 0)
            self.assertEqual(json.loads((directory / "summary.json").read_text())["summary"]["behaviorFailures"], 0)

    def test_label_content_cannot_close_report_script(self):
        payload = "</script><script>alert(1)</script>"
        output = audit.render({"label": payload})
        self.assertNotIn(payload, output)
        self.assertIn("\\u003c/script>", output)

    def test_actual_input_and_capture_evidence_survive_merge(self):
        item = report("graphs")
        item["transitions"][0].update(dispatch="qt_input_mouse", beforeImage="captures/mode-before.png",
                                      afterImage="captures/mode-after.png")
        result = audit.merge([item])
        self.assertEqual(result["summary"]["inputTransitions"], 1)
        self.assertEqual(result["transitions"][0]["afterImage"], "captures/mode-after.png")


if __name__ == "__main__":
    unittest.main()
