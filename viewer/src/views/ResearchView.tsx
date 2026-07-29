import { useMemo, useState } from "react";
import type {
  Domain,
  ResearchData,
  ResearchItem,
  ResearchKind,
  ResearchSeverity,
} from "../lib/types";
import { humanize } from "../lib/format";
import type { OpenHandler } from "../components/common";

type KindFilter = ResearchKind | "all";
type SeverityFilter = ResearchSeverity | "all";

function rawValue(value: unknown): string {
  if (typeof value === "string") return value;
  return JSON.stringify(value, null, 2);
}

function ResearchCard({
  item,
  domain,
  onOpen,
}: {
  item: ResearchItem;
  domain: Domain;
  onOpen: OpenHandler;
}) {
  const linkedWork = item.workId ? domain.workById.get(item.workId) : undefined;
  const leftWork =
    item.entityType === "work" && item.leftId
      ? domain.workById.get(item.leftId)
      : undefined;
  const rightWork =
    item.entityType === "work" && item.rightId
      ? domain.workById.get(item.rightId)
      : undefined;

  return (
    <article className={`research-card severity-${item.severity}`}>
      <header className="research-card-header">
        <div>
          <div className="research-badges">
            <span className={`research-badge kind-${item.kind}`}>
              {humanize(item.kind)}
            </span>
            <span className={`research-badge severity-${item.severity}`}>
              {humanize(item.severity)}
            </span>
            <span className="research-badge">{humanize(item.category)}</span>
          </div>
          <h3>{item.title}</h3>
          <p>{item.message}</p>
        </div>
        {linkedWork || leftWork || rightWork ? (
          <div>
            {linkedWork ? (
              <button type="button" onClick={() => onOpen(linkedWork.id)}>
                Open work
              </button>
            ) : null}
            {leftWork ? (
              <button type="button" onClick={() => onOpen(leftWork.id)}>
                Open left work
              </button>
            ) : null}
            {rightWork ? (
              <button type="button" onClick={() => onOpen(rightWork.id)}>
                Open right work
              </button>
            ) : null}
          </div>
        ) : null}
      </header>

      {item.score !== undefined ? (
        <div
          className="quality-meter"
          aria-label={`Metadata quality ${item.score} out of 100`}
        >
          <span style={{ width: `${item.score}%` }} />
          <strong>{item.score}/100</strong>
        </div>
      ) : null}

      {item.similarityScore !== undefined ? (
        <div
          className="quality-meter"
          aria-label={`Similarity ${(item.similarityScore * 100).toFixed(1)} percent`}
        >
          <span style={{ width: `${item.similarityScore * 100}%` }} />
          <strong>{(item.similarityScore * 100).toFixed(1)}%</strong>
        </div>
      ) : null}

      {item.batchId ? (
        <p className="research-field">
          Batch: <code>{item.batchId}</code>
          {item.jsonPath ? (
            <>
              {" · "}Path: <code>{item.jsonPath}</code>
            </>
          ) : null}
        </p>
      ) : null}

      {item.leftId && item.rightId ? (
        <ul className="research-details">
          <li>
            Left: {item.leftLabel ?? item.leftId} (<code>{item.leftId}</code>)
          </li>
          <li>
            Right: {item.rightLabel ?? item.rightId} (<code>{item.rightId}</code>)
          </li>
          {item.textScore !== undefined && item.textScore !== null ? (
            <li>Text score: {(item.textScore * 100).toFixed(1)}%</li>
          ) : null}
          {item.graphScore !== undefined && item.graphScore !== null ? (
            <li>Graph score: {(item.graphScore * 100).toFixed(1)}%</li>
          ) : null}
          {item.contextScore !== undefined && item.contextScore !== null ? (
            <li>Context score: {(item.contextScore * 100).toFixed(1)}%</li>
          ) : null}
        </ul>
      ) : null}

      {item.details?.length ? (
        <ul className="research-details">
          {item.details.map((detail) => (
            <li key={detail}>{detail}</li>
          ))}
        </ul>
      ) : null}

      {item.value !== undefined ? (
        <details>
          <summary>Rejected value</summary>
          <pre>{rawValue(item.value)}</pre>
        </details>
      ) : null}

      {item.signals !== undefined ? (
        <details>
          <summary>Merge signals</summary>
          <pre>{JSON.stringify(item.signals, null, 2)}</pre>
        </details>
      ) : null}
    </article>
  );
}

