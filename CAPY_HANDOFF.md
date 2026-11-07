# Reprendre le travail Lichii dans Capy

Point de situation du 7 novembre 2026. Cette branche `codex/lichii-workspace`
réunit le candidat shaders `94b1957e8bc1f9bf97f15097454f2e5ad6aaced7` et la
pipeline de développement issue de `89242ce8a7e1c23ab39c8b1ed6db35f145129f86`.
Les autres branches `codex/*` conservent les étapes et expériences séparées.
Une branche publiée n'est pas une validation pour fusion dans `nx2`.

## Où travailler

- Dépôt de développement sur ce PC : `C:/lx/src`.
- Branche de reprise préparée dans : `C:/lx/capy-handoff-src`.
- Clone séparé du fork : `C:/lx/src/lichii`. Ce clone était sur `nx2` et ne
  contenait pas les correctifs locaux avant leur publication sur les branches dédiées.
- Sources de la pipeline déjà utilisée : `C:/lx/pipeline-src`.
- Configuration et état locaux : `C:/lx/pipeline/config.json` et
  `C:/lx/pipeline/state`. Tableau de bord : `C:/lx/pipeline/dashboard.html`.

Dans Capy, sélectionner le dépôt `WareSven/Lichii` et la branche
`codex/lichii-workspace` pour retrouver le code et la pipeline ensemble.
La branche `nx2` du fork est conservée ; ses changements ne sont pas remplacés.
Son historique et celui du candidat ont divergé : leur intégration future demande
une revue et des tests, pas un envoi forcé.

## Branches principales

| Branche | Rôle |
| --- | --- |
| `codex/lichii-workspace` | Point d'entrée : candidat shaders et pipeline |
| `codex/first-visit-upstream` | Candidat shaders original `94b1957e` |
| `codex/work-pipeline` | Pipeline originale `89242ce8` |
| `codex/task-mesh-precompile` | Réutilisation des bibliothèques Task/Mesh |
| `codex/mesh-unused-task-payload` | Correction des payloads Task/Mesh inutilisés |
| `codex/oot-used-pipeline-checkpoint` | Sauvegarde périodique des pipelines utilisés |
| `codex/warm-authenticated-cb1` | Contextes CB1 authentiques pour la précompilation |
| `codex/texture-index-dominance` | Expérience séparée ; preuve en jeu encore nécessaire |

L'inventaire exact des branches conservées et de leurs commits figure dans
[`docs/nx2/local-branch-inventory.json`](docs/nx2/local-branch-inventory.json).

## Ce qui est vérifié et ce qui reste à faire

Le candidat `94b1957e` corrige la conservation d'anciens contextes de shaders
incomplets : un échec de relecture autorise une nouvelle capture en jeu, puis
le chargement suivant choisit le dernier contexte complet de chaque clé.

Le build Windows de ce candidat et ses suites ciblées ont réussi : 16 entrées
JUnit pour le serveur de diagnostic, 481 pour vidéo/cache, 44 pour NUP et
41 pour les contrats mémoire supplémentaires, dont 21 déjà présents dans la
suite vidéo/cache. Les deux nouveaux tests de recapture passent. Seize modules
Task/Mesh ont aussi passé la validation SPIR-V hors ligne.

Le paquet `shader-upstream-preload-94b1957e8bc1` a été préparé et revu. Un essai
a été lancé sans validation Vulkan ; le contrôle automatique a ensuite été
interrompu. La réutilisation du cache sur deux lancements, les transitions de
zones et l'absence de saccades restent donc à vérifier pour ce candidat.
Les faibles FPS et le temps d'attente GPU observés précédemment restent un sujet
distinct. Ne pas annoncer que toutes les saccades ou la baisse de FPS sont corrigées.

La génération du corpus a conservé 38 programmes ignorés et 8 728 erreurs de
membres d'archives. La présence de fichiers de précompilation ne prouve pas une
couverture complète du jeu. Les rapports et captures détaillés restent sur le PC.

La branche de reprise ajoute la pipeline au candidat, sans changement du code
d'émulation de `94b1957e`. Les résultats C++ ci-dessus concernent ce candidat exact,
pas un nouveau build de cette branche de reprise.

## Utiliser la pipeline

Voir [`docs/nx2/work-pipeline.md`](docs/nx2/work-pipeline.md) pour la configuration,
les étapes de revue et les commandes. Depuis cette branche :

```powershell
python -I tools/nx2/work_pipeline.py --config C:/lx/pipeline/config.json sync
python -I tools/nx2/work_pipeline.py --config C:/lx/pipeline/config.json status
```

La configuration, les journaux, les caches du jeu, les paquets compilés et les
captures ne sont pas des sources à publier. Ils restent disponibles dans les
dossiers locaux indiqués par le suivi. Une nouvelle machine doit configurer ses
propres chemins et commandes ; le code Git seul ne recrée pas cet environnement.

Pour la prochaine investigation : consulter la tâche `shader-upstream-preload`,
vérifier d'abord si l'instance d'essai est encore active, puis reprendre le protocole
avec validation Vulkan et dumps de shaders désactivés. Conserver les caches appris
entre les lancements et enregistrer séparément build, tests, paquet et résultat en jeu.
