export type EntityId = string;
export type RatingValue = -1 | 1;
export type Ratings = Record<EntityId, RatingValue>;
export type RatingFamily = "work" | "agent" | "concept";
export type CentralityScale = "none" | "binary" | "ordinal" | "graded";
export type WorkMedium =
  | "film"
  | "short_film"
  | "television"
  | "novel"
  | "novella"
  | "short_story"
  | "poetry"
  | "play"
  | "essay"
  | "album"
  | "single"
  | "composition"
  | "painting"
  | "print"
  | "engraving"
  | "drawing"
  | "sculpture"
  | "installation"
  | "photography"
  | "mixed_media"
  | "nonfiction"
  | "comic"
  | "performance";
export type DatePrecision =
  | "year"
  | "month"
  | "exact"
  | "decade"
  | "approximate"
  | "range";
export type AgentType = "person" | "organization" | "group";
export type ManifestationType =
  | "edition"
  | "translation"
  | "release"
  | "pressing"
  | "cut"
  | "restoration"
  | "reissue";
export type CreditRole =
  | "author"
  | "director"
  | "screenwriter"
  | "producer"
  | "actor"
  | "composer"
  | "performer"
  | "artist"
  | "engraver"
  | "sculptor"
  | "photographer"
  | "editor"
  | "cinematographer"
  | "production_company"
  | "publisher"
  | "record_label"
  | "band"
  | "distributor"
  | "broadcaster"
  | "platform"
  | "translator"
  | "illustrator"
  | "printer"
  | "curator"
  | "choreographer"
  | "narrator"
  | "lyricist"
  | "songwriter"
  | "arranger"
  | "sound_engineer"
  | "designer"
  | "animator";
export type CreditImportance = "primary" | "key" | "supporting";
export type ConceptType =
  | "genre"
  | "style"
  | "theme"
  | "keyword"
  | "motif"
  | "trope"
  | "phobia"
  | "taboo"
  | "technique"
  | "movement"
  | "setting"
  | "mood"
  | "content_warning";
export type WorkConceptRelationType =
  | "exemplifies"
  | "contains"
  | "anticipates"
  | "influenced_by"
  | "influences"
  | "revives"
  | "parodies"
  | "deconstructs"
  | "associated_with";
export type HistoricalRole =
  | "formative"
  | "canonical"
  | "transitional"
  | "hybrid"
  | "revival"
  | "late_derivative"
  | "peripheral"
  | "precursor";

export interface EntityOpenContext {
  kind: "recommendation";
  title: string;
  details: string[];
}

export interface ExplicitRating {
  family: RatingFamily;
  value: RatingValue;
}

export interface LocalTasteProfile {
  formatVersion: 2;
  ratings: Record<EntityId, ExplicitRating>;
}

export interface ConceptAssignment {
  id: EntityId;
  label: string;
  conceptType: ConceptType;
  slug: string;
  relationType: WorkConceptRelationType;
  centrality: number | null;
  centralityScale: CentralityScale;
  historicalRole: HistoricalRole | null;
  confidence: number | null;
}

export interface Agent {
  id: EntityId;
  label: string;
  agentType: AgentType;
  identifiers: Identifier[];
}

export interface Contributor extends Agent {
  role: CreditRole;
  order: number | null;
  importance: CreditImportance;
  creditedAs: string | null;
}

export interface Advisory {
  id: string;
  conceptId: EntityId;
  label: string;
  category:
    | "violence"
    | "sex_nudity"
    | "language"
    | "drugs"
    | "frightening"
    | "self_harm"
    | "discrimination"
    | "abuse"
    | "taboo";
  intensity: number | null;
  explicitness: number | null;
  frequency: number | null;
  centrality: number | null;
  realism: number | null;
  spoilerLevel: "none" | "mild" | "major" | null;
  confidence: number | null;
}

export interface Measurement {
  type: "duration" | "height" | "width" | "depth" | "pages";
  value: number;
  unit: "seconds" | "millimetres" | "pages" | null;
  qualifier: string | null;
}

export interface Identifier {
  scheme: string;
  value: string;
  url: string | null;
}

export interface Manifestation {
  id: EntityId;
  type: ManifestationType;
  releaseYear: number | null;
  regionCode: string | null;
  languageCode: string | null;
  label: string | null;
  contributors: Contributor[];
  events: ProductEvent[];
}

export interface ProductEvent {
  id: string;
  entityId: EntityId;
  eventType:
    | "created"
    | "published"
    | "released"
    | "premiered"
    | "broadcast"
    | "performed"
    | "exhibited"
    | "recorded";
  yearStart: number | null;
  yearEnd: number | null;
  dateText: string | null;
  datePrecision: DatePrecision | null;
  placeText: string | null;
}

export interface FinancialFact {
  type: "budget";
  amountMin: number | null;
  amountMax: number | null;
  currencyCode: string | null;
  valueYear: number | null;
  isEstimate: boolean;
  confidence: number | null;
}

