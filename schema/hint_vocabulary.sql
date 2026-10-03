-- Indexed, compiled research-hint authority concordance.
--
-- Built by `python3 scripts/hint_vocabulary.py compile` from a reviewed JSONL
-- term stream so that a large authority subset is queried on disk instead of
-- being loaded into memory. Like the JSON form it only normalizes and
-- deduplicates research leads; it never creates canonical concepts. Latest
-- only: a file compiled by older code is recompiled, never migrated.

CREATE TABLE hint_vocabulary_info (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    source TEXT
) STRICT;

CREATE TABLE terms (
    id INTEGER PRIMARY KEY,
    term_kind TEXT NOT NULL CHECK (term_kind IN ('genre_form','topical')),
    label TEXT NOT NULL CHECK (length(label) > 0),
    generic INTEGER NOT NULL DEFAULT 0 CHECK (generic IN (0, 1))
) STRICT;

-- Exact crosswalks only: these identify a term and drive dedup.
CREATE TABLE term_ids (
    scheme TEXT NOT NULL CHECK (scheme IN
        ('aat','gnd','iconclass','lcgft','lcsh','rameau')),
    identifier TEXT NOT NULL CHECK (length(identifier) > 0),
    term_id INTEGER NOT NULL REFERENCES terms(id),
    PRIMARY KEY (scheme, identifier)
) STRICT, WITHOUT ROWID;
CREATE INDEX term_ids_term_idx ON term_ids(term_id);

-- First exact normalized label per term kind wins, as in the JSON form.
CREATE TABLE term_labels (
    term_kind TEXT NOT NULL,
    normalized_label TEXT NOT NULL,
    term_id INTEGER NOT NULL REFERENCES terms(id),
    PRIMARY KEY (term_kind, normalized_label)
) STRICT, WITHOUT ROWID;

-- Weaker reviewed mappings: shown as analytical context, never used to
-- resolve or deduplicate a hint.
CREATE TABLE related_ids (
    term_id INTEGER NOT NULL REFERENCES terms(id),
    scheme TEXT NOT NULL,
    identifier TEXT NOT NULL,
    match TEXT NOT NULL CHECK (match IN ('close','broader','narrower','related')),
    PRIMARY KEY (term_id, scheme, identifier, match)
) STRICT, WITHOUT ROWID;

-- Non-authority vocabulary IDs (for example Wikidata QIDs) reviewed as broad.
CREATE TABLE generic_ids (
    vocabulary_id TEXT PRIMARY KEY
) STRICT, WITHOUT ROWID;
