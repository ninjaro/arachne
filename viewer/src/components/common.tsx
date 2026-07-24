import type {
  ConceptAssignment,
  EntityId,
  RatingValue,
  Ratings,
  Work,
} from "../lib/types";
import { dateLabel, humanize } from "../lib/format";

export type OpenHandler = (id: EntityId) => void;
export type RateHandler = (id: EntityId, value: RatingValue) => void;

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
    <div className="rating-buttons" onClick={(event) => event.stopPropagation()}>
      <button
        type="button"
        className={ratings[work.id] === 1 ? "rate like active" : "rate like"}
        onClick={() => onRate(work.id, 1)}
        aria-label={`Like ${work.label}`}
        aria-pressed={ratings[work.id] === 1}
      >
        +
      </button>
      <button
        type="button"
        className={ratings[work.id] === -1 ? "rate dislike active" : "rate dislike"}
        onClick={() => onRate(work.id, -1)}
        aria-label={`Dislike ${work.label}`}
        aria-pressed={ratings[work.id] === -1}
      >
        −
      </button>
    </div>
  );
}

export function ConceptChips({
  concepts,
  limit = 6,
}: {
  concepts: ConceptAssignment[];
  limit?: number;
}) {
  const visible = concepts.slice(0, limit);
  return (
    <div className="chips">
      {visible.map((concept) => (
        <span className="chip" key={concept.id} title={humanize(concept.conceptType)}>
          {concept.label}
        </span>
      ))}
      {concepts.length > visible.length ? (
        <span className="chip muted-chip">+{concepts.length - visible.length}</span>
      ) : null}
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
  return (
    <div className="pagination">
      <span>{total.toLocaleString()} results</span>
      <button type="button" disabled={page <= 1} onClick={() => onPage(page - 1)}>
        Previous
      </button>
      <span>
        {page} / {pageCount}
      </span>
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
