import { useMemo } from "react";
import type { Domain, Ratings, Settings } from "../lib/types";
import type { BrowseFilters, BrowseSort } from "../lib/browse";
import type { FeatureIndex } from "../lib/features";
import {
  filterWorks,
  relevanceScores,
  sortWorks,
} from "../lib/browse";
import {
  ConceptChips,
  Pagination,
  RatingButtons,
} from "../components/common";
import type { OpenHandler, RateHandler } from "../components/common";
import { dateLabel, humanize } from "../lib/format";
import { buildQueryToken, queryDiagnostics } from "../lib/query";

export function BrowseView({
  domain,
  index,
  settings,
  ratings,
  filters,
  sort,
  page,
  pageSize,
  onFilters,
  onSort,
  onPage,
  onPageSize,
  onOpen,
  onRate,
}: {
  domain: Domain;
  index: FeatureIndex;
  settings: Settings;
  ratings: Ratings;
  filters: BrowseFilters;
  sort: BrowseSort;
  page: number;
  pageSize: number;
  onFilters: (filters: BrowseFilters) => void;
  onSort: (sort: BrowseSort) => void;
  onPage: (page: number) => void;
  onPageSize: (size: number) => void;
  onOpen: OpenHandler;
  onRate: RateHandler;
}) {
  const filtered = useMemo(
    () => filterWorks(domain, filters),
    [domain, filters],
  );
  const relevance = useMemo(
    () => relevanceScores(domain, index, filtered, filters),
    [domain, index, filtered, filters],
  );
  const visible = useMemo(
    () => sortWorks(filtered, sort, relevance),
    [filtered, sort, relevance],
  );
  const queryErrors = useMemo(
    () => queryDiagnostics(filters.query),
    [filters.query],
  );

  const pageCount = Math.max(1, Math.ceil(visible.length / pageSize));
  const safePage = Math.min(Math.max(1, page), pageCount);
  const pageItems = visible.slice(
    (safePage - 1) * pageSize,
    safePage * pageSize,
  );

  const setFilter = <K extends keyof BrowseFilters,>(
    key: K,
    value: BrowseFilters[K],
  ) => onFilters({ ...filters, [key]: value });

  const pagination = (
    <Pagination
      page={safePage}
      pageCount={pageCount}
      total={visible.length}
      pageSize={pageSize}
      pageSizeOptions={settings.browse.pageSizeOptions}
      onPage={onPage}
      onPageSize={onPageSize}
    />
  );

  return (
    <>
      <section className="filters sticky">
        <input
          type="search"
          value={filters.query}
          placeholder='Search or use agent:"Johnny Rotten" genre:punk'
          onChange={(event) => setFilter("query", event.target.value)}
          aria-label="Search catalog"
        />
        <input
          type="number"
          value={filters.minimumYear}
          placeholder="From year"
          onChange={(event) => setFilter("minimumYear", event.target.value)}
          aria-label="Minimum year"
        />
        <input
          type="number"
          value={filters.maximumYear}
          placeholder="Until year"
          onChange={(event) => setFilter("maximumYear", event.target.value)}
          aria-label="Maximum year"
        />
        <select
          value={filters.medium}
          onChange={(event) => setFilter("medium", event.target.value)}
          aria-label="Medium"
        >
          <option value="">All media</option>
          {domain.mediumOptions.map((option) => (
            <option value={option.value} key={option.value}>
              {humanize(option.value)} ({option.count})
            </option>
          ))}
        </select>
        <select
          value={filters.conceptId}
          onChange={(event) => setFilter("conceptId", event.target.value)}
          aria-label="Concept"
        >
          <option value="">All concepts</option>
          {domain.conceptOptions.map((concept) => (
            <option value={concept.id} key={concept.id}>
              {concept.label} ({concept.count})
            </option>
          ))}
        </select>
        <select
          value={sort}
          onChange={(event) => onSort(event.target.value as BrowseSort)}
          aria-label="Sort"
        >
          <option value="date">Date</option>
          <option value="label">Label</option>
          <option value="medium">Medium</option>
          <option
            value="relevance"
            disabled={!filters.query.trim() && !filters.conceptId}
          >
            Relevance
          </option>
        </select>
        <button
          type="button"
          onClick={() =>
            onFilters({
              query: "",
              minimumYear: "",
              maximumYear: "",
              medium: "",
              conceptId: "",
            })
          }
        >
          Clear
        </button>
      </section>

      {queryErrors.length ? (
        <div className="query-error" role="alert">
          {queryErrors.join(" · ")}
        </div>
      ) : null}

      <details className="advanced-search">
        <summary>Advanced search syntax</summary>
        <p>
          Terms are combined with AND. Quotes preserve phrases; a leading minus
          excludes a term.
        </p>
        <code>
          agent:&quot;Johnny Rotten&quot; genre:punk -guide:violence year:1976..1981
        </code>
        <p>
          Fields: title, agent, role, concept, genre, movement, theme, style,
          medium, country, lang, id, guide and year. Use <code>word:punk</code> for
          a whole word or <code>regex:/\bpost[- ]punk\b/i</code> for a regular
          expression.
        </p>
      </details>

      {pagination}
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Date</th>
              <th>Work</th>
              <th>Medium</th>
              <th>Concepts</th>
              <th>Rating</th>
            </tr>
          </thead>
          <tbody>
            {pageItems.map((work) => (
              <tr
                key={work.id}
                tabIndex={0}
                onClick={() => onOpen(work.id)}
                onKeyDown={(event) => {
                  if (event.key === "Enter" || event.key === " ") {
                    event.preventDefault();
                    onOpen(work.id);
                  }
                }}
              >
                <td className="date-cell">{dateLabel(work)}</td>
                <td className="label-cell">{work.label}</td>
                <td>
                  <button
                    type="button"
                    className="table-filter-link"
                    onClick={(event) => {
                      event.stopPropagation();
                      setFilter("medium", work.medium);
                    }}
                  >
                    {humanize(work.medium)}
                  </button>
                </td>
                <td>
                  <ConceptChips
                    concepts={work.concepts}
                    onFilter={(concept) =>
                      setFilter("query", buildQueryToken("concept", concept.label))
                    }
                  />
                </td>
                <td>
                  <RatingButtons
                    work={work}
                    ratings={ratings}
                    onRate={onRate}
                  />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {pagination}
    </>
  );
}
