export type RatingValue = -1 | 1;
export type Ratings = Record<string, RatingValue>;

export interface ProjectionNode {
  node_id: string;
  node_type: string;
  label: string;
  graph_domain: "product" | "candidate";
  provenance?: Record<string, unknown>;
  attributes?: Record<string, unknown>;
}

export interface ProjectionEdge {
  edge_id: string;
  source: string;
  target: string;
  edge_type: string;
  provenance?: Record<string, unknown>;
  attributes?: Record<string, unknown>;
}

export interface Projection {
  artifact_type: "viewer_projection_data_v1";
  format_version: 1;
  projection_id: string;
  projection_version: string;
  product_snapshot_id: string;
  candidate_snapshot_id: string;
  nodes: ProjectionNode[];
  edges: ProjectionEdge[];
}

export interface ConceptRef {
  id: string;
  label: string;
  relation: string;
}

export interface ContributorRef {
  id: string;
  label: string;
  role: string;
}

export interface Work {
  id: string;
  label: string;
  medium: string;
  year: number | null;
  concepts: ConceptRef[];
  contributors: ContributorRef[];
}

export interface Domain {
  works: Work[];
  workById: Map<string, Work>;
  nodeById: Map<string, ProjectionNode>;
}
