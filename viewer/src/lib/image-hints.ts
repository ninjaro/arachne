import type {
  ImageEntity,
  ImageKind,
  LocalImageHint,
  LocalImageHints,
} from "./image-providers";

const MAX_ARTIFACT_BYTES = 64 * 1024 * 1024;
const ARTIFACT_REQUEST_TIMEOUT_MS = 4_000;
const SHA256 = /^[0-9a-f]{64}$/;
const STABLE_ID = /^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$/;
const QID = /^Q[1-9]\d{0,19}$/;

export interface ImageHintProductIdentity {
  snapshotId?: string;
  contentSha256?: string;
  exportSha256?: string;
}

export interface ImageHintsForEntityRequest {
  entity: Pick<ImageEntity, "id" | "family" | "identifiers">;
  product: ImageHintProductIdentity;
  signal?: AbortSignal;
}

interface ValidatedImageHintsArtifact {
  product: {
    snapshotId: string;
    contentSha256: string;
    exportSha256: string;
  };
  entities: Map<
    string,
    { family: "work" | "agent"; images: LocalImageHint[] }
  >;
}

function record(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : null;
}

function exactKeys(
  value: Record<string, unknown>,
  expected: readonly string[],
): boolean {
  const actual = Object.keys(value).sort();
  const sortedExpected = [...expected].sort();
  return (
    actual.length === sortedExpected.length &&
    actual.every((key, index) => key === sortedExpected[index])
  );
}

function validStableId(value: unknown): value is string {
  return typeof value === "string" && STABLE_ID.test(value);
}

function validSha256(value: unknown): value is string {
  return typeof value === "string" && SHA256.test(value);
}

function validSourceSnapshot(value: unknown): boolean {
  const source = record(value);
  return Boolean(
    source &&
      exactKeys(source, ["snapshot_id", "storage_ref", "sha256"]) &&
      validStableId(source.snapshot_id) &&
      typeof source.storage_ref === "string" &&
      source.storage_ref.length > 0 &&
      validSha256(source.sha256),
  );
}

function productSnapshot(
  value: unknown,
): {
  snapshotId: string;
  contentSha256: string;
  exportSha256: string;
} | null {
  const product = record(value);
  if (
    !product ||
    !exactKeys(product, ["snapshot_id", "content_sha256", "export_sha256"]) ||
    !validStableId(product.snapshot_id) ||
    !validSha256(product.content_sha256) ||
    !validSha256(product.export_sha256)
  ) {
    return null;
  }
  return {
    snapshotId: product.snapshot_id,
    contentSha256: product.content_sha256,
    exportSha256: product.export_sha256,
  };
}

function matchesProductIdentity(
  actual: NonNullable<ReturnType<typeof productSnapshot>>,
  expected: ImageHintProductIdentity,
): boolean {
  const comparisons = [
    expected.snapshotId === undefined || expected.snapshotId === actual.snapshotId,
    expected.contentSha256 === undefined ||
      expected.contentSha256 === actual.contentSha256,
    expected.exportSha256 === undefined ||
      expected.exportSha256 === actual.exportSha256,
  ];
  return (
    Object.values(expected).some((value) => value !== undefined) &&
    comparisons.every(Boolean)
  );
}

function entityWikidataQids(
  entity: Pick<ImageEntity, "identifiers">,
): Set<string> {
  return new Set(
    entity.identifiers
      .filter((identifier) => identifier.scheme.trim().toLowerCase() === "wikidata")
      .map((identifier) => identifier.value.trim())
      .filter((qid) => QID.test(qid)),
  );
}

function validImageRecord(
  value: unknown,
  family: "work" | "agent",
): {
  file: string;
  kind: ImageKind;
  property: "P18" | "P154" | "P3383";
  rank: "preferred" | "normal";
  wikidataQid: string;
} | null {
  const image = record(value);
  if (
    !image ||
    !exactKeys(image, [
      "file",
      "kind",
      "property",
      "rank",
      "source",
      "wikidata_qid",
    ]) ||
    typeof image.file !== "string" ||
    image.file.length < 1 ||
    image.file.length > 512 ||
    image.source !== "wikimedia_commons" ||
    (image.rank !== "preferred" && image.rank !== "normal") ||
    (image.property !== "P18" &&
      image.property !== "P154" &&
      image.property !== "P3383") ||
    typeof image.wikidata_qid !== "string" ||
    !QID.test(image.wikidata_qid)
  ) {
    return null;
  }
  const expectedKinds: Record<string, ImageKind> =
    family === "work"
      ? { P18: "work_image", P3383: "work_poster" }
      : { P18: "agent_portrait", P154: "agent_logo" };
  const kind = expectedKinds[image.property];
  if (!kind || image.kind !== kind) return null;
  return {
    file: image.file,
    kind,
    property: image.property,
    rank: image.rank,
    wikidataQid: image.wikidata_qid,
  };
}

