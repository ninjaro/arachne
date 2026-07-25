-- Product graph schema v4. Canonical entities use compact readable text IDs;
-- internal records use row-local integer keys. Operational provenance and
-- retired-ID compatibility metadata are deliberately absent.
PRAGMA user_version = 4;

CREATE TABLE entities (
    id TEXT PRIMARY KEY,
    entity_type TEXT NOT NULL CHECK (entity_type IN
        ('work','manifestation','person','organization','group','concept')),
    CHECK (
        (entity_type = 'work'
         AND length(id) >= 11
         AND substr(id, 1, 5) = 'work-'
         AND substr(id, 6) NOT GLOB '*[^0-9]*')
        OR
        (entity_type = 'manifestation'
         AND length(id) >= 20
         AND substr(id, 1, 14) = 'manifestation-'
         AND substr(id, 15) NOT GLOB '*[^0-9]*')
        OR
        (entity_type IN ('person','organization','group')
         AND length(id) >= 12
         AND substr(id, 1, 6) = 'agent-'
         AND substr(id, 7) NOT GLOB '*[^0-9]*')
        OR
        (entity_type = 'concept'
         AND length(id) >= 14
         AND substr(id, 1, 8) = 'concept-'
         AND substr(id, 9) NOT GLOB '*[^0-9]*')
    )
) STRICT;

CREATE TABLE works (
    entity_id TEXT PRIMARY KEY REFERENCES entities(id) ON DELETE CASCADE,
    medium TEXT NOT NULL CHECK (medium IN
        ('film','short_film','television','novel','novella','short_story',
         'poetry','play','essay','album','single','composition','painting',
         'print','engraving','drawing','sculpture','installation',
         'photography','mixed_media')),
    year_start INTEGER,
    year_end INTEGER,
    date_precision TEXT CHECK (date_precision IS NULL OR date_precision IN
        ('year','decade','approximate','range','exact')),
    date_start_text TEXT,
    date_end_text TEXT,
    date_qualifier TEXT,
    language_code TEXT,
    country_code TEXT,
    production_info_json TEXT CHECK
        (production_info_json IS NULL OR json_valid(production_info_json)),
    CHECK (
        length(entity_id) >= 11
        AND substr(entity_id, 1, 5) = 'work-'
        AND substr(entity_id, 6) NOT GLOB '*[^0-9]*'
    ),
    CHECK (year_start IS NULL OR year_start BETWEEN -9999 AND 9999),
    CHECK (year_end IS NULL OR year_end BETWEEN -9999 AND 9999),
    CHECK (year_start IS NULL OR year_end IS NULL OR year_end >= year_start)
) STRICT;

CREATE TABLE manifestations (
    entity_id TEXT PRIMARY KEY REFERENCES entities(id) ON DELETE CASCADE,
    work_id TEXT NOT NULL REFERENCES works(entity_id) ON DELETE CASCADE,
    manifestation_type TEXT NOT NULL CHECK (manifestation_type IN
        ('edition','translation','release','pressing','cut','restoration',
         'reissue')),
    release_year INTEGER,
    region_code TEXT,
    language_code TEXT,
    label TEXT NOT NULL,
    CHECK (
        length(entity_id) >= 20
        AND substr(entity_id, 1, 14) = 'manifestation-'
        AND substr(entity_id, 15) NOT GLOB '*[^0-9]*'
    ),
    CHECK (
        length(work_id) >= 11
        AND substr(work_id, 1, 5) = 'work-'
        AND substr(work_id, 6) NOT GLOB '*[^0-9]*'
    ),
    CHECK (release_year IS NULL OR release_year BETWEEN -9999 AND 9999)
) STRICT;

CREATE TABLE names (
    id INTEGER PRIMARY KEY,
    entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
    name_type TEXT NOT NULL CHECK (name_type IN
        ('original','english','transliteration','translation','alias','credited')),
    language_code TEXT,
    script_code TEXT,
    value TEXT NOT NULL CHECK (length(value) > 0),
    is_preferred INTEGER NOT NULL DEFAULT 0 CHECK (is_preferred IN (0,1))
) STRICT;
CREATE INDEX names_entity_idx ON names(entity_id);
-- Mirrors the former hash key. NULL and an empty optional component denote
-- the same logical identity.
CREATE UNIQUE INDEX names_logical_unique ON names(
    entity_id,
    name_type,
    COALESCE(language_code, ''),
    COALESCE(script_code, ''),
    value
);

