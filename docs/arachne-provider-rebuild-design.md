# Arachne Provider-Driven Rebuild Notes

This document records the technical decisions and implementation details agreed in the current discussion. It is intentionally written as a working engineering note: theory is separated from actionable tasks, tasks use checkboxes, there are no phases/stages, and there are no acceptance-criteria placeholders or generic “test everything” tasks.

---

## Core authority split

Arachne should maintain two deliberately separate trust domains.

**Automatic provider mining owns general information.** General information is descriptive metadata that can be re-derived from foreign databases and does not require human evidence in Arachne.

Examples include:

- foreign/provider IDs;
- names and aliases;
- work/agent type;
- birth/death dates and years;
- work creation/publication/release dates;
- country;
- language;
- credits;
- series/season/episode grouping;
- album/track grouping;
- volume/issue/chapter/collection grouping;
- images and other provider-supplied media hints;
- other descriptive fields that are part of the automatic provider contract.

**Human mining owns tags, assertions, and evidence.**

Humans should not create ordinary agents or works manually, should not fill ordinary general metadata manually, and should not manually create work groupings that the provider pipeline can derive.

Provider databases are not accepted as evidence for the human-evidence layer. If a provider is automatically mined for general information, a miner must not use that same provider as evidence for a tag/assertion that requires evidence.

The HPC/provider pipeline must not search for tag evidence, supporting citations, or interpretive proof. Its job stops at automatically recoverable general information and graph structure.

This is a hard trust-boundary decision, not merely a workflow preference.


---

## Clean rebuild instead of backward-compatible migration

Backward compatibility, rollback compatibility, and preservation of the old product state are not design requirements.

The existing product database should be backed up once and retained as a read-only reference for miners. The new database is a clean rebuild.

The new database must **not** inherit:

- old human tags;
- old evidence;
- old assertions;
- old general-information values;
- old grouping rows;
- old credits;
- old images;
- old score state.

The reason for dropping old human knowledge is deliberate: some old tags/evidence were derived from external databases that are now considered invalid evidence sources, and the old score logic has not been migrated. Miners can inspect the backup and manually rebuild tags/evidence under the new rules.

The only thing extracted from the old database for the rebuild is known foreign identity, represented through `priority.json`.

The old backup is therefore a miner reference, not a migration source of truth.


---

## `priority.json`

Priority seeds should use JSON rather than CSV.

The required semantic content is only a provider name and an array of IDs.

Example:

```json
{
  "wikidata": [
    "Q123",
    "Q456"
  ],
  "musicbrainz": [
    "abc-def",
    "xyz-123"
  ]
}
```

No timestamps, notes, requested-by metadata, priorities, or extra bookkeeping are required unless a later concrete need appears.

Priority is an explicit bypass of normal candidate ranking and normal expansion-budget limits.

Priority IDs are processed before ordinary algorithmic expansion.

A successfully materialized priority ID is removed from `priority.json`.

A priority ID that cannot yet be resolved/materialized remains in `priority.json` for a future full provider pass.

The first clean rebuild uses the old database only to seed this file with existing provider IDs.


---

## Priority agents

The work-budget formula does not directly constrain agents.

A priority entry will often be an agent because a miner may be interested in a direction that does not yet exist in the product database.

A priority agent should cause the provider pipeline to discover and materialize all relevant works that can be found for that agent across the connected external databases, even if this exceeds the normal work-expansion budget.

If the system cannot find any work for the priority agent:

- do not materialize the agent into the product database;
- keep the agent’s provider ID in `priority.json`.

If works are found:

- materialize the agent;
- materialize the reachable relevant works/grouping needed for that interest direction;
- remove the successful priority ID from `priority.json`.

Outside the priority mechanism, an agent with no materialized work is considered an orphan/bug and should not stay in the product database.

Agents are graph closure, not work-budget units.


---

## Provider expansion

Wikidata must stop being the only large automatically mined provider.

The system should support multiple dumped foreign databases. Existing provider-door work should be extended rather than creating a Wikidata-specific product path.

The desired topology is:

```text
provider dump A ─┐
provider dump B ─┤
provider dump C ─┤
...             ─┘
        ↓
normalized observations
        ↓
one unified candidate graph
        ↓
selection/materialization
        ↓
product database
```

There should not be one independent candidate graph per provider.

