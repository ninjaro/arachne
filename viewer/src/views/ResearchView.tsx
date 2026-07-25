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
        {linkedWork ? (
          <button type="button" onClick={() => onOpen(linkedWork.id)}>
            Open work
          </button>
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

      {item.field ? (
        <p className="research-field">
          Field: <code>{item.field}</code>
        </p>
      ) : null}

      {item.details?.length ? (
        <ul className="research-details">
          {item.details.map((detail) => (
            <li key={detail}>{detail}</li>
          ))}
        </ul>
      ) : null}

      {item.occurrences?.length ? (
        <div className="research-occurrences">
          {item.occurrences.map((occurrence, index) => (
            <details
              key={`${occurrence.source.container}:${occurrence.jsonPointer}:${index}`}
            >
              <summary>
                {occurrence.source.batchId ? `${occurrence.source.batchId} · ` : ""}
                {occurrence.source.container}
                {occurrence.source.member ? ` / ${occurrence.source.member}` : ""}
                {" · "}
                <code>{occurrence.jsonPointer}</code>
              </summary>
              {occurrence.value !== undefined ? (
                <pre>{rawValue(occurrence.value)}</pre>
              ) : null}
            </details>
          ))}
        </div>
      ) : null}

      {item.value !== undefined ? (
        <details>
          <summary>Preserved value</summary>
          <pre>{rawValue(item.value)}</pre>
        </details>
      ) : null}

      {item.dependencies?.length ? (
        <details>
          <summary>Dependencies ({item.dependencies.length})</summary>
          <pre>{JSON.stringify(item.dependencies, null, 2)}</pre>
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
      if (linkedOnly && (!item.workId || !domain.workById.has(item.workId))) {
        return false;
      }
      if (!normalized) return true;
      const haystack = [
        item.title,
        item.message,
        item.category,
        item.field ?? "",
        item.workId ?? "",
        item.workLabel ?? "",
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
        <div><strong>{data.summary.conflicts}</strong><span>Conflicts</span></div>
        <div><strong>{data.summary.remainders}</strong><span>Remainders</span></div>
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
          <option value="conflict">Conflicts</option>
          <option value="remainder">Remainders</option>
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
