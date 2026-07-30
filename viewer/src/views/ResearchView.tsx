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
import { Pagination } from "../components/common";
import {
  matchesResearchQuery,
  parseQuery,
  queryDiagnostics,
} from "../lib/query";

type KindFilter = ResearchKind | "all";
type SeverityFilter = ResearchSeverity | "all";
type ResearchSort = "severity" | "quality" | "similarity" | "title";
type ResearchLayout = "list" | "grid";

const PAGE_SIZES = [25, 50, 100];
const SEVERITY_RANK: Record<ResearchSeverity, number> = {
  problem: 0,
  weak: 1,
  info: 2,
};

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
  const qualityWidth = Math.max(0, Math.min(100, item.score ?? 0));
  const similarityWidth = Math.max(
    0,
    Math.min(100, (item.similarityScore ?? 0) * 100),
  );

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
          <span style={{ width: `${qualityWidth}%` }} />
          <strong>{item.score}/100</strong>
        </div>
      ) : null}

      {item.similarityScore !== undefined ? (
        <div
          className="quality-meter"
          aria-label={`Similarity ${similarityWidth.toFixed(1)} percent`}
        >
          <span style={{ width: `${similarityWidth}%` }} />
          <strong>{similarityWidth.toFixed(1)}%</strong>
        </div>
      ) : null}

      {item.batchId ||
      item.jsonPath ||
      (item.leftId && item.rightId) ||
      item.details?.length ||
      item.value !== undefined ||
      item.signals !== undefined ? (
        <details className="research-card-more">
          <summary>
            Details
            {item.details?.length ? ` · ${item.details.length}` : ""}
          </summary>
          <div className="research-card-more-body">
            {item.batchId || item.jsonPath ? (
              <p className="research-field">
                {item.batchId ? (
                  <>
                    Batch: <code>{item.batchId}</code>
                  </>
                ) : null}
                {item.batchId && item.jsonPath ? " · " : null}
                {item.jsonPath ? (
                  <>
                    Path: <code>{item.jsonPath}</code>
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
                {item.details.map((detail, index) => (
                  <li key={`${index}:${detail}`}>{detail}</li>
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
          </div>
        </details>
      ) : null}
    </article>
  );
}

function sortResearch(items: ResearchItem[], sort: ResearchSort): ResearchItem[] {
  return [...items].sort((left, right) => {
    if (sort === "quality") {
      return (
        (left.score ?? Number.MAX_SAFE_INTEGER) -
          (right.score ?? Number.MAX_SAFE_INTEGER) ||
        SEVERITY_RANK[left.severity] - SEVERITY_RANK[right.severity] ||
        left.title.localeCompare(right.title) ||
        left.id.localeCompare(right.id)
      );
    }
    if (sort === "similarity") {
      return (
        (right.similarityScore ?? -1) - (left.similarityScore ?? -1) ||
        SEVERITY_RANK[left.severity] - SEVERITY_RANK[right.severity] ||
        left.title.localeCompare(right.title) ||
        left.id.localeCompare(right.id)
      );
    }
    if (sort === "title") {
      return left.title.localeCompare(right.title) || left.id.localeCompare(right.id);
    }
    return (
      SEVERITY_RANK[left.severity] - SEVERITY_RANK[right.severity] ||
      left.kind.localeCompare(right.kind) ||
      (right.similarityScore ?? -1) - (left.similarityScore ?? -1) ||
      (left.score ?? 101) - (right.score ?? 101) ||
      left.title.localeCompare(right.title) ||
      left.id.localeCompare(right.id)
    );
  });
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
  const [sort, setSort] = useState<ResearchSort>("severity");
  const [layout, setLayout] = useState<ResearchLayout>("list");
  const [page, setPage] = useState(1);
  const [pageSize, setPageSize] = useState(50);

  const categories = useMemo(
    () => [...new Set(data.items.map((item) => item.category))].sort(),
    [data.items],
  );
  const parsedQuery = useMemo(() => parseQuery(query), [query]);
  const queryErrors = useMemo(() => queryDiagnostics(query), [query]);

  const filtered = useMemo(() => {
    return data.items.filter((item) => {
      if (kind !== "all" && item.kind !== kind) return false;
      if (severity !== "all" && item.severity !== severity) return false;
      if (category !== "all" && item.category !== category) return false;
      const hasLinkedWork =
        (item.workId !== undefined && domain.workById.has(item.workId)) ||
        (item.entityType === "work" &&
          ((item.leftId !== undefined && domain.workById.has(item.leftId)) ||
            (item.rightId !== undefined && domain.workById.has(item.rightId))));
      if (linkedOnly && !hasLinkedWork) return false;
      return matchesResearchQuery(item, parsedQuery);
    });
  }, [
    data.items,
    parsedQuery,
    kind,
    severity,
    category,
    linkedOnly,
    domain.workById,
  ]);

  const visible = useMemo(() => sortResearch(filtered, sort), [filtered, sort]);
  const pageCount = Math.max(1, Math.ceil(visible.length / pageSize));
  const safePage = Math.min(Math.max(1, page), pageCount);
  const pageItems = visible.slice(
    (safePage - 1) * pageSize,
    safePage * pageSize,
  );

  function resetPage() {
    setPage(1);
  }

  const pagination = (
    <Pagination
      page={safePage}
      pageCount={pageCount}
      total={visible.length}
      pageSize={pageSize}
      pageSizeOptions={PAGE_SIZES}
      onPage={setPage}
      onPageSize={(next) => {
        setPageSize(next);
        resetPage();
      }}
    />
  );

  return (
    <section className="research-view">
      <div className="research-summary research-summary-actions">
        <button type="button" onClick={() => { setKind("all"); setSeverity("all"); resetPage(); }}>
          <strong>{data.summary.total}</strong><span>Total</span>
        </button>
        <button type="button" onClick={() => { setKind("quality_gap"); setSeverity("all"); resetPage(); }}>
          <strong>{data.summary.qualityGaps}</strong><span>Quality gaps</span>
        </button>
        <button type="button" onClick={() => { setKind("ingest_issue"); setSeverity("all"); resetPage(); }}>
          <strong>{data.summary.ingestIssues}</strong><span>Ingest issues</span>
        </button>
        <button type="button" onClick={() => { setKind("merge_hint"); setSeverity("all"); resetPage(); }}>
          <strong>{data.summary.mergeHints}</strong><span>Merge hints</span>
        </button>
        <button className="problem" type="button" onClick={() => { setKind("all"); setSeverity("problem"); resetPage(); }}>
          <strong>{data.summary.problems}</strong><span>Problems</span>
        </button>
      </div>

      <div className="research-filters">
        <input
          type="search"
          value={query}
          placeholder='Search or use severity:problem quality:<40'
          onChange={(event) => {
            setQuery(event.target.value);
            resetPage();
          }}
        />
        <select
          value={kind}
          onChange={(event) => {
            setKind(event.target.value as KindFilter);
            resetPage();
          }}
        >
          <option value="all">All kinds</option>
          <option value="quality_gap">Quality gaps</option>
          <option value="ingest_issue">Ingest issues</option>
          <option value="merge_hint">Merge hints</option>
        </select>
        <select
          value={severity}
          onChange={(event) => {
            setSeverity(event.target.value as SeverityFilter);
            resetPage();
          }}
        >
          <option value="all">All severities</option>
          <option value="problem">Problem</option>
          <option value="weak">Weak</option>
          <option value="info">Info</option>
        </select>
        <select
          value={category}
          onChange={(event) => {
            setCategory(event.target.value);
            resetPage();
          }}
        >
          <option value="all">All categories</option>
          {categories.map((value) => (
            <option value={value} key={value}>{humanize(value)}</option>
          ))}
        </select>
        <select
          value={sort}
          onChange={(event) => {
            setSort(event.target.value as ResearchSort);
            resetPage();
          }}
        >
          <option value="severity">Severity</option>
          <option value="quality">Lowest quality</option>
          <option value="similarity">Highest similarity</option>
          <option value="title">Title</option>
        </select>
        <label className="research-checkbox">
          <input
            type="checkbox"
            checked={linkedOnly}
            onChange={(event) => {
              setLinkedOnly(event.target.checked);
              resetPage();
            }}
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
            setSort("severity");
            resetPage();
          }}
        >
          Clear
        </button>
      </div>

      {queryErrors.length ? (
        <div className="query-error" role="alert">
          {queryErrors.join(" · ")}
        </div>
      ) : null}

      <details className="advanced-search">
        <summary>Research search syntax</summary>
        <p>
          Terms use AND. Supported fields: kind, severity, category, batch, path,
          entity, id, work, title, quality and similarity. Quotes, negation,
          <code> word:</code> and <code>regex:/.../i</code> work here too.
        </p>
      </details>

      <div className="research-view-controls">
        <p className="research-count">
          Showing {pageItems.length.toLocaleString()} items on page {safePage} of{" "}
          {pageCount}; {visible.length.toLocaleString()} match the filters.
        </p>
        <div
          className="research-layout-toggle"
          role="group"
          aria-label="Research result layout"
        >
          <button
            type="button"
            className={layout === "list" ? "active" : ""}
            aria-pressed={layout === "list"}
            onClick={() => setLayout("list")}
          >
            List
          </button>
          <button
            type="button"
            className={layout === "grid" ? "active" : ""}
            aria-pressed={layout === "grid"}
            onClick={() => setLayout("grid")}
          >
            Grid
          </button>
        </div>
      </div>

      {pagination}
      <div className={`research-list ${layout}`}>
        {pageItems.map((item) => (
          <ResearchCard item={item} domain={domain} onOpen={onOpen} key={item.id} />
        ))}
      </div>
      {pagination}

      {!visible.length ? (
        <div className="empty">No research items match the current filters.</div>
      ) : null}
    </section>
  );
}
