-- Product graph schema v2. Operational provenance is deliberately absent.
PRAGMA user_version = 2;

CREATE TABLE entities (
    id TEXT PRIMARY KEY,
    entity_type TEXT NOT NULL CHECK (entity_type IN
        ('work','manifestation','person','organization','group','concept')),
    UNIQUE (id, entity_type)
) STRICT;

-- Redirect aliases are retired stable IDs, not active entity rows. The
-- composite foreign key makes cross-type redirects impossible.
CREATE TABLE entity_redirects (
    alias_id TEXT PRIMARY KEY CHECK (
        length(alias_id) BETWEEN 1 AND 128
        AND alias_id NOT GLOB '*[^-A-Za-z0-9_]*'
    ),
    canonical_id TEXT NOT NULL,
    entity_type TEXT NOT NULL CHECK (entity_type IN
        ('work','manifestation','person','organization','group','concept')),
    CHECK (alias_id <> canonical_id),
    FOREIGN KEY (canonical_id, entity_type)
        REFERENCES entities(id, entity_type) ON DELETE RESTRICT
) STRICT;
CREATE INDEX entity_redirects_canonical_idx
    ON entity_redirects(canonical_id);

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
    CHECK (release_year IS NULL OR release_year BETWEEN -9999 AND 9999)
) STRICT;

CREATE TABLE names (
    id TEXT PRIMARY KEY,
    entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
    name_type TEXT NOT NULL CHECK (name_type IN
        ('original','english','transliteration','translation','alias','credited')),
    language_code TEXT,
    script_code TEXT,
    value TEXT NOT NULL CHECK (length(value) > 0),
    is_preferred INTEGER NOT NULL DEFAULT 0 CHECK (is_preferred IN (0,1))
) STRICT;
CREATE INDEX names_entity_idx ON names(entity_id);

CREATE TABLE external_ids (
    id TEXT PRIMARY KEY,
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
    CHECK (birth_year IS NULL OR birth_year BETWEEN -9999 AND 9999),
    CHECK (death_year IS NULL OR death_year BETWEEN -9999 AND 9999),
    CHECK (birth_year IS NULL OR death_year IS NULL OR death_year >= birth_year)
) STRICT;

CREATE TABLE credits (
    id TEXT PRIMARY KEY,
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

CREATE TABLE measurements (
    id TEXT PRIMARY KEY,
    entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
    measurement_type TEXT NOT NULL CHECK (measurement_type IN
        ('duration','height','width','depth','pages')),
    value REAL NOT NULL CHECK (value >= 0),
    unit TEXT NOT NULL CHECK (unit IN ('seconds','millimetres','pages')),
    qualifier TEXT
) STRICT;

CREATE TABLE financial_facts (
    id TEXT PRIMARY KEY,
    work_id TEXT NOT NULL REFERENCES works(entity_id) ON DELETE CASCADE,
    fact_type TEXT NOT NULL CHECK (fact_type = 'budget'),
    amount_min INTEGER NOT NULL CHECK (amount_min >= 0),
    amount_max INTEGER CHECK (amount_max IS NULL OR amount_max >= amount_min),
    currency_code TEXT NOT NULL CHECK (length(currency_code) = 3),
    value_year INTEGER,
    is_estimate INTEGER NOT NULL DEFAULT 0 CHECK (is_estimate IN (0,1)),
    confidence REAL CHECK (confidence IS NULL OR confidence BETWEEN 0 AND 1)
) STRICT;

CREATE TABLE remote_assets (
    id TEXT PRIMARY KEY,
    entity_id TEXT NOT NULL REFERENCES entities(id) ON DELETE CASCADE,
    provider TEXT NOT NULL,
    external_id_id TEXT REFERENCES external_ids(id) ON DELETE SET NULL,
    remote_key TEXT,
    direct_url TEXT,
    resolver_rule TEXT,
    rights_note TEXT,
    CHECK (remote_key IS NOT NULL OR direct_url IS NOT NULL)
) STRICT;

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
    )
) STRICT;

