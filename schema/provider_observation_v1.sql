-- Disposable, provider-neutral observation graph.
--
-- Provider adapters write current non-null observations here. The database is
-- an input to later selection/materialization; it is neither product state nor
-- a historical provider archive. Integer cluster IDs are local implementation
-- details. Exact provider identifiers, rather than those IDs, carry identity.
PRAGMA user_version = 1;

CREATE TABLE provider_graph_info (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    format_version INTEGER NOT NULL CHECK (format_version = 1)
) STRICT;
INSERT INTO provider_graph_info VALUES (1, 1);

CREATE TABLE provider_sources (
    provider TEXT PRIMARY KEY CHECK (length(provider) > 0),
    snapshot_id TEXT NOT NULL CHECK (length(snapshot_id) > 0),
    storage_ref TEXT NOT NULL CHECK (length(storage_ref) > 0),
    sha256 TEXT NOT NULL CHECK (
        length(sha256) = 64 AND sha256 NOT GLOB '*[^0-9a-f]*'
    )
) STRICT;

CREATE TABLE entity_clusters (
    id INTEGER PRIMARY KEY,
    entity_type TEXT NOT NULL CHECK (entity_type IN
        ('unknown','work','person','organization','group'))
) STRICT;

CREATE TABLE provider_ids (
    id INTEGER PRIMARY KEY,
    cluster_id INTEGER NOT NULL
        REFERENCES entity_clusters(id) ON DELETE CASCADE,
    provider TEXT NOT NULL CHECK (length(provider) > 0),
    namespace TEXT NOT NULL CHECK (length(namespace) > 0),
    external_id TEXT NOT NULL CHECK (length(external_id) > 0),
    UNIQUE (provider, namespace, external_id)
) STRICT;
CREATE INDEX provider_ids_cluster_idx
ON provider_ids(cluster_id, provider, namespace, external_id);

-- Retain which provider record asserted each exact identity crosswalk even
-- after the two identifiers have been unified into one cluster.
CREATE TABLE provider_identity_links (
    id INTEGER PRIMARY KEY,
    subject_provider_id INTEGER NOT NULL
        REFERENCES provider_ids(id) ON DELETE CASCADE,
    object_provider_id INTEGER NOT NULL
        REFERENCES provider_ids(id) ON DELETE CASCADE,
    observation_provider TEXT NOT NULL CHECK (length(observation_provider) > 0),
    metadata_json TEXT NOT NULL DEFAULT '{}'
        CHECK (json_valid(metadata_json) AND json_type(metadata_json) = 'object'),
    CHECK (subject_provider_id <> object_provider_id),
    UNIQUE (
        subject_provider_id,
        object_provider_id,
        observation_provider,
        metadata_json
    )
) STRICT;

CREATE TABLE provider_names (
    id INTEGER PRIMARY KEY,
    subject_provider_id INTEGER NOT NULL
        REFERENCES provider_ids(id) ON DELETE CASCADE,
    observation_provider TEXT NOT NULL CHECK (length(observation_provider) > 0),
    name_type TEXT NOT NULL CHECK (length(name_type) > 0),
    language_code TEXT,
    script_code TEXT,
    value TEXT NOT NULL CHECK (length(value) > 0)
) STRICT;
CREATE UNIQUE INDEX provider_names_logical_unique ON provider_names(
    subject_provider_id,
    observation_provider,
    name_type,
    COALESCE(language_code, ''),
    COALESCE(script_code, ''),
    value
);

CREATE TABLE provider_facts (
    id INTEGER PRIMARY KEY,
    subject_provider_id INTEGER NOT NULL
        REFERENCES provider_ids(id) ON DELETE CASCADE,
    observation_provider TEXT NOT NULL CHECK (length(observation_provider) > 0),
    field TEXT NOT NULL CHECK (length(field) > 0),
    value_json TEXT NOT NULL CHECK
        (json_valid(value_json) AND json_type(value_json) <> 'null'),
    metadata_json TEXT NOT NULL DEFAULT '{}'
        CHECK (json_valid(metadata_json) AND json_type(metadata_json) = 'object'),
    UNIQUE (
        subject_provider_id,
        observation_provider,
        field,
        value_json,
        metadata_json
    )
) STRICT;

CREATE TABLE provider_media (
    id INTEGER PRIMARY KEY,
    subject_provider_id INTEGER NOT NULL
        REFERENCES provider_ids(id) ON DELETE CASCADE,
    observation_provider TEXT NOT NULL CHECK (length(observation_provider) > 0),
    media_kind TEXT NOT NULL CHECK (length(media_kind) > 0),
    media_key TEXT NOT NULL CHECK (length(media_key) > 0),
    media_json TEXT NOT NULL
        CHECK (json_valid(media_json) AND json_type(media_json) = 'object'),
    UNIQUE (
        subject_provider_id,
        observation_provider,
        media_kind,
        media_key,
        media_json
    )
) STRICT;

