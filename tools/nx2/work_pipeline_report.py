#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render a local, read-only view of pipeline state and its current assessment."""

from datetime import datetime, timezone
from html import escape
from pathlib import PurePosixPath, PureWindowsPath
import re
from urllib.parse import quote


STAGES = (
    ("triage", "Anomalie"),
    ("upstream", "Amont"),
    ("implementation", "Correctif"),
    ("review", "Revue"),
    ("build", "Build"),
    ("tests", "Tests"),
    ("package", "Paquet"),
    ("gameplay", "En jeu"),
)
READABLE_SUFFIXES = {
    ".txt", ".log", ".json", ".jsonl", ".md", ".csv", ".xml", ".png",
    ".jpg", ".jpeg", ".webp", ".pdf", ".yaml", ".yml", ".ini", ".diff", ".patch",
}


def _text(value):
    return "" if value is None else str(value)


def _html(value):
    return escape(_text(value), quote=True)


def _local_uri(value):
    path = _text(value)
    if not path or any(ord(character) < 32 for character in path):
        return None
    if path.startswith(("\\\\", "//", "\\?\\")):
        return None
    if re.match(r"^[A-Za-z]:[\\/]", path):
        if ":" in path[2:]:
            return None
        parsed = PureWindowsPath(path)
    elif path.startswith("/") and "\\" not in path and ":" not in path:
        parsed = PurePosixPath(path)
    else:
        return None
    if parsed.suffix.lower() not in READABLE_SUFFIXES:
        return None
    normalized = parsed.as_posix()
    return "file:///" + quote(normalized.lstrip("/"), safe="/:")


def _evidence(entries):
    rows = []
    for entry in entries or []:
        if not isinstance(entry, dict):
            continue
        path = _text(entry.get("path"))
        if not path:
            continue
        uri = _local_uri(path)
        label = _html(path)
        if uri:
            label = f'<a href="{_html(uri)}" rel="noopener noreferrer">{label}</a>'
        digest = _text(entry.get("sha256"))
        fingerprint = (
            f'<span class="fingerprint" title="{_html(digest)}">SHA256 {_html(digest[:12])}</span>'
            if digest else '<span class="muted">Empreinte non renseignée</span>'
        )
        rows.append(f'<li>{label}{fingerprint}</li>')
    return '<ul class="evidence">' + "".join(rows) + "</ul>" if rows else ""


def _assessment(task, assessments):
    value = assessments.get(task.get("id"), {})
    return value if isinstance(value, dict) else {}


def _stage_view(name, stage, assessment):
    status = stage.get("status", "pending")
    result = assessment.get("stages", {}).get(name, {})
    if status == "reported":
        reported = {"passed": "réussite", "failed": "échec"}.get(
            stage.get("reported_status"), "résultat"
        )
        return "reported", f"Importé · {reported} à vérifier"
    if status == "failed":
        return "failed", "Échec constaté"
    if status == "passed":
        if result.get("valid") is False:
            return "invalid", "Validation non recevable"
        if result.get("valid") is True or assessment.get("ready") is True:
            return "passed", "Validé avec preuves"
        return "declared", "Déclaré réussi · à vérifier"
    return "pending", "À faire"


