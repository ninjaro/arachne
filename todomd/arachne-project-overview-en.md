# Arachne — project and Evolution overview

This document is a compact orientation guide to Arachne: what the project is trying to preserve, how the data model fits together, what Evolution represents, and where the math comes from.

You do **not** need the math section to understand the product. It is there to make the implementation model less mysterious, not to turn the project into an academic paper.

---

## The short version

Arachne is a provenance-first research system for cultural works, people/groups, concepts, chronology, relationships, and evidence.

The important distinction is between:

- **canonical research data** — human-authored, sourced, reviewable product state;
- **derived analysis** — ranking, similarity, clustering, aggregation, layout, hints, and other disposable interpretations.

Algorithms are allowed to observe and organize the corpus. They are not allowed to silently rewrite its meaning.

The main Viewer product is **Evolution**: an interactive temporal map where works are stations and cultural trajectories pass through them over time.

A trajectory can represent things such as:

- a genre;
- a style;
- a movement;
- a theme;
- a technique;
- a motif;
- later, potentially an agent such as an artist, author, director, composer, or group.

The visual metaphor is metro-like, but the semantics matter more than the metaphor.

A line means:

> these works share a trajectory membership across time.

It does **not** mean:

> work A caused, influenced, or descended into work B.

Documented influence and other explicit work-to-work relations are a different layer.

---

# 1. The project philosophy

Arachne treats cultural interpretation as research rather than as an automatic graph-completion problem.

Human researchers own semantic decisions such as:

- identity;
- source selection;
- quotations and evidence;
- concept assignments;
- historical roles;
- centrality/significance;
- confidence;
- explicit relations.

Algorithms may:

- compare;
- rank;
- cluster;
- measure;
- align;
- find candidates;
- build projections;
- build layouts.

But a score is not permission to change canonical data.

The write boundary is intentionally simple:

```text
algorithm
    ↓
observation / hint
    ↓
human review and research
    ↓
human-authored batch
    ↓
validation
    ↓
explicit apply
    ↓
canonical product database
```

This separation matters throughout the whole project.

A viewer aggregation is not a new historical fact.
A similarity score is not evidence of influence.
A cluster is not automatically a movement.
A visual connection is not automatically a canonical relation.

---

# 2. The main pieces of Arachne

The internal components have mythology-inspired names, but their boundaries are practical.

## Arachne

Owns external orchestration:

- intake;
- scheduling;
- queueing;
- external API surface;
- run status;
- publication orchestration.

It should not own cultural ranking, graph semantics, or layout.

## Pheidippides

Moves bytes.

It deals with:

- fetching;
- retries;
- redirects;
- checksums;
- transport metadata;
- caching.

It deliberately does not decide what the bytes mean.

## Ariadne

Owns derived analysis and presentation logic:

- coverage;
- candidate algorithms;
- ranking;
- grouping;
- projections;
- query planning;
- layouts;
- Viewer-related analysis.

Evolution belongs conceptually on this side.

## Penelope

Owns durable product materialization:

- schemas;
- constraints;
- transactions;
- canonical SQLite;
- staging;
- snapshots;
- exports.

The useful mental split is:

```text
Ariadne asks:
"What useful view can we derive from the current product?"

Penelope asks:
"What is actually stored as product truth?"
```

---

# 3. Canonical data model

The canonical product is broader than Evolution.

The most important entity families are:

```text
works
agents
concepts
manifestations
```

A **work** is the cultural work itself.

A **manifestation** is a particular release/edition/translation/pressing/cut/etc. where that distinction matters.

An **agent** may be a person, organization, or group.

A **concept** may be a genre, style, movement, theme, technique, motif, trope, mood, and similar cultural classification.

Around them are relationships such as:

```text
work ↔ concept
work ↔ work
work ↔ parent work
work / manifestation ↔ agent credits
agent ↔ agent
entity ↔ events
assertion ↔ evidence
```

The Viewer should preserve these distinctions instead of flattening everything into one generic network.

---

# 4. Works can now have hierarchy

Some works contain other works.

Examples:

```text
Series
└── Season
    └── Episode

Album
└── Track

Collection
└── Contained work
```

Canonical membership types include things such as:

- `episode_of`;
- `season_of`;
- `track_of`;
- `volume_of`;
- `issue_of`;
- `chapter_of`;
- `part_of`;
- `collected_in`.

This hierarchy is useful, but it does **not** mean Evolution must become a tree viewer.

Its main value is as information for:

- ordering;
- locality;
- aggregation;
- semantic zoom;
- choosing a useful level of detail.

