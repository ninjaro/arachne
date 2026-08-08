import type { EntityId } from "./types";
import {
  segmentDisplayStrength,
  tagStrengthBand,
  trajectorySegmentWidth,
  type TagTrajectorySegment,
} from "./evolution-strength";

/**
 * Width changes below a quarter CSS pixel are not materially distinguishable
 * in the dense scene. Semantic strength bands are still signed separately.
 */
export const BUNDLE_STRENGTH_WIDTH_QUANTUM = 0.25;

/** User-facing explanation kept beside the structural tolerance it describes. */
export const BUNDLE_EQUIVALENCE_REASON =
  `Equivalent visible station sequence, branch behavior, origins, endpoints, ` +
  `strength band, and materially equivalent rendered-width profile (within ${BUNDLE_STRENGTH_WIDTH_QUANTUM}px)`;

export interface StructuralTrajectoryEntry {
  tagId: EntityId;
  label: string;
  stationIds: readonly string[];
  /** Accepted temporal stop identity aligned with stationIds. */
  temporalGroupIds?: readonly string[];
  /** One normalized value per station; null remains structurally distinct. */
  strengthProfile: readonly (number | null)[];
  /** Optional caller-supplied branch behavior at each station. */
  branchProfile?: readonly string[];
  originBehavior?: string;
  terminationBehavior?: string;
  seed?: boolean;
  selected?: boolean;
  provenanceRequired?: boolean;
  expanded?: boolean;
}

export interface BundledSegment
  extends Omit<TagTrajectorySegment, "tagId"> {
  tagIds: EntityId[];
}

export interface TagTrajectoryBundle {
  id: string;
  kind: "bundle";
  tagIds: EntityId[];
  stationIds: string[];
  segments: BundledSegment[];
  entries: StructuralTrajectoryEntry[];
  reason: "equivalent-visible-structure";
}

export type UnbundledTrajectoryReason =
  | "seed"
  | "selected"
  | "provenance-required"
  | "explicitly-expanded"
  | "no-equivalent-trajectory";

export interface UnbundledTrajectory {
  id: string;
  kind: "singleton";
  tagIds: [EntityId];
  stationIds: string[];
  segments: TagTrajectorySegment[];
  entries: [StructuralTrajectoryEntry];
  reason: UnbundledTrajectoryReason;
}

export type TagTrajectoryGroup = TagTrajectoryBundle | UnbundledTrajectory;

export interface TagTrajectoryBundleResult {
  groups: TagTrajectoryGroup[];
  bundles: TagTrajectoryBundle[];
  singletons: UnbundledTrajectory[];
  groupByTagId: Map<EntityId, TagTrajectoryGroup>;
}

export interface UniqueTagLabelInput {
  tagId: EntityId;
  label: string;
  strength?: number | null;
}

export interface UniqueTagLabelGroup {
  normalizedLabel: string;
  label: string;
  tagIds: EntityId[];
  conceptRecordCount: number;
  strongestStrength: number | null;
}

export interface StrongestTagSummary {
  tagId: EntityId;
  label: string;
  strength: number | null;
}

