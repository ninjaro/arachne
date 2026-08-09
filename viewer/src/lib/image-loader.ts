import {
  imageProviderDispatches,
  isAllowedHttpsUrl,
  validProviderCandidate,
  type ImageCandidate,
  type ImageEntity,
  type ImageProvider,
  type LocalImageHints,
  type ProviderResolution,
} from "./image-providers";

const DEFAULT_TARGET_IMAGES = 2;
const DEFAULT_MAX_IMAGES = 3;
const DEFAULT_REQUEST_TIMEOUT_MS = 8_000;
const DEFAULT_IMAGE_TIMEOUT_MS = 10_000;
const DEFINITE_MISSING_TTL_MS = 24 * 60 * 60 * 1_000;
const TRANSIENT_FAILURE_TTL_MS = 2 * 60 * 1_000;
const DEFAULT_RATE_LIMIT_TTL_MS = 5 * 60 * 1_000;

export type ImageFailureKind = "missing" | "transient";

export interface ImageProbeResult {
  ok: boolean;
  width?: number;
  height?: number;
  failure?: ImageFailureKind;
}

export type ImageProbe = (
  src: string,
  options: { signal: AbortSignal; timeoutMs: number },
) => Promise<ImageProbeResult>;

export interface LoadedImage extends ImageCandidate {
  width: number | null;
  height: number | null;
}

export interface LoadEntityImagesRequest {
  entity: ImageEntity;
  localHints?: LocalImageHints;
  signal?: AbortSignal;
  target?: number;
  max?: number;
  onImage?: (image: LoadedImage) => void;
}

export interface ImageRuntimeOptions {
  fetchImpl?: typeof fetch;
  probe?: ImageProbe;
  now?: () => number;
  requestTimeoutMs?: number;
  imageTimeoutMs?: number;
  definiteMissingTtlMs?: number;
  transientFailureTtlMs?: number;
}

interface CachedResolution {
  expiresAt: number;
  resolution?: ProviderResolution;
  failure?: ImageFailureKind;
}

interface ProviderFailureOptions {
  kind: ImageFailureKind;
  retryAt?: number;
}

export class ImageProviderFailure extends Error {
  readonly kind: ImageFailureKind;
  readonly retryAt?: number;

  constructor(message: string, options: ProviderFailureOptions) {
    super(message);
    this.name = "ImageProviderFailure";
    this.kind = options.kind;
    this.retryAt = options.retryAt;
  }
}

function abortError(): DOMException {
  return new DOMException("Image lookup aborted", "AbortError");
}

function throwIfAborted(signal: AbortSignal): void {
  if (signal.aborted) throw abortError();
}

function online(): boolean {
  return typeof navigator === "undefined" || navigator.onLine !== false;
}

export const browserImageProbe: ImageProbe = (
  src,
  { signal, timeoutMs },
) =>
  new Promise((resolve, reject) => {
    if (signal.aborted) {
      reject(abortError());
      return;
    }
    if (typeof Image === "undefined") {
      resolve({ ok: false, failure: "transient" });
      return;
    }
    const image = new Image();
    let settled = false;
    const finish = (result: ImageProbeResult, cancelRequest = false) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      signal.removeEventListener("abort", onAbort);
      image.onload = null;
      image.onerror = null;
      if (cancelRequest) image.src = "";
      resolve(result);
    };
    const onAbort = () => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      image.onload = null;
      image.onerror = null;
      image.src = "";
      reject(abortError());
    };
    const timer = globalThis.setTimeout(
      () => finish({ ok: false, failure: "transient" }, true),
      timeoutMs,
    );
    image.decoding = "async";
    image.referrerPolicy = "no-referrer";
    image.onload = () =>
      finish({
        ok: true,
        width: image.naturalWidth,
        height: image.naturalHeight,
      });
    image.onerror = () =>
      finish({ ok: false, failure: online() ? "missing" : "transient" });
    signal.addEventListener("abort", onAbort, { once: true });
    image.src = src;
  });

