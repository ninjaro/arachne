import type {
  Agent,
  AgentRelation,
  Catalog,
  ConceptAssignment,
  Domain,
  Identifier,
  ProductEvent,
  ResearchData,
  WorkMembership,
  WorkRelation,
} from "./types";

const WORK_MEDIA = new Set([
  "film", "short_film", "television", "novel", "novella", "short_story",
  "poetry", "play", "essay", "album", "single", "composition", "painting",
  "print", "engraving", "drawing", "sculpture", "installation",
  "photography", "mixed_media", "nonfiction", "comic", "performance",
]);
const DATE_PRECISIONS = new Set([
  "year", "month", "exact", "decade", "approximate", "range",
]);
const EVENT_TYPES = new Set([
  "created", "published", "released", "premiered", "broadcast", "performed",
  "exhibited", "recorded",
]);
const MEMBERSHIP_TYPES = new Set([
  "episode_of", "season_of", "track_of", "volume_of", "issue_of",
  "chapter_of", "part_of", "collected_in",
]);
const AGENT_RELATION_TYPES = new Set([
  "member_of", "founder_of", "subsidiary_of", "division_of", "imprint_of",
  "owned_by", "successor_of", "predecessor_of",
]);
const MANIFESTATION_TYPES = new Set([
  "edition", "translation", "release", "pressing", "cut", "restoration", "reissue",
]);
const AGENT_TYPES = new Set(["person", "organization", "group"]);
const CREDIT_ROLES = new Set([
  "author", "director", "screenwriter", "producer", "actor", "composer",
  "performer", "artist", "engraver", "sculptor", "photographer", "editor",
  "cinematographer", "production_company", "publisher", "record_label", "band",
  "distributor", "broadcaster", "platform", "translator", "illustrator",
  "printer", "curator", "choreographer", "narrator", "lyricist", "songwriter",
  "arranger", "sound_engineer", "designer", "animator",
]);
const CONCEPT_TYPES = new Set([
  "genre", "style", "theme", "keyword", "motif", "trope", "phobia", "taboo",
  "technique", "movement", "setting", "mood", "content_warning",
]);
const WORK_CONCEPT_RELATIONS = new Set([
  "exemplifies", "contains", "anticipates", "influenced_by", "influences",
  "revives", "parodies", "deconstructs", "associated_with",
]);
const HISTORICAL_ROLES = new Set([
  "formative", "canonical", "transitional", "hybrid", "revival",
  "late_derivative", "peripheral", "precursor",
]);
const ADVISORY_CATEGORIES = new Set([
  "violence", "sex_nudity", "language", "drugs", "frightening", "self_harm",
  "discrimination", "abuse", "taboo",
]);
const MEASUREMENT_TYPES = new Set(["duration", "height", "width", "depth", "pages"]);
const MEASUREMENT_UNITS = new Set(["seconds", "millimetres", "pages"]);

function isNullableString(value: unknown): value is string | null {
  return value === null || (typeof value === "string" && value.length > 0);
}

function isNullableInteger(value: unknown): value is number | null {
  return value === null || Number.isInteger(value);
}

function isIdentifier(value: unknown): value is Identifier {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.scheme === "string" &&
    record.scheme.length > 0 &&
    typeof record.value === "string" &&
    record.value.length > 0 &&
    (record.url === null || typeof record.url === "string")
  );
}

function isAgent(value: unknown): value is Agent {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.id === "string" &&
    record.id.length > 0 &&
    typeof record.label === "string" &&
    record.label.length > 0 &&
    typeof record.agentType === "string" &&
    AGENT_TYPES.has(record.agentType) &&
    Array.isArray(record.identifiers) &&
    record.identifiers.every(isIdentifier)
  );
}

function identifiersEqual(left: Identifier[], right: Identifier[]): boolean {
  return (
    left.length === right.length &&
    left.every(
      (identifier, index) =>
        identifier.scheme === right[index].scheme &&
        identifier.value === right[index].value &&
        identifier.url === right[index].url,
    )
  );
}

