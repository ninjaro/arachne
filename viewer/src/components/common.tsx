import { useEffect, useState } from "react";
import type {
  ConceptAssignment,
  EntityOpenContext,
  EntityId,
  RatingValue,
  Ratings,
  Work,
} from "../lib/types";
import { centralityScaleLabel, dateLabel, humanize } from "../lib/format";

export type OpenHandler = (
  id: EntityId,
  context?: EntityOpenContext,
) => void;
export type RateHandler = (id: EntityId, value: RatingValue) => void;

export function EntityRatingButtons({
  id,
  label,
  ratings,
  onRate,
}: {
  id: EntityId;
  label: string;
  ratings: Ratings;
  onRate: RateHandler;
}) {
  return (
    <div className="rating-buttons" onClick={(event) => event.stopPropagation()}>
      <button
        type="button"
        className={ratings[id] === 1 ? "rate like active" : "rate like"}
        onClick={() => onRate(id, 1)}
        aria-label={`Like ${label}`}
        aria-pressed={ratings[id] === 1}
      >
        +
      </button>
      <button
        type="button"
        className={ratings[id] === -1 ? "rate dislike active" : "rate dislike"}
        onClick={() => onRate(id, -1)}
        aria-label={`Dislike ${label}`}
        aria-pressed={ratings[id] === -1}
      >
        −
      </button>
    </div>
  );
}

export function RatingButtons({
  work,
  ratings,
  onRate,
}: {
  work: Work;
  ratings: Ratings;
  onRate: RateHandler;
}) {
  return (
    <EntityRatingButtons
      id={work.id}
      label={work.label}
      ratings={ratings}
      onRate={onRate}
    />
  );
}

export function ConceptChips({
  concepts,
  limit = 6,
  onFilter,
}: {
  concepts: ConceptAssignment[];
  limit?: number;
  onFilter?: (concept: ConceptAssignment) => void;
}) {
  const visible = [...concepts]
    .sort(
      (left, right) =>
        (right.centrality ?? 0) - (left.centrality ?? 0) ||
        (right.confidence ?? 0) - (left.confidence ?? 0) ||
        left.label.localeCompare(right.label),
    )
    .slice(0, limit);

  return (
    <div className="chips">
      {visible.map((concept) => {
        const title = `${humanize(concept.conceptType)} · centrality ${concept.centrality ?? "unknown"} · ${centralityScaleLabel(concept.centralityScale)}`;
        return (
          onFilter ? (
            <button
              type="button"
              className="chip chip-button"
              key={concept.id}
              title={`${title} · filter by this concept`}
              onClick={(event) => {
                event.stopPropagation();
                onFilter(concept);
              }}
            >
              {concept.label}
            </button>
          ) : (
            <span className="chip" key={concept.id} title={title}>
              {concept.label}
            </span>
          )
        );
      })}
      {concepts.length > visible.length ? (
        <span className="chip muted-chip">+{concepts.length - visible.length}</span>
      ) : null}
    </div>
  );
}

export function GroupedConceptChips({
  concepts,
  onFilter,
}: {
  concepts: ConceptAssignment[];
  onFilter?: (concept: ConceptAssignment) => void;
}) {
  const groups = new Map<string, ConceptAssignment[]>();
  for (const concept of concepts) {
    const group = groups.get(concept.conceptType);
    if (group) group.push(concept);
    else groups.set(concept.conceptType, [concept]);
  }

  return (
    <div className="concept-groups">
      {[...groups.entries()]
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([type, items]) => (
          <section className="concept-group" key={type}>
            <h4>{humanize(type)}</h4>
            <ConceptChips concepts={items} limit={30} onFilter={onFilter} />
          </section>
        ))}
    </div>
  );
}

export function WorkSummary({ work }: { work: Work }) {
  return (
    <>
      <span className="work-date">{dateLabel(work)}</span>
      <span className="work-label">{work.label}</span>
      <span className="work-medium">{humanize(work.medium)}</span>
    </>
  );
}

export function Pagination({
  page,
  pageCount,
  total,
  pageSize,
  pageSizeOptions,
  onPage,
  onPageSize,
}: {
  page: number;
  pageCount: number;
  total: number;
  pageSize: number;
  pageSizeOptions: number[];
  onPage: (page: number) => void;
  onPageSize: (size: number) => void;
}) {
  const [requestedPage, setRequestedPage] = useState(String(page));

  useEffect(() => setRequestedPage(String(page)), [page]);

  return (
    <div className="pagination">
      <span>{total.toLocaleString()} results</span>
      <button type="button" disabled={page <= 1} onClick={() => onPage(page - 1)}>
        Previous
      </button>
      <form
        className="page-jump"
        onSubmit={(event) => {
          event.preventDefault();
          const requested = Number(requestedPage);
          if (!Number.isFinite(requested)) return;
          onPage(Math.min(pageCount, Math.max(1, Math.trunc(requested))));
        }}
      >
        <input
          aria-label="Page number"
          type="number"
          min={1}
          max={pageCount}
          value={requestedPage}
          onChange={(event) => setRequestedPage(event.target.value)}
        />
        <span>/ {pageCount}</span>
        <button type="submit">Go</button>
      </form>
      <button
        type="button"
        disabled={page >= pageCount}
        onClick={() => onPage(page + 1)}
      >
        Next
      </button>
      <label>
        Page size{" "}
        <select value={pageSize} onChange={(event) => onPageSize(Number(event.target.value))}>
          {pageSizeOptions.map((value) => (
            <option key={value} value={value}>
              {value}
            </option>
          ))}
        </select>
      </label>
    </div>
  );
}