Provider adapters normalize their data into one common graph.

A candidate entity can carry several foreign IDs at the same time, for example:

```text
candidate-X
  wikidata: Q123
  musicbrainz: ...
  provider-C: ...
```

Multiple independent provider IDs are a positive ranking signal because they increase the amount of available general information and topology.

The system should not blindly copy every foreign entity into the product database. Provider dumps can be huge; product materialization remains selective.


---

## General-information update semantics

General information is automatically refreshed by full provider passes.

There is no requirement to preserve the history of every provider value.

For a general field:

| Existing product/provider value | Newly observed value | Action |
|---|---|---|
| `X` | `Y` | replace `X` with `Y`; this is a real change |
| `NULL` / absent | `Y` | add `Y`; this is an addition, not an anomaly change |
| `X` | `NULL` / absent | keep `X`; there is nothing to replace it with |
| `X` | `X` | no change |

A new full pass is allowed to update previously known non-null information when a provider now supplies a different non-null value.

A provider becoming less complete must not erase already materialized non-null general information.

This means the automatic provider layer is monotone with respect to missingness, but not monotone with respect to actual non-null values.


---

## Dates

The project does not need republishing, reruns, rebroadcast dates, home-video rereleases, or similar repeated publication metadata.

For the product-level general date, prefer the earliest relevant original date.

If a provider supplies a concrete known day, preserve day precision.

If only month precision exists, preserve month precision.

If only a year exists, preserve year precision.

When comparing candidate dates at the same usable precision, use the earliest relevant original/creation/publication/release date.

Do not add conflict-resolution workflows merely because providers disagree about dates.


---

## Provider disagreements

Foreign databases may contradict one another. This is expected.

The project does not need to resolve these disagreements manually and does not need to search for evidence to adjudicate them.

Provider provenance should remain available so disagreement can be identified.

Conflict reporting is peripheral functionality. It may later be useful for sending fixes upstream to the source database, but Arachne must not automatically edit the upstream source.

Adding another provider is generally preferred over spending project-miner time resolving ordinary general-information disagreements.


---

## `>50%` foreign-record anomaly detector

A foreign ID that changes too much between passes may indicate:

- someone significantly reworked that source record; or
- the foreign ID now represents a different entity / the mapping has become wrong.

This is not a reason to block the provider pass.

The detector compares only **general facts that are actually materialized into the product database**.

It does not compare raw provider fields, provider-internal metadata, or data that Arachne does not store as general information.

It also does not require retaining old 100 GB provider dumps or performing a second historical scan. The comparison is between:

- the non-null general values already stored in the product/product-provider state; and
- the new normalized non-null values about to replace them.

Only true non-null-to-different-non-null replacements count as changes.

The following do **not** enter either the anomaly numerator or denominator:

- `NULL/absent → value`;
- `value → NULL/absent` (the old value is retained anyway);
- `value → same value`.

For a provider ID, let:

- `C` = number of comparable stored general fields for which the new pass supplies a different non-null value;
- `K` = number of materialized general fields for which both the stored value and the newly observed value are non-null.

Then:

```text
change_ratio = C / K
```

Only IDs with:

```text
change_ratio > 0.5
```

are anomalous.

Exactly `0.5` is not anomalous.

The anomaly report is one table for the whole pass. The exact representation of old/new differences is not critical.

A simple format can store two rows per anomalous ID:

```csv
provider,external_id,version,birth_year,country_code,language_code,...
wikidata,Q123,old,1942,FR,en,...
wikidata,Q123,new,1943,BE,en,...
```

Every anomalous ID is therefore expected to occur twice: old and new.

It is also acceptable to store only changed fields in the single anomaly table; this is an implementation detail. The important constraints are:

- one anomaly table per pass;
- only IDs above the strict `>50%` threshold are recorded;
- additions caused by formerly missing fields do not count as changes;
- disappearance of a new provider value does not count as a change and does not erase the old value.

An anomalous foreign ID is still updated normally.

The anomaly only reduces its normal automatic-expansion priority.

If a miner later determines that the foreign ID mapping is wrong, the miner may replace the foreign ID with the correct one. The next full provider pass will repopulate general information from that identity. A subsequent `>50%` anomaly caused by this correction is expected and can be ignored.


---

## Candidate ranking