export function ResearchView({
  data,
  domain,
  onOpen,
}: {
  data: ResearchData;
  domain: Domain;
  onOpen: OpenHandler;
}) {
  const [query, setQuery] = useState("");
  const [kind, setKind] = useState<KindFilter>("all");
  const [severity, setSeverity] = useState<SeverityFilter>("all");
  const [category, setCategory] = useState("all");
  const [linkedOnly, setLinkedOnly] = useState(false);
  const [limit, setLimit] = useState(100);

  const categories = useMemo(
    () => [...new Set(data.items.map((item) => item.category))].sort(),
    [data.items],
  );

  const visible = useMemo(() => {
    const normalized = query.trim().toLocaleLowerCase();
    return data.items.filter((item) => {
      if (kind !== "all" && item.kind !== kind) return false;
      if (severity !== "all" && item.severity !== severity) return false;
      if (category !== "all" && item.category !== category) return false;
      const hasLinkedWork =
        (item.workId !== undefined && domain.workById.has(item.workId)) ||
        (item.entityType === "work" &&
          ((item.leftId !== undefined && domain.workById.has(item.leftId)) ||
            (item.rightId !== undefined && domain.workById.has(item.rightId))));
      if (linkedOnly && !hasLinkedWork) {
        return false;
      }
      if (!normalized) return true;
      const haystack = [
        item.title,
        item.message,
        item.category,
        item.batchId ?? "",
        item.jsonPath ?? "",
        item.workId ?? "",
        item.workLabel ?? "",
        item.leftId ?? "",
        item.leftLabel ?? "",
        item.rightId ?? "",
        item.rightLabel ?? "",
        ...(item.details ?? []),
      ]
        .join(" ")
        .toLocaleLowerCase();
      return haystack.includes(normalized);
    });
  }, [data.items, query, kind, severity, category, linkedOnly, domain.workById]);

  return (
    <section className="research-view">
      <div className="research-summary">
        <div><strong>{data.summary.total}</strong><span>Total</span></div>
        <div><strong>{data.summary.qualityGaps}</strong><span>Quality gaps</span></div>
        <div><strong>{data.summary.ingestIssues}</strong><span>Ingest issues</span></div>
        <div><strong>{data.summary.mergeHints}</strong><span>Merge hints</span></div>
        <div className="problem"><strong>{data.summary.problems}</strong><span>Problems</span></div>
      </div>

      <div className="research-filters">
        <input
          type="search"
          value={query}
          placeholder="Search research queue"
          onChange={(event) => {
            setQuery(event.target.value);
            setLimit(100);
          }}
        />
        <select value={kind} onChange={(event) => setKind(event.target.value as KindFilter)}>
          <option value="all">All kinds</option>
          <option value="quality_gap">Quality gaps</option>
          <option value="ingest_issue">Ingest issues</option>
          <option value="merge_hint">Merge hints</option>
        </select>
        <select
          value={severity}
          onChange={(event) => setSeverity(event.target.value as SeverityFilter)}
        >
          <option value="all">All severities</option>
          <option value="problem">Problem</option>
          <option value="weak">Weak</option>
          <option value="info">Info</option>
        </select>
        <select value={category} onChange={(event) => setCategory(event.target.value)}>
          <option value="all">All categories</option>
          {categories.map((value) => (
            <option value={value} key={value}>{humanize(value)}</option>
          ))}
        </select>
        <label className="research-checkbox">
          <input
            type="checkbox"
            checked={linkedOnly}
            onChange={(event) => setLinkedOnly(event.target.checked)}
          />
          Linked works only
        </label>
        <button
          type="button"
          onClick={() => {
            setQuery("");
            setKind("all");
            setSeverity("all");
            setCategory("all");
            setLinkedOnly(false);
            setLimit(100);
          }}
        >
          Clear
        </button>
      </div>

      <p className="research-count">
        Showing {Math.min(limit, visible.length).toLocaleString()} of{" "}
        {visible.length.toLocaleString()} matching items.
      </p>

      <div className="research-list">
        {visible.slice(0, limit).map((item) => (
          <ResearchCard item={item} domain={domain} onOpen={onOpen} key={item.id} />
        ))}
      </div>

      {limit < visible.length ? (
        <button
          type="button"
          className="load-more"
          onClick={() => setLimit((current) => current + 100)}
        >
          Show 100 more
        </button>
      ) : null}

      {!visible.length ? (
        <div className="empty">No research items match the current filters.</div>
      ) : null}
    </section>
  );
}
