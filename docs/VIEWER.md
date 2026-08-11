# Arachne Viewer {#arachne_viewer}

The static viewer is built from the canonical product database and does not
depend on local merge or structural analysis. A local viewer may explicitly
add the bounded `.arachne/merge-hints-review.json` identity projection together
with `database/merge-hint-decisions.json`. In that mode `productSha256` must
match the exact product bytes, while `decisionsSha256` and `ignoredPairCount`
must match the exact decision-file bytes and pair count. Supplying only one of
the two artifacts is rejected.

The viewer is deployed as a standalone site rather than embedded in the
Doxygen frame.

<a href="../../viewer/" target="_top">Open the Arachne viewer</a>

Generated browser data and local hint SQLite state are disposable. The
canonical SQLite database remains the product source of truth. The ignored
review contains identity candidates only; structural observations and
projections remain queryable in `.arachne/tmp/merge-hints.sqlite`. The decision
JSON is durable human-review state used to prove that an explicitly included
review was built against the current ignored-pair choices.

Identity review and generic structural observations are separate consumer
surfaces. Structural views may expose directional
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