CREATE TABLE external_ids (
    id INTEGER PRIMARY KEY,
    entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
    scheme TEXT NOT NULL CHECK (length(scheme) > 0),
    value TEXT NOT NULL CHECK (length(value) > 0),
    canonical_url TEXT,
    UNIQUE (scheme, value)
) STRICT;
CREATE INDEX external_ids_entity_idx ON external_ids(entity_id);

CREATE TABLE agents (
    entity_id TEXT PRIMARY KEY REFERENCES entities(id) ON DELETE CASCADE,
    agent_type TEXT NOT NULL CHECK (agent_type IN ('person','organization','group')),
    birth_year INTEGER,
    death_year INTEGER,
    CHECK (
        length(entity_id) >= 12
        AND substr(entity_id, 1, 6) = 'agent-'
        AND substr(entity_id, 7) NOT GLOB '*[^0-9]*'
    ),
    CHECK (birth_year IS NULL OR birth_year BETWEEN -9999 AND 9999),
    CHECK (death_year IS NULL OR death_year BETWEEN -9999 AND 9999),
    CHECK (birth_year IS NULL OR death_year IS NULL OR death_year >= birth_year)
) STRICT;

CREATE TABLE credits (
    id INTEGER PRIMARY KEY,
    work_id TEXT NOT NULL REFERENCES works(entity_id) ON DELETE CASCADE,
    agent_id TEXT NOT NULL REFERENCES agents(entity_id) ON DELETE CASCADE,
    role TEXT NOT NULL CHECK (role IN
        ('author','director','screenwriter','producer','actor','composer',
         'performer','artist','engraver','sculptor','photographer','editor',
         'cinematographer','production_company','publisher','record_label','band')),
    credit_order INTEGER CHECK (credit_order IS NULL OR credit_order >= 0),
    importance TEXT NOT NULL CHECK (importance IN ('primary','key','supporting')),
    credited_as TEXT
) STRICT;
CREATE INDEX credits_work_idx ON credits(work_id);
CREATE UNIQUE INDEX credits_logical_unique ON credits(
    work_id,
    agent_id,
    role,
    COALESCE(credit_order, -1),
    COALESCE(credited_as, '')
);

CREATE TABLE measurements (
    id INTEGER PRIMARY KEY,
    entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
    measurement_type TEXT NOT NULL CHECK (measurement_type IN
        ('duration','height','width','depth','pages')),
    value REAL NOT NULL CHECK (value >= 0),
    unit TEXT NOT NULL CHECK (unit IN ('seconds','millimetres','pages')),
    qualifier TEXT
) STRICT;
CREATE UNIQUE INDEX measurements_logical_unique ON measurements(
    entity_id,
    measurement_type,
    value,
    unit,
    COALESCE(qualifier, '')
);

CREATE TABLE financial_facts (
    id INTEGER PRIMARY KEY,
    work_id TEXT NOT NULL REFERENCES works(entity_id) ON DELETE CASCADE,
    fact_type TEXT NOT NULL CHECK (fact_type = 'budget'),
    amount_min INTEGER NOT NULL CHECK (amount_min >= 0),
    amount_max INTEGER CHECK (amount_max IS NULL OR amount_max >= amount_min),
    currency_code TEXT NOT NULL CHECK (length(currency_code) = 3),
    value_year INTEGER,
    is_estimate INTEGER NOT NULL DEFAULT 0 CHECK (is_estimate IN (0,1)),
    confidence REAL CHECK (confidence IS NULL OR confidence BETWEEN 0 AND 1)
) STRICT;
CREATE UNIQUE INDEX financial_facts_logical_unique ON financial_facts(
    work_id,
    fact_type,
    amount_min,
    COALESCE(CAST(amount_max AS TEXT), ''),
    currency_code,
    COALESCE(CAST(value_year AS TEXT), '')
);

