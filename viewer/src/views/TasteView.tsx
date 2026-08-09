import { useMemo, useState } from "react";
import type { OpenHandler, RateHandler } from "../components/common";
import { EntityRatingButtons } from "../components/common";
import { humanize } from "../lib/format";
import { resolveStaticDemoTasteProfile } from "../lib/demo-taste-profile";
import {
  exportTasteProfileJson,
  inferRatingFamily,
  importTasteProfileJson,
  mergeRatings,
} from "../lib/ratings";
import {
  inferConceptTaste,
  exportInterestProfileJson,
  resolveRatingFamily,
} from "../lib/taste";
import type { InferredConceptTaste, TasteIndex } from "../lib/taste";
import type {
  Domain,
  EntityId,
  RatingFamily,
  RatingValue,
  Ratings,
} from "../lib/types";

type RatingFilter = "all" | "positive" | "negative";
type InferredSort = "strongest" | "positive" | "negative";

interface RatedRow {
  id: EntityId;
  family: RatingFamily;
  label: string;
  detail: string;
  value: RatingValue;
}

function matchesRatingFilter(value: RatingValue, filter: RatingFilter): boolean {
  return filter === "all" || (filter === "positive" ? value === 1 : value === -1);
}

function familyRows(domain: Domain, ratings: Ratings): {
  works: RatedRow[];
  agents: RatedRow[];
  concepts: RatedRow[];
  unavailable: RatedRow[];
} {
  const result = {
    works: [] as RatedRow[],
    agents: [] as RatedRow[],
    concepts: [] as RatedRow[],
    unavailable: [] as RatedRow[],
  };
  for (const [id, value] of Object.entries(ratings)) {
    const family = resolveRatingFamily(domain, id);
    if (family === "work") {
      const work = domain.workById.get(id)!;
      result.works.push({ id, family, label: work.label, detail: humanize(work.medium), value });
    } else if (family === "agent") {
      const agent = domain.agentById.get(id)!;
      result.agents.push({ id, family, label: agent.label, detail: humanize(agent.agentType), value });
    } else if (family === "concept") {
      const concept = domain.conceptById.get(id)!;
      result.concepts.push({ id, family, label: concept.label, detail: humanize(concept.conceptType), value });
    } else {
      result.unavailable.push({
        id,
        family: id.startsWith("agent-") ? "agent" : id.startsWith("concept-") ? "concept" : "work",
        label: id,
        detail: "Not present in this product snapshot",
        value,
      });
    }
  }
  for (const rows of Object.values(result)) {
    rows.sort((left, right) => left.label.localeCompare(right.label) || left.id.localeCompare(right.id));
  }
  return result;
}

export function demoTasteRatings(domain: Domain): Ratings {
  return resolveStaticDemoTasteProfile(domain);
}