For example, an album whose tracks have nearly identical cultural profiles may be more useful as one station.

A horror anthology whose episodes have very different profiles may need to remain episode-level.

The medium does not decide the level of detail by itself.

The useful question is:

> how much meaningful trajectory information would be lost if these children were summarized together?

---

# 5. Agent membership is different from work hierarchy

This distinction is important.

```text
episode_of season
```

is containment.

But:

```text
person member_of band
```

is affiliation.

A person may:

- belong to multiple groups;
- join and leave over time;
- have solo credits;
- have credits that must not automatically become group credits.

So this:

```text
Series
└── Episode
```

and this:

```text
Band
└── Person
```

must not be treated as the same kind of hierarchy.

If agent trajectories are added later, a band and its members can be separate trajectories with affiliation shown as additional context.

---

# 6. What the Viewer is

The Viewer is intentionally static-first.

Roughly:

```text
canonical SQLite / product snapshot
        ↓
build-time projections
        ↓
static immutable assets
        ↓
browser
```

The browser should not need write access to the canonical database.

The static Viewer can perform quite sophisticated local analysis because the important product state is already present in the snapshot.

That includes:

- Browse;
- entity inspection;
- Research views;
- Taste/recommendations;
- Evolution.

A future read-only query backend can improve scale and access patterns without changing the product's write boundary.

---

# 7. Evolution: the mental model

The simplest visual model is:

```text
trajectory = line
work       = station
shared work = interchange
time       = left → right
```

Example:

```text
New Wave ─────●──────────●──────────────●────
              │          │
Jump cut ─────●──────────┘
                         │
Handheld ────────────────●────────●──────────
```

The same work can belong to several trajectories, so it becomes an interchange.

This allows cultural relationships to emerge through shared works without claiming a direct causal relationship between the trajectories.

A work may simultaneously be:

- New Wave;
- Jump cut;
- handheld;
- existential;
- crime;
- black-and-white;
- associated with particular agents.

Evolution lets those memberships intersect in temporal space.

---

# 8. A trajectory is not an influence edge

This is probably the most important semantic rule in the Viewer.

Suppose:

```text
Work A   1977   Punk
Work B   1979   Punk
```

Evolution may draw:

```text
Punk ─────● Work A ─────● Work B ─────
```

The connecting line means:

> both works are stops on the Punk trajectory in chronological order.

It does **not** claim:

```text
Work A influenced Work B
```

If the research corpus explicitly contains an influence/adaptation/derivation relation, that relation should be drawn separately.

For example:

```text
trajectory:
──────────────

explicit relation:
- - - - - - - - >
```

This separation keeps the visualization useful without turning layout into historical evidence.

---

# 9. Time is ordered, not metrically linear

Evolution is not a normal timeline.

The horizontal direction preserves chronology, but physical distance is optimized for readability.

So this is allowed:

```text
1950        1951                          1952
|-----|     |--------------------------|  |----|
few works            dense year             few
```

The width of a year may depend on:

- number of visible stations;
- labels;
- interchanges;
- route complexity;
- aggregate density.

Large empty historical gaps may also be compressed.

Therefore:

```text
screen distance ≠ elapsed calendar time
```

The important guarantee is ordering:

```text
earlier → later
```

not a fixed pixel-per-day scale.

---

# 10. Incomplete dates are useful layout freedom

Not every cultural work has an exact date.

Sometimes the corpus only knows:

```text
1978
```

instead of:

```text
1978-05-12
```

If two works cannot be strictly ordered from the available data, Evolution can use that uncertainty as layout freedom.

It may place them in whichever valid order produces:

- fewer crossings;
- straighter trajectories;
- better sibling grouping;
- easier aggregation;
- less label collision.

Exact dates remove this freedom.

A useful implementation principle is:

```text
known order       = constraint
unknown tie/order = optimization freedom
```

This keeps the graph readable while allowing better source data to tighten the layout later.

---

# 11. Earlier and Later are exploration budgets

Evolution is not intended to dump the entire corpus onto the screen at once.

A user starts from one or more trajectories.

Example:

```text
seed: New Wave

Earlier = 1
Later   = 2
```

The seed trajectory is visible as the starting context.

At a work that belongs to another trajectory, the user can switch trajectories without paying a temporal step.

Moving to the next chronological stop costs one step.

Conceptually:

```text
switch trajectory at same work = cost 0

move to next earlier stop = Earlier + 1

move to next later stop   = Later + 1
```

This makes Earlier/Later a domain-specific exploration distance rather than a generic graph radius.

The distinction is useful because historical exploration is directional.

---

# 12. Reachable graph and rendered graph are different

