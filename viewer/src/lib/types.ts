export type EntityId = string;
export type RatingValue = -1 | 1;
export type Ratings = Record<EntityId, RatingValue>;

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

export interface Contributor {
  id: EntityId;
  label: string;
  agentType: string;
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

export interface RemoteAsset {
  provider: string;
  remoteKey: string | null;
  directUrl: string | null;
  resolverRule: string | null;
  rightsNote: string | null;
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
  assets: RemoteAsset[];
  manifestations: Manifestation[];
  financialFacts: FinancialFact[];
}

export interface Catalog {
  formatVersion: 1;
  productSnapshotId: string;
  databaseSha256?: string;
  databaseUserVersion?: number;
  works: Work[];
}

export interface Domain {
  works: Work[];
  workById: Map<EntityId, Work>;
  conceptOptions: Array<{ id: EntityId; label: string; count: number }>;
  mediumOptions: Array<{ value: string; count: number }>;
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

export interface EvolutionSettings {
  visibleChildrenPerNode: number;
  maxInitialRoots: number;
  minimumSimilarity: number;
  minimumSharedFeatures: number;
  kindMismatchFactor: number;
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
  evolution: EvolutionSettings;
  islands: IslandsSettings;
  browse: BrowseSettings;
}