CREATE TABLE concept_slug_aliases (
    slug TEXT PRIMARY KEY CHECK (
        length(slug) > 0
        AND slug NOT GLOB '*[^a-z0-9-]*'
        AND slug NOT GLOB '-*'
        AND slug NOT GLOB '*-'
        AND slug NOT GLOB '*--*'
    ),
    concept_id TEXT NOT NULL
        REFERENCES concepts(entity_id) ON DELETE RESTRICT
) STRICT;
CREATE INDEX concept_slug_aliases_concept_idx
    ON concept_slug_aliases(concept_id);

CREATE TABLE concept_relations (
    id TEXT PRIMARY KEY,
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
    id TEXT PRIMARY KEY,
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
    id TEXT PRIMARY KEY,
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
    CHECK (title IS NOT NULL OR bibliography_text IS NOT NULL OR url IS NOT NULL)
) STRICT;
CREATE UNIQUE INDEX sources_url_unique ON sources(url) WHERE url IS NOT NULL;
CREATE UNIQUE INDEX sources_doi_unique ON sources(doi) WHERE doi IS NOT NULL;
CREATE UNIQUE INDEX sources_isbn_unique ON sources(isbn) WHERE isbn IS NOT NULL;

-- sources.url remains the primary URL. This table preserves any additional
-- stable locators without manufacturing additional source identities.
CREATE TABLE source_urls (
    id TEXT PRIMARY KEY,
    source_id TEXT NOT NULL REFERENCES sources(id) ON DELETE RESTRICT,
    url TEXT NOT NULL UNIQUE CHECK (length(url) > 0)
) STRICT;
CREATE INDEX source_urls_source_idx ON source_urls(source_id);

CREATE TABLE source_redirects (
    alias_id TEXT PRIMARY KEY CHECK (
        length(alias_id) BETWEEN 1 AND 128
        AND alias_id NOT GLOB '*[^-A-Za-z0-9_]*'
    ),
    canonical_id TEXT NOT NULL REFERENCES sources(id) ON DELETE RESTRICT,
    CHECK (alias_id <> canonical_id)
) STRICT;
CREATE INDEX source_redirects_canonical_idx
    ON source_redirects(canonical_id);

CREATE TABLE source_archives (
    id TEXT PRIMARY KEY,
    source_id TEXT NOT NULL REFERENCES sources(id) ON DELETE CASCADE,
    storage_ref TEXT NOT NULL,
    sha256 TEXT NOT NULL CHECK (length(sha256) = 64),
    media_type TEXT NOT NULL,
    archive_scope TEXT NOT NULL CHECK (archive_scope IN
        ('full','article_text','excerpt_bundle')),
    is_verbatim INTEGER NOT NULL CHECK (is_verbatim = 1),
    rights_note TEXT
) STRICT;

CREATE TABLE evidence (
    id TEXT PRIMARY KEY,
    source_id TEXT NOT NULL REFERENCES sources(id) ON DELETE CASCADE,
    source_archive_id TEXT REFERENCES source_archives(id) ON DELETE SET NULL,
    exact_quote TEXT NOT NULL CHECK (length(exact_quote) > 0),
    quote_language TEXT,
    quote_translation TEXT,
    locator_json TEXT CHECK (locator_json IS NULL OR json_valid(locator_json)),
    stance TEXT NOT NULL CHECK (stance IN ('supports','contradicts','contextualizes'))
) STRICT;

CREATE TABLE work_concept_evidence (
    assertion_id TEXT NOT NULL REFERENCES work_concepts(id) ON DELETE CASCADE,
    evidence_id TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE,
    PRIMARY KEY (assertion_id, evidence_id)
) STRICT;

CREATE TABLE concept_relation_evidence (
    assertion_id TEXT NOT NULL REFERENCES concept_relations(id) ON DELETE CASCADE,
    evidence_id TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE,
    PRIMARY KEY (assertion_id, evidence_id)
) STRICT;