function isContributor(value: unknown): boolean {
  if (!isAgent(value)) return false;
  const record = value as unknown as Record<string, unknown>;
  return (
    typeof record.role === "string" && CREDIT_ROLES.has(record.role) &&
    (record.order === null || (Number.isInteger(record.order) && (record.order as number) >= 0)) &&
    (record.importance === "primary" || record.importance === "key" || record.importance === "supporting") &&
    isNullableString(record.creditedAs)
  );
}

function isWorkRelation(value: unknown): value is WorkRelation {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.subjectId === "string" &&
    record.subjectId.length > 0 &&
    typeof record.objectId === "string" &&
    record.objectId.length > 0 &&
    typeof record.relationType === "string" &&
    record.relationType.trim().length > 0
  );
}

function isProductEvent(value: unknown): value is ProductEvent {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.id === "string" && record.id.length > 0 &&
    typeof record.entityId === "string" && record.entityId.length > 0 &&
    typeof record.eventType === "string" && EVENT_TYPES.has(record.eventType) &&
    isNullableInteger(record.yearStart) &&
    isNullableInteger(record.yearEnd) &&
    (record.yearStart === null || record.yearEnd === null || record.yearEnd >= record.yearStart) &&
    isNullableString(record.dateText) &&
    (record.datePrecision === null ||
      (typeof record.datePrecision === "string" && DATE_PRECISIONS.has(record.datePrecision))) &&
    isNullableString(record.placeText)
  );
}

function eventsEqual(left: ProductEvent, right: ProductEvent): boolean {
  return (
    left.id === right.id &&
    left.entityId === right.entityId &&
    left.eventType === right.eventType &&
    left.yearStart === right.yearStart &&
    left.yearEnd === right.yearEnd &&
    left.dateText === right.dateText &&
    left.datePrecision === right.datePrecision &&
    left.placeText === right.placeText
  );
}

function isWorkMembership(value: unknown): value is WorkMembership {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.id === "string" && record.id.length > 0 &&
    typeof record.childId === "string" && record.childId.length > 0 &&
    typeof record.parentId === "string" && record.parentId.length > 0 &&
    record.childId !== record.parentId &&
    typeof record.membershipType === "string" &&
    MEMBERSHIP_TYPES.has(record.membershipType) &&
    (record.position === null ||
      (Number.isInteger(record.position) && (record.position as number) >= 0)) &&
    isNullableString(record.positionText)
  );
}

function isAdvisory(value: unknown): boolean {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.id === "string" && record.id.length > 0 &&
    typeof record.category === "string" && ADVISORY_CATEGORIES.has(record.category) &&
    (record.spoilerLevel === null || record.spoilerLevel === "none" ||
      record.spoilerLevel === "mild" || record.spoilerLevel === "major")
  );
}

function isMeasurement(value: unknown): boolean {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.type === "string" && MEASUREMENT_TYPES.has(record.type) &&
    typeof record.value === "number" && Number.isFinite(record.value) && record.value >= 0 &&
    (record.unit === null ||
      (typeof record.unit === "string" && MEASUREMENT_UNITS.has(record.unit))) &&
    (record.qualifier === null || typeof record.qualifier === "string")
  );
}

function isFinancialFact(value: unknown): boolean {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    record.type === "budget" &&
    isNullableInteger(record.amountMin) &&
    isNullableInteger(record.amountMax) &&
    isNullableInteger(record.valueYear) &&
    (record.currencyCode === null ||
      (typeof record.currencyCode === "string" && /^[A-Z]{3}$/u.test(record.currencyCode))) &&
    typeof record.isEstimate === "boolean" &&
    (record.confidence === null ||
      (typeof record.confidence === "number" && record.confidence >= 0 && record.confidence <= 1))
  );
}

function isAgentRelation(value: unknown): value is AgentRelation {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.id === "string" && record.id.length > 0 &&
    typeof record.subjectId === "string" && record.subjectId.length > 0 &&
    typeof record.objectId === "string" && record.objectId.length > 0 &&
    record.subjectId !== record.objectId &&
    typeof record.relationType === "string" &&
    AGENT_RELATION_TYPES.has(record.relationType) &&
    isNullableInteger(record.fromYear) &&
    isNullableInteger(record.toYear) &&
    (record.fromYear === null || record.toYear === null || record.toYear >= record.fromYear) &&
    isNullableString(record.periodText) &&
    isNullableString(record.roleText)
  );
}