function resolutionKey(
  provider: ImageProvider,
  entity: ImageEntity,
  scheme: string,
  value: string,
): string {
  return [provider.id, entity.id, entity.family, scheme, value.trim()].join("\u0000");
}

function parseRetryAfter(value: string | null, now: number): number {
  if (!value) return now + DEFAULT_RATE_LIMIT_TTL_MS;
  const seconds = Number(value);
  const requested = Number.isFinite(seconds)
    ? now + Math.max(0, seconds) * 1_000
    : Date.parse(value);
  if (!Number.isFinite(requested) || requested <= now) {
    return now + DEFAULT_RATE_LIMIT_TTL_MS;
  }
  return requested;
}

/**
 * Session-only, opportunistic image runtime. Calls are intentionally serialized
 * per API provider; there are no retries inside a card opening.
 */
export class ImageRuntime {
  private readonly fetchImpl: typeof fetch;
  private readonly probe: ImageProbe;
  private readonly now: () => number;
  private readonly requestTimeoutMs: number;
  private readonly imageTimeoutMs: number;
  private readonly definiteMissingTtlMs: number;
  private readonly transientFailureTtlMs: number;
  private readonly resolutionCache = new Map<string, CachedResolution>();
  private readonly imageFailureCache = new Map<string, number>();
  private readonly providerCooldowns = new Map<string, number>();
  private readonly providerQueues = new Map<string, Promise<void>>();

  constructor(options: ImageRuntimeOptions = {}) {
    this.fetchImpl = options.fetchImpl ?? globalThis.fetch.bind(globalThis);
    this.probe = options.probe ?? browserImageProbe;
    this.now = options.now ?? Date.now;
    this.requestTimeoutMs =
      options.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
    this.imageTimeoutMs = options.imageTimeoutMs ?? DEFAULT_IMAGE_TIMEOUT_MS;
    this.definiteMissingTtlMs =
      options.definiteMissingTtlMs ?? DEFINITE_MISSING_TTL_MS;
    this.transientFailureTtlMs =
      options.transientFailureTtlMs ?? TRANSIENT_FAILURE_TTL_MS;
  }

  clear(): void {
    this.resolutionCache.clear();
    this.imageFailureCache.clear();
    this.providerCooldowns.clear();
  }

  providerCooldownUntil(providerId: string): number | null {
    const until = this.providerCooldowns.get(providerId) ?? 0;
    return until > this.now() ? until : null;
  }

  private async serializeProvider<T>(
    providerId: string,
    signal: AbortSignal,
    operation: () => Promise<T>,
  ): Promise<T> {
    const previous = this.providerQueues.get(providerId) ?? Promise.resolve();
    let release!: () => void;
    const ownTurn = new Promise<void>((resolve) => {
      release = resolve;
    });
    const queue = previous.then(() => ownTurn);
    this.providerQueues.set(providerId, queue);
    await previous;
    try {
      throwIfAborted(signal);
      return await operation();
    } finally {
      release();
      void queue.finally(() => {
        if (this.providerQueues.get(providerId) === queue) {
          this.providerQueues.delete(providerId);
        }
      });
    }
  }