CREATE TABLE parent_guide_assertions (
    id TEXT PRIMARY KEY,
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
    assertion_id TEXT NOT NULL REFERENCES parent_guide_assertions(id) ON DELETE CASCADE,
    evidence_id TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE,
    PRIMARY KEY (assertion_id, evidence_id)
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

-- An ID is either active or an alias, never both. Redirect targets are active
-- rows, so chains and cycles cannot be represented.
CREATE TRIGGER entity_redirect_alias_not_active_insert
BEFORE INSERT ON entity_redirects
WHEN EXISTS (SELECT 1 FROM entities WHERE id = NEW.alias_id)
BEGIN SELECT RAISE(ABORT, 'entity redirect alias is still active'); END;

CREATE TRIGGER entity_redirect_alias_not_active_update
BEFORE UPDATE OF alias_id ON entity_redirects
WHEN EXISTS (SELECT 1 FROM entities WHERE id = NEW.alias_id)
BEGIN SELECT RAISE(ABORT, 'entity redirect alias is still active'); END;

CREATE TRIGGER entity_id_not_redirect_alias_insert
BEFORE INSERT ON entities
WHEN EXISTS (SELECT 1 FROM entity_redirects WHERE alias_id = NEW.id)
BEGIN SELECT RAISE(ABORT, 'entity ID is already a redirect alias'); END;

CREATE TRIGGER entity_id_not_redirect_alias_update
BEFORE UPDATE OF id ON entities
WHEN EXISTS (SELECT 1 FROM entity_redirects WHERE alias_id = NEW.id)
BEGIN SELECT RAISE(ABORT, 'entity ID is already a redirect alias'); END;

CREATE TRIGGER source_redirect_alias_not_active_insert
BEFORE INSERT ON source_redirects
WHEN EXISTS (SELECT 1 FROM sources WHERE id = NEW.alias_id)
BEGIN SELECT RAISE(ABORT, 'source redirect alias is still active'); END;

CREATE TRIGGER source_redirect_alias_not_active_update
BEFORE UPDATE OF alias_id ON source_redirects
WHEN EXISTS (SELECT 1 FROM sources WHERE id = NEW.alias_id)
BEGIN SELECT RAISE(ABORT, 'source redirect alias is still active'); END;

CREATE TRIGGER source_id_not_redirect_alias_insert
BEFORE INSERT ON sources
WHEN EXISTS (SELECT 1 FROM source_redirects WHERE alias_id = NEW.id)
BEGIN SELECT RAISE(ABORT, 'source ID is already a redirect alias'); END;

CREATE TRIGGER source_id_not_redirect_alias_update
BEFORE UPDATE OF id ON sources
WHEN EXISTS (SELECT 1 FROM source_redirects WHERE alias_id = NEW.id)
BEGIN SELECT RAISE(ABORT, 'source ID is already a redirect alias'); END;

-- A slug can identify one active concept either canonically or as an alias.
CREATE TRIGGER concept_slug_not_alias_insert
BEFORE INSERT ON concepts
WHEN EXISTS (
    SELECT 1 FROM concept_slug_aliases WHERE slug = NEW.slug
)
BEGIN SELECT RAISE(ABORT, 'concept slug is already an alias'); END;

CREATE TRIGGER concept_slug_not_alias_update
BEFORE UPDATE OF slug ON concepts
WHEN EXISTS (
    SELECT 1 FROM concept_slug_aliases WHERE slug = NEW.slug
)
BEGIN SELECT RAISE(ABORT, 'concept slug is already an alias'); END;

CREATE TRIGGER concept_alias_not_canonical_insert
BEFORE INSERT ON concept_slug_aliases
WHEN EXISTS (SELECT 1 FROM concepts WHERE slug = NEW.slug)
BEGIN SELECT RAISE(ABORT, 'concept alias is already a canonical slug'); END;

CREATE TRIGGER concept_alias_not_canonical_update
BEFORE UPDATE OF slug ON concept_slug_aliases
WHEN EXISTS (SELECT 1 FROM concepts WHERE slug = NEW.slug)
BEGIN SELECT RAISE(ABORT, 'concept alias is already a canonical slug'); END;

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
