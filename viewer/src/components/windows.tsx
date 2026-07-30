import { useCallback, useRef, useState } from "react";
import type { Domain, EntityId, Ratings } from "../lib/types";
import type { RateHandler } from "./common";
import { GroupedConceptChips, RatingButtons } from "./common";
import {
  dateLabel,
  externalUrl,
  humanize,
  moneyLabel,
  schemeLabel,
} from "../lib/format";
import { buildQueryToken } from "../lib/query";

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

function InlineFilterMenu({
  label,
  options,
}: {
  label: string;
  options: Array<{ label: string; query: string }>;
}) {
  return (
    <details className="inline-filter-menu">
      <summary>{label}</summary>
      <div className="inline-filter-options">
        {options.map((option) => (
          <button
            type="button"
            key={`${option.label}:${option.query}`}
            data-query={option.query}
          >
            {option.label}
          </button>
        ))}
      </div>
    </details>
  );
}

export function FloatingEntityWindows({
  windows,
  domain,
  ratings,
  onRate,
  onSearch,
  onFocus,
  onClose,
  onMove,
}: {
  windows: WindowState[];
  domain: Domain;
  ratings: Ratings;
  onRate: RateHandler;
  onSearch: (query: string) => void;
  onFocus: (id: EntityId) => void;
  onClose: (id: EntityId) => void;
  onMove: (id: EntityId, x: number, y: number) => void;
}) {
  return (
    <>
      {windows.map((windowState) => {
        const work = domain.workById.get(windowState.id);
        if (!work) return null;
        return (
          <article
            className="entity-window"
            key={windowState.id}
            style={{ left: windowState.x, top: windowState.y, zIndex: windowState.z }}
            onPointerDown={() => onFocus(windowState.id)}
            onClick={(event) => {
              const target = event.target as HTMLElement;
              const query = target.closest<HTMLElement>("button[data-query]")?.dataset.query;
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
                  const maximumX = Math.max(0, globalThis.innerWidth - 120);
                  const maximumY = Math.max(58, globalThis.innerHeight - 48);
                  onMove(
                    windowState.id,
                    Math.min(maximumX, Math.max(0, originX + moveEvent.clientX - startX)),
                    Math.min(maximumY, Math.max(58, originY + moveEvent.clientY - startY)),
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
                onClick={() => onClose(windowState.id)}
                aria-label={`Close ${work.label}`}
              >
                ×
              </button>
            </header>
            <div className="entity-window-body">
              <div className="window-meta">
                <span>{dateLabel(work)}</span>
                <button
                  type="button"
                  className="meta-filter-link"
                  data-query={buildQueryToken("medium", work.medium)}
                >
                  {humanize(work.medium)}
                </button>
                {work.countryCode ? (
                  <button
                    type="button"
                    className="meta-filter-link"
                    data-query={buildQueryToken("country", work.countryCode)}
                  >
                    {work.countryCode}
                  </button>
                ) : null}
                {work.languageCode ? (
                  <button
                    type="button"
                    className="meta-filter-link"
                    data-query={buildQueryToken("lang", work.languageCode)}
                  >
                    {work.languageCode}
                  </button>
                ) : null}
              </div>
              <RatingButtons work={work} ratings={ratings} onRate={onRate} />

              {work.concepts.length ? (
                <section>
                  <h3>Concepts</h3>
                  <GroupedConceptChips
                    concepts={work.concepts}
                    onFilter={(concept) =>
                      onSearch(buildQueryToken("tag", concept.label))
                    }
                  />
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
                          <InlineFilterMenu
                            label={contributor.label}
                            options={[
                              {
                                label: "Filter by this person",
                                query: buildQueryToken("agent", contributor.label),
                              },
                              {
                                label: "Exclude this person",
                                query: buildQueryToken("agent", contributor.label, true),
                              },
                              {
                                label: `Filter as ${humanize(contributor.role)}`,
                                query: buildQueryToken(
                                  contributor.role,
                                  contributor.label,
                                ),
                              },
                            ]}
                          />
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
                  <dl className="detail-list content-guide-list">
                    {work.advisories.map((advisory) => (
                      <div key={advisory.id}>
                        <dt>
                          <button
                            type="button"
                            className="detail-filter-link"
                            data-query={buildQueryToken("guide", advisory.category)}
                          >
                            {humanize(advisory.category)}
                          </button>
                        </dt>
                        <dd>
                          <span className="guide-meter" aria-label={`Intensity ${advisory.intensity ?? "unknown"} out of 5`}>
                            {Array.from({ length: 5 }, (_, index) => (
                              <i
                                key={index}
                                className={
                                  advisory.intensity !== null && index < advisory.intensity
                                    ? "active"
                                    : ""
                                }
                              />
                            ))}
                          </span>
                          <span>{advisory.label}</span>
                          <details>
                            <summary>Details</summary>
                            <dl className="advisory-details">
                              {advisory.intensity !== null ? <div><dt>Intensity</dt><dd>{advisory.intensity}/5</dd></div> : null}
                              {advisory.frequency !== null ? <div><dt>Frequency</dt><dd>{advisory.frequency}/5</dd></div> : null}
                              {advisory.explicitness !== null ? <div><dt>Explicitness</dt><dd>{advisory.explicitness}/5</dd></div> : null}
                              {advisory.realism !== null ? <div><dt>Realism</dt><dd>{advisory.realism}/5</dd></div> : null}
                              {advisory.confidence !== null ? <div><dt>Confidence</dt><dd>{Math.round(advisory.confidence * 100)}%</dd></div> : null}
                            </dl>
                          </details>
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
                          {measurement.qualifier ? ` (${measurement.qualifier})` : ""}
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
                        {manifestation.releaseYear ? ` · ${manifestation.releaseYear}` : ""}
                        {manifestation.regionCode ? ` · ${manifestation.regionCode}` : ""}
                      </li>
                    ))}
                  </ul>
                </section>
              ) : null}

              {work.identifiers.length ? (
                <section>
                  <h3>Identifiers</h3>
                  <ul className="plain-list">
                    {work.identifiers.map((identifier) => {
                      const url = externalUrl(
                        identifier.scheme,
                        identifier.value,
                        identifier.url,
                      );
                      return (
                        <li key={`${identifier.scheme}:${identifier.value}`}>
                          {url ? (
                            <a
                              href={url}
                              target="_blank"
                              rel="noreferrer"
                              onClick={(event) => event.stopPropagation()}
                            >
                              {schemeLabel(identifier.scheme)}: {identifier.value}
                            </a>
                          ) : (
                            <>
                              {schemeLabel(identifier.scheme)}: {identifier.value}
                            </>
                          )}
                        </li>
                      );
                    })}
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