A trajectory may be reachable under the current Earlier/Later rules but still not be drawn prominently.

The Viewer may further control complexity through:

- focus;
- filters;
- pinning;
- trajectory ranking;
- visible trajectory limits;
- bundling;
- aggregation;
- semantic zoom.

So:

```text
reachable graph
    ↓
eligible trajectories
    ↓
visible trajectories
    ↓
rendered scene
```

These are intentionally different layers.

Hiding a trajectory for presentation must not rewrite the underlying traversal semantics.

---

# 13. Aggregation: summarize when detail adds little

Hierarchy makes aggregation especially useful.

Consider an album:

```text
Track 1   Industrial 0.8
Track 2   Industrial 0.9
Track 3   Industrial 0.7
Track 4   Industrial 0.8
...
```

Showing ten separate stations may add little.

An aggregate station can summarize the album.

But another album might look like:

```text
Track 1   Industrial
Track 2   Folk
Track 3   Ambient
Track 4   Noise
Track 5   Spoken word
```

That is a much worse aggregation candidate.

The same logic applies to television.

A largely homogeneous long-running series may collapse well.

An anthology may not.

The basic idea is:

```text
homogeneous children   → collapse is cheap
heterogeneous children → detail is valuable
```

---

# 14. Aggregation can depend on focus

An aggregate does not have to be permanently collapsed.

Suppose a series has 150 mostly similar episodes, but one episode strongly uses Film Noir conventions.

In a general view:

```text
Series ●
```

is probably enough.

But when Film Noir is the active trajectory:

```text
Series ●
        \
         ● exceptional episode
```

may be more useful.

This is not a new canonical fact.

It is a focus-sensitive projection decision:

> normally this child can be summarized, but for the current question it matters.

This lets the Viewer stay simple without permanently discarding detail.

---

# 15. Thickness carries significance

Trajectory width is intended to represent how important a trajectory is to a work or aggregate.

Roughly:

```text
strong / central membership     ═══════

weak / peripheral membership    ───────
```

A trajectory can therefore change width over time:

```text
strong                               weak
● ═══════════════════─────────────── ●
```

This is especially useful for aggregates.

If only 20% of an album's tracks meaningfully belong to Industrial, the Industrial trajectory through the album should be thin.

If almost every track strongly belongs to Industrial, it should be thick.

Width should **not** also encode:

- confidence;
- popularity;
- number of children;
- traversal depth;
- selection.

One visual channel should keep one main meaning.

---

# 16. Centrality has scale semantics

Canonical work-concept assignments contain a numeric centrality value, but not every value should automatically be interpreted under the same scale.

A pair may be marked as:

```text
binary
ordinal
graded
none
```

`none` means the semantic interpretation of the numeric scale has not yet been reviewed.

It does not mean:

```text
zero
irrelevant
unknown
binary
```

This is why Viewer normalization should remain a derived policy.

The raw value can be preserved while the Viewer applies a scale-aware display mapping.

---

# 17. The math, gently

Everything above can be implemented without thinking in mathematical notation.

The notation below is only a compact way to say the same things.

## Works and trajectories

Let:

```text
W = all works
T = all trajectories
```

A membership is simply:

```text
(work, trajectory)
```

So we can think of all memberships as a set:

```text
M ⊆ W × T
```

Example:

```text
(Work A, Punk)
(Work A, DIY)
(Work B, Punk)
```

This already explains the metro structure:

- work = station;
- trajectory = all stations that share one trajectory ID;
- work with several memberships = interchange.

No graph edge is required in canonical data to say that two Punk works are consecutive.

The Viewer derives that line from membership + time.

---

## Temporal order

Each work has some usable temporal value or range.

For simple cases:

```text
time(Work A) < time(Work B)
```

means A must appear before B.

If both are only known as the same year:

```text
time(A) ≈ time(B)
```

the Viewer may have freedom to choose a readable local order.

So layout is not trying to discover historical truth.

It is trying to find a readable arrangement **inside the constraints supplied by the data**.

---

## Traversal distance

For a current stop:

```text
same work, new trajectory:
cost = 0
```

For chronological movement:

```text
previous temporal stop:
earlier cost += 1

next temporal stop:
later cost += 1
```

A traversal state can therefore be thought of as:

```text
(trajectory, temporal group, earlier used, later used)
```

The Viewer keeps states that fit inside the user's budgets.

For connected exploration, more than one path may be useful if neither is strictly more expensive in both directions.

That is the only reason the implementation needs a Pareto-like frontier.

You do not need to expose that term in the UI.

---

## Aggregate coverage

Suppose an aggregate contains `N` child works.

