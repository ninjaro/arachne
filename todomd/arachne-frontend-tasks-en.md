# Arachne Evolution — frontend tasks

Evolution is the primary screen of Arachne Viewer. It is a static research viewer over snapshot/canonical data, without a full runtime backend.

Core model that must not be lost during implementation:

- work — primary cultural unit;
- trajectory — a membership family over works; currently concepts, architecturally later also agents;
- station — an atomic work or a derived aggregate of several works;
- interchange — an atomic work with multiple trajectories;
- ordinary trajectory continuity **does not mean influence**;
- explicit work-to-work relations are a separate overlay layer;
- X is chronological ordering space, not a metric time scale;
- year width depends on visible data density;
- Earlier and Later are independent traversal budgets;
- switching trajectory at the same atomic work does not consume a temporal step;
- moving to a neighboring temporal group consumes an Earlier or Later step;
- hierarchy/aggregation is presentation/projection logic and must not change atomic traversal semantics;
- line width encodes only trajectory membership significance/centrality.

The current implementation already contains directional/connected traversal, Earlier/Later provenance, temporal buckets/groups, trajectory ranking, visible trajectory limit, structural bundling, explicit relation overlay, density-based year bands, and centrality-based line width. New work should extend this model rather than replace it.

## Evolution shell / UI

- [ ] Restructure `EvolutionView` into `left sidebar / central canvas / right inspector`; the Evolution canvas should occupy the main width and height.
- [ ] Keep `Earlier` and `Later` as separate numeric controls; do not restore a generic `Reach`.
- [ ] Move rare traversal/date/visibility settings out of the persistent panel into a compact advanced/dropdown UI.
- [ ] Keep `directional` as the primary mode; do not make `connected` a major visual control.
- [ ] Make the inspector narrow, collapsible, and optionally resizable; opening an entity should not heavily shrink the canvas.
- [ ] For aggregate stations, show a short count label (`10 tracks`, `12 episodes`, `3 works`) and expose details in the inspector rather than inside the graph.
- [ ] Add `Show children` / `Show tracks` / `Show episodes` for a selected aggregate. This changes projection granularity; it does not draw nested hierarchy in the graph.
- [ ] For selected trajectories/stations, de-emphasize irrelevant context with opacity/contrast without changing semantic line width.
- [ ] Do not use increased stroke width for selected/highlight state: width is reserved for significance.
- [ ] Keep explicit relations visually and logically in a separate overlay layer.
- [ ] Do not build separate graph UI for `person -> group`; show `member_of` in the inspector / secondary relation context.
- [ ] Keep Browse secondary; do not bind the table to a mandatory `Maker` column. Use `Credits` / context-aware credit summary.

## Split the current EvolutionView

- [ ] Reduce the responsibilities of `viewer/src/views/EvolutionView.tsx`: move graph controls, canvas rendering, inspector presentation, and projection state into separate components/hooks.
- [ ] Do not move traversal semantics into React components; `EvolutionView` should assemble state and call pure projection/layout functions.
- [ ] Preserve current keyboard/pointer interaction helpers and delayed hover preview, but do not mix them with hierarchy aggregation logic.
- [ ] Stabilize aggregate station IDs and rendered trajectory IDs so focus/depth changes do not unnecessarily recreate all SVG nodes.

## Projection pipeline

- [ ] Keep the computation order approximately:
  1. static catalog -> atomic Evolution index;
  2. date acceptance / excluded tags / seed selection;
  3. atomic traversal with separate Earlier/Later budgets;
  4. trajectory ranking/visibility;
  5. hierarchy-aware aggregation for current visible/focus context;
  6. aggregate strength profiles;
  7. trajectory projection + structural bundling;
  8. TimeNets layout;
  9. SVG scene.
- [ ] Do not run traversal over already-collapsed parent/aggregate stations.
- [ ] Aggregation may change representation but must not create a new `shared-work` transition between trajectories.
- [ ] Keep reach reasons/provenance on atomic paths; an aggregate station must know which children produced a given reach.
- [ ] `Show children` changes detail projection but not atomic Earlier/Later reachability under the same seeds/filters.
- [ ] Run focus-sensitive exception extraction after atomic traversal: an exceptional child may be surfaced without creating an artificial parent interchange.