The existing graph-expansion idea where “coverage” can exceed 100% remains useful as a ranking signal.

The name `coverage` is misleading because the score is not a literal bounded percentage. It should be treated as an expansion/ranking score.

The existing form discussed was:

```text
base_score = (parsed_children + 1.2 * grey_children) / active_children
```

The grey-node contribution intentionally allows values above `1.0` / `100%`.

Candidate ranking should also reward entities with multiple foreign-provider IDs.

Current provisional provider-count multiplier:

```text
1 provider  -> ×1.00
2 providers -> ×1.10
3 providers -> ×1.20
4+          -> ×1.30
```

A foreign ID that triggered the `>50%` change anomaly should receive a strong ranking penalty during normal algorithmic expansion.

Current provisional anomaly multiplier:

```text
normal ID     -> ×1.0
anomalous ID  -> ×0.1
```

Conceptually:

```text
score =
  base_score
  * provider_count_multiplier
  * anomaly_multiplier
```

Priority IDs bypass this ranking.

The earlier candidate-plan field called `rank` was effectively a pool rank before balanced retention; if that concept remains, it should be named `pool_rank`.

The earlier candidate-plan field called `coverage` was a weighted selection score and could exceed 100%; if retained, it should be renamed accordingly.


---

## Work-tail definition

The tail is **not a percentile**.

A work is in the tail when its number of human evidence-backed tags is below a fixed threshold.

Current selected threshold:

```text
tag_threshold = 16
```

So:

```text
tail = works where evidence_backed_tag_count < 16
```

This absolute threshold is intentional. A percentile-based tail would guarantee that some proportion remains “bad” even if the whole database becomes well mined.

The tag count belongs to the human knowledge layer. Automatically mined provider metadata does not reduce this tag tail.

A demo or public browsing surface may simply omit the tail instead of pretending the under-mined portion is ready for presentation. The tail is retained in the database as mining capacity/backlog, not necessarily exposed as finished content.

---

## Expansion-budget formula

For ordinary algorithmic expansion:

- `N` = number of currently materialized works that participate in the work-selection population;
- `T` = number of those works in the tag tail (`tag_count < 16`);
- `y` = target maximum tail fraction after ordinary expansion;
- `B` = ordinary expansion budget in effective work-cost units.

Current target:

```text
y = 0.5
```

New ordinary works are assumed to enter the tail initially.

The budget is derived from:

```text
(T + B) / (N + B) = y
```

which gives:

```text
B = max(0, floor((y * N - T) / (1 - y)))
```

For `y = 0.5`:

```text
B = max(0, N - 2*T)
```

This is not used for priority expansion.

On the first clean rebuild, the normal formula alone would not reproduce the old area of interest because the database is new. That is why existing foreign IDs from the backup are fed through the unlimited priority mechanism first.

After priority expansion, the current `N` and `T` are used for normal algorithmic expansion.


---

## Effective work costs and bundles

The ordinary budget is not a literal count of all rows inserted into the product database.

Related works can be treated as bundles with fixed effective costs.

Agents are not charged against these bundle costs.

### Ordinary work

Current effective cost:

```text
ordinary work = 1
```

### Album bundle

If an album is selected, materialize the album and all tracks that can be recovered from the external providers.

The effective expansion cost is fixed:

```text
album bundle = 5
```

This cost is independent of the actual number of tracks.

Examples:

- album + 2 tracks still consumes `5`;
- album + 14 tracks still consumes `5`;
- album + 30 tracks still consumes `5`.

The real database receives the album and all selected tracks. The budget is merely charged five units.

The bundle may only be selected by normal expansion if at least five budget units remain.

If fewer than five remain, skip/defer the album bundle rather than partially importing it.

Priority expansion bypasses this budget restriction.

### Series / anthology-like bundle

For a series that passes the series-selection heuristic, materialize the relevant series structure and its episodes as a bundle.

Current effective cost:

```text
series bundle = 25
```

The effective cost is fixed at `25` even if the series has fewer than 25 episodes, and remains `25` even if it has far more.

The bundle may only be selected by normal expansion when at least 25 effective budget units remain.

Priority expansion bypasses this restriction.

These magic constants are deliberate heuristics. They represent expected mining cost/benefit rather than literal row count.


---

## Series anti-sitcom / anthology-like heuristic