def _task_card(task, assessment, index):
    ready = assessment.get("ready") is True
    category = "ready" if ready else "pending"
    imported = task.get("imported") is True
    title = task.get("title") or task.get("id") or "Tâche sans titre"
    label = assessment.get("label") or ("Prêt à intégrer" if ready else "À évaluer")
    candidate = _text(task.get("candidate_commit"))
    owner = task.get("owner")
    owner = "À attribuer" if owner in (None, "", "unassigned") else owner
    action = task.get("next_action") or "Définir la prochaine action et son responsable."
    stages = task.get("stages") or {}
    steps, details = [], []
    for name, stage_label in STAGES:
        stage = stages.get(name, {})
        colour, status_label = _stage_view(name, stage, assessment)
        steps.append(
            f'<li class="step {colour}" title="{_html(status_label)}">'
            f'<span class="step-dot" aria-hidden="true"></span><span>{stage_label}</span>'
            f'<small>{_html(status_label)}</small></li>'
        )
        evidence = _evidence(stage.get("evidence"))
        stage_details = _html(stage.get("details"))
        reviewer = _html(stage.get("actor"))
        commit = _html(stage.get("commit"))
        issues = assessment.get("stages", {}).get(name, {}).get("problems", [])
        issue_html = "".join(f"<li>{_html(issue)}</li>" for issue in issues)
        details.append(
            f'<section class="stage-detail"><h4>{stage_label} '
            f'<span class="status {colour}">{_html(status_label)}</span></h4>'
            + (f'<p class="preserve">{stage_details}</p>' if stage_details else "")
            + (f'<p class="muted">Responsable de la preuve : {reviewer}</p>' if reviewer else "")
            + (f'<p class="muted">Commit : <code>{commit}</code></p>' if commit else "")
            + (f'<ul class="blockers">{issue_html}</ul>' if issue_html else "")
            + (evidence or '<p class="muted">Aucune preuve enregistrée.</p>')
            + "</section>"
        )
    blockers = assessment.get("blockers") or []
    blocker_rows = "".join(f"<li>{_html(value)}</li>" for value in blockers)
    notes = "".join(f"<li>{_html(value)}</li>" for value in task.get("import_notes", []))
    sources = _evidence(task.get("sources"))
    search = " ".join(_text(task.get(key)) for key in (
        "id", "title", "game_id", "owner", "next_action", "candidate_commit"
    ))
    return (
        f'<article class="task" data-category="{category}" data-imported="{str(imported).lower()}" '
        f'data-search="{_html(search)}" aria-labelledby="task-{index}">'
        f'<div class="task-top"><div><p class="eyebrow">{_html(task.get("id"))}</p>'
        f'<h2 id="task-{index}">{_html(title)}</h2></div>'
        f'<span class="badge {category}">{_html(label)}</span></div>'
        '<dl class="metadata">'
        f'<div><dt>Responsable</dt><dd>{_html(owner)}</dd></div>'
        f'<div><dt>Jeu / programme</dt><dd>{_html(task.get("game_id") or "Transversal")}</dd></div>'
        f'<div><dt>Candidat</dt><dd><code title="{_html(candidate)}">'
        f'{_html(candidate[:12] or "Non défini")}</code></dd></div>'
        f'<div><dt>Mise à jour</dt><dd>{_html(task.get("updated_at") or "Non renseignée")}</dd></div>'
        '</dl><ol class="pipeline">' + "".join(steps) + '</ol>'
        f'<div class="next-action"><span>Prochaine action</span><p>{_html(action)}</p></div>'
        + ('<p class="import-note">Importé = rapport existant, non revalidé.</p>' if imported else "")
        + (f'<details class="reasons"><summary>Points à résoudre ({len(blockers)})</summary>'
           f'<ul class="blockers">{blocker_rows}</ul></details>' if blockers else "")
        + '<details class="proofs"><summary>Étapes et preuves</summary>' + "".join(details)
        + (f'<section class="stage-detail"><h4>Sources de l’import</h4>{sources}</section>' if sources else "")
        + (f'<section class="stage-detail"><h4>Remarques d’import</h4><ul>{notes}</ul></section>' if notes else "")
        + '</details></article>'
    )