## Work hierarchy index

Canonical hierarchy uses `workMemberships`:

`episode_of`, `season_of`, `track_of`, `volume_of`, `issue_of`, `chapter_of`, `part_of`, `collected_in`.

This is work composition/partonomy. Do not mix it with `agentRelations.member_of`.

- [ ] Add a dedicated module such as `viewer/src/lib/evolution-hierarchy.ts`; do not keep expanding `evolution.ts`.
- [ ] Build `parentByChildId`, `childrenByParentId`, membership type, `position`, `positionText`, ancestor/descendant helpers, and stable sibling ordering.
- [ ] Guard against cycles/bad hierarchy edges in projection logic: do not hang or recurse forever.
- [ ] Do not treat the presence of a parent as automatic permission to collapse.
- [ ] Use `position` as an ordering hint among siblings, especially under tied/incomplete dates; do not turn it into a timestamp.

## Atomic work × trajectory profile

- [ ] Extract atomic membership profile into a pure structure containing:
  - `tagId`;
  - raw centrality;
  - centrality scale;
  - remapped/display strength;
  - relation type;
  - historical role;
  - confidence as metadata, not line width.
- [ ] Store the profile sparsely (`Map<tagId, value>`), not as a dense vector over all concepts.
- [ ] Do not include confidence in similarity/width by default.
- [ ] Keep direct parent assignment and child-derived aggregate support separate.
- [ ] Do not write a derived aggregate profile back into `Work.concepts`.

## Centrality remapping

Canonical `centrality` is stored on 1..100, but `centrality_scale` may be `binary`, `ordinal`, `graded`, or `none`. These scales must not be silently treated as fully comparable.

- [ ] Stop treating `rawCentrality / 100` as final semantic normalization.
- [ ] Add one explicit remapping layer in `evolution-strength.ts` based on `centralityScale`.
- [ ] `graded`: continuous mapping once the scale semantics are defined.
- [ ] `ordinal`: discrete/calibrated levels; do not pretend it is an interval scale.
- [ ] `binary`: fixed states without false precision.
- [ ] `none`: configurable compatibility fallback; do not infer semantic scale from the numeric value.
- [ ] Preserve `rawStrength`, `centralityScale`, and remapped value together.
- [ ] Ranking/aggregation/width calculations that require comparable strength must use the same remapper.
- [ ] Use line width only for significance/centrality; do not encode confidence, popularity, number of children, traversal depth, or selected state with the same channel.

The exact remapping policy is still open. Keep it configurable/disposable rather than hard-coding it as new canonical semantics.

## Hierarchy-aware aggregation

Main rule: collapse is determined by semantic homogeneity of descendants, not by medium.

Examples:

- album with almost identical tracks — good candidate;
- homogeneous multi-year series — also a candidate;
- horror anthology with highly varied episodes — bad candidate;
- homogeneous series with one rare Film Noir episode may be collapsed by default, while that episode becomes visible separately when Film Noir is the focus.

- [ ] Add a dedicated module such as `viewer/src/lib/evolution-aggregation.ts`.
- [ ] Treat hierarchy as a source of candidate groups, not as a visual structure.
- [ ] Try to collapse at the highest reasonable level: series, otherwise season, otherwise episodes; album, otherwise tracks.
- [ ] Use temporal compactness as a bonus, not a hard requirement.
- [ ] A collapsed multi-year series does not need to occupy the full `year_start..year_end` as a duration bar; it needs a representative anchor in ordering space.
- [ ] For homogeneity, start with a simple deterministic sparse similarity. Practical baseline: weighted Jaccard over remapped concept strengths:
  `sum(min(a_t,b_t)) / sum(max(a_t,b_t))`.
- [ ] Make the collapse threshold configurable.
- [ ] Do not use backend tag-tag similarity in this algorithm yet; that is a separate future feature.

## Aggregate profile

