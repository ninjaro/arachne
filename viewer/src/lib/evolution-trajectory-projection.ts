import { compareEvolutionDates } from "./evolution-date";
import type {
  VisibleAggregateRelation,
  VisibleEvolution,
  VisibleEvolutionTag,
} from "./evolution";
import type { EntityId } from "./types";
import {
  buildTrajectoryBundles,
  type StructuralTrajectoryEntry,
  type TagTrajectoryBundleResult,
  type TagTrajectoryGroup,
} from "./trajectory-bundles";

export interface EvolutionTrajectoryProjectionOptions {
  selectedTagId?: EntityId | null;
  expandedBundleIds?: Iterable<string>;
  expandedTagIds?: Iterable<EntityId>;
  provenanceRequiredTagIds?: Iterable<EntityId>;
  /** Optional test/performance diagnostic; never affects projection output. */
  onBundleProjectionPass?: () => void;
}

export interface EvolutionTrajectoryProjection
  extends TagTrajectoryBundleResult {
  entries: StructuralTrajectoryEntry[];
  groupsByStationId: Map<string, TagTrajectoryGroup[]>;
  groupsByRelationKey: Map<string, TagTrajectoryGroup[]>;
  appliedExpandedTagIds: EntityId[];
}

interface RouteIncidence {
  tagId: EntityId;
  previousStationId: string | null;
  nextStationId: string | null;
}

function stableUnique(values: Iterable<string>): string[] {
  return [...new Set(values)].sort((left, right) => left.localeCompare(right));
}

function orderedStationIds(
  visible: VisibleEvolution,
  tag: VisibleEvolutionTag,
): string[] {
  return [...new Set(tag.stationIds)]
    .filter((stationId) => visible.stationById.has(stationId))
    .sort((leftId, rightId) => {
      const left = visible.stationById.get(leftId)!;
      const right = visible.stationById.get(rightId)!;
      return (
        compareEvolutionDates(left.temporal, right.temporal) ||
        leftId.localeCompare(rightId)
      );
    });
}

function routeIncidenceByStation(
  routesByTagId: ReadonlyMap<EntityId, readonly string[]>,
): Map<string, RouteIncidence[]> {
  const result = new Map<string, RouteIncidence[]>();
  for (const [tagId, stationIds] of [...routesByTagId.entries()].sort(
    ([left], [right]) => left.localeCompare(right),
  )) {
    stationIds.forEach((stationId, index) => {
      const incidence: RouteIncidence = {
        tagId,
        previousStationId: stationIds[index - 1] ?? null,
        nextStationId: stationIds[index + 1] ?? null,
      };
      const atStation = result.get(stationId);
      if (atStation) atStation.push(incidence);
      else result.set(stationId, [incidence]);
    });
  }
  for (const incidences of result.values()) {
    incidences.sort((left, right) => left.tagId.localeCompare(right.tagId));
  }
  return result;
}

/**
 * Describe both the current tag's turn and all visible route incidence at the
 * station. The latter makes a branch point structural rather than dependent on
 * whichever trajectory happened to be iterated first.
 */
function branchProfileAtStation(
  visible: VisibleEvolution,
  incidenceByStationId: ReadonlyMap<string, readonly RouteIncidence[]>,
  tagId: EntityId,
  stationId: string,
): string {
  const incidences = incidenceByStationId.get(stationId) ?? [];
  const own = incidences.find((incidence) => incidence.tagId === tagId);
  const station = visible.stationById.get(stationId);
  const stationTagIds = stableUnique(station?.visibleTagIds ?? []);
  const incomingStationIds = stableUnique(
    incidences.flatMap((incidence) =>
      incidence.previousStationId ? [incidence.previousStationId] : [],
    ),
  );
  const outgoingStationIds = stableUnique(
    incidences.flatMap((incidence) =>
      incidence.nextStationId ? [incidence.nextStationId] : [],
    ),
  );
  return JSON.stringify([
    ["self", own?.previousStationId ?? "origin", own?.nextStationId ?? "termination"],
    ["station-tags", stationTagIds],
    ["incoming", incomingStationIds],
    ["outgoing", outgoingStationIds],
    [
      "incidence",
      incidences.map((incidence) => [
        incidence.tagId,
        incidence.previousStationId ?? "origin",
        incidence.nextStationId ?? "termination",
      ]),
    ],
  ]);
}