function downloadJson(contents: string, filename = "arachne-taste-profile.json") {
  const blob = new Blob([contents], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = filename;
  anchor.click();
  URL.revokeObjectURL(url);
}

function ProfileSection({
  title,
  rows,
  ratings,
  filter,
  readOnly,
  onOpen,
  onRate,
}: {
  title: string;
  rows: RatedRow[];
  ratings: Ratings;
  filter: RatingFilter;
  readOnly: boolean;
  onOpen?: OpenHandler;
  onRate: RateHandler;
}) {
  const visible = rows.filter((row) => matchesRatingFilter(row.value, filter));
  return (
    <section className="taste-section">
      <div className="section-heading">
        <h2>{title}</h2>
        <span>{visible.length.toLocaleString()}</span>
      </div>
      {visible.length ? (
        <div className="taste-rating-list">
          {visible.map((row) => (
            <div className="taste-rating-row" key={row.id}>
              <button
                type="button"
                className="taste-entity-link"
                disabled={!onOpen}
                onClick={() => onOpen?.(row.id)}
              >
                <strong>{row.label}</strong>
                <span>{row.detail}</span>
              </button>
              <span className={row.value === 1 ? "taste-value positive" : "taste-value negative"}>
                {row.value === 1 ? "+" : "−"}
              </span>
              {!readOnly ? (
                <EntityRatingButtons id={row.id} label={row.label} ratings={ratings} onRate={onRate} />
              ) : null}
            </div>
          ))}
        </div>
      ) : <p className="empty compact">No ratings in this section.</p>}
    </section>
  );
}

function InferredPreferenceRow({
  preference,
  ratings,
  readOnly,
  onOpenConcept,
  onRate,
}: {
  preference: InferredConceptTaste;
  ratings: Ratings;
  readOnly: boolean;
  onOpenConcept?: (id: EntityId) => void;
  onRate: RateHandler;
}) {
  const explicit = ratings[preference.conceptId];
  return (
    <div className="taste-inferred-row">
      <button
        type="button"
        className="taste-entity-link"
        disabled={!onOpenConcept}
        onClick={() => onOpenConcept?.(preference.conceptId)}
      >
        <strong>{preference.label}</strong>
        <span>{humanize(preference.conceptType)}</span>
      </button>
      <div className="taste-score">
        <strong>{preference.score >= 0 ? "+" : ""}{preference.score.toFixed(2)}</strong>
        <span>Inferred projection</span>
      </div>
      <div className="taste-evidence">
        {preference.evidence.slice(0, 3).map((evidence) => (
          <span key={`${evidence.family}:${evidence.entityId}`}>
            {evidence.rating === 1 ? "+" : "−"} {evidence.label}
          </span>
        ))}
      </div>
      <div className="taste-explicit">
        <span>Explicit: {explicit === 1 ? "+" : explicit === -1 ? "−" : "unrated"}</span>
        {!readOnly ? (
          <EntityRatingButtons
            id={preference.conceptId}
            label={preference.label}
            ratings={ratings}
            onRate={onRate}
          />
        ) : null}
      </div>
    </div>
  );
}

export function TasteView({
  domain,
  ratings,
  productSnapshotId,
  tasteIndex = null,
  onOpen,
  onOpenConcept,
  onRate,
  onReplaceRatings,
}: {
  domain: Domain;
  ratings: Ratings;
  productSnapshotId: string;
  tasteIndex?: TasteIndex | null;
  onOpen: OpenHandler;
  onOpenConcept?: (id: EntityId) => void;
  onRate: RateHandler;
  onReplaceRatings: (ratings: Ratings) => void;
}) {
  const [filter, setFilter] = useState<RatingFilter>("all");
  const [inferredSort, setInferredSort] = useState<InferredSort>("strongest");
  const [conceptType, setConceptType] = useState("");
  const [demo, setDemo] = useState<Ratings | null>(null);
  const [importMessage, setImportMessage] = useState("");
  const displayRatings = demo ?? ratings;
  const staticDemoRatings = useMemo(() => demoTasteRatings(domain), [domain]);
  const rows = useMemo(() => familyRows(domain, displayRatings), [domain, displayRatings]);
  const inferred = useMemo(
    () => inferConceptTaste(domain, displayRatings, tasteIndex),
    [domain, displayRatings, tasteIndex],
  );
  const conceptTypes = useMemo(() => [...new Set(inferred.map((item) => item.conceptType))]
    .sort((left, right) => left.localeCompare(right)), [inferred]);
  const visibleInferred = useMemo(() => inferred
    .filter((item) => !conceptType || item.conceptType === conceptType)
    .sort((left, right) => {
      if (inferredSort === "positive") return right.score - left.score || left.label.localeCompare(right.label);
      if (inferredSort === "negative") return left.score - right.score || left.label.localeCompare(right.label);
      return Math.abs(right.score) - Math.abs(left.score) || right.score - left.score || left.label.localeCompare(right.label);
    }), [conceptType, inferred, inferredSort]);

  const values = Object.values(displayRatings);
  const positive = values.filter((value) => value === 1).length;
  const negative = values.filter((value) => value === -1).length;
  const familyCount = (family: "works" | "agents" | "concepts", value: RatingValue) =>
    rows[family].filter((row) => row.value === value).length;
  const hasInterestSignals = inferred.length > 0 || Object.keys(ratings).some((id) => {
    const family = resolveRatingFamily(domain, id);
    return family === "agent" || family === "concept";
  });
  const demoAvailable = Object.keys(staticDemoRatings).length > 0;

  return (
    <div className="taste-view">
      <section className="taste-intro">
        <div>
          <h2>{demo ? "Example taste profile" : "Taste"}</h2>
          <p>
            {demo
              ? "Read-only example data. It is not mixed with ratings stored in this browser."
              : "An anonymous profile stored only in this browser. No login or remote account is used."}
          </p>
        </div>
        <div className="taste-actions">
          {demo ? (
            <button type="button" onClick={() => setDemo(null)}>Return to my data</button>
          ) : (
            <button type="button" disabled={!demoAvailable} onClick={() => setDemo(staticDemoRatings)}>
              Load demo profile
            </button>
          )}
          <button
            type="button"
            disabled={Boolean(demo) || !values.length}
            onClick={() => downloadJson(exportTasteProfileJson(
              ratings,
              productSnapshotId,
              (id) => resolveRatingFamily(domain, id) ?? inferRatingFamily(id),
            ))}
          >Export JSON</button>
          <button
            type="button"
            disabled={Boolean(demo) || !hasInterestSignals}
            onClick={() => downloadJson(
              exportInterestProfileJson(domain, ratings, inferred, productSnapshotId),
              "arachne-interest-profile.json",
            )}
          >Export interest JSON</button>
          <label className={demo ? "button-label disabled" : "button-label"}>
            Import JSON
            <input
              type="file"
              accept="application/json,.json"
              disabled={Boolean(demo)}
              onChange={async (event) => {
                const file = event.target.files?.[0];
                event.target.value = "";
                if (!file) return;
                try {
                  if (file.size > 8 * 1024 * 1024) {
                    throw new Error("Taste profile import is limited to 8 MiB");
                  }
                  const imported = importTasteProfileJson(
                    await file.text(),
                    productSnapshotId,
                    (id) => resolveRatingFamily(domain, id),
                  );
                  onReplaceRatings(mergeRatings(ratings, imported.ratings));
                  setImportMessage([
                    `Imported ${imported.accepted} ratings`,
                    imported.ignored.length ? `${imported.ignored.length} ignored` : "",
                    imported.snapshotMismatch ? "source snapshot differs" : "",
                  ].filter(Boolean).join(" · "));
                } catch (cause) {
                  setImportMessage(cause instanceof Error ? cause.message : String(cause));
                }
              }}
            />
          </label>
          <button
            type="button"
            disabled={Boolean(demo) || !Object.keys(ratings).length}
            onClick={() => {
              if (window.confirm("Reset all local taste ratings?")) onReplaceRatings({});
            }}
          >Reset</button>
        </div>
      </section>

      {importMessage ? <p className="taste-import-status" role="status">{importMessage}</p> : null}

      <section className="taste-summary" aria-label="Rating totals">
        <div><strong>{values.length}</strong><span>Total</span></div>
        <div><strong>{positive}</strong><span>Positive</span></div>
        <div><strong>{negative}</strong><span>Negative</span></div>
        <div><strong>{rows.works.length}</strong><span>Works · {familyCount("works", 1)}+ / {familyCount("works", -1)}−</span></div>
        <div><strong>{rows.agents.length}</strong><span>Agents · {familyCount("agents", 1)}+ / {familyCount("agents", -1)}−</span></div>
        <div><strong>{rows.concepts.length}</strong><span>Tags · {familyCount("concepts", 1)}+ / {familyCount("concepts", -1)}−</span></div>
      </section>

      <div className="taste-filter" role="group" aria-label="Filter explicit ratings">
        {(["all", "positive", "negative"] as const).map((value) => (
          <button
            type="button"
            className={filter === value ? "active" : ""}
            aria-pressed={filter === value}
            key={value}
            onClick={() => setFilter(value)}
          >{humanize(value)}</button>
        ))}
      </div>

      <div className="taste-sections">
        <ProfileSection title="Rated works" rows={rows.works} ratings={displayRatings} filter={filter} readOnly={Boolean(demo)} onOpen={onOpen} onRate={onRate} />
        <ProfileSection title="Rated agents" rows={rows.agents} ratings={displayRatings} filter={filter} readOnly={Boolean(demo)} onOpen={onOpen} onRate={onRate} />
        <ProfileSection title="Rated tags" rows={rows.concepts} ratings={displayRatings} filter={filter} readOnly={Boolean(demo)} onOpen={onOpenConcept} onRate={onRate} />
      </div>

      {rows.unavailable.length ? (
        <ProfileSection title="Unavailable in this snapshot" rows={rows.unavailable} ratings={displayRatings} filter={filter} readOnly={Boolean(demo)} onRate={onRate} />
      ) : null}

      <section className="taste-inferred">
        <div className="section-heading">
          <div>
            <h2>Inferred concept preference</h2>
            <p>A live, derived projection from rated works and indexed agent affinities. Explicit tag ratings remain separate.</p>
          </div>
          <div className="taste-inferred-controls">
            <select value={inferredSort} onChange={(event) => setInferredSort(event.target.value as InferredSort)} aria-label="Sort inferred preferences">
              <option value="strongest">Strongest either way</option>
              <option value="positive">Most positive</option>
              <option value="negative">Most negative</option>
            </select>
            <select value={conceptType} onChange={(event) => setConceptType(event.target.value)} aria-label="Inferred concept type">
              <option value="">All useful concept types</option>
              {conceptTypes.map((type) => <option value={type} key={type}>{humanize(type)}</option>)}
            </select>
          </div>
        </div>
        {visibleInferred.length ? (
          <div className="taste-inferred-list">
            {visibleInferred.slice(0, 100).map((preference) => (
              <InferredPreferenceRow
                key={preference.conceptId}
                preference={preference}
                ratings={displayRatings}
                readOnly={Boolean(demo)}
                onOpenConcept={onOpenConcept}
                onRate={onRate}
              />
            ))}
          </div>
        ) : <p className="empty compact">Rate works or indexed agents to see inferred concept preferences.</p>}
      </section>
    </div>
  );
}