  private async requestJson(
    provider: ImageProvider,
    urlValue: string,
    outerSignal: AbortSignal,
  ): Promise<unknown> {
    if (!isAllowedHttpsUrl(urlValue, provider.allowedApiHosts)) {
      throw new ImageProviderFailure("Provider API URL is not allowed", {
        kind: "missing",
      });
    }
    const cooldown = this.providerCooldownUntil(provider.id);
    if (cooldown !== null) {
      throw new ImageProviderFailure("Provider is cooling down", {
        kind: "transient",
        retryAt: cooldown,
      });
    }
    return this.serializeProvider(provider.id, outerSignal, async () => {
      const refreshedCooldown = this.providerCooldownUntil(provider.id);
      if (refreshedCooldown !== null) {
        throw new ImageProviderFailure("Provider is cooling down", {
          kind: "transient",
          retryAt: refreshedCooldown,
        });
      }
      const controller = new AbortController();
      const abort = () => controller.abort();
      outerSignal.addEventListener("abort", abort, { once: true });
      const timeout = globalThis.setTimeout(
        () => controller.abort(),
        this.requestTimeoutMs,
      );
      try {
        const response = await this.fetchImpl(urlValue, {
          method: "GET",
          headers: { Accept: "application/json" },
          redirect: "error",
          signal: controller.signal,
        });
        const now = this.now();
        if (response.status === 429) {
          const retryAt = parseRetryAfter(response.headers.get("Retry-After"), now);
          this.providerCooldowns.set(provider.id, retryAt);
          throw new ImageProviderFailure("Provider rate limited the request", {
            kind: "transient",
            retryAt,
          });
        }
        if (response.status === 404 || response.status === 403) {
          throw new ImageProviderFailure("Provider has no usable image", {
            kind: "missing",
          });
        }
        if (!response.ok) {
          throw new ImageProviderFailure(
            `Provider request failed (${response.status})`,
            { kind: response.status >= 500 ? "transient" : "missing" },
          );
        }
        try {
          return await response.json();
        } catch {
          throw new ImageProviderFailure("Provider returned invalid JSON", {
            kind: "transient",
          });
        }
      } catch (cause) {
        if (outerSignal.aborted) throw abortError();
        if (cause instanceof ImageProviderFailure) throw cause;
        throw new ImageProviderFailure("Provider request was unavailable", {
          kind: "transient",
        });
      } finally {
        clearTimeout(timeout);
        outerSignal.removeEventListener("abort", abort);
      }
    });
  }

  private async resolveIdentifier(
    provider: ImageProvider,
    entity: ImageEntity,
    identifier: { scheme: string; value: string },
    localHints: LocalImageHints | undefined,
    signal: AbortSignal,
  ): Promise<ProviderResolution> {
    const key = resolutionKey(
      provider,
      entity,
      identifier.scheme,
      identifier.value,
    );
    // Local artifacts can finish loading after an entity card first opens. They
    // are already in-memory and cheap, so do not let an earlier empty artifact
    // create a stale session miss.
    const cacheable = provider.mode !== "local";
    const cached = cacheable ? this.resolutionCache.get(key) : undefined;
    if (cached && cached.expiresAt > this.now()) {
      if (cached.resolution) return cached.resolution;
      throw new ImageProviderFailure("Cached provider failure", {
        kind: cached.failure ?? "transient",
        retryAt: cached.expiresAt,
      });
    }
    if (cacheable) this.resolutionCache.delete(key);
    throwIfAborted(signal);
    try {
      const resolution = await provider.resolve(identifier, {
        entity,
        localHints,
        signal,
        requestJson: (url) => this.requestJson(provider, url, signal),
      });
      const sanitized: ProviderResolution = {
        status: resolution.status,
        candidates: resolution.candidates.filter((candidate) =>
          validProviderCandidate(provider, entity.family, candidate),
        ),
      };
      if (sanitized.candidates.length === 0) sanitized.status = "missing";
      if (cacheable) {
        this.resolutionCache.set(key, {
          resolution: sanitized,
          expiresAt:
            sanitized.status === "missing"
              ? this.now() + this.definiteMissingTtlMs
              : Number.POSITIVE_INFINITY,
        });
      }
      return sanitized;
    } catch (cause) {
      if (signal.aborted) throw abortError();
      const failure =
        cause instanceof ImageProviderFailure
          ? cause
          : new ImageProviderFailure("Provider lookup failed", {
              kind: "transient",
            });
      if (cacheable) {
        this.resolutionCache.set(key, {
          failure: failure.kind,
          expiresAt:
            failure.retryAt ??
            this.now() +
              (failure.kind === "missing"
                ? this.definiteMissingTtlMs
                : this.transientFailureTtlMs),
        });
      }
      throw failure;
    }
  }