CREATE TABLE provider_edges (
    id INTEGER PRIMARY KEY,
    subject_provider_id INTEGER NOT NULL
        REFERENCES provider_ids(id) ON DELETE CASCADE,
    object_provider_id INTEGER NOT NULL
        REFERENCES provider_ids(id) ON DELETE CASCADE,
    observation_provider TEXT NOT NULL CHECK (length(observation_provider) > 0),
    relation_family TEXT NOT NULL CHECK (relation_family IN
        ('credit','work_membership','agent_relation')),
    relation_type TEXT NOT NULL CHECK (length(relation_type) > 0),
    metadata_json TEXT NOT NULL DEFAULT '{}'
        CHECK (json_valid(metadata_json) AND json_type(metadata_json) = 'object'),
    CHECK (subject_provider_id <> object_provider_id),
    UNIQUE (
        subject_provider_id,
        object_provider_id,
        observation_provider,
        relation_family,
        relation_type,
        metadata_json
    )
) STRICT;
CREATE INDEX provider_edges_subject_idx
ON provider_edges(subject_provider_id, relation_family, relation_type);
CREATE INDEX provider_edges_object_idx
ON provider_edges(object_provider_id, relation_family, relation_type);

-- Hint-only semantic signals and source/search leads. The product materializer
-- never reads this table: a signal is a research lead for a miner, never a
-- general fact, a canonical concept, or evidence. It feeds only the separate
-- disposable research-hint artifact (schema/research_hint_v1.sql).
CREATE TABLE provider_signals (
    id INTEGER PRIMARY KEY,
    subject_provider_id INTEGER NOT NULL
        REFERENCES provider_ids(id) ON DELETE CASCADE,
    observation_provider TEXT NOT NULL CHECK (length(observation_provider) > 0),
    signal_kind TEXT NOT NULL CHECK (signal_kind IN
        ('concept','content_signal','source_lead','search_lead')),
    semantic_family TEXT CHECK (semantic_family IS NULL OR semantic_family IN
        ('genre','style','theme','keyword','motif','trope','phobia','taboo',
         'technique','movement','setting','mood','content_warning')),
    signal_type TEXT NOT NULL CHECK (length(signal_type) > 0),
    value TEXT NOT NULL CHECK (length(value) > 0),
    vocabulary_id TEXT CHECK (vocabulary_id IS NULL OR length(vocabulary_id) > 0),
    strength REAL,
    url TEXT CHECK (url IS NULL OR length(url) > 0),
    metadata_json TEXT NOT NULL DEFAULT '{}'
        CHECK (json_valid(metadata_json) AND json_type(metadata_json) = 'object'),
    CHECK (
        signal_kind NOT IN ('concept','content_signal')
        OR semantic_family IS NOT NULL
    )
) STRICT;
CREATE UNIQUE INDEX provider_signals_logical_unique ON provider_signals(
    subject_provider_id,
    observation_provider,
    signal_kind,
    COALESCE(semantic_family, ''),
    signal_type,
    value,
    COALESCE(vocabulary_id, ''),
    COALESCE(url, ''),
    metadata_json
);

CREATE VIEW clustered_provider_facts AS
SELECT i.cluster_id,
       i.provider AS subject_provider,
       i.namespace AS subject_namespace,
       i.external_id AS subject_external_id,
       f.observation_provider,
       f.field,
       f.value_json,
       f.metadata_json
FROM provider_facts AS f
JOIN provider_ids AS i ON i.id = f.subject_provider_id;

CREATE VIEW clustered_provider_names AS
SELECT i.cluster_id,
       i.provider AS subject_provider,
       i.namespace AS subject_namespace,
       i.external_id AS subject_external_id,
       n.observation_provider,
       n.name_type,
       n.language_code,
       n.script_code,
       n.value
FROM provider_names AS n
JOIN provider_ids AS i ON i.id = n.subject_provider_id;

CREATE VIEW clustered_provider_media AS
SELECT i.cluster_id,
       i.provider AS subject_provider,
       i.namespace AS subject_namespace,
       i.external_id AS subject_external_id,
       m.observation_provider,
       m.media_kind,
       m.media_key,
       m.media_json
FROM provider_media AS m
JOIN provider_ids AS i ON i.id = m.subject_provider_id;

CREATE VIEW clustered_provider_edges AS
SELECT subject.cluster_id AS subject_cluster_id,
       subject.provider AS subject_provider,
       subject.namespace AS subject_namespace,
       subject.external_id AS subject_external_id,
       object.cluster_id AS object_cluster_id,
       object.provider AS object_provider,
       object.namespace AS object_namespace,
       object.external_id AS object_external_id,
       edge.observation_provider,
       edge.relation_family,
       edge.relation_type,
       edge.metadata_json
FROM provider_edges AS edge
JOIN provider_ids AS subject ON subject.id = edge.subject_provider_id
JOIN provider_ids AS object ON object.id = edge.object_provider_id;

CREATE VIEW clustered_provider_signals AS
SELECT i.cluster_id,
       i.provider AS subject_provider,
       i.namespace AS subject_namespace,
       i.external_id AS subject_external_id,
       s.observation_provider,
       s.signal_kind,
       s.semantic_family,
       s.signal_type,
       s.value,
       s.vocabulary_id,
       s.strength,
       s.url,
       s.metadata_json
FROM provider_signals AS s
JOIN provider_ids AS i ON i.id = s.subject_provider_id;
