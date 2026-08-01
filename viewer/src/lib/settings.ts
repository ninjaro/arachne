import type { Settings } from "./types";

export const DEFAULT_SETTINGS: Settings = {
  recommendation: {
    likeWeight: 1,
    dislikeWeight: 1.5,
    limit: 100,
  },
  features: {
    directConceptMultiplier: 1,
    creatorMultiplier: 0.55,
    directorMultiplier: 0.5,
    authorMultiplier: 0.55,
    producerMultiplier: 0.3,
    performerMultiplier: 0.25,
    organizationMultiplier: 0.2,
    contentGuideMultiplier: 0.25,
  },
  islands: {
    maxRecommendationNodes: 120,
    maxInferredNeighborsPerNode: 6,
    maxEdges: 400,
    minimumSimilarity: 0.12,
  },
  browse: {
    defaultPageSize: 50,
    pageSizeOptions: [25, 50, 100],
  },
};