export interface Work {
  id: EntityId;
  label: string;
  medium: WorkMedium;
  yearStart: number | null;
  yearEnd: number | null;
  datePrecision: DatePrecision | null;
  dateStartText: string | null;
  dateEndText: string | null;
  dateQualifier: string | null;
  languageCode: string | null;
  countryCode: string | null;
  productionInfo: unknown;
  concepts: ConceptAssignment[];
  conceptAssignmentCount: number;
  missingCentralityScaleCount: number;
  missingCentralityScaleFraction: number;
  contributors: Contributor[];
  events: ProductEvent[];
  advisories: Advisory[];
  measurements: Measurement[];
  identifiers: Identifier[];
  manifestations: Manifestation[];
  financialFacts: FinancialFact[];
}

export interface WorkRelation {
  subjectId: EntityId;
  objectId: EntityId;
  relationType: string;
}

export interface WorkMembership {
  id: string;
  childId: EntityId;
  parentId: EntityId;
  membershipType:
    | "episode_of"
    | "season_of"
    | "track_of"
    | "volume_of"
    | "issue_of"
    | "chapter_of"
    | "part_of"
    | "collected_in";
  position: number | null;
  positionText: string | null;
}

export interface AgentRelation {
  id: string;
  subjectId: EntityId;
  objectId: EntityId;
  relationType:
    | "member_of"
    | "founder_of"
    | "subsidiary_of"
    | "division_of"
    | "imprint_of"
    | "owned_by"
    | "successor_of"
    | "predecessor_of";
  fromYear: number | null;
  toYear: number | null;
  periodText: string | null;
  roleText: string | null;
}

export interface Catalog {
  formatVersion: 1;
  productSnapshotId: string;
  databaseSha256?: string;
  agents: Agent[];
  works: Work[];
  workRelations?: WorkRelation[];
  workMemberships: WorkMembership[];
  agentRelations: AgentRelation[];
  events: ProductEvent[];
}

export interface Domain {
  agents: Agent[];
  agentById: Map<EntityId, Agent>;
  works: Work[];
  workById: Map<EntityId, Work>;
  conceptById: Map<EntityId, ConceptAssignment>;
  workRelations: WorkRelation[];
  workMemberships: WorkMembership[];
  agentRelations: AgentRelation[];
  conceptOptions: Array<{ id: EntityId; label: string; count: number }>;
  mediumOptions: Array<{ value: string; count: number }>;
}

export type ResearchKind = "quality_gap" | "ingest_issue" | "merge_hint";
export type ResearchSeverity = "info" | "weak" | "problem";

export interface ResearchItem {
  id: string;
  kind: ResearchKind;
  severity: ResearchSeverity;
  category: string;
  title: string;
  message: string;
  workId?: EntityId;
  workLabel?: string;
  score?: number;
  conceptAssignmentCount?: number;
  missingCentralityScaleCount?: number;
  missingCentralityScaleFraction?: number;
  details?: string[];
  batchId?: string;
  jsonPath?: string;
  value?: unknown;
  entityType?: "agent" | "work" | "concept";
  leftId?: EntityId;
  leftLabel?: string;
  rightId?: EntityId;
  rightLabel?: string;
  similarityScore?: number;
  textScore?: number | null;
  graphScore?: number | null;
  contextScore?: number | null;
  signals?: unknown;
}

export interface ResearchSummary {
  total: number;
  qualityGaps: number;
  ingestIssues: number;
  mergeHints: number;
  problems: number;
  weak: number;
  info: number;
}

export interface WorkCentralityScaleCoverage {
  work_id: EntityId;
  concept_assignment_count: number;
  missing_centrality_scale_count: number;
  missing_centrality_scale_fraction: number;
  semantic_review_missing: boolean;
}

export interface CentralityScaleCoverage {
  centrality_scale_scope: "work_concept_assignment";
  concept_assignment_count: number;
  missing_centrality_scale_count: number;
  missing_centrality_scale_fraction: number;
  none_is_missing_semantic_review: true;
  none_numeric_compatibility_fallback: "stored_centrality_unchanged";
  fallback_is_proof_of_numeric_calibration: false;
  centrality_scale_inferred: false;
  canonical_values_written: false;
  works: WorkCentralityScaleCoverage[];
}

export interface ResearchData {
  artifact_type: "product_research_report_v1";
  format_version: 1;
  product_snapshot: {
    snapshot_id: string;
    sha256: string;
  };
  formatVersion: 1;
  productSnapshotId: string;
  centrality_scale_coverage: CentralityScaleCoverage;
  summary: ResearchSummary;
  items: ResearchItem[];
}

export interface RecommendationSettings {
  likeWeight: number;
  dislikeWeight: number;
  limit: number;
}

export interface FeatureSettings {
  directConceptMultiplier: number;
  creatorMultiplier: number;
  directorMultiplier: number;
  authorMultiplier: number;
  producerMultiplier: number;
  performerMultiplier: number;
  organizationMultiplier: number;
  contentGuideMultiplier: number;
}

export interface IslandsSettings {
  maxRecommendationNodes: number;
  maxInferredNeighborsPerNode: number;
  maxEdges: number;
  minimumSimilarity: number;
}

export interface BrowseSettings {
  defaultPageSize: number;
  pageSizeOptions: number[];
}

export interface Settings {
  recommendation: RecommendationSettings;
  features: FeatureSettings;
  islands: IslandsSettings;
  browse: BrowseSettings;
}
