-- Product graph v2 -> v3.
--
-- The caller must verify user_version=2 before executing this migration.
-- VACUUM is deliberately performed by Penelope after this transaction commits.
PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;

DROP TRIGGER entity_redirect_alias_not_active_insert;
DROP TRIGGER entity_redirect_alias_not_active_update;
DROP TRIGGER entity_id_not_redirect_alias_insert;
DROP TRIGGER entity_id_not_redirect_alias_update;

DROP TRIGGER source_redirect_alias_not_active_insert;
DROP TRIGGER source_redirect_alias_not_active_update;
DROP TRIGGER source_id_not_redirect_alias_insert;
DROP TRIGGER source_id_not_redirect_alias_update;

DROP TRIGGER concept_slug_not_alias_insert;
DROP TRIGGER concept_slug_not_alias_update;
DROP TRIGGER concept_alias_not_canonical_insert;
DROP TRIGGER concept_alias_not_canonical_update;

-- Rebuilding entities removes the composite UNIQUE(id, entity_type) constraint
-- that existed solely to support entity_redirects. These four live subtype
-- triggers must be absent while SQLite reparses the temporarily parentless
-- child-table schema during ALTER TABLE.
DROP TRIGGER works_entity_type;
DROP TRIGGER manifestations_entity_type;
DROP TRIGGER agents_entity_type;
DROP TRIGGER concepts_entity_type;

DROP TABLE entity_redirects;
DROP TABLE source_redirects;
DROP TABLE concept_slug_aliases;

CREATE TABLE entities_v3 (
    id TEXT PRIMARY KEY,
    entity_type TEXT NOT NULL CHECK (entity_type IN
        ('work','manifestation','person','organization','group','concept'))
) STRICT;

INSERT INTO entities_v3(id, entity_type)
SELECT id, entity_type
FROM entities;

DROP TABLE entities;
ALTER TABLE entities_v3 RENAME TO entities;

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

PRAGMA user_version = 3;
COMMIT;
PRAGMA foreign_keys = ON;
