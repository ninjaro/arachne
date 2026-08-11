# Arachne Viewer {#arachne_viewer}

The static viewer is built from the canonical product database, the explicit
bounded `database/merge-hints-review.json` projection, and the durable
`database/merge-hint-decisions.json` ignored-pair decisions. The review
artifact's source identity binds it to both inputs: `productSha256` must match
the exact database bytes used for the viewer catalog, while `decisionsSha256`
and `ignoredPairCount` must match the exact decision-file bytes and its current
pair count. Viewer data generation rejects a missing or stale review or decision
artifact and never falls back to merge-hint tables in the product database.

The viewer is deployed as a standalone site rather than embedded in the
Doxygen frame.

<a href="../../viewer/" target="_top">Open the Arachne viewer</a>

Generated browser data and temporary hint SQLite state are disposable. The
canonical SQLite database remains the product source of truth, while the
hash-bound review JSON is the viewer's only source of merge-hint content. The
decision JSON is durable human-review state used to prove that the exported
hints were built against the current ignored-pair choices.

The review data keeps identity-oriented merge candidates visibly separate from
generic structural observations. Structural views may expose directional
containment, temporal and stability measurements, quality-scope comparisons,
sequences, trajectories, or cross-family fingerprints, but must retain their
support and snapshot/algorithm provenance. Labels such as bridge, cluster, or
parallel trajectory are disposable analytical descriptions: the viewer must
not present them as canonical relationships, calibrated truth, or merge advice.

Metric neighbor projections are grouped by entity and corpus scope, with a
separate bounded top-K list for each metric instead of one global leaderboard.
Research priorities reserve space per advisory category before filling the
remaining global cap, so sparse evidence, temporal gaps, unstable boundaries,
bridge works, and concentration warnings remain independently visible.
