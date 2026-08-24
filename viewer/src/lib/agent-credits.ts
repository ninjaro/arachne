import type { Contributor, EntityId, Manifestation, Work } from "./types";

export interface ManifestationCreditReference {
  work: Work;
  manifestation: Manifestation;
  contributor: Contributor;
}

/** Preserve the release/edition target when showing an agent's credits. */
export function manifestationCreditsForAgent(
  works: readonly Work[],
  agentId: EntityId,
): ManifestationCreditReference[] {
  return works.flatMap((work) =>
    work.manifestations.flatMap((manifestation) =>
      manifestation.contributors
        .filter((contributor) => contributor.id === agentId)
        .map((contributor) => ({ work, manifestation, contributor })),
    ),
  );
}
