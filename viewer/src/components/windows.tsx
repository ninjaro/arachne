import { useCallback, useRef, useState } from "react";
import type { Domain, EntityId, Ratings } from "../lib/types";
import type { RateHandler } from "./common";
import { ConceptChips, RatingButtons } from "./common";
import { dateLabel, humanize, moneyLabel } from "../lib/format";

interface WindowState {
  id: EntityId;
  x: number;
  y: number;
  z: number;
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

  const openWindow = useCallback((id: EntityId) => {
    zCounter.current += 1;
    setWindows((current) => {
      const existing = current.find((window) => window.id === id);
      if (existing) {
        return current.map((window) =>
          window.id === id ? { ...window, z: zCounter.current } : window,
        );
      }
      const index = current.length;
      return [
        ...current,
        {
          id,
          x: 28 + (index % 6) * 34,
          y: 90 + (index % 5) * 30,
          z: zCounter.current,
        },
      ];
    });
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

  return { windows, openWindow, focusWindow, closeWindow, moveWindow };
}

export function FloatingEntityWindows({
  windows,
  domain,
  ratings,
  onRate,
  onFocus,
  onClose,
  onMove,
}: {
  windows: WindowState[];
  domain: Domain;
  ratings: Ratings;
  onRate: RateHandler;
  onFocus: (id: EntityId) => void;
  onClose: (id: EntityId) => void;
  onMove: (id: EntityId, x: number, y: number) => void;
}) {
  return (
    <>
      {windows.map((window) => {
        const work = domain.workById.get(window.id);
        if (!work) return null;
        return (
          <article
            className="entity-window"
            key={window.id}
            style={{ left: window.x, top: window.y, zIndex: window.z }}
            onPointerDown={() => onFocus(window.id)}
          >
            <header
              className="entity-window-title"
              onPointerDown={(event) => {
                if (event.button !== 0) return;
                event.currentTarget.setPointerCapture(event.pointerId);
                const startX = event.clientX;
                const startY = event.clientY;
                const originX = window.x;
                const originY = window.y;
                const target = event.currentTarget;
                const move = (moveEvent: PointerEvent) => {
                  onMove(
                    window.id,
                    Math.max(0, originX + moveEvent.clientX - startX),
                    Math.max(58, originY + moveEvent.clientY - startY),
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
              <strong>{work.label}</strong>
              <button
                type="button"
                onPointerDown={(event) => event.stopPropagation()}
                onClick={() => onClose(window.id)}
                aria-label={`Close ${work.label}`}
              >
                ×
              </button>
            </header>
            <div className="entity-window-body">
              <div className="window-meta">
                <span>{dateLabel(work)}</span>
                <span>{humanize(work.medium)}</span>
                {work.countryCode ? <span>{work.countryCode}</span> : null}
                {work.languageCode ? <span>{work.languageCode}</span> : null}
              </div>
              <RatingButtons work={work} ratings={ratings} onRate={onRate} />

              {work.concepts.length ? (
                <section>
                  <h3>Concepts</h3>
                  <ConceptChips concepts={work.concepts} limit={30} />
                </section>
              ) : null}

              {work.contributors.length ? (
                <section>
                  <h3>Contributors</h3>
                  <dl className="detail-list">
                    {work.contributors.map((contributor) => (
                      <div key={`${contributor.role}:${contributor.id}`}>
                        <dt>{humanize(contributor.role)}</dt>
                        <dd>
                          {contributor.label}
                          {contributor.creditedAs &&
                          contributor.creditedAs !== contributor.label
                            ? ` as ${contributor.creditedAs}`
                            : ""}
                        </dd>
                      </div>
                    ))}
                  </dl>
                </section>
              ) : null}

              {work.advisories.length ? (
                <section>
                  <h3>Content guide</h3>
                  <dl className="detail-list">
                    {work.advisories.map((advisory) => (
                      <div key={advisory.id}>
                        <dt>{humanize(advisory.category)}</dt>
                        <dd>
                          {advisory.label}
                          {advisory.intensity !== null
                            ? ` · ${advisory.intensity}/5`
                            : ""}
                        </dd>
                      </div>
                    ))}
                  </dl>
                </section>
              ) : null}

              {work.measurements.length ? (
                <section>
                  <h3>Measurements</h3>
                  <dl className="detail-list">
                    {work.measurements.map((measurement, index) => (
                      <div key={`${measurement.type}:${index}`}>
                        <dt>{humanize(measurement.type)}</dt>
                        <dd>
                          {measurement.value}
                          {measurement.unit ? ` ${measurement.unit}` : ""}
                          {measurement.qualifier
                            ? ` (${measurement.qualifier})`
                            : ""}
                        </dd>
                      </div>
                    ))}
                  </dl>
                </section>
              ) : null}

              {work.financialFacts.length ? (
                <section>
                  <h3>Financial facts</h3>
                  <dl className="detail-list">
                    {work.financialFacts.map((fact, index) => (
                      <div key={`${fact.type}:${index}`}>
                        <dt>{humanize(fact.type)}</dt>
                        <dd>
                          {moneyLabel(
                            fact.amountMin,
                            fact.amountMax,
                            fact.currencyCode,
                          )}
                          {fact.valueYear ? ` (${fact.valueYear})` : ""}
                          {fact.isEstimate ? " · estimate" : ""}
                        </dd>
                      </div>
                    ))}
                  </dl>
                </section>
              ) : null}

              {work.manifestations.length ? (
                <section>
                  <h3>Manifestations</h3>
                  <ul className="plain-list">
                    {work.manifestations.map((manifestation) => (
                      <li key={manifestation.id}>
                        {manifestation.label || humanize(manifestation.type)}
                        {manifestation.releaseYear
                          ? ` · ${manifestation.releaseYear}`
                          : ""}
                        {manifestation.regionCode
                          ? ` · ${manifestation.regionCode}`
                          : ""}
                      </li>
                    ))}
                  </ul>
                </section>
              ) : null}

              {work.identifiers.length ? (
                <section>
                  <h3>Identifiers</h3>
                  <ul className="plain-list">
                    {work.identifiers.map((identifier) => (
                      <li key={`${identifier.scheme}:${identifier.value}`}>
                        {identifier.url ? (
                          <a href={identifier.url} target="_blank" rel="noreferrer">
                            {humanize(identifier.scheme)}: {identifier.value}
                          </a>
                        ) : (
                          <>
                            {humanize(identifier.scheme)}: {identifier.value}
                          </>
                        )}
                      </li>
                    ))}
                  </ul>
                </section>
              ) : null}
            </div>
          </article>
        );
      })}
    </>
  );
}