CREATE TABLE remote_assets (
    id INTEGER PRIMARY KEY,
    entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
    provider TEXT NOT NULL,
    external_id_id INTEGER REFERENCES external_ids(id) ON DELETE SET NULL,
    remote_key TEXT,
    direct_url TEXT,
    resolver_rule TEXT,
    rights_note TEXT,
    CHECK (remote_key IS NOT NULL OR direct_url IS NOT NULL)
) STRICT;
CREATE UNIQUE INDEX remote_assets_logical_unique ON remote_assets(
    entity_id,
    provider,
    COALESCE(remote_key, ''),
    COALESCE(direct_url, '')
);

CREATE TABLE concepts (
    entity_id TEXT PRIMARY KEY REFERENCES entities(id) ON DELETE CASCADE,
    concept_type TEXT NOT NULL CHECK (concept_type IN
        ('genre','style','theme','keyword','motif','trope','phobia','taboo',
         'technique','movement','setting','mood','content_warning')),
    slug TEXT NOT NULL UNIQUE CHECK (
        length(slug) > 0
        AND slug NOT GLOB '*[^a-z0-9-]*'
        AND slug NOT GLOB '-*'
        AND slug NOT GLOB '*-'
        AND slug NOT GLOB '*--*'
    ),
    CHECK (
        length(entity_id) >= 14
        AND substr(entity_id, 1, 8) = 'concept-'
        AND substr(entity_id, 9) NOT GLOB '*[^0-9]*'
    )
) STRICT;

CREATE TABLE concept_relations (
    id INTEGER PRIMARY KEY,
    subject_concept_id TEXT NOT NULL REFERENCES concepts(entity_id) ON DELETE CASCADE,
    relation_type TEXT NOT NULL CHECK (relation_type IN
        ('broader_than','narrower_than','derived_from','precursor_of','hybrid_of',
         'revival_of','regional_variant_of','influenced_by','opposes','alias_of')),
    object_concept_id TEXT NOT NULL REFERENCES concepts(entity_id) ON DELETE CASCADE,
    strength INTEGER CHECK (strength IS NULL OR strength BETWEEN 1 AND 100),
    from_year INTEGER,
    to_year INTEGER,
    region_code TEXT,
    confidence REAL CHECK (confidence IS NULL OR confidence BETWEEN 0 AND 1),
    CHECK (subject_concept_id <> object_concept_id),
    CHECK (from_year IS NULL OR to_year IS NULL OR to_year >= from_year),
    UNIQUE (subject_concept_id, relation_type, object_concept_id)
) STRICT;

CREATE TABLE work_concepts (
    id INTEGER PRIMARY KEY,
    work_id TEXT NOT NULL REFERENCES works(entity_id) ON DELETE CASCADE,
    concept_id TEXT NOT NULL REFERENCES concepts(entity_id) ON DELETE CASCADE,
    relation_type TEXT NOT NULL CHECK (relation_type IN
        ('exemplifies','contains','anticipates','influenced_by','influences',
         'revives','parodies','deconstructs','associated_with')),
    centrality INTEGER NOT NULL CHECK (centrality BETWEEN 1 AND 100),
    historical_role TEXT CHECK (historical_role IS NULL OR historical_role IN
        ('formative','canonical','transitional','hybrid','revival',
         'late_derivative','peripheral','precursor')),
    confidence REAL CHECK (confidence IS NULL OR confidence BETWEEN 0 AND 1),
    UNIQUE (work_id, concept_id, relation_type)
) STRICT;

