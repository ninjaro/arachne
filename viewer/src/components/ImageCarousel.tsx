import { useEffect, useMemo, useState } from "react";
import {
  defaultImageRuntime,
  type ImageRuntime,
  type LoadedImage,
} from "../lib/image-loader";
import {
  loadImageHintsForEntity,
  type ImageHintProductIdentity,
} from "../lib/image-hints";
import type {
  ImageEntity,
  LocalImageHints,
} from "../lib/image-providers";
import "./ImageCarousel.css";

export interface ImageCarouselProps {
  images: readonly LoadedImage[];
  label: string;
}

/** A small, non-autoplaying carousel containing only browser-loaded images. */
export function ImageCarousel({ images, label }: ImageCarouselProps) {
  const [failed, setFailed] = useState<ReadonlySet<string>>(() => new Set());
  const [current, setCurrent] = useState(0);
  const available = useMemo(
    () => images.filter((image) => !failed.has(image.src)).slice(0, 3),
    [failed, images],
  );

  useEffect(() => {
    setFailed((previous) => {
      const next = new Set(
        [...previous].filter((src) => images.some((image) => image.src === src)),
      );
      return next.size === previous.size &&
        [...next].every((src) => previous.has(src))
        ? previous
        : next;
    });
  }, [images]);

  useEffect(() => {
    setCurrent((index) => Math.min(index, Math.max(0, available.length - 1)));
  }, [available.length]);

  // No candidate, heading, placeholder, or reserved layout is rendered until a
  // browser probe has succeeded. A later <img> failure also removes the UI.
  if (available.length === 0) return null;

  const image = available[current] ?? available[0];
  const imageElement = (
    <img
      src={image.src}
      alt=""
      loading="lazy"
      decoding="async"
      referrerPolicy="no-referrer"
      width={image.width ?? undefined}
      height={image.height ?? undefined}
      onError={() => {
        setFailed((previous) => new Set(previous).add(image.src));
      }}
    />
  );

  return (
    <figure className="image-carousel" aria-label={`Images for ${label}`}>
      <div className="image-carousel-frame">
        {image.sourceUrl ? (
          <a
            href={image.sourceUrl}
            target="_blank"
            rel="noreferrer"
            aria-label={`View image at ${image.source}`}
          >
            {imageElement}
          </a>
        ) : (
          imageElement
        )}
      </div>
      <figcaption>
        <span>{image.source}</span>
        {image.attribution ? <span> · {image.attribution}</span> : null}
        {image.licenseUrl ? (
          <>
            {" · "}
            <a href={image.licenseUrl} target="_blank" rel="noreferrer">
              {image.license ?? "License"}
            </a>
          </>
        ) : image.license ? (
          <span> · {image.license}</span>
        ) : null}
      </figcaption>
      {available.length > 1 ? (
        <nav aria-label="Image carousel controls">
          <button
            type="button"
            onClick={() =>
              setCurrent((index) =>
                (index - 1 + available.length) % available.length,
              )
            }
            aria-label="Previous image"
          >
            ‹
          </button>
          <span aria-live="polite">
            {current + 1} / {available.length}
          </span>
          <button
            type="button"
            onClick={() =>
              setCurrent((index) => (index + 1) % available.length)
            }
            aria-label="Next image"
          >
            ›
          </button>
        </nav>
      ) : null}
    </figure>
  );
}

export interface EntityImageCarouselProps {
  entity: ImageEntity;
  label: string;
  localHints?: LocalImageHints;
  imageHintsUrl?: string;
  imageHintProduct?: ImageHintProductIdentity;
  runtime?: ImageRuntime;
}

/** Resolves images only while its entity card is mounted/visible. */
export function EntityImageCarousel({
  entity,
  label,
  localHints,
  imageHintsUrl,
  imageHintProduct,
  runtime = defaultImageRuntime,
}: EntityImageCarouselProps) {
  const [images, setImages] = useState<LoadedImage[]>([]);
  const entitySignature = JSON.stringify([
    entity.id,
    entity.family,
    entity.medium ?? null,
    entity.agentType ?? null,
    entity.identifiers.map(({ scheme, value }) => [scheme, value]),
  ]);
  const selectedHints = localHints?.[entity.id];
  const hintSignature = JSON.stringify(selectedHints ?? []);
  const productSignature = JSON.stringify(imageHintProduct ?? null);

  useEffect(() => {
    const controller = new AbortController();
    const entitySnapshot = entity;
    setImages([]);
    void (async () => {
      const hintsSnapshot: LocalImageHints | undefined = selectedHints
        ? { [entitySnapshot.id]: selectedHints }
        : imageHintsUrl && imageHintProduct
          ? await loadImageHintsForEntity(imageHintsUrl, {
              entity: entitySnapshot,
              product: imageHintProduct,
              signal: controller.signal,
            })
          : undefined;
      if (controller.signal.aborted) return;
      await runtime.loadEntityImages({
        entity: entitySnapshot,
        localHints: hintsSnapshot,
        signal: controller.signal,
        onImage: (image) => {
          if (controller.signal.aborted) return;
          setImages((current) =>
            current.some((candidate) => candidate.src === image.src)
              ? current
              : [...current, image].slice(0, 3),
          );
        },
      });
    })()
      .catch((cause: unknown) => {
        if (!(cause instanceof DOMException && cause.name === "AbortError")) {
          // Optional image enrichment fails closed without changing card UI.
        }
      });
    return () => controller.abort();
    // Signatures prevent focus, drag, or rating rerenders with structurally
    // identical entity literals from restarting provider requests.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [
    entitySignature,
    hintSignature,
    imageHintsUrl,
    productSignature,
    runtime,
  ]);

  return <ImageCarousel images={images} label={label} />;
}
