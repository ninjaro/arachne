# Extend the product model and remove schema/version compatibility

The project should keep **one current product schema and one current batch format only**. There are no maintained release branches and no requirement to open older database versions with newer code. If an older state is needed, use the corresponding Git commit.

Do not build compatibility layers, multi-version dispatch, upgrade chains, or permanent migration infrastructure. When the schema changes, update the canonical database and the current contract together. Any temporary conversion script used while making the change should be removed once the repository is updated.

- [ ] Replace versioned product schema files such as `schema/product_v7.sql` with a single current schema file, e.g. `schema/product.sql`.
- [ ] Remove old product schema files that are kept only for historical compatibility.
- [ ] Remove permanent `vN -> vN+1` database migration scripts and their compatibility tests.
- [ ] Remove `PRAGMA user_version` as an application-level compatibility mechanism unless SQLite itself still needs a harmless internal value.
- [ ] Remove code that rejects a database because it is not a particular historical schema version.
- [ ] Keep validation of the **current expected table/index/trigger structure**, but do not treat that structure as one member of a supported version set.
- [ ] Replace `arachne_batch_v2` with one unversioned current batch discriminator, preferably `arachne_batch`.
- [ ] Rename the batch schema/example files accordingly, e.g. `arachne_batch.schema.json` and `arachne_batch.json`.
- [ ] Remove batch major-version discovery, unsupported-major handling, compatibility wording, and version dispatch for product-inbox batches.
- [ ] Do not add `format_version`, extension negotiation, or compatibility aliases to the product batch.
- [ ] Update docs so they state that the repository supports only the schema and batch shape present in the current commit.

The product database should continue to be strict about its **current** structure. This simplification is about removing historical compatibility, not about making the schema loose.

## Work-to-work structure

The database needs first-class structural relationships between works. `production_info_json` currently carries information such as series, season, episode, collection, volume, and similar parent/child structure that should be queryable as graph data.

- [ ] Add a `work_memberships` table.

Suggested shape:

```sql
work_memberships
----------------
id
child_work_id
parent_work_id
membership_type
position
position_text
```

- [ ] `child_work_id` and `parent_work_id` should reference `works`.
- [ ] Reject self-membership.
- [ ] Allow the same child and parent to have different meaningful membership rows when necessary.
- [ ] Do not require evidence/source rows for this metadata.
- [ ] Add indexes for both child and parent lookup.

Initial `membership_type` values:

```text
episode_of
season_of
track_of
volume_of
issue_of
chapter_of
part_of
collected_in
```

`position` is for machine-friendly ordering when known. `position_text` is for source-native numbering such as `S06E06`, `Disc 2 / Track 4`, `Vol. 3`, or other irregular labels.

Do not require artificial intermediate works. An episode may point directly to a series when no separate season entity is useful. A season may also be represented as a work when it is useful as an entity.

- [ ] Extend the batch contract with `create.work_memberships`.
- [ ] Add deletion support for membership rows.
- [ ] Make local IDs usable for works created in the same batch.
- [ ] Expose memberships in product export/projection code where work structure is needed.
- [ ] Move clearly structural fields out of `production_info_json` when they are encountered during normal mining or maintenance; do not require a full cleanup of all historical JSON immediately.

A separate general-purpose `work_relations` table may still be useful for non-membership relationships such as:

```text
adaptation_of
remake_of
sequel_to
prequel_to
spin_off_of
version_of
based_on
```

Do not overload `work_memberships` with these. Membership answers “this work is structurally contained in that work”; these relationships express derivation or narrative/editorial relation.

## Agent-to-agent relationships

The database needs first-class relationships between people, groups, organizations, studios, labels, publishers, and companies.

- [ ] Add an `agent_relations` table.

Suggested shape:

```sql
agent_relations
---------------
id
subject_agent_id
relation_type
object_agent_id
from_year
to_year
period_text
role_text
```

