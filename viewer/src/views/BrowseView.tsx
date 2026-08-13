import { useMemo, useState } from "react";
import type { Domain, Ratings, Settings, Work } from "../lib/types";
import type {
  AgentBrowseRow,
  BrowseFamily,
  BrowseFilters,
  BrowseSort,
} from "../lib/browse";
import type { FeatureIndex } from "../lib/features";
import {
  buildAgentBrowseRows,
  filterAgents,
  filterWorks,
  relevanceScores,
  sortAgents,
  sortWorks,
} from "../lib/browse";
import {
  ConceptChips,
  EntityRatingButtons,
  Pagination,
  RatingButtons,
} from "../components/common";
import type { OpenHandler, RateHandler } from "../components/common";
import { dateLabel, humanize } from "../lib/format";
import {
  appendQueryTerms,
  buildQueryToken,
  parseQuery,
  queryDiagnostics,
  removeQueryTermAt,
} from "../lib/query";

function openFromKeyboard(
  event: React.KeyboardEvent,
  id: string,
  onOpen: OpenHandler,
) {
  if (event.key === "Enter" || event.key === " ") {
    event.preventDefault();
    onOpen(id);
  }
}

function WorkTable({
  works,
  ratings,
  onOpen,
  onRate,
  onMedium,
  onConcept,
}: {
  works: Work[];
  ratings: Ratings;
  onOpen: OpenHandler;
  onRate: RateHandler;
  onMedium: (medium: string) => void;
  onConcept: (label: string) => void;
}) {
  return (
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
          {works.map((work) => (
            <tr
              key={work.id}
              tabIndex={0}
              onClick={() => onOpen(work.id)}
              onKeyDown={(event) => openFromKeyboard(event, work.id, onOpen)}
            >
              <td className="date-cell">{dateLabel(work)}</td>
              <td className="label-cell">{work.label}</td>
              <td>
                <button
                  type="button"
                  className="table-filter-link"
                  onClick={(event) => {
                    event.stopPropagation();
                    onMedium(work.medium);
                  }}
                >
                  {humanize(work.medium)}
                </button>
              </td>
              <td>
                <ConceptChips
                  concepts={work.concepts}
                  onFilter={(concept) => onConcept(concept.label)}
                />
              </td>
              <td>
                <RatingButtons work={work} ratings={ratings} onRate={onRate} />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function knownForLabel(row: AgentBrowseRow): string {
  if (!row.knownFor.length) return "No credited works in this snapshot";
  return row.knownFor.map((entry) => {
    const examples = entry.works.map((work) => work.label).join(", ");
    return `${humanize(entry.role)} (${entry.count})${examples ? ` · ${examples}` : ""}`;
  }).join("; ");
}

function AgentTable({
  rows,
  ratings,
  onOpen,
  onRate,
  onRole,
}: {
  rows: AgentBrowseRow[];
  ratings: Ratings;
  onOpen: OpenHandler;
  onRate: RateHandler;
  onRole: (role: string) => void;
}) {
  return (
    <div className="table-wrap">
      <table className="agent-table">
        <thead>
          <tr>
            <th>Name</th>
            <th>Type</th>
            <th>Known for</th>
            <th>Rating</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((row) => (
            <tr
              key={row.agent.id}
              tabIndex={0}
              onClick={() => onOpen(row.agent.id)}
              onKeyDown={(event) => openFromKeyboard(event, row.agent.id, onOpen)}
            >
              <td className="label-cell">{row.agent.label}</td>
              <td>{humanize(row.agent.agentType)}</td>
              <td className="known-for-cell">
                {row.knownFor.length ? row.knownFor.map((entry, index) => (
                  <span className="known-for-item" key={entry.role}>
                    {index ? " · " : null}
                    <button
                      type="button"
                      className="table-filter-link"
                      title={`Filter by ${humanize(entry.role)} credits`}
                      onClick={(event) => {
                        event.stopPropagation();
                        onRole(entry.role);
                      }}
                    >
                      {humanize(entry.role)} ({entry.count})
                    </button>
                    {entry.works.length ? ` — ${entry.works.map((work) => work.label).join(", ")}` : null}
                  </span>
                )) : <span className="muted">{knownForLabel(row)}</span>}
              </td>
              <td>
                <EntityRatingButtons
                  id={row.agent.id}
                  label={row.agent.label}
                  ratings={ratings}
                  onRate={onRate}
                />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export function BrowseView({
  domain,
  index,
  settings,
  ratings,
  filters,
  sort,
  page,
  pageSize,
  family,
  onFamily,
  onFilters,
  onSort,
  onPage,
  onPageSize,
  onOpen,
  onRate,
}: {
  domain: Domain;
  index: FeatureIndex | null;
  settings: Settings;
  ratings: Ratings;
  filters: BrowseFilters;
  sort: BrowseSort;
  page: number;
  pageSize: number;
  family?: BrowseFamily;
  onFamily?: (family: BrowseFamily) => void;
  onFilters: (filters: BrowseFilters) => void;
  onSort: (sort: BrowseSort) => void;
  onPage: (page: number) => void;
  onPageSize: (size: number) => void;
  onOpen: OpenHandler;
  onRate: RateHandler;
}) {
  const [localFamily, setLocalFamily] = useState<BrowseFamily>("all");
  const activeFamily = family ?? localFamily;
  const filteredWorks = useMemo(
    () => filterWorks(domain, filters),
    [domain, filters],
  );
  const relevance = useMemo(
    () => relevanceScores(domain, index, filteredWorks, filters),
    [domain, index, filteredWorks, filters],
  );
  const visibleWorks = useMemo(
    () => sortWorks(filteredWorks, sort, relevance),
    [filteredWorks, sort, relevance],
  );
  const agentRows = useMemo(() => buildAgentBrowseRows(domain), [domain]);
  const hasWorkOnlyFilter = Boolean(
    filters.minimumYear || filters.maximumYear || filters.medium || filters.conceptId,
  );
  const hideAgentsForWorkFilter = activeFamily === "all" && hasWorkOnlyFilter;
  const visibleAgents = useMemo(
    () => hideAgentsForWorkFilter
      ? []
      : sortAgents(filterAgents(agentRows, filters.query), Boolean(filters.query.trim())),
    [agentRows, filters.query, hideAgentsForWorkFilter],
  );
  const [draftQuery, setDraftQuery] = useState("");
  const [draftErrors, setDraftErrors] = useState<string[]>([]);
  const activeQuery = useMemo(() => parseQuery(filters.query), [filters.query]);
  const queryErrors = useMemo(
    () => [...queryDiagnostics(filters.query), ...draftErrors],
    [filters.query, draftErrors],
  );

  const counts = activeFamily === "work"
    ? [visibleWorks.length]
    : activeFamily === "agent"
      ? [visibleAgents.length]
      : [visibleWorks.length, visibleAgents.length];
  const total = counts.reduce((sum, count) => sum + count, 0);
  const hasBothFamilies = activeFamily === "all" && visibleAgents.length > 0 && visibleWorks.length > 0;
  const agentPageSize = hasBothFamilies ? Math.max(1, Math.floor(pageSize / 2)) : pageSize;
  const workPageSize = hasBothFamilies ? Math.max(1, pageSize - agentPageSize) : pageSize;
  const pageCount = activeFamily === "all"
    ? Math.max(1, Math.ceil(visibleAgents.length / agentPageSize), Math.ceil(visibleWorks.length / workPageSize))
    : Math.max(1, Math.ceil(total / pageSize));
  const safePage = Math.min(Math.max(1, page), pageCount);
  const pageSlice = <T,>(items: T[], size: number) => items.slice(
    (safePage - 1) * size,
    safePage * size,
  );
  const workPage = pageSlice(visibleWorks, workPageSize);
  const agentPage = pageSlice(visibleAgents, agentPageSize);

  const setFilter = <K extends keyof BrowseFilters,>(
    key: K,
    value: BrowseFilters[K],
  ) => onFilters({ ...filters, [key]: value });

  function setFamily(next: BrowseFamily) {
    if (onFamily) onFamily(next);
    else setLocalFamily(next);
    onPage(1);
  }

  function appendQuery(query: string) {
    setFilter("query", appendQueryTerms(filters.query, query));
  }

  function commitDraftQuery() {
    const candidate = draftQuery.trim();
    if (!candidate) return;
    const errors = queryDiagnostics(candidate);
    setDraftErrors(errors);
    if (errors.length) return;
    appendQuery(candidate);
    setDraftQuery("");
    setDraftErrors([]);
  }

  const pagination = (
    <Pagination
      page={safePage}
      pageCount={pageCount}
      total={total}
      pageSize={pageSize}
      pageSizeOptions={settings.browse.pageSizeOptions}
      onPage={onPage}
      onPageSize={onPageSize}
    />
  );

  return (
    <>
      <div className="family-selector" role="group" aria-label="Entity family">
        {(["all", "work", "agent"] as const).map((value) => (
          <button
            type="button"
            key={value}
            className={activeFamily === value ? "active" : ""}
            aria-pressed={activeFamily === value}
            onClick={() => setFamily(value)}
          >
            {value === "all" ? "All" : value === "work" ? "Works" : "Agents"}
          </button>
        ))}
      </div>

      <section className="filters sticky">
        <div className="atomic-query">
          <form
            className="atomic-query-entry"
            onSubmit={(event) => {
              event.preventDefault();
              commitDraftQuery();
            }}
          >
            <input
              type="search"
              value={draftQuery}
              placeholder='Search or add a field, such as agent:"Akira Kurosawa"'
              onChange={(event) => {
                setDraftQuery(event.target.value);
                if (draftErrors.length) setDraftErrors([]);
              }}
              aria-label="Add catalog search filter"
            />
            <button type="submit" disabled={!draftQuery.trim()}>Add</button>
          </form>

          {activeQuery.terms.length ? (
            <div className="query-chips" aria-label="Active search filters">
              {activeQuery.terms.map((term, queryIndex) => (
                <span
                  className={term.negated ? "query-chip negated" : "query-chip"}
                  key={`${term.raw}:${queryIndex}`}
                >
                  <code>{term.raw}</code>
                  <button
                    type="button"
                    onClick={() => setFilter("query", removeQueryTermAt(filters.query, queryIndex))}
                    aria-label={`Remove filter ${term.raw}`}
                    title={`Remove ${term.raw}`}
                  >×</button>
                </span>
              ))}
            </div>
          ) : (
            <span className="atomic-query-hint">
              Plain text searches both works and agents. Field terms keep explicit semantics.
            </span>
          )}
        </div>

        {activeFamily !== "agent" ? (
          <>
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
            <select value={filters.medium} onChange={(event) => setFilter("medium", event.target.value)} aria-label="Medium">
              <option value="">All media</option>
              {domain.mediumOptions.map((option) => (
                <option value={option.value} key={option.value}>
                  {humanize(option.value)} ({option.count})
                </option>
              ))}
            </select>
            <select value={filters.conceptId} onChange={(event) => setFilter("conceptId", event.target.value)} aria-label="Concept">
              <option value="">All concepts</option>
              {domain.conceptOptions.map((concept) => (
                <option value={concept.id} key={concept.id}>{concept.label} ({concept.count})</option>
              ))}
            </select>
            <select value={sort} onChange={(event) => onSort(event.target.value as BrowseSort)} aria-label="Sort works">
              <option value="date">Date</option>
              <option value="label">Label</option>
              <option value="medium">Medium</option>
              <option value="relevance" disabled={!filters.query.trim() && !filters.conceptId}>Relevance</option>
            </select>
          </>
        ) : null}
        <button
          type="button"
          onClick={() => {
            setDraftQuery("");
            setDraftErrors([]);
            onFilters({ query: "", minimumYear: "", maximumYear: "", medium: "", conceptId: "" });
          }}
        >Clear</button>
      </section>

      {queryErrors.length ? <div className="query-error" role="alert">{queryErrors.join(" · ")}</div> : null}

      <details className="advanced-search">
        <summary>Advanced search syntax</summary>
        <p>Terms are combined with AND. Quotes preserve phrases; a leading minus excludes a term.</p>
        <code>agent:&quot;Akira Kurosawa&quot; role:director type:person id:agent-000001</code>
        <p>
          Agent fields: type, role, id and work. Work fields include title, agent, tag,
          genre, movement, theme, style, medium, country, lang, guide and year.
        </p>
      </details>

      {pagination}
      {activeFamily !== "work" ? (
        <section className="browse-result-group" aria-labelledby="browse-agents-heading">
          <div className="section-heading">
            <h2 id="browse-agents-heading">Agents</h2>
            <span>{visibleAgents.length.toLocaleString()} matches</span>
          </div>
          {agentPage.length ? (
            <AgentTable
              rows={agentPage}
              ratings={ratings}
              onOpen={onOpen}
              onRate={onRate}
              onRole={(role) => appendQuery(buildQueryToken("role", role))}
            />
          ) : <p className="empty compact">No matching agents.</p>}
        </section>
      ) : null}

      {activeFamily !== "agent" ? (
        <section className="browse-result-group" aria-labelledby="browse-works-heading">
          <div className="section-heading">
            <h2 id="browse-works-heading">Works</h2>
            <span>{visibleWorks.length.toLocaleString()} matches</span>
          </div>
          {workPage.length ? (
            <WorkTable
              works={workPage}
              ratings={ratings}
              onOpen={onOpen}
              onRate={onRate}
              onMedium={(medium) => setFilter("medium", medium)}
              onConcept={(label) => appendQuery(buildQueryToken("tag", label))}
            />
          ) : <p className="empty compact">No matching works.</p>}
        </section>
      ) : null}
      {pagination}
    </>
  );
}