For trajectory `t`, let:

```text
support(t) = number of children carrying t
```

Then:

```text
coverage(t) = support(t) / N
```

Example:

```text
10 tracks
2 Industrial

coverage(Industrial) = 2 / 10 = 0.20
```

This is already useful even before centrality is considered.

---

## Aggregate strength

Suppose the children that contain a trajectory have remapped membership strengths:

```text
0.9, 0.8
```

Their average present-child strength is:

```text
mean = 0.85
```

One possible display heuristic is:

```text
aggregate strength
    = coverage × mean strength when present
```

So:

```text
0.20 × 0.85 = 0.17
```

That would produce a thin line.

This formula is only a sensible **viewer heuristic**, not canonical research semantics.

It should remain replaceable.

The useful distinction is:

```text
coverage
```

versus:

```text
strength when present
```

because these cases are different:

```text
20% coverage × very strong

100% coverage × very weak
```

even if a final display score happens to be similar.

---

## Similarity for collapse

To decide whether sibling works are similar enough to aggregate, each work can be represented as a sparse trajectory-strength profile.

Example:

```text
Track A:
Industrial 0.8
Post-punk  0.5

Track B:
Industrial 0.9
Post-punk  0.4
```

A simple starting similarity is weighted Jaccard:

```text
similarity(A, B)
    = sum(min(A_t, B_t))
      ------------------
      sum(max(A_t, B_t))
```

You do not need to remember the formula.

Its behavior is intuitive:

- same tags with similar strengths → close to 1;
- different tags → closer to 0.

It is a useful baseline, not a permanent theory of cultural similarity.

---

## Layout optimization

The layout engine is choosing positions, not facts.

Very roughly it tries to minimize something like:

```text
layout cost =
    crossings
  + bends
  + trajectory wiggle
  + label collisions
  + marker collisions
  + unnecessary spread
```

while respecting hard constraints such as:

```text
chronological order
exact-date placement
visible station identity
trajectory membership
```

Hierarchy can add soft preferences such as:

```text
keep siblings near each other
```

Incomplete dates increase the number of legal layouts and therefore give the optimizer more room to improve readability.

---

# 18. Canonical vs derived: the easiest way to reason about the project

When unsure where something belongs, ask:

> Is this a researched assertion about the cultural object, or is it a useful way of viewing the current corpus?

Canonical examples:

```text
Work X has concept Y
Work X influenced Work Z
Episode A is part of Season B
Person P is a member of Group G
source quote supports assertion Q
```

Derived examples:

```text
these two tags are similar
this trajectory is currently ranked #7
these ten tracks are homogeneous enough to collapse
this aggregate is 20% Industrial
this line should be 2.3 px wide
this node should be at x=481
this work is an outlier under the current Film Noir focus
```

Derived results can be useful, sophisticated, and reproducible.

They still do not become canonical automatically.

---

# 19. What Evolution is ultimately trying to answer

Evolution is useful when a user asks questions such as:

- Where does a cultural trajectory appear over time?
- Which works sit at the intersection of several trajectories?
- What becomes visible if I follow this trajectory earlier or later?
- Where do two trajectories overlap?
- Which works are central examples and which are peripheral?
- Where does a mostly stable series/album contain an important exception?
- Which explicit documented relations connect works inside this larger structural context?
- How does the picture change when I focus on a different concept?

It is less useful as:

- a generic force-directed knowledge graph;
- a literal family tree of genres;
- an automatic influence detector;
- a complete world-history timeline;
- a graph where every possible relationship is permanently visible.

---

# 20. What is intentionally still open

Several implementation policies should remain replaceable for now:

- exact remapping of `binary`, `ordinal`, `graded`, and `none` centrality scales;
- exact aggregate-strength formula;
- exact semantic-homogeneity threshold;
- exact outlier score;
- exact collapse level for multi-year parents;
- exact timing for agent trajectories;
- use of future tag-tag similarity analysis;
- final visual treatment of unknown strength.

These are analysis/viewer policies.

They should not accidentally become permanent product semantics just because one demo implementation needed a number.

---

# 21. If you only remember seven things

1. **Canonical research and derived analysis are different layers.**
2. **Evolution is the main Viewer product.**
3. **Works are stations; trajectories pass through works over time.**
4. **A trajectory is membership continuity, not influence.**
5. **Earlier/Later control exploration, while focus/aggregation control rendering complexity.**
6. **Hierarchy is mostly a tool for better aggregation and ordering, not a reason to make the UI hierarchical.**
7. **The Viewer may simplify the corpus aggressively, but it must not silently invent new cultural facts.**
