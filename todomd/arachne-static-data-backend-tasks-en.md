# Arachne Viewer — static data / DB / backend tasks

Scope of this iteration: **static Viewer and browser-side Evolution projection**. Do not build a full runtime backend yet.

The primary product of the Viewer is Evolution: a metro-like temporal map of works and trajectories. The Viewer reads an immutable/static snapshot. Hierarchy, aggregation, focus-sensitive collapse, and line-width rendering are derived viewer logic, not new canonical facts.

Canonical semantics that must remain intact:

- work hierarchy (`episode_of`, `season_of`, `track_of`, etc.) is part/composition;
- `agent member_of group` is a separate affiliation relation, not work hierarchy;
- concepts/centrality remain canonical assignments;
- aggregate stations, aggregate strengths, collapse decisions, and similarity scores are disposable projection state;
- ordinary trajectory continuity is not a canonical influence relation;
- explicit relations remain separate evidence-backed edges.

## Static data / schema

- [ ] Do not add a runtime backend in this iteration.
- [ ] Keep the Viewer running from immutable/static snapshot and build-time projections.
- [ ] Use existing canonical `work_memberships` as the hierarchy source; do not introduce a duplicate viewer-only hierarchy table in SQLite.
- [ ] Support the existing membership types: `episode_of`, `season_of`, `track_of`, `volume_of`, `issue_of`, `chapter_of`, `part_of`, `collected_in`.
- [ ] Keep `agent_relations.member_of` separate from work hierarchy.
- [ ] Do not write aggregate profiles, collapse decisions, semantic homogeneity, outlier score, similarity score, or display strength back into canonical SQLite.
- [ ] Do not add canonical fields such as `is_anthology`, `collapse_level`, `viewer_strength`, `aggregate_profile`.
- [ ] Do not change canonical centrality values for renderer convenience.

## Catalog / static projection

- [ ] Ensure compact `catalog.json` contains everything Evolution needs:
  - works;
  - dates;
  - concepts;
  - centrality;
  - `centrality_scale`;
  - work memberships;
  - agents/credits;
  - agent relations;
  - explicit work relations.
- [ ] If hierarchy is already fully exported, do not add a second parallel contract just for Evolution.
- [ ] If the inspector needs parent/child labels, derive them from already-exported works + memberships.
- [ ] Do not merge direct parent concept assignments with child-derived aggregate support into a single assignment at build time.

## Centrality

- [ ] Keep `binary`, `ordinal`, `graded`, `none` in the snapshot as original pair-local semantics.
- [ ] Perform remapping for comparable viewer strength in frontend/analysis policy.
- [ ] Do not rewrite raw 1..100 values in the DB to force one scale without a separate canonical data-model decision.
- [ ] Do not persist an aggregate formula such as `coverage × meanStrengthWhenPresent` as canonical centrality.

## Demo data

- [ ] For redesign/static demo, use real hierarchy examples from the current SQLite where possible:
  - album + tracks;
  - series + seasons/episodes;
  - homogeneous hierarchy;
  - heterogeneous/anthology-like hierarchy.
- [ ] If real data is insufficient for a specific behavior, use a clearly synthetic frontend fixture.
- [ ] Synthetic fixture data must not enter the canonical product snapshot.
- [ ] Do not invent plausible influence relations, historical claims, exact dates, or credits and present them as product data.

## Do not wire these yet

- [ ] Do not connect backend tag-tag similarity to hierarchy collapse.
- [ ] Do not make manually curated concept relations a required part of aggregation.
- [ ] Do not build a graph database/Neo4j.
- [ ] Do not build a server-side Evolution scene API.
- [ ] Do not move traversal/layout to the server.
- [ ] Do not add account/user-state backend just for pinned/focus demo.

## Static API / publication

- [ ] If `/api/v1` remains visible in the UI, make sure the static API is actually generated in the publication build path.
- [ ] If the static API still does not make it into the production bundle, either fix build integration or do not make API a visible part of the redesign demo.
- [ ] Do not make UI layout/SVG scene the fundamental API contract: the API should describe works, agents, concepts, memberships, dates, relations, and evidence; the Evolution scene remains a derived projection.