CREATE TABLE sources (
    id INTEGER PRIMARY KEY,
    source_type TEXT NOT NULL CHECK (source_type IN
        ('book','article','catalogue','web_page','interview','database',
         'video','audio','PDF')),
    title TEXT,
    bibliography_text TEXT,
    author_text TEXT,
    publisher TEXT,
    publication_date TEXT,
    url TEXT CHECK (url IS NULL OR length(url) > 0),
    doi TEXT,
    isbn TEXT,
    language_code TEXT,
    CHECK (
        doi IS NOT NULL
        OR isbn IS NOT NULL
        OR url IS NOT NULL
        OR bibliography_text IS NOT NULL
    )
) STRICT;
CREATE UNIQUE INDEX sources_url_unique ON sources(url) WHERE url IS NOT NULL;
CREATE UNIQUE INDEX sources_doi_unique ON sources(doi) WHERE doi IS NOT NULL;
CREATE UNIQUE INDEX sources_isbn_unique ON sources(isbn) WHERE isbn IS NOT NULL;
CREATE UNIQUE INDEX sources_bibliography_fallback_unique
ON sources(bibliography_text)
WHERE doi IS NULL
  AND isbn IS NULL
  AND url IS NULL
  AND bibliography_text IS NOT NULL;

-- sources.url remains the primary URL. This table preserves any additional
-- stable locators without manufacturing additional source identities.
CREATE TABLE source_urls (
    id INTEGER PRIMARY KEY,
    source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE RESTRICT,
    url TEXT NOT NULL UNIQUE CHECK (length(url) > 0)
) STRICT;
CREATE INDEX source_urls_source_idx ON source_urls(source_id);

CREATE TABLE source_archives (
    id INTEGER PRIMARY KEY,
    source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,
    storage_ref TEXT NOT NULL,
    sha256 TEXT NOT NULL CHECK (
        length(sha256) = 64
        AND sha256 NOT GLOB '*[^0-9a-f]*'
    ),
    media_type TEXT NOT NULL,
    archive_scope TEXT NOT NULL CHECK (archive_scope IN
        ('full','article_text','excerpt_bundle')),
    is_verbatim INTEGER NOT NULL CHECK (is_verbatim = 1),
    rights_note TEXT,
    UNIQUE (source_id, storage_ref, sha256)
) STRICT;

CREATE TABLE evidence (
    id INTEGER PRIMARY KEY,
    source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,
    source_archive_id INTEGER REFERENCES source_archives(id) ON DELETE SET NULL,
    exact_quote TEXT NOT NULL CHECK (length(exact_quote) > 0),
    quote_language TEXT,
    quote_translation TEXT,
    locator_json TEXT CHECK (locator_json IS NULL OR json_valid(locator_json)),
    stance TEXT NOT NULL CHECK (stance IN ('supports','contradicts','contextualizes'))
) STRICT;
CREATE UNIQUE INDEX evidence_logical_unique ON evidence(
    source_id,
    exact_quote,
    COALESCE(locator_json, ''),
    stance
);

CREATE TABLE work_concept_evidence (
    id INTEGER PRIMARY KEY,
    assertion_id INTEGER NOT NULL REFERENCES work_concepts(id) ON DELETE CASCADE,
    evidence_id INTEGER NOT NULL REFERENCES evidence(id) ON DELETE CASCADE,
    UNIQUE (assertion_id, evidence_id)
) STRICT;

CREATE TABLE concept_relation_evidence (
    id INTEGER PRIMARY KEY,
    assertion_id INTEGER NOT NULL REFERENCES concept_relations(id) ON DELETE CASCADE,
    evidence_id INTEGER NOT NULL REFERENCES evidence(id) ON DELETE CASCADE,
    UNIQUE (assertion_id, evidence_id)
) STRICT;

CREATE TABLE parent_guide_assertions (
    id INTEGER PRIMARY KEY,
    work_id TEXT NOT NULL REFERENCES works(entity_id) ON DELETE CASCADE,
    concept_id TEXT NOT NULL REFERENCES concepts(entity_id) ON DELETE CASCADE,
    category TEXT NOT NULL CHECK (category IN
        ('violence','sex_nudity','language','drugs','frightening','self_harm',
         'discrimination','abuse','taboo')),
    intensity INTEGER NOT NULL CHECK (intensity BETWEEN 1 AND 5),
    explicitness INTEGER NOT NULL CHECK (explicitness BETWEEN 1 AND 5),
    frequency INTEGER NOT NULL CHECK (frequency BETWEEN 1 AND 5),
    centrality INTEGER NOT NULL CHECK (centrality BETWEEN 1 AND 5),
    realism INTEGER NOT NULL CHECK (realism BETWEEN 1 AND 5),
    spoiler_level TEXT NOT NULL CHECK (spoiler_level IN ('none','mild','major')),
    confidence REAL CHECK (confidence IS NULL OR confidence BETWEEN 0 AND 1),
    UNIQUE (work_id, concept_id, category)
) STRICT;