  private async loadCandidate(
    candidate: ImageCandidate,
    signal: AbortSignal,
  ): Promise<LoadedImage | null> {
    const missingUntil = this.imageFailureCache.get(candidate.src) ?? 0;
    if (missingUntil > this.now()) return null;
    this.imageFailureCache.delete(candidate.src);
    throwIfAborted(signal);
    const result = await this.serializeProvider(
      candidate.providerId,
      signal,
      () =>
        this.probe(candidate.src, {
          signal,
          timeoutMs: this.imageTimeoutMs,
        }),
    );
    throwIfAborted(signal);
    if (!result.ok) {
      const failure = result.failure ?? "transient";
      this.imageFailureCache.set(
        candidate.src,
        this.now() +
          (failure === "missing"
            ? this.definiteMissingTtlMs
            : this.transientFailureTtlMs),
      );
      return null;
    }
    return {
      ...candidate,
      width: result.width ?? null,
      height: result.height ?? null,
    };
  }

  async loadEntityImages(
    request: LoadEntityImagesRequest,
  ): Promise<LoadedImage[]> {
    const controller = new AbortController();
    const abort = () => controller.abort();
    request.signal?.addEventListener("abort", abort, { once: true });
    if (request.signal?.aborted) controller.abort();
    const signal = controller.signal;
    const maximum = Math.min(
      DEFAULT_MAX_IMAGES,
      Math.max(1, Math.floor(request.max ?? DEFAULT_MAX_IMAGES)),
    );
    const target = Math.min(
      maximum,
      Math.max(1, Math.floor(request.target ?? DEFAULT_TARGET_IMAGES)),
    );
    const loaded: LoadedImage[] = [];
    const seenSources = new Set<string>();
    const seenUrls = new Set<string>();
    const deferred: ImageCandidate[] = [];

    try {
      for (const dispatch of imageProviderDispatches(request.entity)) {
        if (loaded.length >= target) break;
        const candidates: ImageCandidate[] = [];
        const candidateUrls = new Set(seenUrls);
        for (const identifier of dispatch.identifiers) {
          throwIfAborted(signal);
          try {
            const resolution = await this.resolveIdentifier(
              dispatch.provider,
              request.entity,
              identifier,
              request.localHints,
              signal,
            );
            for (const candidate of resolution.candidates) {
              if (!candidateUrls.has(candidate.src)) {
                candidates.push(candidate);
                candidateUrls.add(candidate.src);
              }
            }
          } catch (cause) {
            if (signal.aborted) throw abortError();
            // Images are optional. A failed provider simply falls through.
            if (!(cause instanceof ImageProviderFailure)) throw cause;
          }
        }

        // Prefer one successful image from each source before asking another
        // provider. This avoids near-identical carousels from a single source.
        for (let index = 0; index < candidates.length; index += 1) {
          const candidate = candidates[index];
          if (loaded.length >= maximum) break;
          seenUrls.add(candidate.src);
          if (seenSources.has(candidate.source)) continue;
          const accepted = await this.loadCandidate(candidate, signal);
          if (!accepted) continue;
          loaded.push(accepted);
          seenSources.add(accepted.source);
          request.onImage?.(accepted);
          for (const extra of candidates.slice(index + 1)) {
            if (!seenUrls.has(extra.src)) deferred.push(extra);
          }
          break;
        }
      }

      // If source diversity did not reach the target, retain useful additional
      // local claims (for example a poster and representative work image).
      for (const candidate of deferred) {
        if (loaded.length >= target || loaded.length >= maximum) break;
        if (seenUrls.has(candidate.src)) continue;
        seenUrls.add(candidate.src);
        const accepted = await this.loadCandidate(candidate, signal);
        if (!accepted) continue;
        loaded.push(accepted);
        request.onImage?.(accepted);
      }
      return loaded;
    } finally {
      request.signal?.removeEventListener("abort", abort);
    }
  }
}

export const defaultImageRuntime = new ImageRuntime();
