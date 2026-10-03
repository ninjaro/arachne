-- Disposable research-hint artifact.
--
-- Research hints are leads for miners, never evidence and never canonical
-- concepts or work-concept assertions. This database is rebuilt from the
-- provider observation graph plus a read-only product snapshot and lives
-- outside schema/product.sql. It is created only for under-mined works.
-- `research_priority` orders what a miner inspects first; it is not a truth
-- probability and must not be read as, or copied into, canonical confidence.
--
-- Latest-only: the schema in the selected repository commit is the supported
-- schema. An artifact built by older code is rebuilt, never migrated, so this
-- file carries no format version. Provenance (snapshots and digests) is kept.

CREATE TABLE research_hint_info (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    tag_threshold INTEGER NOT NULL CHECK (tag_threshold > 0),
    -- provider -> {snapshot_id, sha256, files: [{kind, sha256}]}
    provider_snapshots_json TEXT NOT NULL
        CHECK (json_valid(provider_snapshots_json)
               AND json_type(provider_snapshots_json) = 'object'),
    -- {} or {sha256, signal_types, datasets} of the manual signal file
    manual_signals_json TEXT NOT NULL
        CHECK (json_valid(manual_signals_json)
               AND json_type(manual_signals_json) = 'object'),
    -- opted-in licence-restricted signal type -> signals admitted to the build
    restricted_signals_json TEXT NOT NULL
        CHECK (json_valid(restricted_signals_json)
               AND json_type(restricted_signals_json) = 'object'),
    -- {} or {sha256, format} of the authority concordance
    vocabulary_json TEXT NOT NULL
        CHECK (json_valid(vocabulary_json) AND json_type(vocabulary_json) = 'object'),
    -- build policy and suppression counts (generic hints, capped agent leads)
    build_json TEXT NOT NULL
        CHECK (json_valid(build_json) AND json_type(build_json) = 'object')
) STRICT;

-- One row per under-mined product work that was considered.
CREATE TABLE hint_works (
    work_id TEXT PRIMARY KEY CHECK (length(work_id) > 0),
    evidence_backed_tag_count INTEGER NOT NULL
        CHECK (evidence_backed_tag_count >= 0),
    weighted_coverage REAL NOT NULL CHECK (weighted_coverage BETWEEN 0 AND 1),
    work_need REAL NOT NULL CHECK (work_need > 0 AND work_need <= 1),
    covered_families_json TEXT NOT NULL
        CHECK (json_valid(covered_families_json)
               AND json_type(covered_families_json) = 'array'),
    useful_hint_count INTEGER NOT NULL DEFAULT 0 CHECK (useful_hint_count >= 0),
    top_priority REAL NOT NULL DEFAULT 0 CHECK (top_priority >= 0)
) STRICT;