For each aggregate keep at least:

- represented child IDs;
- child count;
- per-tag support count;
- per-tag coverage = supportCount / representedChildCount;
- known-strength count;
- mean/median/max remapped strength among children where the tag is present;
- direct parent assignment separately, if one exists;
- outlier child IDs.

- [ ] Do not use `max child centrality` as the default aggregate strength.
- [ ] One very strong Industrial track must not make the whole album strongly Industrial.
- [ ] An initial display policy may use `coverage × meanStrengthWhenPresent`, but this is **not a fixed canonical formula**; keep it in a pure configurable function.
- [ ] If semantic strength is unknown, keep coverage separately and do not invent arbitrary precise centrality.
- [ ] Do not irreversibly mix direct canonical parent assignment with child-derived support.
- [ ] The inspector should be able to show the difference between direct assignment and derived support.

## Focus-sensitive exceptions

- [ ] An aggregate may be collapsed by default, but selected/seed/pinned trajectories should participate in exception extraction.
- [ ] If a child differs significantly from the aggregate specifically on the active trajectory, render that child as a separate station near the aggregate.
- [ ] A horror-anthology-like group should naturally fall to a finer resolution because of high heterogeneity; do not create a separate `anthology mode`.
- [ ] An explicit-relation endpoint hidden inside an aggregate may be temporarily surfaced as an exception so the relation arc points to the real work.
- [ ] Manual `Show children` takes precedence over automatic collapse until local expansion is closed/reset.
- [ ] Focus-sensitive extraction must not change canonical assignments or create new relations.

The exact outlier score is still open; keep it simple and configurable.

## Aggregate station semantics

- [ ] Keep one visual grammar: ordinary and aggregate stations differ by size/count label, not by a separate container UI.
- [ ] An aggregate may visually support several trajectories even if those trajectories come from different children.
- [ ] Renderer/traversal must not interpret that visual overlap as an atomic interchange between all trajectories.
- [ ] Preserve exact child IDs for inspector, provenance, and relation endpoints.
- [ ] In the aggregate inspector show:
  - label;
  - membership type;
  - represented child count;
  - represented date range metadata;
  - trajectories;
  - coverage;
  - aggregate/display strength;
  - direct parent assignment vs derived support;
  - outliers;
  - children list on demand.
- [ ] Use domain-specific count labels when possible: `tracks`, `episodes`, `seasons`, `chapters`, rather than always `works`.

## Line width / variable-width rendering

Strength belongs to station/tag membership. For aggregates in particular, a weak endpoint must not inherit the width of a strong neighboring endpoint across the entire segment.

- [ ] Revisit the current logic `segmentDisplayStrength(source,target) = max(source,target)`.
- [ ] Preserve both endpoint strengths on each segment.
- [ ] Render `0.9 -> 0.2` as an actually narrowing line.
- [ ] SVG `stroke-width` cannot vary along one path; implement variable-width segments either as a filled ribbon/path around the centerline or as multiple interpolated subsegments.
- [ ] Preferred approach: sampled ribbon:
  - get the centerline route;
  - sample 8–16 points;
  - interpolate half-width source -> target;
  - compute the normal;
  - build left/right polygon/path;
  - close the path.
- [ ] Keep a separate invisible wide path for hit testing.
- [ ] At a station port, ribbon width must match that station's membership strength.
- [ ] Update station knockout/port spacing for local ribbon width.
- [ ] Do not allow tapering to create a visual gap at the marker.
- [ ] Selected/highlight trajectory must not alter semantic ribbon width; use a separate underlay/glow/opacity treatment.
- [ ] Unknown strength should use a neutral width policy without noisy special texture in the main graph.

## Bundling

- [ ] Keep structural bundling as a presentation optimization.
- [ ] Bundle equivalence/signature must account for endpoint strength profile, not only a single max-width segment.
- [ ] Do not bundle seed/selected/pinned/provenance-required trajectories.
- [ ] Bundling must not change tag identities or traversal.
- [ ] If variable-width profiles materially differ, do not bundle those trajectories.
- [ ] Expanding a bundle returns the same memberships without semantic relayout.

