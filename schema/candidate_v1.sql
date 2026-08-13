-- Disposable research-candidate graph schema v1.
PRAGMA user_version = 1;

CREATE TABLE candidate_graph_info (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    plan_version INTEGER NOT NULL CHECK (plan_version = 1),
    source_snapshot_id TEXT NOT NULL,
    source_storage_ref TEXT NOT NULL,
    source_snapshot_sha256 TEXT NOT NULL CHECK (length(source_snapshot_sha256) = 64),
    product_snapshot_id TEXT NOT NULL,
    product_snapshot_sha256 TEXT NOT NULL CHECK (length(product_snapshot_sha256) = 64),
    algorithm_version TEXT NOT NULL,
    configuration_sha256 TEXT NOT NULL CHECK (length(configuration_sha256) = 64),
    configuration_json TEXT NOT NULL CHECK (json_valid(configuration_json))
) STRICT;

CREATE TABLE candidate_groups (
    id TEXT PRIMARY KEY,
    label TEXT NOT NULL,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0),
    metadata_json TEXT NOT NULL CHECK (json_valid(metadata_json)),
    UNIQUE (ordinal)
) STRICT;

CREATE TABLE candidate_nodes (
    id TEXT PRIMARY KEY,
    entity_ref TEXT,
    entity_type TEXT NOT NULL,
    label TEXT NOT NULL,
    rank INTEGER CHECK (rank IS NULL OR rank > 0),
    coverage REAL NOT NULL CHECK (coverage >= 0),
    group_id TEXT REFERENCES candidate_groups(id) ON DELETE RESTRICT,
    is_grey INTEGER NOT NULL CHECK (is_grey IN (0,1)),
    selection_reason_json TEXT NOT NULL CHECK (json_valid(selection_reason_json)),
    source_metadata_json TEXT NOT NULL CHECK (json_valid(source_metadata_json))
) STRICT;
CREATE INDEX candidate_nodes_rank_idx ON candidate_nodes(rank, id);
CREATE INDEX candidate_nodes_group_idx ON candidate_nodes(group_id, id);

CREATE TABLE candidate_edges (
    id TEXT PRIMARY KEY,
    subject_id TEXT NOT NULL REFERENCES candidate_nodes(id) ON DELETE CASCADE,
    relation_type TEXT NOT NULL,
    object_id TEXT NOT NULL REFERENCES candidate_nodes(id) ON DELETE CASCADE,
    weight REAL,
    metadata_json TEXT NOT NULL CHECK (json_valid(metadata_json)),
    CHECK (subject_id <> object_id),
    UNIQUE (subject_id, relation_type, object_id)
) STRICT;
