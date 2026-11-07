#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

from html.parser import HTMLParser
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import work_pipeline_report as report


class Document(HTMLParser):
    def __init__(self, content):
        super().__init__()
        self.elements = []
        self.feed(content)

    def handle_starttag(self, tag, attributes):
        self.elements.append((tag, dict(attributes)))


def task(**changes):
    value = {
        "id": "case:zelda", "title": "Saccades — même scène", "game_id": "0400486024DEC000",
        "owner": "unassigned", "next_action": "Comparer les deux captures.",
        "candidate_commit": "a" * 40, "updated_at": "2026-11-06T20:00:00Z",
        "stages": {}, "imported": False, "sources": [], "import_notes": [],
    }
    value.update(changes)
    return value


class ReportTests(unittest.TestCase):
    def test_untrusted_text_cannot_create_elements_or_scripts(self):
        payload = '\"><img src=x onerror=alert(1)><script>alert(2)</script>&'
        entry = task(id=payload, title=payload, owner=payload, next_action=payload,
                     game_id=payload, candidate_commit=payload, updated_at=payload,
                     import_notes=[payload], stages={"triage": {
                         "status": payload, "details": payload, "actor": payload,
                         "commit": payload, "evidence": [{"path": payload, "sha256": payload}],
                     }})
        output = report.render_html({"tasks": [entry]}, {payload: {"label": payload, "blockers": [payload]}})
        doc = Document(output)
        self.assertEqual(sum(tag == "script" for tag, _ in doc.elements), 1)
        self.assertFalse(any(tag == "img" for tag, _ in doc.elements))
        self.assertFalse(any("onerror" in attrs for _, attrs in doc.elements))
        self.assertIn("&lt;script&gt;alert(2)&lt;/script&gt;", output)
        self.assertNotIn(payload, output)
        self.assertIn("script-src &#39;unsafe-inline&#39;", output)

    def test_links_only_open_local_readable_evidence(self):
        paths = [
            "https://example.com/private.json", "javascript:alert(1)", "file:///C:/private.json",
            "//server/share/log.txt", r"\\server\share\log.txt", "../log.txt",
            "C:relative.json", "C:/proof.exe", "C:/proof.cmd", "C:/proof.html",
            "C:/proof.txt:evil", "C:/proof\n.txt",
            "C:/preuve d'été/test #1.json", "/tmp/suite log.txt",
        ]
        entry = task(stages={"triage": {"status": "reported", "evidence": [
            {"path": path, "sha256": "b" * 64} for path in paths
        ]}})
        doc = Document(report.render_html({"tasks": [entry]}, {}))
        links = [attrs["href"] for tag, attrs in doc.elements if tag == "a"]
        self.assertEqual(links, ["file:///C:/preuve%20d%27%C3%A9t%C3%A9/test%20%231.json",
                                 "file:///tmp/suite%20log.txt"])

    def test_imported_success_stays_unvalidated(self):
        entry = task(imported=True, stages={"build": {
            "status": "reported", "reported_status": "passed", "details": "Build terminé.",
        }})
        assessment = {entry["id"]: {"label": "À vérifier", "ready": False}}
        output = report.render_html({"tasks": [entry]}, assessment)
        doc = Document(output)
        classes = [attrs.get("class") for _, attrs in doc.elements]
        self.assertIn("step reported", classes)
        self.assertNotIn("step passed", classes)
        self.assertIn("Importé · réussite à vérifier", output)
        self.assertIn("Importé = rapport existant, non revalidé.", output)
        self.assertIn("Build : Importé · réussite à vérifier", report.render_text({"tasks": [entry]}, assessment))

    def test_changed_evidence_does_not_remain_green(self):
        entry = task(stages={"tests": {"status": "passed"}})
        assessment = {entry["id"]: {"ready": False, "stages": {
            "tests": {"valid": False, "problems": ["Empreinte différente"]},
        }}}
        output = report.render_html({"tasks": [entry]}, assessment)
        self.assertIn('class="step invalid"', output)
        self.assertNotIn('class="step passed"', output)
        self.assertIn("Empreinte différente", output)
        self.assertIn("Tests : Validation non recevable", report.render_text({"tasks": [entry]}, assessment))

    def test_valid_stage_is_shown_even_if_other_stages_pending(self):
        entry = task(stages={"triage": {"status": "passed"}})
        output = report.render_html({"tasks": [entry]}, {entry["id"]: {
            "ready": False, "stages": {"triage": {"valid": True}},
        }})
        self.assertIn('class="step passed"', output)
        self.assertIn('data-category="pending"', output)

    def test_absent_assessment_never_infers_ready(self):
        entry = task(stages={name: {"status": "passed"} for name, _ in report.STAGES})
        output = report.render_html({"tasks": [entry]}, {})
        self.assertNotIn('class="step passed"', output)
        self.assertIn('class="step declared"', output)
        self.assertIn('data-category="pending"', output)

    def test_empty_and_ready_counts(self):
        output = report.render_html({"tasks": []}, {})
        self.assertIn('id="no-results">', output)
        self.assertIn("0 tâche(s)", report.render_text({"tasks": []}, {}))
        entry = task()
        output = report.render_html({"tasks": [entry]}, {entry["id"]: {"ready": True}})
        self.assertIn('data-category="ready"', output)
        self.assertIn("<strong>1</strong><span>Prêt(s) à intégrer", output)
        self.assertIn('id="no-results" hidden>', output)


if __name__ == "__main__":
    unittest.main()
