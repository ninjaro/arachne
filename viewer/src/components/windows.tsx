import { useCallback, useRef, useState } from "react";
import type {
  Domain,
  EntityId,
  EntityOpenContext,
  Ratings,
} from "../lib/types";
import type { ImageHintProductIdentity } from "../lib/image-hints";
import { buildEntityPermalink } from "../lib/location";
import type { RateHandler } from "./common";
import { AgentEntityBody, WorkEntityBody } from "./EntityWindowBody";

export interface WindowState {
  id: EntityId;
  x: number;
  y: number;
  z: number;
  context?: EntityOpenContext;
}

export type EntityWindowMode = "append" | "replace";

export interface EntityWindowOptions {
  mode?: EntityWindowMode;
  /** Maximum retained windows in append mode. Omit for an unbounded comparison set. */
  limit?: number;
  context?: EntityOpenContext;
}

function initialWindowPosition(index: number): Pick<WindowState, "x" | "y"> {
  return {
    x: 28 + (index % 6) * 34,
    y: 90 + (index % 5) * 30,
  };
}

export function useEntityWindows() {
  const [windows, setWindows] = useState<WindowState[]>([]);
  const zCounter = useRef(1);

  const focusWindow = useCallback((id: EntityId) => {
    zCounter.current += 1;
    setWindows((current) =>
      current.map((window) =>
        window.id === id ? { ...window, z: zCounter.current } : window,
      ),
    );
  }, []);

  const openWindow = useCallback((
    id: EntityId,
    options: EntityWindowOptions = {},
  ) => {
    zCounter.current += 1;
    setWindows((current) => {
      const existing = current.find((window) => window.id === id);
      const mode = options.mode ?? "append";
      const limit = options.limit === undefined
        ? Number.POSITIVE_INFINITY
        : Math.max(1, Math.trunc(options.limit));

      if (existing) {
        const focused = current.map((window) =>
          window.id === id
            ? {
                ...window,
                z: zCounter.current,
                context: options.context ?? window.context,
              }
            : window,
        );
        if (mode === "replace") {
          return focused.filter((window) => window.id === id);
        }
        if (focused.length <= limit) return focused;
        return [
          ...focused.filter((window) => window.id !== id).slice(-(limit - 1)),
          focused.find((window) => window.id === id)!,
        ];
      }

      const retained = mode === "replace"
        ? []
        : current.slice(-Math.max(0, limit - 1));
      const position = initialWindowPosition(retained.length);
      return [
        ...retained,
        { id, ...position, z: zCounter.current, context: options.context },
      ];
    });
  }, []);

  const openWindows = useCallback((ids: readonly EntityId[]) => {
    const unique = [...new Set(ids)].slice(0, 2);
    setWindows(unique.map((id, index) => {
      zCounter.current += 1;
      return {
        id,
        x: 28 + index * 430,
        y: 90,
        z: zCounter.current,
      };
    }));
  }, []);

  const closeWindow = useCallback((id: EntityId) => {
    setWindows((current) => current.filter((window) => window.id !== id));
  }, []);

  const moveWindow = useCallback((id: EntityId, x: number, y: number) => {
    setWindows((current) =>
      current.map((window) =>
        window.id === id ? { ...window, x, y } : window,
      ),
    );
  }, []);

  return {
    windows,
    openWindow,
    openWindows,
    focusWindow,
    closeWindow,
    moveWindow,
  };
}

export function FloatingEntityWindows({
  windows,
  domain,
  ratings,
  onRate,
  onOpen,
  imageHintsUrl,
  imageHintProduct,
  onSearch,
  onFocus,
  onClose,
  onMove,
}: {
  windows: WindowState[];
  domain: Domain;
  ratings: Ratings;
  onRate: RateHandler;
  onOpen: (id: EntityId) => void;
  imageHintsUrl: string;
  imageHintProduct: ImageHintProductIdentity;
  onSearch: (query: string) => void;
  onFocus: (id: EntityId) => void;
  onClose: (id: EntityId) => void;
  onMove: (id: EntityId, x: number, y: number) => void;
}) {
  return (
    <>
      {windows.map((windowState) => {
        const work = domain.workById.get(windowState.id);
        const agent = domain.agentById.get(windowState.id);
        const label = work?.label ?? agent?.label;
        if (!label || (!work && !agent)) return null;

        return (
          <article
            className="entity-window"
            key={windowState.id}
            style={{
              left: windowState.x,
              top: windowState.y,
              zIndex: windowState.z,
            }}
            onPointerDown={() => onFocus(windowState.id)}
            onClick={(event) => {
              const target = event.target as HTMLElement;
              const query = target.closest<HTMLElement>(
                "button[data-query]",
              )?.dataset.query;
              if (query) onSearch(query);
            }}
          >
            <header
              className="entity-window-title"
              onPointerDown={(event) => {
                if (event.button !== 0) return;
                event.currentTarget.setPointerCapture(event.pointerId);
                const startX = event.clientX;
                const startY = event.clientY;
                const originX = windowState.x;
                const originY = windowState.y;
                const target = event.currentTarget;
                const move = (moveEvent: PointerEvent) => {
                  const maximumX = Math.max(
                    0,
                    globalThis.innerWidth - 120,
                  );
                  const maximumY = Math.max(
                    58,
                    globalThis.innerHeight - 48,
                  );
                  onMove(
                    windowState.id,
                    Math.min(
                      maximumX,
                      Math.max(
                        0,
                        originX + moveEvent.clientX - startX,
                      ),
                    ),
                    Math.min(
                      maximumY,
                      Math.max(
                        58,
                        originY + moveEvent.clientY - startY,
                      ),
                    ),
                  );
                };
                const stop = () => {
                  target.removeEventListener("pointermove", move);
                  target.removeEventListener("pointerup", stop);
                  target.removeEventListener("pointercancel", stop);
                };
                target.addEventListener("pointermove", move);
                target.addEventListener("pointerup", stop);
                target.addEventListener("pointercancel", stop);
              }}
            >
              <strong>{label}</strong>
              <a
                href={buildEntityPermalink(
                  { family: work ? "work" : "agent", id: windowState.id },
                  import.meta.env.BASE_URL,
                )}
                onPointerDown={(event) => event.stopPropagation()}
                aria-label={`Permalink for ${label}`}
                title="Open canonical viewer permalink"
              >
                Link
              </a>
              <button
                type="button"
                onPointerDown={(event) => event.stopPropagation()}
                onClick={() => onClose(windowState.id)}
                aria-label={`Close ${label}`}
              >
                ×
              </button>
            </header>
            <div className="entity-window-body">
              {windowState.context ? (
                <aside className="entity-window-context">
                  <strong>{windowState.context.title}</strong>
                  <ul>
                    {windowState.context.details.map((detail) => (
                      <li key={detail}>{detail}</li>
                    ))}
                  </ul>
                </aside>
              ) : null}
              {work ? (
                <WorkEntityBody
                  work={work}
                  domain={domain}
                  ratings={ratings}
                  onRate={onRate}
                  onSearch={onSearch}
                  onOpen={onOpen}
                  imageHintsUrl={imageHintsUrl}
                  imageHintProduct={imageHintProduct}
                />
              ) : agent ? (
                <AgentEntityBody
                  agent={agent}
                  domain={domain}
                  ratings={ratings}
                  onRate={onRate}
                  onOpen={onOpen}
                  imageHintsUrl={imageHintsUrl}
                  imageHintProduct={imageHintProduct}
                />
              ) : null}
            </div>
          </article>
        );
      })}
    </>
  );
}