- [ ] `subject_agent_id` and `object_agent_id` should reference `agents`.
- [ ] Reject self-relations.
- [ ] `from_year` and `to_year` are optional but must allow a real time interval when known.
- [ ] `to_year >= from_year` when both are present.
- [ ] `period_text` should preserve non-clean periods such as `late 1970s`, `c. 1994–1996`, `until 2003`, or other source-native wording without forcing fake precision.
- [ ] `role_text` should remain free text rather than a large enum. It can hold values such as `vocals`, `guitar`, `artistic director`, `editor-in-chief`, etc.
- [ ] Do not require evidence/source rows for ordinary membership or corporate metadata.
- [ ] Allow multiple rows for the same subject/relation/object so repeated membership periods can be represented. Do **not** make `(subject_agent_id, relation_type, object_agent_id)` globally unique.

Initial `relation_type` values:

```text
member_of
founder_of
subsidiary_of
division_of
imprint_of
owned_by
successor_of
predecessor_of
```

Potentially add `persona_of` if the corpus needs explicit stage-persona / project-person relations.

- [ ] Extend the batch contract with `create.agent_relations`.
- [ ] Add deletion support for agent-relation rows.
- [ ] Support local IDs for agents created in the same batch.
- [ ] Expose these relations in graph/product projections.
- [ ] Do not infer `member_of` from shared credits. A person being credited on a group work is not automatically proof of membership.

Keep `agent_type` small:

```text
person
organization
group
```

Do not turn `agent_type` into `studio`, `publisher`, `record_label`, `museum`, `gallery`, `distributor`, etc. Those are roles/categories that can overlap.

## Credits should target works or manifestations

The current credit model attaches every credit to a work. This is too coarse for release-specific metadata.

A publisher of a particular edition, a distributor of a film release, a record label of a pressing, a translator of a translation, a broadcaster, or a streaming platform belongs naturally to a manifestation.

- [ ] Change credits from `work_id` to a generic `entity_id`.
- [ ] Restrict credit targets to entities of type `work` or `manifestation`.
- [ ] Preserve existing work-level credits by copying the current `work_id` value into `entity_id`.
- [ ] Update indexes and uniqueness constraints for the new target column.
- [ ] Update batch validation and inbox application logic accordingly.
- [ ] Allow local manifestation IDs as credit targets in the same batch.

Add these credit roles first:

```text
distributor
broadcaster
platform
translator
illustrator
printer
curator
choreographer
narrator
lyricist
songwriter
arranger
sound_engineer
designer
animator
```

`distributor` is especially important. Existing film data is already forced to misuse `publisher` for distributors, releasing companies, broadcasters, and platforms.

Do not add a generic `studio` credit if `production_company` already expresses the work-level relationship adequately. A studio remains an `organization`; its role toward a work is `production_company`.

## Work media

Do not add `autobiography` as a medium. It is a genre/form and can remain a concept.

The current medium enum mixes broad media with literary forms and has several obvious catch-all cases. Add only broad values that remove recurring distortion.

- [ ] Add `nonfiction`.
- [ ] Add `comic`.
- [ ] Add `performance`.

Use concepts for narrower cultural/form distinctions:

```text
autobiography
memoir
biography
manga
documentary
travel writing
reportage
```

For manga, prefer:

```text
medium = comic
concept = manga
```

Do not add both `comic` and `manga` as fundamental media.

Do not add `track` as a medium merely because tracks belong to albums. A musical work can remain `composition` and use `track_of` membership. If the project later needs a strict distinction between composition, recording, and release track, solve that as a separate model change rather than adding overlapping media now.

## Dates and events

The current `date_precision` enum needs at least month precision. Existing data can contain year-month values that should not be called fully exact.

- [ ] Add `month` precision.
- [ ] Prefer date precision semantics that distinguish at least:

```text
year
month
exact
decade
approximate
range
```

`exact` should mean a full exact date, not merely “more precise than a year”.

Different media also have several independent dates: creation, publication, premiere, broadcast, recording, exhibition, performance, release. Do not keep forcing all of them into `works.year_start/year_end`.

- [ ] Add a compact `events` table if these fields continue to recur.

Suggested shape:

```sql
events
------
id
entity_id
event_type
year_start
year_end
date_text
date_precision
place_text
```

Initial `event_type` values:

```text
created
published
released
premiered
broadcast
performed
exhibited
recorded
```