Series are problematic because a conventional long-running series with the same actors in almost every episode can cheaply add many nearly redundant works.

The initial hard-coded heuristic should prefer anthology-like structures.

For every candidate series:

1. count appearances by actor across episodes;
2. select the top six actors by number of episodes;
3. for each of those six, compute:

```text
appearance_ratio =
  episodes_with_actor / total_episodes
```

4. count how many of the top six have:

```text
appearance_ratio >= 0.80
```

Current provisional rule:

```text
allow normal automatic series expansion
only when recurrent_top6_count <= 1
```

This deliberately treats some panel/format shows as anthology-like for expansion purposes even if they are not literally anthologies.

The example discussed was *QI*: Alan Davies might cross 80%, while Stephen Fry, Sandi Toksvig, and other recurring participants cover smaller portions of the complete run. Such a series can pass the heuristic.

A conventional ensemble series where several of the same top actors appear in at least 80% of episodes should be deprioritized/skipped by ordinary automatic expansion for now.

The term “anthology-like” is an algorithmic category here, not a strict genre classification.

Priority expansion may still pull such a series when a priority seed requires it.


---

## Automatic work grouping

Work grouping belongs to automatic provider mining, not human mining.

The existing product model already supports work-membership relations including:

- `episode_of`;
- `season_of`;
- `track_of`;
- `volume_of`;
- `issue_of`;
- `chapter_of`;
- `part_of`;
- `collected_in`.

Series should be able to materialize structures such as:

```text
episode
  -> season
    -> series
```

Music should be able to materialize:

```text
track
  -> album
```

Grouping should be reconstructed from provider data during full passes rather than manually maintained.


---

## Human-miner behavior when automatic information is missing

If a foreign database lacks general information, this is not a reason to edit Arachne manually.

Preferred responses are:

- improve/fix the upstream external database;
- add another external provider;
- wait for the next full provider pass.

If a miner wants to work on an area not yet selected into the product database, the miner should add an appropriate provider ID to `priority.json`.

If the wrong foreign ID is attached to a product entity, a human may replace that ID with the correct provider ID. The automatic pass is responsible for repopulating general information afterward.


---

## Provider passes

General information should be mined automatically by full multi-day passes over dumped external databases.

The pass should be repeatable whenever new provider dumps are available.

A later provider pass is allowed to revise non-null general values according to the update rules above.

The project does not care about the exact historical date/version label of a provider dump for normal operation. The current provider contents matter; historical compatibility does not.

There is no requirement to keep complete prior foreign dumps for Arachne history.


---

## Conflict and provider-quality reports are auxiliary

Detailed statistics for ordinary general-information completeness are not central project-health metrics.

A report saying that many Wikidata dates/countries are missing primarily means:

> improving Wikidata or adding another provider would help.

That is useful provider-quality information, but it is not a core measure of Art Lineages mining progress.

Cross-provider discrepancy reports are similarly useful peripheral outputs.

The project should prefer broader provider coverage over assigning Arachne miners to fill external general-information gaps.


---

## Primary project metrics

Useful project-facing metrics are tied to the human mining layer and graph-selection capacity.

Important values include:

- number of materialized works;
- number of works with fewer than 16 evidence-backed tags;
- tail fraction;
- current ordinary expansion budget `B`;
- number of pending priority IDs;
- optionally human-tag/evidence counts needed to understand mining throughput.

General metadata fill rates are not required as headline project metrics.


---

## Batch simplification

The old `arachne_batch` surface is too broad for the new authority model.

Provider-driven general-information updates should not be expressed as giant human-reviewed mutation batches.

Automatic provider refresh should directly build/materialize the product database from normalized provider/candidate state.

Human batches should become much smaller and focused on human-owned knowledge: tags, assertions, evidence, and minimal identity/priority maintenance where appropriate.

The miner should not have to manually author `create.agents`, `create.works`, ordinary scalar updates, provider metadata, or automatic memberships.


---

## Legacy batch protocol observations

These were identified while reviewing the current batch implementation. They remain useful context while the batch surface is being reduced.

### `batch_id` identity

The current durable `applied_batches` state stores only `batch_id`.

The current semantics allow a repeated `batch_id` with different payload bytes to be treated as already applied without checking whether the payload changed.

A test explicitly preserves that behavior.