STYLE = """
:root{color-scheme:dark;--bg:#10131c;--panel:#191e2b;--line:#30374b;--text:#eef1fa;
--muted:#aab4cd;--accent:#a8b4ff;--green:#83d8af;--red:#ffa4a4;--amber:#e4c287}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.55 system-ui,sans-serif}
main{max-width:1340px;margin:auto;padding:42px 28px 64px}h1{font-size:clamp(28px,4vw,42px);line-height:1.1;margin:8px 0 18px;letter-spacing:-1px}
h2{font-size:20px;line-height:1.3;margin:3px 0;overflow-wrap:anywhere}h4{margin:0 0 8px;font-size:15px}
p{margin:8px 0}.eyebrow{font-size:11px;font-weight:700;letter-spacing:1.5px;text-transform:uppercase;color:var(--accent);overflow-wrap:anywhere}
.intro{max-width:850px;color:var(--muted)}.timestamp{font-size:12px;color:var(--muted)}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin:26px 0}
.stat{border:1px solid var(--line);border-radius:12px;padding:17px 22px;background:var(--panel)}
.stat strong{display:block;font-size:30px;line-height:1.2}.stat span{color:var(--muted);font-size:13px}
.toolbar{display:flex;gap:12px;align-items:end;flex-wrap:wrap;margin:25px 0 10px}
.toolbar label{display:flex;flex-direction:column;gap:5px;color:var(--muted);font-size:12px}.search{flex:1;min-width:240px}
input,select{font:inherit;font-size:14px;color:var(--text);background:#151a26;border:1px solid var(--line);border-radius:8px;padding:11px 12px}
input:focus,select:focus,summary:focus-visible,a:focus-visible{outline:2px solid var(--accent);outline-offset:3px}
.result-count{font-size:12px;color:var(--muted);margin-bottom:18px}.task{border:1px solid var(--line);border-radius:13px;background:var(--panel);padding:24px;margin:16px 0}
.task[hidden]{display:none}.task-top{display:flex;justify-content:space-between;align-items:start;gap:20px}
.badge{font-size:12px;border:1px solid var(--line);border-radius:20px;padding:6px 11px;max-width:40%;flex-shrink:0;overflow-wrap:anywhere}
.badge.ready{color:var(--green);border-color:#3a6956}.badge.pending{color:var(--amber);border-color:#685839}
.metadata{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:18px 0 24px}
dt{font-size:11px;text-transform:uppercase;letter-spacing:.6px;color:var(--muted)}dd{margin:3px 0 0;overflow-wrap:anywhere;font-size:13px}
code{font-family:ui-monospace,Consolas,monospace;font-size:12px;overflow-wrap:anywhere}
.pipeline{list-style:none;display:grid;grid-template-columns:repeat(8,1fr);padding:0;margin:0 0 22px;gap:8px}
.step{border-top:2px solid #454b5e;padding-top:8px;font-size:12px;font-weight:650;color:var(--muted)}
.step small{display:block;font-size:10px;font-weight:400;margin:4px 0 0;line-height:1.35;min-height:28px}
.step-dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:currentColor;margin-right:5px}
.step.passed{color:var(--green);border-color:var(--green)}.step.failed,.step.invalid{color:var(--red);border-color:#cb7c7c}
.step.reported{color:var(--amber);border-color:#9b845d}.step.declared{color:var(--accent);border-color:#6976aa}
.next-action{border-left:3px solid var(--accent);padding:2px 0 2px 14px}.next-action span{font-size:11px;text-transform:uppercase;letter-spacing:.7px;color:var(--accent)}
.next-action p{margin:3px 0;font-size:14px;white-space:pre-wrap;overflow-wrap:anywhere}.import-note{font-size:12px;color:var(--amber);margin-top:14px}
details{border-top:1px solid var(--line);padding-top:13px;margin-top:15px}summary{cursor:pointer;color:var(--accent);font-size:13px;font-weight:600}
.stage-detail{padding:16px 0;border-bottom:1px solid var(--line)}.stage-detail:last-child{border:0}.stage-detail p{font-size:13px}
.status{display:inline-block;margin-left:8px;font-size:11px;font-weight:400;color:var(--muted)}.status.passed{color:var(--green)}.status.failed,.status.invalid{color:var(--red)}.status.reported{color:var(--amber)}
.evidence,.blockers{padding-left:20px;font-size:12px;overflow-wrap:anywhere}.evidence li{margin:8px 0}.blockers li{margin:5px 0}
a{color:var(--accent);text-underline-offset:3px}.fingerprint{display:block;color:var(--muted);font:10px/1.8 ui-monospace,monospace}
.muted{color:var(--muted)}.preserve{white-space:pre-wrap;overflow-wrap:anywhere}.empty{text-align:center;padding:36px;border:1px dashed var(--line);border-radius:12px;color:var(--muted)}
footer{margin-top:28px;font-size:12px;color:var(--muted)}.legend{display:flex;gap:18px;flex-wrap:wrap;font-size:11px;color:var(--muted);margin:16px 0}
.legend span::before{content:"";display:inline-block;width:7px;height:7px;border-radius:50%;background:#8d94a6;margin-right:6px}.legend .green::before{background:var(--green)}.legend .amber::before{background:var(--amber)}.legend .red::before{background:var(--red)}
@media(max-width:900px){.pipeline{grid-template-columns:repeat(4,1fr);row-gap:14px}.metadata{grid-template-columns:repeat(2,1fr)}}
@media(max-width:560px){main{padding:22px 14px}.task{padding:18px}.task-top{display:block}.badge{display:inline-block;max-width:100%;margin-top:12px}.stats{gap:6px}.stat{padding:12px}.stat strong{font-size:25px}.stat span{font-size:11px}.metadata{gap:14px}.pipeline{gap:6px}.step{font-size:11px}}
@media print{body{background:white;color:black}.toolbar{display:none}.task{break-inside:avoid;background:white}.stats,.task{border-color:#888}.muted,.intro,dt,footer,.timestamp{color:#444}}
"""


SCRIPT = """
(() => {
  const search = document.getElementById('search');
  const filter = document.getElementById('filter');
  const cards = [...document.querySelectorAll('.task')];
  const count = document.getElementById('result-count');
  const empty = document.getElementById('no-results');
  const normalize = value => value.normalize('NFD').replace(/[\\u0300-\\u036f]/g, '').toLocaleLowerCase('fr');
  const apply = () => {
    const query = normalize(search.value.trim());
    let visible = 0;
    for (const card of cards) {
      const match = filter.value === 'all' || card.dataset.category === filter.value ||
        (filter.value === 'imported' && card.dataset.imported === 'true');
      card.hidden = !match || !normalize(card.dataset.search).includes(query);
      if (!card.hidden) visible++;
    }
    count.textContent = `${visible} tâche(s) affichée(s) sur ${cards.length}`;
    empty.hidden = visible !== 0;
  };
  search.addEventListener('input', apply);
  filter.addEventListener('change', apply);
  apply();
})();
"""


