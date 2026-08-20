# Arachne Evolution — projection semantics and invariants

Evolution is a metro-like temporal projection over canonical works/concepts/relations. This list fixes the meaning of the graph independently of a specific React/UI/layout implementation.

## Core model

- [ ] Work is the primary cultural unit.
- [ ] Trajectory is a membership family over works.
- [ ] Evolution trajectories are currently built from concepts.
- [ ] The architecture must not prevent adding agent trajectories later.
- [ ] Station is one atomic work or a derived aggregate of several works.
- [ ] Atomic interchange means the same atomic work has multiple visible trajectory memberships.
- [ ] Ordinary trajectory segment is viewer-derived continuity between temporal stops, not a canonical relation.
- [ ] Trajectory continuity does not mean influence, descent, causality, or historical derivation.
- [ ] Explicit work-to-work relations remain a separate evidence-backed graph layer.

## Traversal

- [ ] Earlier and Later are independent budgets.
- [ ] Same-work trajectory switch on an atomic work does not consume a temporal step.
- [ ] Moving to a neighboring temporal group consumes the corresponding Earlier/Later step.
- [ ] `directional` and `connected` may explore the graph differently, but both preserve separate Earlier/Later costs.
- [ ] Explicit relation edge is not an ordinary traversal edge.
- [ ] Traversal is computed on atomic structure before visual hierarchy collapse.
- [ ] Collapse/aggregation does not create reachability that did not exist in the atomic graph.
- [ ] Collapsed and expanded representations under the same seeds/filters describe the same underlying reachable graph.
- [ ] Reach provenance preserves atomic works/path even when some of that path is visually aggregated.

## Time

- [ ] X is chronological ordering space, not a metric time scale.
- [ ] Year order must never be violated.
- [ ] Year-band width depends on visible density/complexity.
- [ ] Historical gaps may be compressed.
- [ ] Exact date constrains layout more strongly.
- [ ] Tied/incomplete dates give the optimizer freedom for ordering/alignment.
- [ ] An incomplete date does not need to be shown as a large uncertainty object in the main graph.
- [ ] `work_membership.position` may be used as an ordering hint among siblings under temporal ambiguity.
- [ ] `position` is not a timestamp.
- [ ] A collapsed multi-year series does not need to occupy its full temporal range as a duration bar.

## Work hierarchy

- [ ] `episode_of`, `season_of`, `track_of`, `volume_of`, `issue_of`, `chapter_of`, `part_of`, `collected_in` are work composition/partonomy.
- [ ] Work hierarchy primarily helps ordering, locality, aggregation, and detail resolution.
- [ ] Work hierarchy does not need to be visualized as nested boxes/tree.
- [ ] Presence of a parent alone does not require collapse.
- [ ] Semantic homogeneity matters more than medium when choosing collapse level.
- [ ] Homogeneous album/series may collapse.
- [ ] Heterogeneous anthology-like structure should remain more detailed.
- [ ] Automatic collapse policy remains disposable/configurable projection state.

## Agent groups

- [ ] `person member_of group` is affiliation, not work containment.
- [ ] Person credit does not automatically inherit to group.
- [ ] Group credit does not automatically inherit to person.
- [ ] Person and group may later become separate agent trajectories.
- [ ] `member_of` does not participate in work hierarchy aggregation.

## Aggregates

- [ ] Aggregate station is a derived projection, not a new canonical work.
- [ ] Exact represented child IDs are preserved.
- [ ] Direct parent concept assignment and child-derived support remain distinguishable.
- [ ] Child-derived support is not written back as canonical parent assignment.
- [ ] An aggregate may visually support multiple trajectories even if they come from different children.
- [ ] Such visual overlap does not automatically create an atomic interchange between all trajectories.
- [ ] If child A carries tag X and child B carries tag Y, collapse does not create free X<->Y traversal.
- [ ] Focus-sensitive outlier extraction does not change canonical assignments or relations.
- [ ] A relevant exceptional child may be surfaced from an aggregate under current focus.
- [ ] An explicit relation endpoint hidden inside an aggregate preserves the exact atomic work identity.

## Strength / line width

- [ ] Line width encodes only trajectory membership significance/centrality.
- [ ] Do not use semantic width for confidence.
- [ ] Do not use semantic width for popularity.
- [ ] Do not use semantic width for traversal depth.
- [ ] Do not use semantic width for number of children.
- [ ] Do not use semantic width for selected/highlight state.
- [ ] `centrality_scale` (`binary`, `ordinal`, `graded`, `none`) affects remapping.
- [ ] `none` is not automatically interpreted as `graded`.
- [ ] Raw 1..100 is not automatically treated as fully comparable across scale types.
- [ ] Remapping policy remains configurable until canonical scale semantics are resolved.
- [ ] Aggregate width must account for descendant coverage; one strong child does not automatically make the whole aggregate strong.
- [ ] The exact aggregate-strength formula is not canonical yet.
- [ ] Source and target membership strengths are preserved separately.
- [ ] Segment may smoothly change width from source to target.
- [ ] Selected/highlight state does not change semantic source/target widths.

## Focus / rendering / bundling

- [ ] `reachable graph != rendered graph`.
- [ ] Visible trajectory ranking is disposable viewer policy.
- [ ] Visible trajectory limit is a presentation/safety cap, not a semantic boundary.
- [ ] Seed/selected/pinned trajectories may survive the ordinary visibility limit.
- [ ] Focus may change visual emphasis and aggregation detail, but not canonical facts.
- [ ] Structural bundling changes presentation, not trajectory identities.
- [ ] Bundling does not change traversal.
- [ ] Bundling does not merge canonical tags into one entity.
- [ ] Manual expand/collapse is local projection state.

## Explicit relations

- [ ] Explicit relation overlay is built separately from trajectory construction.
- [ ] Relation arc points to exact atomic endpoints.
- [ ] If an endpoint is hidden inside an aggregate, the renderer must not pretend the relation applies to the entire parent.
- [ ] Explicit relations may be hidden/shown by focus without changing the trajectory graph.

## Data boundary

- [ ] Aggregation, similarity, collapse score, outlier score, layout coordinates, and display strengths are derived viewer state.
- [ ] These values are not automatically written back into canonical SQLite.
- [ ] Backend tag-tag similarity and manually curated concept relations may later influence viewer analysis, but they are not required parts of the current static demo.
- [ ] Runtime backend, graph database, and 3D are not current requirements.