CREATE TABLE parent_guide_evidence (
    id INTEGER PRIMARY KEY,
    assertion_id INTEGER NOT NULL REFERENCES parent_guide_assertions(id) ON DELETE CASCADE,
    evidence_id INTEGER NOT NULL REFERENCES evidence(id) ON DELETE CASCADE,
    UNIQUE (assertion_id, evidence_id)
) STRICT;

CREATE TRIGGER works_entity_type BEFORE INSERT ON works
WHEN (SELECT entity_type FROM entities WHERE id = NEW.entity_id) <> 'work'
BEGIN SELECT RAISE(ABORT, 'work entity has wrong type'); END;

CREATE TRIGGER manifestations_entity_type BEFORE INSERT ON manifestations
WHEN (SELECT entity_type FROM entities WHERE id = NEW.entity_id) <> 'manifestation'
BEGIN SELECT RAISE(ABORT, 'manifestation entity has wrong type'); END;

CREATE TRIGGER agents_entity_type BEFORE INSERT ON agents
WHEN (SELECT entity_type FROM entities WHERE id = NEW.entity_id) <> NEW.agent_type
BEGIN SELECT RAISE(ABORT, 'agent entity has wrong type'); END;

CREATE TRIGGER concepts_entity_type BEFORE INSERT ON concepts
WHEN (SELECT entity_type FROM entities WHERE id = NEW.entity_id) <> 'concept'
BEGIN SELECT RAISE(ABORT, 'concept entity has wrong type'); END;

CREATE TRIGGER evidence_archive_source_insert
BEFORE INSERT ON evidence
WHEN NEW.source_archive_id IS NOT NULL
 AND NOT EXISTS (
     SELECT 1 FROM source_archives
     WHERE id = NEW.source_archive_id AND source_id = NEW.source_id
 )
BEGIN SELECT RAISE(ABORT, 'evidence archive belongs to another source'); END;

CREATE TRIGGER evidence_archive_source_update
BEFORE UPDATE OF source_id, source_archive_id ON evidence
WHEN NEW.source_archive_id IS NOT NULL
 AND NOT EXISTS (
     SELECT 1 FROM source_archives
     WHERE id = NEW.source_archive_id AND source_id = NEW.source_id
 )
BEGIN SELECT RAISE(ABORT, 'evidence archive belongs to another source'); END;

-- Primary and alternate source URLs share one global identity namespace.
CREATE TRIGGER source_primary_url_not_alternate_insert
BEFORE INSERT ON sources
WHEN NEW.url IS NOT NULL
 AND EXISTS (SELECT 1 FROM source_urls WHERE url = NEW.url)
BEGIN SELECT RAISE(ABORT, 'source URL is already an alternate URL'); END;

CREATE TRIGGER source_primary_url_not_alternate_update
BEFORE UPDATE OF url ON sources
WHEN NEW.url IS NOT NULL
 AND EXISTS (SELECT 1 FROM source_urls WHERE url = NEW.url)
BEGIN SELECT RAISE(ABORT, 'source URL is already an alternate URL'); END;

CREATE TRIGGER source_alternate_url_not_primary_insert
BEFORE INSERT ON source_urls
WHEN EXISTS (SELECT 1 FROM sources WHERE url = NEW.url)
BEGIN SELECT RAISE(ABORT, 'alternate URL is already a primary URL'); END;

CREATE TRIGGER source_alternate_url_not_primary_update
BEFORE UPDATE OF url ON source_urls
WHEN EXISTS (SELECT 1 FROM sources WHERE url = NEW.url)
BEGIN SELECT RAISE(ABORT, 'alternate URL is already a primary URL'); END;