If any general-purpose legacy batch path remains, `batch_id` should identify the logical idempotency key while a canonical payload hash distinguishes identical replay from conflicting reuse.

Desired legacy semantics:

```text
new batch_id
  -> apply
  -> record batch_id + canonical payload hash

same batch_id + same hash
  -> already applied

same batch_id + different hash
  -> hard conflict
```

### Overlapping batch writes

Current pending batches can be applied sequentially with ordinary updates and no expected-old-value precondition. Two valid batches can therefore update the same row/field and effectively make filename/apply order determine the winner.

If the old mutation model remains anywhere, overlapping write sets should not silently depend on filename ordering.

### Update preconditions

A reviewed update is stronger if it means:

```text
change X from old-value to new-value
```

rather than:

```text
whenever this runs, force new-value
```

This concern becomes much less important once general information leaves human batches, but it remains relevant for any destructive/admin mutation surface.

### Other old batch-format observations

- `delete` currently lives under `update`; root-level `create/update/delete/merge` would be clearer if this generic mutation DSL survives.
- Some protocol references use SQLite integer row IDs; stable protocol-level IDs are preferable for long-lived human-authored references.
- Merge is destructive and previously had little durable causal history beyond the batch ID.
- Schema validation and semantic validation are distinct; the JSON Schema does not encode every family/database constraint.
- The old batch format had no explicit major version in its `format` value while other contracts used clearer versioning.

These observations should not be used as a reason to preserve or elaborate the old generic batch protocol. The preferred direction is to need much less of it.

---

## Current provider/candidate artifacts: observations that motivated the redesign

The existing Wikidata HPC run already demonstrated several useful properties and several limitations.

### Provenance strengths

The candidate-plan control artifact cryptographically tied:

- the exact candidate-plan bytes;
- the product snapshot;
- the Wikidata source snapshot;
- the algorithm configuration.

This showed that research transport had stronger content identity than the old canonical applied-batch ledger.

### Mapping-store provenance gap

The persistent Wikidata mapping SQLite file was described in the HPC report by provider, size, and verified source snapshot, but not by a hash of the mapping DB itself.

Under the new architecture, exact long-term mapping-DB historical identity is less important than previously considered because full backward compatibility is not a goal.

### Candidate-plan score semantics

The candidate plan’s `coverage` was not a literal percentage.

The reconstructed score included the grey-node bonus and could exceed 100%.

This supports retaining the algorithm as an expansion score while avoiding percentage-like naming.

### Candidate-plan rank semantics

The candidate `rank` was pool rank before balanced group retention, not rank within the final 1500 selected rows.

`pool_rank` is a more accurate name if retained.

### Candidate-plan graph projection

The candidate plan contained one selected relation per candidate work rather than the complete selected-source subgraph.

In the examined artifact, the source graph had 14,377 selected-agent → uncovered-work adjacencies for 4,209 unique uncovered works, while the candidate plan contained exactly 4,209 research-suggestion relations: one chosen source/owner per work. 2,691 of the 4,209 works had more than one selected candidate-neighbor, so 10,168 legitimate graph adjacencies were intentionally omitted from the plan projection.

The redesigned system should treat the unified candidate graph itself as the topology source and treat any assignment/research plan as a projection, not as the complete graph.

### `noncanonical` ambiguity

All 1,500 selected candidates in the examined plan were marked `noncanonical: true`, yet 21 selected Wikidata QIDs already had accepted canonical mappings in the supplied mapping/product state. Candidate-plan nodes marked `noncanonical: true` could therefore still correspond to already mapped canonical entities.

That boolean therefore described the authority/status of the research node rather than guaranteeing that the foreign subject had no canonical match.

The new unified identity graph should represent actual provider-ID/canonical identity separately from research-node authority.

### Image hints

The examined Wikidata image-hints artifact contained 1,581 image hints for 1,375 canonical entities (1,041 agents and 334 works). Provider image hints were correctly non-authoritative.

Wikidata preferred rank and inferred media kind were not sufficient to treat an image as an automatically proven human-evidence choice.

Images remain general/provider metadata, not human evidence.

---

## Existing grouping capability observed in the old database

The old product database already demonstrated that the work-membership model can represent the structures needed by the redesign.

