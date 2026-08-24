# Mining guidelines

Miners expand the corpus by finding sources and turning them into sourced,
structured data.

Miners are expected to research sources independently. They are **not** expected
to analyse the database as a whole, develop historical theories, redesign the
ontology, or interpret analytical output.

Mining has **two distinct modes**. Choose the mode before starting an assignment.
If an assignment does not explicitly say otherwise, treat it as **Mode 1**.

## The two mining modes

### Mode 1 — initialization and general metadata

Mode 1 is the default high-volume pass.

Its goal is to **create or resolve the right works and agents and close the
important general structure around them**.

Typical Mode 1 work includes:

- canonical identity;
- names, aliases, transliterations, and credited names;
- dates and date ranges;
- countries and languages;
- external identifiers;
- works and manifestations;
- memberships and structural containment;
- agent memberships and corporate relationships when explicitly documented;
- credits;
- production companies, publishers, labels, distributors, broadcasters, and
  similar directly documented relationships;
- release, publication, recording, performance, exhibition, and other events;
- recurring collaborators;
- edition/release/manifestation structure;
- useful direct measurements and production facts;
- sparse long-tail metadata in `production_info_json`.

The Mode 1 goal is **high-volume, reasonably accurate corpus coverage**.

Do not hold up a useful initialization pass because every secondary fact is not
perfectly resolved. Occasional local mistakes can be corrected later.

Mode 1 is **not semantic tagging**. Do not opportunistically add:

- genres;
- styles;
- movements;
- themes;
- motifs;
- historical roles;
- influence claims;
- work-concept assignments;
- concept relations;
- parent-guide assertions;
- centrality judgments;
- other evidence-bearing cultural interpretation.

If you encounter a strong source for such a claim while doing Mode 1, keep the
source as a lead if useful and leave the assertion for Mode 2.

### Mode 2 — semantic and evidence enrichment

Mode 2 is a separate evidence-bearing pass.

Its goal is to add **cultural, semantic, historical, interpretive, or content
assertions that require attributable support**.

Typical Mode 2 work includes:

- genres and styles;
- movements and scenes;
- themes and motifs;
- historical terminology;
- work-concept assignments;
- concept relations;
- parent-guide assertions;
- documented influence or derivation;
- centrality scale and centrality;
- confidence;
- historical role;
- other explicit cultural assertions supported by source text.

Mode 2 should usually operate on works and agents that already exist from Mode
1. If a required entity is missing, create or resolve the minimum structural
record needed to support the semantic work, but do not turn the task into a
general catalogue sweep unless that is explicitly requested.

The Mode 2 goal is **inspectable, source-backed semantic data**, not maximum
throughput.

### Do not blur the modes

A miner should know which mode an assignment is using.

Do not convert a Mode 1 assignment into a semantic sweep merely because tags or
historical claims seem obvious.

Do not convert a Mode 2 assignment into a broad oeuvre-completion task unless
missing structure prevents the semantic work.

A batch may contain both kinds of changes when an assignment explicitly calls
for both, but the distinction should remain clear.

## Rules shared by both modes

### Work from sources, not from the graph

Use the available database snapshot to:

- find existing entities;
- reuse existing IDs;
- check names and external identifiers;
- avoid obvious duplicate works, agents, and concepts;
- avoid knowingly repeating an existing assertion.

Do not use the database to decide what *should* be true.

For example, do not add a concept because similar works already have it, copy
centrality values from neighboring works, infer a missing relation from the
graph, or assume an existing classification is correct because it is common.

The database available to a miner may be slightly stale because multiple miners
work in parallel. Treat it as a best-effort deduplication snapshot, not a
complete current view of the corpus.

Perform reasonable duplicate checking against the snapshot you have. Do not
spend excessive time trying to eliminate every possible concurrent collision.

Absence from your snapshot means only that the entity or assertion is not
visible in that snapshot.

### Find sources independently

Choosing and finding sources is part of mining.

Use sources appropriate to the fact being mined.

For **Mode 1 identity and general metadata**, structured databases, catalogues,
official credits, publisher or label pages, institutional records, and ordinary
reference sources may be sufficient.

For **Mode 2 cultural assertions**, prefer sources that actually make the
relevant claim in attributable text.

Useful sources include:

- books and academic publications;
- journalism and criticism;
- specialist publications;
- interviews and artist statements;
- catalogues and liner notes;
- label, publisher, gallery, or festival material;
- fanzines;
- scene publications;
- archived websites;
- blogs;
- forums and mailing-list archives;
- other contemporary material.

Source prestige is not the only criterion.

For underground, obscure, regional, or historical scenes, a contemporary
fanzine or interview may be more useful than a later general encyclopedia.

A database tag can be a useful lead. It should not automatically become a
cultural assertion merely because another database contains it.

### Underground material is normal material

Do not lower coverage ambitions because a work or agent is obscure.