function compareText(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

function cleanLabel(value: string): string {
  return value.trim().replace(/\s+/g, " ");
}

function normalizedLabel(value: string): string {
  // Locale-independent case folding keeps the same projection stable across
  // browsers whose host locale differs from the data producer's locale.
  return cleanLabel(value).toLowerCase();
}

function compareDisplayLabels(left: string, right: string): number {
  const cleanLeft = cleanLabel(left);
  const cleanRight = cleanLabel(right);
  const leftIsNormalized = cleanLeft === normalizedLabel(cleanLeft) ? 0 : 1;
  const rightIsNormalized = cleanRight === normalizedLabel(cleanRight) ? 0 : 1;
  return leftIsNormalized - rightIsNormalized || compareText(cleanLeft, cleanRight);
}

function finiteProfileValue(value: number | null): number | null {
  if (value === null || !Number.isFinite(value)) return null;
  return Math.max(0, Math.min(1, value));
}

function visualStrengthToken(value: number | null): string {
  const normalized = finiteProfileValue(value);
  if (normalized === null) return "unknown";
  const widthStep = Math.round(
    trajectorySegmentWidth(normalized) / BUNDLE_STRENGTH_WIDTH_QUANTUM,
  );
  return `${tagStrengthBand(normalized)}:${widthStep}`;
}

function encodedProfile(values: readonly (number | null)[]): string {
  return values
    .map(visualStrengthToken)
    .join(",");
}

function defaultBranchProfile(stationIds: readonly string[]): string[] {
  return stationIds.map((stationId, index) => {
    const previous = stationIds[index - 1] ?? "origin";
    const next = stationIds[index + 1] ?? "termination";
    return `${previous}>${stationId}>${next}`;
  });
}

function validatedEntry(entry: StructuralTrajectoryEntry): StructuralTrajectoryEntry {
  if (entry.stationIds.length !== entry.strengthProfile.length) {
    throw new Error(
      `Trajectory ${entry.tagId} has ${entry.stationIds.length} stops but ${entry.strengthProfile.length} strengths`,
    );
  }
  if (entry.branchProfile && entry.branchProfile.length !== entry.stationIds.length) {
    throw new Error(
      `Trajectory ${entry.tagId} has ${entry.stationIds.length} stops but ${entry.branchProfile.length} branch values`,
    );
  }
  if (
    entry.temporalGroupIds &&
    entry.temporalGroupIds.length !== entry.stationIds.length
  ) {
    throw new Error(
      `Trajectory ${entry.tagId} has ${entry.stationIds.length} stops but ${entry.temporalGroupIds.length} temporal groups`,
    );
  }
  return {
    ...entry,
    stationIds: [...entry.stationIds],
    strengthProfile: entry.strengthProfile.map(finiteProfileValue),
    branchProfile: [...(entry.branchProfile ?? defaultBranchProfile(entry.stationIds))],
    temporalGroupIds: entry.temporalGroupIds
      ? [...entry.temporalGroupIds]
      : entry.stationIds.map((_stationId, index) => `station:${index}`),
  };
}

function structuralSignature(entry: StructuralTrajectoryEntry): string {
  const first = entry.stationIds[0] ?? "empty";
  const last = entry.stationIds.at(-1) ?? "empty";
  return JSON.stringify([
    entry.stationIds,
    entry.temporalGroupIds,
    entry.branchProfile,
    entry.originBehavior ?? `origin:${first}`,
    entry.terminationBehavior ?? `termination:${last}`,
    encodedProfile(entry.strengthProfile),
  ]);
}

function stableHash(value: string): string {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0).toString(36);
}

function strengthMap(entry: StructuralTrajectoryEntry): Map<string, number | null> {
  return new Map(
    entry.stationIds.map((stationId, index) => [
      stationId,
      entry.strengthProfile[index] ?? null,
    ]),
  );
}

function structuralTrajectorySegments(
  tagId: EntityId,
  entry: StructuralTrajectoryEntry,
  strengths: ReadonlyMap<string, number | null>,
): TagTrajectorySegment[] {
  const groups: Array<{ id: string; stationIds: string[] }> = [];
  const groupById = new Map<string, { id: string; stationIds: string[] }>();
  entry.stationIds.forEach((stationId, index) => {
    const id = entry.temporalGroupIds?.[index] ?? `station:${index}`;
    let group = groupById.get(id);
    if (!group) {
      group = { id, stationIds: [] };
      groupById.set(id, group);
      groups.push(group);
    }
    group.stationIds.push(stationId);
  });
  const segments: TagTrajectorySegment[] = [];
  for (let groupIndex = 1; groupIndex < groups.length; groupIndex += 1) {
    for (const sourceStopId of groups[groupIndex - 1]!.stationIds) {
      for (const targetStopId of groups[groupIndex]!.stationIds) {
        const sourceStrength = finiteProfileValue(
          strengths.get(sourceStopId) ?? null,
        );
        const targetStrength = finiteProfileValue(
          strengths.get(targetStopId) ?? null,
        );
        segments.push({
          tagId,
          sourceStopId,
          targetStopId,
          sourceStrength,
          targetStrength,
          displayStrength: segmentDisplayStrength(
            sourceStrength,
            targetStrength,
          ),
        });
      }
    }
  }
  return segments;
}

