export type EntityId = string;
export type RatingValue = -1 | 1;
export type Ratings = Record<EntityId, RatingValue>;
export type RatingFamily = "work" | "agent" | "concept";

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
  conceptType: string;
  slug: string;
  relationType: string;
  centrality: number | null;
  historicalRole: string | null;
  confidence: number | null;
}

export interface Agent {
  id: EntityId;
  label: string;
  agentType: string;
  identifiers: Identifier[];
}

export interface Contributor extends Agent {
  role: string;
  order: number | null;
  importance: string;
  creditedAs: string | null;
}

export interface Advisory {
  id: string;
  conceptId: EntityId;
  label: string;
  category: string;
  intensity: number | null;
  explicitness: number | null;
  frequency: number | null;
  centrality: number | null;
  realism: number | null;
  spoilerLevel: string | null;
  confidence: number | null;
}

export interface Measurement {
  type: string;
  value: number;
  unit: string | null;
  qualifier: string | null;
}

export interface Identifier {
  scheme: string;
  value: string;
  url: string | null;
}

export interface Manifestation {
  id: EntityId;
  type: string;
  releaseYear: number | null;
  regionCode: string | null;
  languageCode: string | null;
  label: string | null;
}

export interface FinancialFact {
  type: string;
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
  medium: string;
  yearStart: number | null;
  yearEnd: number | null;
  datePrecision: string | null;
  dateStartText: string | null;
  dateEndText: string | null;
  dateQualifier: string | null;
  languageCode: string | null;
  countryCode: string | null;
  productionInfo: unknown;
  concepts: ConceptAssignment[];
  contributors: Contributor[];
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

export interface Catalog {
  formatVersion: 1;
  productSnapshotId: string;
  databaseSha256?: string;
  databaseUserVersion?: number;
  agents: Agent[];
  works: Work[];
  workRelations?: WorkRelation[];
}

export interface Domain {
  agents: Agent[];
  agentById: Map<EntityId, Agent>;
  works: Work[];
  workById: Map<EntityId, Work>;
  conceptById: Map<EntityId, ConceptAssignment>;
  workRelations: WorkRelation[];
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

export interface ResearchData {
  artifact_type: "product_research_report_v1";
  format_version: 1;
  product_snapshot: {
    snapshot_id: string;
    sha256: string;
  };
  formatVersion: 1;
  productSnapshotId: string;
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