function validateImageHintsArtifact(
  value: unknown,
): ValidatedImageHintsArtifact | null {
  const root = record(value);
  if (
    !root ||
    !exactKeys(root, [
      "artifact_type",
      "format_version",
      "source_snapshot",
      "product_snapshot",
      "entities",
    ]) ||
    root.artifact_type !== "wikidata_image_hints_v1" ||
    root.format_version !== 1 ||
    !validSourceSnapshot(root.source_snapshot) ||
    !Array.isArray(root.entities)
  ) {
    return null;
  }
  const product = productSnapshot(root.product_snapshot);
  if (!product) return null;

  const entities = new Map<
    string,
    { family: "work" | "agent"; images: LocalImageHint[] }
  >();
  for (const value of root.entities) {
    const entity = record(value);
    if (
      !entity ||
      !exactKeys(entity, ["entity_id", "family", "images"]) ||
      !validStableId(entity.entity_id) ||
      (entity.family !== "work" && entity.family !== "agent") ||
      !Array.isArray(entity.images) ||
      entity.images.length < 1 ||
      entity.images.length > 3
    ) {
      return null;
    }
    const key = `${entity.entity_id}\u0000${entity.family}`;
    if (entities.has(key)) return null;
    const images: LocalImageHint[] = [];
    const seenFiles = new Set<string>();
    for (const imageValue of entity.images) {
      const image = validImageRecord(imageValue, entity.family);
      if (!image || seenFiles.has(image.file)) return null;
      seenFiles.add(image.file);
      images.push({
        file: image.file,
        kind: image.kind,
        property: image.property,
        rank: image.rank,
        source: "wikimedia_commons",
        wikidataQid: image.wikidataQid,
      });
    }
    entities.set(key, { family: entity.family, images });
  }
  return { product, entities };
}

function hintsForEntity(
  artifact: ValidatedImageHintsArtifact,
  request: ImageHintsForEntityRequest,
): LocalImageHints {
  if (!matchesProductIdentity(artifact.product, request.product)) return {};
  const wikidataQids = entityWikidataQids(request.entity);
  if (wikidataQids.size === 0) return {};
  const selected = artifact.entities.get(
    `${request.entity.id}\u0000${request.entity.family}`,
  );
  if (!selected || selected.family !== request.entity.family) return {};
  if (selected.images.some((image) => !wikidataQids.has(image.wikidataQid))) {
    return {};
  }
  return { [request.entity.id]: selected.images };
}

/**
 * Strictly validates the closed v1 artifact and returns hints only for the
 * currently opened canonical entity. Any mismatch fails closed to no hints.
 */
export function parseImageHintsForEntity(
  value: unknown,
  request: ImageHintsForEntityRequest,
): LocalImageHints {
  const artifact = validateImageHintsArtifact(value);
  return artifact ? hintsForEntity(artifact, request) : {};
}

const ARTIFACT_PROMISES = new WeakMap<
  typeof fetch,
  Map<string, Promise<ValidatedImageHintsArtifact | null>>
>();

function cachedArtifact(
  url: string,
  fetchImpl: typeof fetch,
): Promise<ValidatedImageHintsArtifact | null> {
  let byUrl = ARTIFACT_PROMISES.get(fetchImpl);
  if (!byUrl) {
    byUrl = new Map();
    ARTIFACT_PROMISES.set(fetchImpl, byUrl);
  }
  const existing = byUrl.get(url);
  if (existing) return existing;
  const pending = (async () => {
    const controller = new AbortController();
    const timeout = globalThis.setTimeout(
      () => controller.abort(),
      ARTIFACT_REQUEST_TIMEOUT_MS,
    );
    try {
      const response = await fetchImpl(url, {
        method: "GET",
        cache: "force-cache",
        credentials: "same-origin",
        redirect: "error",
        signal: controller.signal,
      });
      if (!response.ok) return null;
      const declaredLength = Number(response.headers.get("Content-Length"));
      if (Number.isFinite(declaredLength) && declaredLength > MAX_ARTIFACT_BYTES) {
        return null;
      }
      const body = await response.text();
      if (new TextEncoder().encode(body).byteLength > MAX_ARTIFACT_BYTES) {
        return null;
      }
      return validateImageHintsArtifact(JSON.parse(body) as unknown);
    } catch {
      return null;
    } finally {
      clearTimeout(timeout);
    }
  })();
  byUrl.set(url, pending);
  return pending;
}

/** Fetches the optional artifact on card opening; missing/invalid is normal. */
export async function loadImageHintsForEntity(
  url: string,
  request: ImageHintsForEntityRequest,
  fetchImpl: typeof fetch = globalThis.fetch,
): Promise<LocalImageHints> {
  try {
    if (
      request.signal?.aborted ||
      entityWikidataQids(request.entity).size === 0
    ) {
      return {};
    }
    const artifact = await cachedArtifact(url, fetchImpl);
    if (request.signal?.aborted || !artifact) return {};
    return hintsForEntity(artifact, request);
  } catch {
    return {};
  }
}