function bundleStrengthMap(
  entries: readonly StructuralTrajectoryEntry[],
): Map<string, number | null> {
  const representative = entries[0]!;
  return new Map(
    representative.stationIds.map((stationId, index) => {
      const known = entries
        .map((entry) => finiteProfileValue(entry.strengthProfile[index] ?? null))
        .filter((value): value is number => value !== null);
      return [stationId, known.length ? Math.max(...known) : null];
    }),
  );
}

function exemptionReason(
  entry: StructuralTrajectoryEntry,
): Exclude<UnbundledTrajectoryReason, "no-equivalent-trajectory"> | null {
  if (entry.seed) return "seed";
  if (entry.selected) return "selected";
  if (entry.provenanceRequired) return "provenance-required";
  if (entry.expanded) return "explicitly-expanded";
  return null;
}

function singleton(
  entry: StructuralTrajectoryEntry,
  reason: UnbundledTrajectoryReason,
): UnbundledTrajectory {
  return {
    id: `trajectory:${encodeURIComponent(entry.tagId)}`,
    kind: "singleton",
    tagIds: [entry.tagId],
    stationIds: [...entry.stationIds],
    segments: structuralTrajectorySegments(
      entry.tagId,
      entry,
      strengthMap(entry),
    ),
    entries: [entry],
    reason,
  };
}

/**
 * Collapse only routes whose ordered stops, endpoint behavior, branch profile,
 * strength band, and quarter-pixel rendered width profile are equivalent.
 * Unknown remains distinct from zero/weak. Opted-out routes always remain
 * singleton groups; station selection alone is intentionally not an opt-out.
 */
export function buildTrajectoryBundles(
  sourceEntries: readonly StructuralTrajectoryEntry[],
): TagTrajectoryBundleResult {
  const entries = sourceEntries
    .map(validatedEntry)
    .sort((left, right) => compareText(left.tagId, right.tagId));
  const groupsBySignature = new Map<string, StructuralTrajectoryEntry[]>();
  const singletons: UnbundledTrajectory[] = [];

  for (const entry of entries) {
    const reason = exemptionReason(entry);
    if (reason) {
      singletons.push(singleton(entry, reason));
      continue;
    }
    const signature = structuralSignature(entry);
    const candidates = groupsBySignature.get(signature);
    if (candidates) candidates.push(entry);
    else groupsBySignature.set(signature, [entry]);
  }

  const bundles: TagTrajectoryBundle[] = [];
  for (const [signature, candidates] of [...groupsBySignature.entries()].sort(
    ([left], [right]) => compareText(left, right),
  )) {
    candidates.sort((left, right) => compareText(left.tagId, right.tagId));
    if (candidates.length === 1) {
      singletons.push(singleton(candidates[0]!, "no-equivalent-trajectory"));
      continue;
    }
    const representative = candidates[0]!;
    const tagIds = candidates.map((entry) => entry.tagId);
    const segments = structuralTrajectorySegments(
      representative.tagId,
      representative,
      bundleStrengthMap(candidates),
    ).map(({ tagId: _tagId, ...segment }): BundledSegment => ({
      ...segment,
      tagIds,
    }));
    bundles.push({
      id: `bundle:${stableHash(signature)}:${tagIds.map(encodeURIComponent).join("+")}`,
      kind: "bundle",
      tagIds,
      stationIds: [...representative.stationIds],
      segments,
      entries: candidates,
      reason: "equivalent-visible-structure",
    });
  }

  bundles.sort((left, right) => compareText(left.id, right.id));
  singletons.sort((left, right) => compareText(left.id, right.id));
  const groups: TagTrajectoryGroup[] = [...bundles, ...singletons].sort((left, right) =>
    compareText(left.id, right.id),
  );
  const groupByTagId = new Map<EntityId, TagTrajectoryGroup>();
  for (const group of groups) {
    for (const tagId of group.tagIds) groupByTagId.set(tagId, group);
  }
  return { groups, bundles, singletons, groupByTagId };
}