Do not require mainstream or academic recognition before mining something.

For poorly documented scenes, search closer to the scene itself.

Contemporary terminology, local criticism, fanzines, archived discussions,
artist interviews, catalogues, and specialist publications can be particularly
valuable.

Do not "clean up" unusual historical terminology merely because a modern
classification would be simpler.

### Optimize for useful mass

Do not attempt to make every entity perfect before moving on.

In Mode 1, prefer broad useful coverage of identity, works, credits, and direct
structure over disproportionate polishing of one record.

In Mode 2, prefer a smaller number of genuinely supported assertions over a
large quantity of weak tags.

It is acceptable to omit uncertain secondary information.

It is acceptable for later miners or researchers to enrich and correct earlier
work.

Do not fill gaps with guesses merely to make a record look complete.

Invented facts, unsupported interpretation, and systematic copying of existing
database assumptions are not acceptable.

## Mode 1 rules — initialization and structure

### Prefer direct product facts

Mode 1 should primarily extract facts such as:

- work identity and metadata;
- agent identity;
- names and aliases;
- credits;
- structural work memberships;
- explicit agent relations;
- work and manifestation events;
- manifestation or edition identity;
- external identifiers;
- directly documented production, publication, label, distribution, or
  organizational relationships.

General metadata is stored on a best-effort basis and is not an authoritative
factual record. Values may be incomplete, stale, or incorrect.

External identifiers and links are useful because they let users consult the
original databases and sources when authoritative detail matters.

### Choose the structural target deliberately

Use `work_memberships` only for structural containment.

An episode may point directly to its series; add a season work only when the
season is independently useful.

Use `position` for reliable machine order and `position_text` for the
source-native expression (`S06E06`, `Disc 2 / Track 4`, `Vol. 3`).

Do not put adaptations, remakes, sequels, prequels, spin-offs, or influence into
a membership row. Those are derivation/editorial relations, not containment.

Use `agent_relations` only when a source explicitly supports membership or a
corporate relationship.

Preserve uncertain or irregular periods in `period_text` rather than inventing
exact years, and use free-text `role_text` for the role during that relation.

A shared work credit is not proof that a person was a member of a group.

A credit targets the work only when it describes the work across versions.

Target a manifestation for release/edition-specific publishers, distributors,
labels, platforms, broadcasters, translators, illustrators, or printers.

Do not copy a manifestation credit onto its work merely to make it visible.

A production company may remain a work-level `production_company`; do not add a
generic studio role.

Use `events` for recurring independent dates such as creation, publication,
release, premiere, broadcast, performance, exhibition, or recording.

Target the manifestation when the event belongs to that particular
edition/release.

Use `date_precision = month` for year-month knowledge and `exact` only for a
full exact date.

Preserve a source-native expression in `date_text` instead of forcing fake
precision.

Keep `works.year_start/year_end` as the compact summary; an event does not
automatically replace it.

### Keep the long tail in the long tail

Keep `production_info_json` as the long tail.

Move clearly repeated graph structure, relations, release-specific credits, and
recurring events into their first-class tables during ordinary mining or
maintenance, but do not attempt a wholesale rewrite.

Materials, processes, instruments, tools, apparatus, components, technique
notes, format details, and rare museum fields normally remain JSON.

Current corpus frequency does not justify separate holdings, agent-category, or
manifestation-level copy-count tables, extra measurement kinds, or new
manifestation types.

Do not improvise those structures in a batch.

Keep sparse holdings, edition-size, and unusual technical facts in
`production_info_json`.

Keep organization specialties out of `agent_type`.

Keep the existing version-like manifestation types.

Broadcast, premiere, performance, and exhibition are events, never
manifestation types.

## Mode 2 rules — semantic and evidence enrichment

### Preserve the claim and its evidence

For meaningful cultural assertions, preserve evidence well enough that another
person can later inspect the decision.

Prefer an exact relevant quotation.

Record the source and locator accurately.

Preserve translation where needed.

Do not alter a quotation to make it support a stronger claim.

A miner does not need to prove an entire historical theory. The evidence only
needs to support the assertion being added.

Historical use of terminology is itself useful evidence.

A contemporary source showing that a scene called something `X` is worth
preserving even when it does not prove that `X` is objectively a separate
genre.

Contradictory evidence is valid data. Do not silently convert disagreement into
consensus.

### Encode what the source supports

Mode 2 may extract claims such as:

- concepts applied to a work;
- explicitly described relationships;
- historical terminology;
- documented influence or derivation;
- parent-guide/content assertions;
- historical roles;
- other cultural assertions directly supported by the source.

Do not extend the source beyond what it reasonably says.

In particular:

```text
earlier + similar
    != influence

frequent co-occurrence
    != broader/narrower relation

same agent
    != influence

algorithmic similarity
    != canonical relation

database pattern
    != evidence
```