The old database contained 2,355 membership rows. The observed distribution was 1,259 `episode_of`, 652 `collected_in`, 362 `track_of`, 49 `season_of`, 25 `volume_of`, 8 `issue_of`, and zero `chapter_of`/`part_of` rows at the time of inspection. Observed membership families included:

- `episode_of`;
- `season_of`;
- `track_of`;
- `volume_of`;
- `issue_of`;
- `chapter_of`;
- `part_of`;
- `collected_in`.

The old database also had 64 `episode_of` rows whose parent was itself connected by `season_of`, demonstrating real two-level TV structure such as:

```text
episode -> season -> series
```

and music structure such as:

```text
track/composition -> album
```

The redesign should preserve this representational capability while moving population of those rows into automatic provider mining.

---


## Recently applied enrichment batches are legacy-only

Several large enrichment batches were generated and applied during the discussion before the architecture changed. They added provider IDs, provider image references, some dates, and new candidate-derived agents.

The clean rebuild decision supersedes that enrichment direction. Those rows remain part of the database being backed up, but they do not become privileged migration input. Their only reusable contribution is any foreign identity that ends up exported through the same `priority.json` mechanism as the rest of the backed-up database.

Do not special-case these recent batches when rebuilding human knowledge or general metadata.

## Explicitly discarded approaches

The following ideas were discussed and then rejected or superseded.

### Do not keep historical provider snapshots for ordinary operation

No `snapshot A` / `snapshot B` provider-fact history is needed for general information.

Do not keep previous 100 GB foreign dumps merely to compare historical product values.

Use the current product/provider materialization as the “old” side of the anomaly comparison.

### Do not maintain separate provider product databases

Do not build one independent product/candidate SQLite per provider and later resolve them.

Normalize all sources into one candidate graph.

### Do not manually resolve ordinary cross-provider conflicts

Record them if useful, but do not make human resolution part of normal project work.

### Do not use foreign provider databases as human evidence

Automatically mined providers supply general information only.

### Do not preserve old human tags/evidence during the clean rebuild

Miners will reconstruct useful knowledge manually from the read-only backup using the new evidence rules and score logic.

### Do not use a percentile tail

The work tail uses a fixed human-tag threshold, currently `<16`.

### Do not constrain priority expansion with the ordinary budget

Priority is explicitly allowed to exceed the normal expansion bound.

### Do not count agents against the ordinary work-expansion budget

Agents are graph closure and should be removed if they become unreferenced/orphaned.

### Do not erase known general facts because a later provider pass returns `NULL`

A later `NULL` means “nothing new to replace the existing value with”.

---

## Current hard-coded/provisional constants

These are the concrete defaults currently discussed. They are intentionally simple and can be changed later when there is a real reason.

| Constant | Current value |
|---|---:|
| Human-tag tail threshold | `16` tags |
| Target ordinary tail fraction `y` | `0.50` |
| Ordinary independent work cost | `1` |
| Album bundle cost | `5` |
| Series/anthology-like bundle cost | `25` |
| Series actor recurrence threshold | `80%` of episodes |
| Allowed top-six actors at/above recurrence threshold | `<= 1` |
| Grey-child score weight | `1.2` relative to parsed child `1.0` |
| Multi-provider score bonus | `+10%` per additional provider |
| Multi-provider bonus cap | `4+ providers -> ×1.30` |
| Foreign-ID anomaly threshold | strict `change_ratio > 0.5` |
| Foreign-ID anomaly ranking multiplier | `×0.1` |
| Priority expansion budget | unlimited |

These values should remain obvious constants rather than being hidden behind unnecessary configuration machinery unless operational use demonstrates a need for configurability.

---

## Concrete implications for Arachne

The intended result is:

```text
foreign dumped databases
        ↓
provider adapters
        ↓
one normalized candidate graph
        ↓
priority expansion (unlimited)
        ↓
ordinary expansion:
  tag-tail budget
  old >100% graph score
  provider-count bonus
  anomaly penalty
  bundle costs / series heuristic
        ↓
automatic general-information materialization
        ↓
product database

human miners
        ↓
tags + evidence/assertions
provider-ID correction
priority.json seeds
```

General-information refresh is automatic and reproducible from foreign sources plus current identity state.

Human mining is intentionally reserved for knowledge that requires human evidence and project-specific interpretation.

The clean rebuild starts with no inherited human knowledge and uses only the old foreign IDs as priority seeds.