def render_html(state, assessments):
    """Return a standalone HTML snapshot; assessments must be refreshed by the caller."""
    tasks = state.get("tasks") or []
    ready = sum(_assessment(task, assessments).get("ready") is True for task in tasks)
    imported = sum(task.get("imported") is True for task in tasks)
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    cards = "".join(_task_card(task, _assessment(task, assessments), i) for i, task in enumerate(tasks))
    return (
        '<!doctype html><html lang="fr"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width, initial-scale=1">'
        '<meta name="referrer" content="no-referrer">'
        '<meta http-equiv="Content-Security-Policy" content="default-src &#39;none&#39;; '
        'script-src &#39;unsafe-inline&#39;; style-src &#39;unsafe-inline&#39;; '
        'base-uri &#39;none&#39;; form-action &#39;none&#39;; connect-src &#39;none&#39;">'
        '<title>Lichii — suivi de développement</title>'
        f'<style>{STYLE}</style></head><body><main>'
        '<header><p class="eyebrow">Lichii · Développement local</p>'
        '<h1>Du problème à la validation.</h1>'
        '<p class="intro">Un état partagé pour suivre chaque anomalie, son responsable et les preuves '
        'de chaque étape. Les résultats de compilation, les tests et les observations en jeu restent distincts.</p>'
        f'<p class="timestamp">Instantané généré le {generated}. Relancer la synchronisation pour actualiser les preuves.</p></header>'
        '<section class="stats" aria-label="État global">'
        f'<div class="stat"><strong>{ready}</strong><span>Prêt(s) à intégrer</span></div>'
        f'<div class="stat"><strong>{len(tasks) - ready}</strong><span>À traiter</span></div>'
        f'<div class="stat"><strong>{imported}</strong><span>Avec rapports importés</span></div></section>'
        '<div class="toolbar"><label class="search" for="search">Rechercher une tâche, un jeu ou un responsable'
        '<input id="search" type="search" placeholder="Titre, identifiant, commit…" autocomplete="off"></label>'
        '<label for="filter">Afficher<select id="filter"><option value="all">Toutes les tâches</option>'
        '<option value="pending">À traiter</option><option value="ready">Prêtes à intégrer</option>'
        '<option value="imported">Rapports importés</option></select></label></div>'
        '<div class="legend" aria-label="Légende"><span class="green">Preuve validée</span>'
        '<span class="amber">Rapport importé, à vérifier</span><span class="red">Échec ou preuve non recevable</span>'
        '<span>Étape à faire</span></div>'
        f'<p class="result-count" id="result-count" aria-live="polite">{len(tasks)} tâche(s)</p>'
        + cards
        + f'<p class="empty" id="no-results"{ " hidden" if tasks else ""}>Aucune tâche ne correspond à cette sélection.</p>'
        '<footer>Consultation locale uniquement. Aucun envoi de données, lancement de jeu ou publication depuis ce tableau. '
        'Les liens de preuves ouvrent les fichiers locaux lisibles. Un rapport importé ne valide aucune étape à lui seul.</footer>'
        f'</main><script>{SCRIPT}</script></body></html>'
    )


def render_text(state, assessments):
    """Return the same factual status in plain text for terminals and saved reports."""
    tasks = state.get("tasks") or []
    ready = sum(_assessment(task, assessments).get("ready") is True for task in tasks)
    lines = ["Lichii — suivi de développement", f"{len(tasks)} tâche(s), {ready} prête(s) à intégrer.",
             "Importé = rapport existant, non revalidé."]
    for task in tasks:
        assessment = _assessment(task, assessments)
        lines += ["", f"[{_text(task.get('id'))}] {_text(task.get('title'))}",
                  f"État : {_text(assessment.get('label') or 'À évaluer')}",
                  f"Responsable : {_text(task.get('owner') or 'unassigned')}",
                  f"Candidat : {_text(task.get('candidate_commit') or 'Non défini')}",
                  f"Prochaine action : {_text(task.get('next_action') or 'À définir')}"]
        for name, label in STAGES:
            stage = (task.get("stages") or {}).get(name, {})
            _, status_label = _stage_view(name, stage, assessment)
            lines.append(f"  {label} : {status_label}")
            if stage.get("details"):
                lines.append("    " + _text(stage["details"]).replace("\n", "\n    "))
            for entry in stage.get("evidence") or []:
                lines.append(f"    Preuve : {_text(entry.get('path'))} [SHA256 {_text(entry.get('sha256'))}]")
        for blocker in assessment.get("blockers") or []:
            lines.append(f"  À résoudre : {_text(blocker)}")
        for note in task.get("import_notes") or []:
            lines.append(f"  Import : {_text(note)}")
    return "\n".join(lines) + "\n"
