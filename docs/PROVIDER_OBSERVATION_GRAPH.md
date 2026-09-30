# Provider observation graph

The provider observation graph is a disposable, provider-neutral SQLite input
to later product selection. It preserves current foreign observations; it is
not canonical product state, a provider-history archive, or human evidence.
Its schema is `schema/provider_observation.sql`.

The graph is latest-only: the schema in the selected commit is the supported
schema, the file carries no format version, and a graph built by older code is
rejected with a request to rebuild it rather than migrated. Provenance is
kept: `provider_sources` has one logical snapshot per provider and
`provider_source_files` records every input file's kind and SHA-256. All files
of one provider must claim the same snapshot ID, and the provider digest is
bound to the sorted `(kind, sha256)` list of its files.

Each normalized record has one exact provider identity, an entity type, and
arrays of additional exact identifiers, names, facts, media hints, typed
edges, and hint-only signals. Facts and edges keep the provider that made the observation. Missing
fact values are omitted, while different non-null values from different
providers remain separate and queryable.

Provider identity is the tuple `(provider, namespace, external_id)`. An adapter
may assert that another exact tuple denotes the same entity; the graph then
unions their local clusters while retaining every provider ID and the
crosswalk's source. It never joins entities by similar labels, dates, or other
heuristics. Cluster integers are disposable implementation details and must not
become product identifiers.

The narrow fixture adapters in `scripts/provider_fixture_adapters.py` cover:

- IMDb name/title basics, akas, crew, principals, and episode membership TSV
  rows; explicit season numbers become stable provider-local season nodes, and
  only original or language-tagged akas are kept;
- MusicBrainz artist, label, recording, release, release-group, and work JSON
  rows, including Wikidata URL crosswalks, artist/writer credits, and
  recording-to-album membership. A release additionally contributes its own
  dates and its release group's declared type, so the earliest original date
  and album topology improve without any manifestation entity. Date filtering
  is fail-closed: only releases with an explicit `Official` or `Promotion`
  status date the work, and the release group's aggregate
  `first-release-date` is never used because it can include the excluded
  bootleg and pseudo-release statuses. A release's labels and catalog numbers
  describe that edition, not the work, so they are detected and counted but
  never become release-group credits;
- Open Library author, work, edition, and redirect rows, including remote-ID
  crosswalks, authorship, image keys, merged-key identity, and edition dates
  attached to their work (editions never become manifestations);
- Discogs artist, label, master, and release XML elements. Masters are work
  identities (matching Wikidata P1954); releases only contribute the earliest
  date and main-release album/single type to their master. Artist and label
  pages also contribute their useful third-party links as leads, with Discogs
  itself, identity crosswalks, social profiles, stores, and streaming services
  dropped;
- GND authority records converted from the official DNB MARC 21 export by
  `scripts/convert_gnd_marc.py` and bound by `scripts/resolve_gnd_identities.py`
  to an entity Arachne knows, supplying agent identity crosswalks and hint-only
  subject terms with their GND IDs. Only individualized persons, corporate
  bodies, and works receive a product type; conferences/events, families, and
  undifferentiated names stay `unknown` until an exact crosswalk types them.

`scripts/ingest_provider_dump.py` is the streaming entrypoint for the supported
IMDb TSV, MusicBrainz core JSON archive, Open Library tab/JSON, Discogs XML,
and resolved GND JSONL dump families. Reuse one graph path across calls; only the first call uses
`--create`:

```sh
python3 scripts/ingest_provider_dump.py \
  --graph /run/provider-observations.sqlite --create \
  --provider imdb --kind name-basics --snapshot-id 2026-09-20 \
  --input /acquired/name.basics.tsv.gz
```