const CENTRALITY_SCALES = new Set(["none", "binary", "ordinal", "graded"]);

function isConceptAssignment(value: unknown): value is ConceptAssignment {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  return (
    typeof record.id === "string" &&
    record.id.length > 0 &&
    typeof record.conceptType === "string" &&
    CONCEPT_TYPES.has(record.conceptType) &&
    typeof record.relationType === "string" &&
    WORK_CONCEPT_RELATIONS.has(record.relationType) &&
    (record.historicalRole === null ||
      (typeof record.historicalRole === "string" && HISTORICAL_ROLES.has(record.historicalRole))) &&
    typeof record.centralityScale === "string" &&
    CENTRALITY_SCALES.has(record.centralityScale) &&
    (record.centrality === null ||
      (typeof record.centrality === "number" && Number.isFinite(record.centrality)))
  );
}

export function isCatalog(value: unknown): value is Catalog {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  if (
    record.formatVersion !== 1 ||
    !Array.isArray(record.agents) ||
    !record.agents.every(isAgent) ||
    !Array.isArray(record.works) ||
    !Array.isArray(record.workMemberships) ||
    !record.workMemberships.every(isWorkMembership) ||
    !Array.isArray(record.agentRelations) ||
    !record.agentRelations.every(isAgentRelation) ||
    !Array.isArray(record.events) ||
    !record.events.every(isProductEvent) ||
    (record.workRelations !== undefined &&
      (!Array.isArray(record.workRelations) ||
        !record.workRelations.every(isWorkRelation)))
  ) {
    return false;
  }

  const agentById = new Map(record.agents.map((agent) => [agent.id, agent]));
  if (agentById.size !== record.agents.length) return false;

  const workIds = new Set(
    record.works
      .filter((work): work is Record<string, unknown> => !!work && typeof work === "object")
      .map((work) => work.id)
      .filter((id): id is string => typeof id === "string" && id.length > 0),
  );
  const manifestationIds = new Set<string>();
  const eventById = new Map((record.events as ProductEvent[]).map((event) => [event.id, event]));
  if (eventById.size !== record.events.length) return false;
  if (!(record.workMemberships as WorkMembership[]).every(
    (membership) => workIds.has(membership.childId) && workIds.has(membership.parentId),
  )) return false;
  if (!(record.agentRelations as AgentRelation[]).every(
    (relation) => agentById.has(relation.subjectId) && agentById.has(relation.objectId),
  )) return false;

  const validWorks = record.works.every((work) => {
    if (!work || typeof work !== "object") return false;
    const workRecord = work as Record<string, unknown>;
    const contributors = workRecord.contributors;
    const concepts = workRecord.concepts;
    if (
      typeof workRecord.medium !== "string" || !WORK_MEDIA.has(workRecord.medium) ||
      !(workRecord.datePrecision === null ||
        (typeof workRecord.datePrecision === "string" && DATE_PRECISIONS.has(workRecord.datePrecision))) ||
      !Array.isArray(workRecord.events) ||
      !(workRecord.events as unknown[]).every(
        (event) => isProductEvent(event) &&
          event.entityId === workRecord.id &&
          eventById.has(event.id) &&
          eventsEqual(event, eventById.get(event.id) as ProductEvent),
      ) ||
      !Array.isArray(workRecord.manifestations) ||
      !Array.isArray(workRecord.advisories) || !workRecord.advisories.every(isAdvisory) ||
      !Array.isArray(workRecord.measurements) || !workRecord.measurements.every(isMeasurement) ||
      !Array.isArray(workRecord.financialFacts) || !workRecord.financialFacts.every(isFinancialFact)
    ) return false;
    for (const manifestation of workRecord.manifestations) {
      if (!manifestation || typeof manifestation !== "object") return false;
      const item = manifestation as Record<string, unknown>;
      if (
        typeof item.id !== "string" || item.id.length === 0 || manifestationIds.has(item.id) ||
        typeof item.type !== "string" || !MANIFESTATION_TYPES.has(item.type) ||
        !isNullableInteger(item.releaseYear) ||
        isNullableString(item.regionCode) === false ||
        isNullableString(item.languageCode) === false ||
        isNullableString(item.label) === false ||
        !Array.isArray(item.contributors) ||
        !item.contributors.every((contributor) => {
          if (!isContributor(contributor)) return false;
          const canonical = agentById.get((contributor as Agent).id);
          return canonical !== undefined &&
            canonical.label === (contributor as Agent).label &&
            canonical.agentType === (contributor as Agent).agentType &&
            identifiersEqual(canonical.identifiers, (contributor as Agent).identifiers);
        }) ||
        !Array.isArray(item.events) ||
        !item.events.every((event) => isProductEvent(event) &&
          event.entityId === item.id && eventById.has(event.id) &&
          eventsEqual(event, eventById.get(event.id) as ProductEvent))
      ) return false;
      manifestationIds.add(item.id);
    }
    const missing = Array.isArray(concepts)
      ? concepts.filter(
          (concept) =>
            isConceptAssignment(concept) && concept.centralityScale === "none",
        ).length
      : -1;
    return (
      Array.isArray(concepts) &&
      concepts.every(isConceptAssignment) &&
      workRecord.conceptAssignmentCount === concepts.length &&
      workRecord.missingCentralityScaleCount === missing &&
      typeof workRecord.missingCentralityScaleFraction === "number" &&
      Number.isFinite(workRecord.missingCentralityScaleFraction) &&
      workRecord.missingCentralityScaleFraction ===
        (concepts.length === 0 ? 0 : missing / concepts.length) &&
      Array.isArray(contributors) &&
      contributors.every((contributor) => {
        if (!isContributor(contributor)) return false;
        const canonical = agentById.get(contributor.id);
        return (
          canonical !== undefined &&
          canonical.label === contributor.label &&
          canonical.agentType === contributor.agentType &&
          identifiersEqual(canonical.identifiers, contributor.identifiers)
        );
      })
    );
  });
  if (!validWorks) return false;
  const targetIds = new Set([...workIds, ...manifestationIds]);
  return (record.events as ProductEvent[]).every((event) => targetIds.has(event.entityId));
}