If a source explicitly makes the stronger claim, mine it.

If the stronger claim is your own conclusion, leave it for researchers.

### Centrality scale, centrality, confidence, and historical role

Both `centrality_scale` and `centrality` describe one specific work × concept
assignment. They are never global properties of a concept.

Choose among the reviewed scales using only this work, this concept, and the
available sources:

- `binary` means presence/absence is the meaningful distinction;
- `ordinal` means a small number of ordered salience levels are meaningful, but
  fine numerical precision is not;
- `graded` means a substantially continuous `1–100` importance distinction is
  meaningful for this pair.

The same concept may legitimately be binary on one work, ordinal on another,
and graded on a third. Do not copy a scale from another work, infer it from the
concept type, or choose it from global database distributions.

`centrality_scale = none` is reserved for legacy assignments that have not yet
received this pair-level semantic review. It does not mean binary, irrelevant,
zero, absent, or unknown centrality.

The stored numeric `centrality` remains available as a numeric fallback for
consumers while the scale is unreviewed. That fallback is not evidence that the
stored number is semantically calibrated. Never translate `none` to another
number or mode.

New work-concept assignments must use `binary`, `ordinal`, or `graded`.

When revisiting a legacy `none` assignment, make the semantic decision manually
in an ordinary miner-authored batch. If its old number no longer expresses the
chosen scale, update both `centrality_scale` and `centrality`.

A batch that changes the centrality of a `none` assignment must set a reviewed
scale in the same update. Unrelated changes do not require resolving old scale
debt, and a legacy assignment may remain `none` until it is genuinely reviewed.

`centrality` means how important the concept is for understanding this specific
work. It does not mean certainty that the concept is present.

Reconsider the numeric value under the chosen pair-level scale, but do not
manufacture precision, spread values merely to fill the `1–100` range, or
algorithmically commit a guessed scale.

The local decision is:

```text
this work + this concept + available sources/evidence
    -> centrality_scale
    -> centrality
```

`confidence` means how certain the assertion is given the available evidence.
Keep it independent from both centrality fields.

`historical_role` is a stronger historical interpretation. Use it when the
source material provides reasonable support.

Do not assign `precursor`, `formative`, `canonical`, `transitional`, `revival`,
or similar roles merely from date, popularity, graph position, or personal
judgement.

When a stronger value is unclear, leaving it unset is preferable to inventing
precision.

### Concepts

Reuse an existing concept when it clearly represents what the source is
describing.

Do not create an intentional synonym merely because another spelling or wording
is convenient.

At the same time, do not collapse genuinely different historical terminology
merely because two labels look similar.

Do not attempt to solve the global ontology while mining.

Questions such as:

- whether a label is a "real genre";
- whether two concepts should eventually merge;
- whether one concept is really a subtype of another;
- whether a concept should be reclassified globally;

belong to researchers when they require corpus-level reasoning.

The miner's task is to encode the source reasonably using the current model.

### Prefer work-level observations

When possible, attach cultural concepts to specific works rather than treating
an agent as permanently belonging to one style or genre.

Agents change over time.

Accurate credits and dated works are valuable even when their wider historical
importance is not yet clear.

Do not attempt to reconstruct an agent's artistic evolution yourself. Later
analysis and research can use the accumulated work-level data to do that.

### Do not manually perform structural analysis

Mode 2 does not mean inventing every observable feature of a work.

Miners are not expected to manually annotate every feature or reconstruct a
work's complete internal structure.

Do not routinely invent annotations for:

- narrative arcs;
- camera patterns;
- composition statistics;
- motif frequency;
- musical transitions;
- structural similarity;
- genre trajectories;
- cluster membership.

Mine such information when a source explicitly discusses it and it fits the
current data model.

Otherwise leave structural analysis to researchers and analytical tooling.

### Do not follow analytical hints as facts

If miners are exposed to generated hints or research notes, treat them only as
possible directions for source search.

A hint may tell you:

> investigate whether A influenced B.

Your task is then to find evidence.

If no convincing source is found, do not add the influence relation.

The hint itself is never evidence.

## Miner and researcher boundary

A **Mode 1 miner** asks:

> What reliable general information can I find about this agent or work, and
> what stable entities and structural facts should exist?

A **Mode 2 miner** asks:

> What cultural or semantic claims do the sources explicitly support, and what
> evidence should be preserved with those assertions?

A researcher may later ask:

> What does this accumulated corpus imply about genres, lineages, historical
> transitions, concept boundaries, cross-media relationships, missing data, or
> the data model itself?

That third question is outside ordinary mining.

A good Mode 1 contribution leaves behind a broad, useful, navigable corpus.

A good Mode 2 contribution leaves behind inspectable semantic assertions with
enough trustworthy evidence for someone else to review.

Neither mode needs to explain the whole history around an entity.