- [ ] Allow event targets to be works or manifestations.
- [ ] Keep `place_text` simple for now; do not introduce a geographic ontology just for event locations.
- [ ] Do not require evidence/source rows for ordinary event metadata.
- [ ] Keep `works.year_start/year_end` as compact canonical summary fields if they remain useful for common queries.

Do not model `broadcast`, `premiere`, `performance`, or `exhibition` as manifestation types. They are events, not versions of a work.

## Manifestations

Keep manifestations focused on versions/releases/editions of a work.

Current useful types:

```text
edition
translation
release
pressing
cut
restoration
reissue
```

- [ ] Review whether a few additional version-like manifestation types are actually needed after credits and events are fixed.
- [ ] Do not add event-like values merely to absorb missing schema elsewhere.

## Museum and collection holdings

Museum/repository/accession metadata exists, but it is less important than structural graph relationships.

If this becomes common enough to query directly:

- [ ] Add a small `holdings` table instead of encoding museums as credits.

Possible shape:

```sql
holdings
--------
id
entity_id
agent_id
holding_type
accession_number
```

Possible types:

```text
collection
repository
custodian
owner
```

Until there is enough value in normalizing this, leaving museum-specific details in `production_info_json` and external IDs is acceptable.

## Agent categories

Do not expand `agent_type` into organization specialties.

If the viewer/research layer eventually needs direct queries such as “all museums”, “all labels”, or “all studios”:

- [ ] Add an optional many-to-many agent classification mechanism, or represent these categories through concepts.

Possible categories include:

```text
studio
publisher
record_label
museum
gallery
festival
distributor
platform
```

This is lower priority than `agent_relations`.

## Measurements and edition-specific facts

Do not expand measurements aggressively.

Current measurements are useful for:

```text
duration
height
width
depth
pages
```

Edition size/copy count is not really a geometric measurement.

- [ ] If edition-size data becomes useful, store something like `copy_count` on a manifestation-level fact rather than forcing it into `measurements`.
- [ ] Leave rare values such as weight, technical channels, frame counts, and unusual physical details in `production_info_json` until there is a real query need.

## `production_info_json`

Keep this field. It is useful as a low-cost tail for irregular metadata.

Do not try to normalize everything currently stored there.

Normalize data when it:

- connects two canonical entities;
- is required for identity/deduplication;
- is repeatedly needed in core queries;
- or currently forces the database to record a clearly wrong semantic role.

Good candidates to move out:

```text
series / season / episode / collected_in
parent-child work structure
agent memberships and corporate relations
release-specific publisher/label/distributor metadata
recurrent event dates
```

Good candidates to leave in JSON:

```text
materials
processes
instruments
tools
filming locations
apparatus
components
technique notes
format details
rare museum-specific fields
other descriptive metadata
```

The goal is not to eliminate JSON. The goal is to stop using JSON as a substitute for missing graph edges.

## Ordinary metadata reliability

Do **not** add source/evidence tables to every ordinary metadata fact.

The project accepts that general metadata can be incomplete or wrong. External IDs and links to specialist databases are sufficient for provenance at this level.

Document this clearly:

> General metadata is stored on a best-effort basis and is not an authoritative factual record. Values may be incomplete, stale, or incorrect. External identifiers and links are provided so users who need authoritative detail can consult the original databases and sources.

Keep the stronger evidence model for cultural assertions, historical interpretation, influence, terminology, and similar claims where the source text itself matters.

## Current-contract cleanup in code

The repository currently duplicates enums and current-schema assumptions across SQLite schema, JSON Schema, C++ validation, inbox application, and projections.

- [ ] Update all of those places to the new single current schema/contract.
- [ ] Remove code whose only purpose is recognizing or rejecting historical product schema or batch versions.
- [ ] Rename versioned constants and filenames where versioning no longer serves a purpose.
- [ ] Keep current-contract validation strict: unknown fields and invalid enum values should still be rejected.
- [ ] Keep the product database table/index/trigger shape explicit so accidental schema drift is still detected.
- [ ] Update product export/viewer code for new relation tables and the generalized credit target.
- [ ] Update mining documentation so miners know when to use work memberships, agent relations, manifestation-level credits, events, and `production_info_json`.

The repository history is the compatibility mechanism. The current commit defines the only supported product schema and the only supported batch format.