export function isResearchData(value: unknown): value is ResearchData {
  if (!value || typeof value !== "object") return false;
  const record = value as Record<string, unknown>;
  const snapshot = record.product_snapshot;
  return (
    record.artifact_type === "product_research_report_v1" &&
    record.format_version === 1 &&
    record.formatVersion === 1 &&
    typeof record.productSnapshotId === "string" &&
    record.productSnapshotId.length > 0 &&
    snapshot !== null &&
    typeof snapshot === "object" &&
    !Array.isArray(snapshot) &&
    (snapshot as Record<string, unknown>).snapshot_id === record.productSnapshotId &&
    typeof (snapshot as Record<string, unknown>).sha256 === "string" &&
    /^[a-f0-9]{64}$/u.test(
      (snapshot as Record<string, unknown>).sha256 as string,
    ) &&
    isCentralityScaleCoverage(record.centrality_scale_coverage) &&
    record.summary !== null &&
    typeof record.summary === "object" &&
    Array.isArray(record.items)
  );
}

function isCentralityScaleCoverage(value: unknown): boolean {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const coverage = value as Record<string, unknown>;
  if (!Array.isArray(coverage.works)) return false;
  const ids = new Set<string>();
  let assignmentCount = 0;
  let missingCount = 0;
  for (const value of coverage.works) {
    if (!value || typeof value !== "object" || Array.isArray(value)) return false;
    const work = value as Record<string, unknown>;
    if (
      typeof work.work_id !== "string" ||
      work.work_id.length === 0 ||
      ids.has(work.work_id) ||
      !Number.isInteger(work.concept_assignment_count) ||
      (work.concept_assignment_count as number) < 0 ||
      !Number.isInteger(work.missing_centrality_scale_count) ||
      (work.missing_centrality_scale_count as number) < 0 ||
      (work.missing_centrality_scale_count as number) >
        (work.concept_assignment_count as number) ||
      typeof work.missing_centrality_scale_fraction !== "number" ||
      !Number.isFinite(work.missing_centrality_scale_fraction) ||
      work.missing_centrality_scale_fraction !==
        ((work.concept_assignment_count as number) === 0
          ? 0
          : (work.missing_centrality_scale_count as number) /
            (work.concept_assignment_count as number)) ||
      work.semantic_review_missing !==
        ((work.missing_centrality_scale_count as number) > 0)
    ) {
      return false;
    }
    ids.add(work.work_id);
    assignmentCount += work.concept_assignment_count as number;
    missingCount += work.missing_centrality_scale_count as number;
  }
  return (
    coverage.centrality_scale_scope === "work_concept_assignment" &&
    coverage.concept_assignment_count === assignmentCount &&
    coverage.missing_centrality_scale_count === missingCount &&
    coverage.missing_centrality_scale_fraction ===
      (assignmentCount === 0 ? 0 : missingCount / assignmentCount) &&
    coverage.none_is_missing_semantic_review === true &&
    coverage.none_numeric_compatibility_fallback ===
      "stored_centrality_unchanged" &&
    coverage.fallback_is_proof_of_numeric_calibration === false &&
    coverage.centrality_scale_inferred === false &&
    coverage.canonical_values_written === false
  );
}