function projectionEntries(
  visible: VisibleEvolution,
  selectedTagId: EntityId | null,
  expandedTagIds: ReadonlySet<EntityId>,
  provenanceRequiredTagIds: ReadonlySet<EntityId>,
): StructuralTrajectoryEntry[] {
  const orderedTags = visible.tags
    .slice()
    .sort((left, right) => left.tag.id.localeCompare(right.tag.id));
  const routesByTagId = new Map<EntityId, string[]>(
    orderedTags.map((tag) => [tag.tag.id, orderedStationIds(visible, tag)]),
  );
  const incidenceByStationId = routeIncidenceByStation(routesByTagId);
  const temporalGroupIdByTagStation = new Map<string, string>();
  for (const stop of visible.temporalTagStops) {
    for (const stationId of stop.stationIds) {
      temporalGroupIdByTagStation.set(
        `${stop.tagId}\u0000${stationId}`,
        stop.temporalGroupId,
      );
    }
  }

  return orderedTags.map((tag): StructuralTrajectoryEntry => {
    const tagId = tag.tag.id;
    const stationIds = routesByTagId.get(tagId)!;
    const strengthByStationId = new Map(
      (visible.aggregateMembershipsByTagId.get(tagId) ?? []).map((membership) => [
        membership.stationId,
        membership.strength,
      ]),
    );
    const originTargets = stableUnique(
      tag.origin.targetStationIds.filter((stationId) =>
        visible.stationById.has(stationId),
      ),
    );
    return {
      tagId,
      label: tag.tag.label,
      stationIds,
      temporalGroupIds: stationIds.map(
        (stationId) =>
          temporalGroupIdByTagStation.get(`${tagId}\u0000${stationId}`) ??
          `station:${stationId}`,
      ),
      strengthProfile: stationIds.map(
        (stationId) => strengthByStationId.get(stationId) ?? null,
      ),
      branchProfile: stationIds.map((stationId) =>
        branchProfileAtStation(visible, incidenceByStationId, tagId, stationId),
      ),
      originBehavior: JSON.stringify([
        "synthetic-origin",
        originTargets.length ? originTargets : stationIds.slice(0, 1),
      ]),
      terminationBehavior: JSON.stringify([
        "visible-termination",
        stationIds.at(-1) ?? null,
      ]),
      seed: tag.seed,
      selected: selectedTagId === tagId,
      provenanceRequired: provenanceRequiredTagIds.has(tagId),
      expanded: expandedTagIds.has(tagId),
    };
  });
}

function groupsByStation(
  groups: readonly TagTrajectoryGroup[],
): Map<string, TagTrajectoryGroup[]> {
  const result = new Map<string, TagTrajectoryGroup[]>();
  for (const group of groups) {
    for (const stationId of new Set(group.stationIds)) {
      const atStation = result.get(stationId);
      if (atStation) atStation.push(group);
      else result.set(stationId, [group]);
    }
  }
  for (const atStation of result.values()) {
    atStation.sort((left, right) => left.id.localeCompare(right.id));
  }
  return result;
}

function groupsForRelation(
  groups: readonly TagTrajectoryGroup[],
  relation: Pick<
    VisibleAggregateRelation,
    "sourceStationId" | "targetStationId"
  >,
): TagTrajectoryGroup[] {
  return groups
    .filter((group) => {
      const stationIds = new Set(group.stationIds);
      return (
        stationIds.has(relation.sourceStationId) &&
        stationIds.has(relation.targetStationId)
      );
    })
    .sort((left, right) => left.id.localeCompare(right.id));
}

/** Compose filtered Evolution data into stable structural trajectory groups. */
export function buildEvolutionTrajectoryProjection(
  visible: VisibleEvolution,
  options: EvolutionTrajectoryProjectionOptions = {},
): EvolutionTrajectoryProjection {
  const selectedTagId = options.selectedTagId ?? null;
  const explicitExpandedTagIds = new Set(options.expandedTagIds ?? []);
  const provenanceRequiredTagIds = new Set(
    options.provenanceRequiredTagIds ?? [],
  );
  const requestedBundleIds = new Set(options.expandedBundleIds ?? []);

  const provisionalEntries = projectionEntries(
    visible,
    selectedTagId,
    explicitExpandedTagIds,
    provenanceRequiredTagIds,
  );
  options.onBundleProjectionPass?.();
  const provisional = buildTrajectoryBundles(provisionalEntries);
  const appliedExpandedTagIds = new Set(explicitExpandedTagIds);
  for (const bundle of provisional.bundles) {
    if (!requestedBundleIds.has(bundle.id)) continue;
    for (const tagId of bundle.tagIds) appliedExpandedTagIds.add(tagId);
  }

  const needsExpandedProjection = [...appliedExpandedTagIds].some(
    (tagId) => !explicitExpandedTagIds.has(tagId),
  );
  const entries = needsExpandedProjection
    ? projectionEntries(
        visible,
        selectedTagId,
        appliedExpandedTagIds,
        provenanceRequiredTagIds,
      )
    : provisionalEntries;
  if (needsExpandedProjection) options.onBundleProjectionPass?.();
  const grouped = needsExpandedProjection
    ? buildTrajectoryBundles(entries)
    : provisional;
  const groupsByStationId = groupsByStation(grouped.groups);
  const groupsByRelationKey = new Map<string, TagTrajectoryGroup[]>();
  for (const relation of visible.aggregateRelations
    .slice()
    .sort((left, right) => left.key.localeCompare(right.key))) {
    groupsByRelationKey.set(
      relation.key,
      groupsForRelation(grouped.groups, relation),
    );
  }
  return {
    entries,
    ...grouped,
    groupsByStationId,
    groupsByRelationKey,
    appliedExpandedTagIds: [...appliedExpandedTagIds].sort(),
  };
}

export function trajectoryGroupsPassingThroughStation(
  projection: EvolutionTrajectoryProjection,
  stationId: string,
): TagTrajectoryGroup[] {
  return [...(projection.groupsByStationId.get(stationId) ?? [])];
}

export function trajectoryGroupsPassingThroughRelation(
  projection: EvolutionTrajectoryProjection,
  relation: Pick<
    VisibleAggregateRelation,
    "key" | "sourceStationId" | "targetStationId"
  >,
): TagTrajectoryGroup[] {
  const indexed = projection.groupsByRelationKey.get(relation.key);
  return indexed ? [...indexed] : groupsForRelation(projection.groups, relation);
}
