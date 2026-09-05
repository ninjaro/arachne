# Provider observation graph

The provider observation graph is a disposable, provider-neutral SQLite input
to later product selection. It preserves current foreign observations; it is
not canonical product state, a provider-history archive, or human evidence.
Its schema is `schema/provider_observation_v1.sql`.

Each normalized record has one exact provider identity, an entity type, and
arrays of additional exact identifiers, names, facts, media hints, and typed
edges. Facts and edges keep the provider that made the observation. Missing
fact values are omitted, while different non-null values from different
providers remain separate and queryable.

Provider identity is the tuple `(provider, namespace, external_id)`. An adapter
may assert that another exact tuple denotes the same entity; the graph then
unions their local clusters while retaining every provider ID and the
crosswalk's source. It never joins entities by similar labels, dates, or other
heuristics. Cluster integers are disposable implementation details and must not
become product identifiers.

The narrow fixture adapters in `scripts/provider_fixture_adapters.py` cover:

- IMDb name/title basics, title credits, and episode membership TSV rows;
  explicit season numbers become stable provider-local season nodes;
- MusicBrainz artist, recording, release, and release-group JSON rows,
  including Wikidata URL crosswalks, artist credits, and recording-to-album
  membership;
- Open Library author and work JSON rows, including remote-ID crosswalks,
  authorship, and image keys.

`scripts/ingest_provider_dump.py` is the streaming entrypoint for the supported
IMDb TSV, MusicBrainz core JSON archive, and Open Library tab/JSON dump
families. Reuse one graph path across calls; only the first call uses `--create`:

```sh
python3 scripts/ingest_provider_dump.py \
  --graph /run/provider-observations.sqlite --create \
  --provider imdb --kind name-basics \
  --input /acquired/name.basics.tsv.gz
```

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
disagreements and strict `> 0.5` change anomalies. It never writes concepts,
human assertions, sources, or evidence. Missing observations do not erase a
stored non-null general value; changed non-empty provider-owned name, media,
credit, membership, and agent-relation sets replace their compact current sets
rather than accumulating stale rows.