## Time / ordering / layout

Keep the current model: X is ordered temporal space, not a linear time scale.

- [ ] Preserve content/density-based year widths.
- [ ] Preserve compressed historical gaps.
- [ ] Do not reorder exact dates for crossing minimization.
- [ ] Use tied/incomplete dates as layout freedom for:
  - crossing reduction;
  - fewer bends;
  - straighter trajectories;
  - sibling locality;
  - easier collapse;
  - label placement.
- [ ] Do not add heavy uncertainty visualization to the main graph.
- [ ] Add hierarchy locality to the layout score.
- [ ] Use sibling `position` as a stable ordering hint under temporal ambiguity.
- [ ] On `Show children`, try to preserve the aggregate's outer anchor and nearby lane positions; locally widen the year band only when needed.
- [ ] Use seed/pinned trajectories as the most stable spatial backbone after Earlier/Later/focus changes.

## Trajectory selection / focus

- [ ] Preserve the distinction `reachable != rendered`.
- [ ] Treat `DEFAULT_VISIBLE_TRAJECTORY_LIMIT = 80` as a software/presentation cap, not a perceptual target.
- [ ] Keep disposable ranking by support/centrality/rarity/continuity/structural importance.
- [ ] Preserve the family-neutral model `concept | agent`.
- [ ] Do not wire agent trajectories into `buildEvolutionIndex` yet if that expands the scope of the static demo.
- [ ] Seed/selected/pinned trajectories must survive the ordinary visible limit.
- [ ] Use focus state both for aggregation exception logic and visual emphasis.

## Explicit relations

- [ ] Keep explicit relations separate from trajectory construction.
- [ ] Build relation overlay after aggregation mapping.
- [ ] If a relation endpoint is hidden inside an aggregate:
  - either temporarily surface the endpoint as an exception;
  - or open the exact child from the aggregate inspector;
  - do not render the relation as if it applies to the whole parent.
- [ ] Explicit relation edges do not participate in ordinary trajectory depth traversal.

## Agent groups

`person member_of group` is affiliation, not work containment.

- [ ] Do not inherit credits person -> group or group -> person.
- [ ] Do not use `member_of` for work aggregation.
- [ ] If agent trajectories are added later, person and group remain separate trajectories.
- [ ] Temporal membership period may be used as secondary relation/context, but not as primary metro containment.

## Performance / caching

- [ ] Do not recompute hierarchy tree and atomic profiles on hover.
- [ ] Cache hierarchy index by Domain/snapshot identity.
- [ ] Cache remapped atomic profiles by work ID + remapping config/version.
- [ ] Compute aggregate profiles sparsely and only for candidate groups in current reachable/visible context.
- [ ] Cache aggregation result by hierarchy group + relevant focus set + remapping version.
- [ ] Layout should not depend on inspector open/close except for available viewport width.
- [ ] Do not move to WebGL/3D in this iteration without a concrete measured performance problem.

## Practical fixtures

- [ ] Album: 10 tracks, Industrial on 2/10 -> aggregate Industrial is visibly weaker than full membership.
- [ ] Album: 10 nearly identical tracks -> collapse into one station.
- [ ] Homogeneous multi-year series -> may collapse despite a wide date range.
- [ ] Horror anthology with strongly different episode profiles -> do not collapse at series level.
- [ ] Homogeneous series + one Film Noir outlier -> collapsed by default; outlier visible under Film Noir focus.
- [ ] Aggregate: tag A only on child 1, tag B only on child 2 -> collapse does not create free atomic A<->B traversal.
- [ ] Collapsed and expanded representations produce the same atomic Earlier/Later reach set.
- [ ] Same-year siblings may be reordered/layout-optimized without violating exact-date ordering.
- [ ] `centralityScale=none` does not automatically become `graded`.
- [ ] Segment `0.9 -> 0.2` has different source/target widths.
- [ ] Selected trajectory highlight does not change semantic width.
- [ ] Explicit relation endpoint inside an aggregate does not become a relation to the whole parent.