-- One deduplicated lead per work. The top level is analytical and may be
-- normalized: signals with the same exact vocabulary ID, or the same
-- normalized label when no ID exists, collapse into one hint, and a resolved
-- authority term supplies the display label. `vocabulary_id` prefers a
-- resolved authority ID; `authority_ids_json` keeps every exact crosswalked
-- ID; `related_authority_ids_json` keeps weaker reviewed mappings that never
-- drive dedup; `term_kind` keeps genre/form terms separate from topical ones.
--
-- `assignment_quality` answers how trustworthy the provider's claim that the
-- term applies to the work is (A-E, from the reviewed signal policy; E is a
-- broad generic classification). `resolution_quality` answers how confidently
-- Arachne identified the term the provider meant. Neither overwrites the
-- other. Generic (E) hints are counted and dropped unless a build keeps them.
CREATE TABLE research_hints (
    id INTEGER PRIMARY KEY,
    work_id TEXT NOT NULL REFERENCES hint_works(work_id) ON DELETE CASCADE,
    hint_kind TEXT NOT NULL CHECK (hint_kind IN
        ('concept','content_signal','source_lead','search_lead')),
    semantic_family TEXT CHECK (semantic_family IS NULL OR semantic_family IN
        ('genre','style','theme','keyword','motif','trope','phobia','taboo',
         'technique','movement','setting','mood','content_warning')),
    dedup_key TEXT NOT NULL CHECK (length(dedup_key) > 0),
    display_value TEXT NOT NULL CHECK (length(display_value) > 0),
    normalized_value TEXT,
    vocabulary_id TEXT,
    term_kind TEXT CHECK (term_kind IS NULL OR term_kind IN ('genre_form','topical')),
    authority_ids_json TEXT NOT NULL DEFAULT '{}'
        CHECK (json_valid(authority_ids_json)
               AND json_type(authority_ids_json) = 'object'),
    related_authority_ids_json TEXT NOT NULL DEFAULT '[]'
        CHECK (json_valid(related_authority_ids_json)
               AND json_type(related_authority_ids_json) = 'array'),
    lead_kind TEXT CHECK (lead_kind IS NULL OR lead_kind IN
        ('article','review','interview','catalogue','book','essay','blog',
         'bibliography_entry')),
    source_url TEXT,
    assignment_quality TEXT NOT NULL
        CHECK (assignment_quality IN ('A','B','C','D','E')),
    resolution_quality TEXT CHECK (resolution_quality IS NULL OR resolution_quality IN
        ('exact_id','reviewed_crosswalk','exact_label','unresolved')),
    specificity REAL NOT NULL CHECK (specificity > 0 AND specificity <= 1),
    candidate_tag_weight REAL NOT NULL CHECK (candidate_tag_weight > 0),
    signal_quality REAL NOT NULL CHECK (signal_quality > 0 AND signal_quality <= 1),
    resolution_weight REAL NOT NULL
        CHECK (resolution_weight > 0 AND resolution_weight <= 1),
    independent_origins INTEGER NOT NULL CHECK (independent_origins >= 1),
    research_priority REAL NOT NULL CHECK (research_priority >= 0),
    CHECK (
        hint_kind NOT IN ('concept','content_signal')
        OR (semantic_family IS NOT NULL AND resolution_quality IS NOT NULL)
    )
) STRICT;
CREATE UNIQUE INDEX research_hints_logical_unique ON research_hints(
    work_id,
    hint_kind,
    COALESCE(semantic_family, ''),
    dedup_key
);
CREATE INDEX research_hints_work_priority_idx
ON research_hints(work_id, research_priority DESC);

-- Every provider-native observation behind a hint: the audit trail that
-- keeps the analytical top level recoverable. `raw_semantic_family` is the
-- provider's own category (for example Wikidata `main_subject`), while
-- `semantic_family` is the analytical family assigned to this signal before
-- any merge. `resolution_basis` records how the signal reached the hint's
-- dedup key.
CREATE TABLE research_hint_signals (
    id INTEGER PRIMARY KEY,
    hint_id INTEGER NOT NULL REFERENCES research_hints(id) ON DELETE CASCADE,
    provider TEXT NOT NULL CHECK (length(provider) > 0),
    provider_entity_id TEXT NOT NULL CHECK (length(provider_entity_id) > 0),
    provider_signal_type TEXT NOT NULL CHECK (length(provider_signal_type) > 0),
    raw_value TEXT NOT NULL CHECK (length(raw_value) > 0),
    raw_vocabulary_id TEXT,
    raw_semantic_family TEXT,
    semantic_family TEXT,
    normalized_value TEXT,
    provider_strength REAL,
    assignment_quality TEXT NOT NULL
        CHECK (assignment_quality IN ('A','B','C','D','E')),
    resolution_basis TEXT NOT NULL CHECK (resolution_basis IN
        ('provider_authority_id','reviewed_crosswalk','concordance_label',
         'provider_vocabulary_id','normalized_label','normalized_url','doi','isbn')),
    -- Independence key: a declared shared upstream, otherwise the provider.
    origin TEXT NOT NULL CHECK (length(origin) > 0),
    -- `work` for a signal on the work itself; `credited_agent` for a source or
    -- search lead attached to an agent credited on the work.
    attachment TEXT NOT NULL CHECK (attachment IN ('work','credited_agent')),
    provenance_json TEXT NOT NULL
        CHECK (json_valid(provenance_json) AND json_type(provenance_json) = 'object'),
    source_url TEXT,
    source_snapshot TEXT,
    source_sha256 TEXT CHECK (
        source_sha256 IS NULL
        OR (length(source_sha256) = 64 AND source_sha256 NOT GLOB '*[^0-9a-f]*')
    )
) STRICT;
CREATE INDEX research_hint_signals_hint_idx ON research_hint_signals(hint_id);

CREATE VIEW miner_work_queue AS
SELECT work_id,
       evidence_backed_tag_count,
       weighted_coverage,
       useful_hint_count,
       top_priority
FROM hint_works
WHERE useful_hint_count > 0
ORDER BY top_priority DESC, work_id;