export async function loadCatalog(): Promise<Catalog> {
  const response = await fetch(`${import.meta.env.BASE_URL}data/catalog.json`, {
    cache: "no-store",
  });
  if (!response.ok) throw new Error(`Catalog load failed (${response.status})`);
  const value: unknown = await response.json();
  if (!isCatalog(value)) throw new Error("Unsupported or invalid catalog.json");
  return value;
}

export async function loadResearch(): Promise<ResearchData> {
  const response = await fetch(`${import.meta.env.BASE_URL}data/research.json`, {
    cache: "no-store",
  });
  if (!response.ok) throw new Error(`Research data load failed (${response.status})`);
  const value: unknown = await response.json();
  if (!isResearchData(value)) throw new Error("Unsupported or invalid research.json");
  return value;
}

export function buildDomain(catalog: Catalog): Domain {
  const agents = [...catalog.agents].sort(
    (left, right) =>
      left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
  );
  const agentById = new Map(agents.map((agent) => [agent.id, agent]));
  const works = [...catalog.works].sort(
    (left, right) =>
      (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
        (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
      left.label.localeCompare(right.label) ||
      left.id.localeCompare(right.id),
  );

  const conceptCounts = new Map<string, { id: string; label: string; count: number }>();
  const conceptById = new Map<string, ConceptAssignment>();
  const mediumCounts = new Map<string, number>();

  for (const work of works) {
    mediumCounts.set(work.medium, (mediumCounts.get(work.medium) ?? 0) + 1);
    for (const concept of work.concepts) {
      if (!conceptById.has(concept.id)) conceptById.set(concept.id, concept);
      const current = conceptCounts.get(concept.id);
      if (current) current.count += 1;
      else conceptCounts.set(concept.id, { id: concept.id, label: concept.label, count: 1 });
    }
  }

  const workIds = new Set(works.map((work) => work.id));

  return {
    agents,
    agentById,
    works,
    workById: new Map(works.map((work) => [work.id, work])),
    conceptById,
    workRelations: (catalog.workRelations ?? []).filter(
      (relation) =>
        workIds.has(relation.subjectId) &&
        workIds.has(relation.objectId) &&
        relation.subjectId !== relation.objectId,
    ),
    workMemberships: catalog.workMemberships.filter(
      (membership) => workIds.has(membership.childId) && workIds.has(membership.parentId),
    ),
    agentRelations: catalog.agentRelations.filter(
      (relation) =>
        agentById.has(relation.subjectId) &&
        agentById.has(relation.objectId) &&
        relation.subjectId !== relation.objectId,
    ),
    conceptOptions: [...conceptCounts.values()].sort(
      (left, right) => left.label.localeCompare(right.label) || left.id.localeCompare(right.id),
    ),
    mediumOptions: [...mediumCounts.entries()]
      .map(([value, count]) => ({ value, count }))
      .sort((left, right) => left.value.localeCompare(right.value)),
  };
}