/** Group duplicate normalized labels while retaining every distinct tag ID. */
export function groupUniqueTagLabels(
  inputs: readonly UniqueTagLabelInput[],
): UniqueTagLabelGroup[] {
  const uniqueById = new Map<EntityId, UniqueTagLabelInput>();
  for (const input of inputs) {
    const strength = finiteProfileValue(input.strength ?? null);
    const existing = uniqueById.get(input.tagId);
    if (!existing) {
      uniqueById.set(input.tagId, { ...input, strength });
      continue;
    }
    const existingStrength = finiteProfileValue(existing.strength ?? null);
    const strongest =
      existingStrength === null
        ? strength
        : strength === null
          ? existingStrength
          : Math.max(existingStrength, strength);
    const labels = [cleanLabel(existing.label), cleanLabel(input.label)].sort(
      compareDisplayLabels,
    );
    uniqueById.set(input.tagId, {
      tagId: input.tagId,
      label: labels[0]!,
      strength: strongest,
    });
  }

  const grouped = new Map<string, UniqueTagLabelInput[]>();
  for (const input of uniqueById.values()) {
    const key = normalizedLabel(input.label);
    const values = grouped.get(key);
    if (values) values.push(input);
    else grouped.set(key, [input]);
  }
  return [...grouped.entries()]
    .map(([key, values]): UniqueTagLabelGroup => {
      values.sort((left, right) => compareText(left.tagId, right.tagId));
      const known = values
        .map((value) => finiteProfileValue(value.strength ?? null))
        .filter((value): value is number => value !== null);
      return {
        normalizedLabel: key,
        label: values
          .map((value) => cleanLabel(value.label))
          .sort(compareDisplayLabels)[0]!,
        tagIds: values.map((value) => value.tagId),
        conceptRecordCount: values.length,
        strongestStrength: known.length ? Math.max(...known) : null,
      };
    })
    .sort(
      (left, right) =>
        compareText(left.normalizedLabel, right.normalizedLabel) ||
        compareText(left.tagIds[0]!, right.tagIds[0]!),
    );
}

/** Return unique tags ordered by strongest known membership, then identity. */
export function strongestTagSummaries(
  inputs: readonly UniqueTagLabelInput[],
  limit = 3,
): StrongestTagSummary[] {
  const unique = new Map<EntityId, StrongestTagSummary>();
  for (const input of inputs) {
    const strength = finiteProfileValue(input.strength ?? null);
    const current = unique.get(input.tagId);
    if (!current) {
      unique.set(input.tagId, { tagId: input.tagId, label: input.label, strength });
      continue;
    }
    const strongest =
      current.strength === null
        ? strength
        : strength === null
          ? current.strength
          : Math.max(current.strength, strength);
    unique.set(input.tagId, {
      tagId: input.tagId,
      label: [current.label, input.label].sort(compareDisplayLabels)[0]!,
      strength: strongest,
    });
  }
  return [...unique.values()]
    .sort((left, right) => {
      if (left.strength === null && right.strength !== null) return 1;
      if (left.strength !== null && right.strength === null) return -1;
      return (
        (right.strength ?? -1) - (left.strength ?? -1) ||
        compareText(normalizedLabel(left.label), normalizedLabel(right.label)) ||
        compareText(left.label, right.label) ||
        compareText(left.tagId, right.tagId)
      );
    })
    .slice(0, Math.max(0, Math.trunc(limit)));
}