Every call registers its input file with the provider snapshot. `--signals
none` stores general information only; a later call with
`--signals-only-for PRODUCT` rescans the same file and stores only the signals
an under-mined work of that product can use (see
[Research hints](RESEARCH_HINTS.md#signal-persistence)).

## Detection is not persistence

Adapters may recognize more than the graph stores. General facts are limited
to `GENERAL_FACT_FIELDS` in `scripts/provider_observation_graph.py`, the
fields `scripts/materialize_provider_rebuild.py` actually consumes; the graph
rejects any other fact field. A field an adapter detects without a current
consumer is named in the record's `unpersisted` list, counted in the ingest
report, and not stored. The raw dump remains the source if a later consumer
needs it.

| Adapter | Persisted facts (consumer) | Detected, counted, not persisted |
|---|---|---|
| IMDb `name.basics` | `birth_year`, `death_year` (agent dates) | `professions` |
| IMDb `title.basics` | `work_type` (medium), `original_date` (dating) | `runtime_minutes`, `adult` |
| IMDb `title.episode` season nodes | `work_type` (medium) | — |
| MusicBrainz artist | `birth_date`, `death_date` (agent dates) | `agent_country_code` (no agent country field) |
| MusicBrainz label | — | `agent_country_code` |
| MusicBrainz release group | `work_type` (medium) | `first_release_date` (unfiltered aggregate) |
| MusicBrainz release | `original_date` (accepted status only), `work_type` | `release_label`, `excluded_status_release_date` |
| MusicBrainz recording/work | `work_type` (medium) | — |
| Open Library author | `birth_date`, `death_date` | — |
| Open Library work/edition | `original_date` | — |
| Discogs master/release | `original_date`, `work_type` | — |
| GND entity | `birth_date`, `death_date` | — |
| Wikidata worker | `original_date`, `medium`, `birth_date`, `death_date` | — |

`language_code`, `country_code` (on works), and `production_info` are consumed
by the materializer when a provider supplies them. Names, identities, credits,
memberships, agent relations, and media are persisted because identity,
topology, dating, and display consume them.

The Wikidata bulk worker writes this same SQLite schema directly. Optional
provider failures leave the already-ingested graph usable and do not make the
required Wikidata pass fail.

IMDb's official TSV rows do not themselves assert Wikidata identity. Such a
join occurs only when an exact crosswalk source (for example, a Wikidata P345
observation) supplies both IDs. Acquisition, dump streaming, candidate
selection, and canonical materialization remain outside these fixture
adapters.

MusicBrainz `release` rows are structural input: release editions themselves
are not promoted to product works. Their explicit recording and release-group
MBIDs produce `track_of` edges. Untyped MusicBrainz artists remain `unknown`
until an exact provider crosswalk supplies a safe person/group type.

`ObservationGraph.ingest(provider, records)` writes one provider input in a
single `BEGIN IMMEDIATE` transaction. Any malformed observation, incompatible
entity-type union, invalid JSON value, or database error rolls back the entire
call. The clustered views expose names, facts, media, and original edge
endpoints without discarding provider provenance.

`scripts/materialize_provider_rebuild.py` is the separate automatic product
writer. It consumes this graph and `arachne-data/priority.json`, materializes
priority closure before ordinary scored expansion, applies fixed bundle costs
and the tag-tail budget, and emits one auxiliary report containing provider
disagreements and strict `> 0.5` change anomalies. It uses the graph as a
database rather than loading it into Python: product external IDs and priority
IDs are looked up through an index, ordinary expansion only visits agents
credited on already-materialized or newly claimed works (the only agents that
can score), and names, facts, media, and edges are queried per selected
cluster. Memory therefore follows the product's neighbourhood, not the size of
the multi-provider corpus. It never writes concepts,
human assertions, sources, or evidence. Missing observations do not erase a
stored non-null general value; changed non-empty provider-owned name, media,
credit, membership, and agent-relation sets replace their compact current sets
rather than accumulating stale rows.

## Hint-only signals

`provider_signals` holds semantic provider values that are research leads, not
general information: IMDb genres, Wikidata P135/P136/P921 values (as
`wikidata:Q…` vocabulary IDs), Open Library subjects, Discogs styles and
genres, GND subject terms (as `gnd:…` vocabulary IDs), and useful URL
relations as leads — MusicBrainz review, interview, biography, discography
entry, and Wikipedia relations, plus the third-party links Discogs artist and
label pages list. Each signal keeps its analytical `semantic_family` and its
provider-native `provider_category` (for example P921 is `main_subject`,
analysed as `theme`). The product materializer never reads this table. The
separate research-hint builder (`docs/RESEARCH_HINTS.md`) is its only
consumer, and a multi-provider pass stores only signals whose subject can
reach an under-mined work.

## One multi-provider pass

`scripts/run_provider_pass.py` connects the left side of the pipeline. It
starts from the required Wikidata graph, streams every acquired dump named
in a `provider_pass_manifest` into that same graph, materializes exactly once,
and then builds research hints from the same graph. The manifest is
latest-only and closed:

```json
{
  "format": "provider_pass_manifest",
  "base_graph": "wikidata-provider-observations.sqlite",
  "inputs": [
    {"provider": "discogs", "kind": "masters",
     "path": "discogs_20260901_masters.xml.gz", "snapshot_id": "20260901"},
    {"provider": "imdb", "kind": "title-basics",
     "path": "title.basics.tsv.gz", "snapshot_id": "2026-09-20"}
  ]
}
```

Each input is ingested atomically. A failed optional input is recorded in the
pass report and the pass continues; an input marked `"required": true` aborts
before materialization. `--vocabulary` passes the reviewed authority
concordance to the hint build. Each ingested file is registered in
`provider_source_files` under its provider's single snapshot. Acquisition
stays behind the Pheidippides boundary; the pass consumes acquired artifacts.

General information and research hints are independent domains. Hint inputs
are preflighted before any ingestion, so a malformed vocabulary or manual
signal file fails the pass before the product changes. After the general pass
commits, a hint failure is reported as `research_hints.status = "failed"`
next to `general.status = "succeeded"` (exit status 3); it never leaves the
general pass's state ambiguous, and the hint build can be rerun on its own.
