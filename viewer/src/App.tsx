import { useEffect, useMemo, useState } from "react";
import { buildDomain, loadProjection } from "./data";
import { loadRatings, saveRatings, toggleRating } from "./ratings";
import { recommendations } from "./recommendations";
import type { Domain, RatingValue, Ratings, Work } from "./types";
import "./styles.css";

type View = "browse" | "recommendations" | "evolution" | "islands";
type Sort = "date" | "label" | "medium";

function RatingButtons({
  work,
  ratings,
  onRate,
}: {
  work: Work;
  ratings: Ratings;
  onRate: (id: string, value: RatingValue) => void;
}) {
  return (
    <div className="ratings" onClick={(event) => event.stopPropagation()}>
      <button
        type="button"
        className={ratings[work.id] === 1 ? "active positive" : ""}
        onClick={() => onRate(work.id, 1)}
        aria-label={`Like ${work.label}`}
      >
        +
      </button>
      <button
        type="button"
        className={ratings[work.id] === -1 ? "active negative" : ""}
        onClick={() => onRate(work.id, -1)}
        aria-label={`Dislike ${work.label}`}
      >
        −
      </button>
    </div>
  );
}

function Details({ work, close }: { work: Work; close: () => void }) {
  return (
    <aside className="details">
      <button type="button" className="close" onClick={close} aria-label="Close details">
        ×
      </button>
      <h2>{work.label}</h2>
      <p className="muted">
        {[work.year, work.medium].filter(Boolean).join(" · ")}
      </p>
      <h3>Concepts</h3>
      <div className="chips">
        {work.concepts.map((concept) => (
          <span className="chip" key={concept.id}>
            {concept.label}
          </span>
        ))}
      </div>
      <h3>Contributors</h3>
      <dl>
        {work.contributors.map((contributor) => (
          <div key={`${contributor.role}:${contributor.id}`}>
            <dt>{contributor.role.replaceAll("_", " ")}</dt>
            <dd>{contributor.label}</dd>
          </div>
        ))}
      </dl>
    </aside>
  );
}

export default function App() {
  const [domain, setDomain] = useState<Domain | null>(null);
  const [error, setError] = useState("");
  const [view, setView] = useState<View>("browse");
  const [ratings, setRatings] = useState<Ratings>(() => loadRatings());
  const [query, setQuery] = useState("");
  const [medium, setMedium] = useState("");
  const [sort, setSort] = useState<Sort>("date");
  const [page, setPage] = useState(1);
  const [selected, setSelected] = useState<string | null>(null);

  useEffect(() => {
    loadProjection()
      .then((projection) => setDomain(buildDomain(projection)))
      .catch((cause: unknown) =>
        setError(cause instanceof Error ? cause.message : String(cause)),
      );
  }, []);

  useEffect(() => saveRatings(ratings), [ratings]);

  const media = useMemo(
    () =>
      domain
        ? [...new Set(domain.works.map((work) => work.medium))].sort()
        : [],
    [domain],
  );

  const visible = useMemo(() => {
    if (!domain) return [];
    const normalized = query.trim().toLocaleLowerCase();
    const result = domain.works.filter((work) => {
      if (medium && work.medium !== medium) return false;
      if (!normalized) return true;
      return [
        work.label,
        work.medium,
        String(work.year ?? ""),
        ...work.concepts.map((concept) => concept.label),
        ...work.contributors.map((contributor) => contributor.label),
      ]
        .join(" ")
        .toLocaleLowerCase()
        .includes(normalized);
    });
    return result.sort((a, b) => {
      if (sort === "label") return a.label.localeCompare(b.label);
      if (sort === "medium")
        return a.medium.localeCompare(b.medium) || a.label.localeCompare(b.label);
      return (
        (a.year ?? Number.MAX_SAFE_INTEGER) -
          (b.year ?? Number.MAX_SAFE_INTEGER) ||
        a.label.localeCompare(b.label)
      );
    });
  }, [domain, query, medium, sort]);

  const recommended = useMemo(
    () => (domain ? recommendations(domain, ratings) : []),
    [domain, ratings],
  );

  if (error) return <main className="state error">{error}</main>;
  if (!domain) return <main className="state">Loading catalog…</main>;

  const selectedWork = selected ? domain.workById.get(selected) : undefined;
  const pageSize = 50;
  const pageCount = Math.max(1, Math.ceil(visible.length / pageSize));
  const safePage = Math.min(page, pageCount);
  const rows = visible.slice((safePage - 1) * pageSize, safePage * pageSize);

  function changeFilters() {
    setPage(1);
  }

  function rate(id: string, value: RatingValue) {
    setRatings((current) => toggleRating(current, id, value));
  }

  return (
    <div className="app">
      <header className="topbar">
        <div>
          <h1>Arachne</h1>
          <span className="muted">{domain.works.length.toLocaleString()} works</span>
        </div>
        <nav>
          {(["browse", "recommendations", "evolution", "islands"] as View[]).map(
            (name) => (
              <button
                type="button"
                key={name}
                className={view === name ? "tab active" : "tab"}
                onClick={() => setView(name)}
              >
                {name[0].toUpperCase() + name.slice(1)}
              </button>
            ),
          )}
        </nav>
        <span>{Object.keys(ratings).length} rated</span>
      </header>

      {view === "browse" ? (
        <main>
          <section className="filters">
            <input
              type="search"
              value={query}
              placeholder="Search works, concepts, people"
              onChange={(event) => {
                setQuery(event.target.value);
                changeFilters();
              }}
            />
            <select
              value={medium}
              onChange={(event) => {
                setMedium(event.target.value);
                changeFilters();
              }}
            >
              <option value="">All media</option>
              {media.map((value) => (
                <option value={value} key={value}>
                  {value}
                </option>
              ))}
            </select>
            <select value={sort} onChange={(event) => setSort(event.target.value as Sort)}>
              <option value="date">Date</option>
              <option value="label">Label</option>
              <option value="medium">Medium</option>
            </select>
          </section>

          <section className="pagination">
            <span>{visible.length.toLocaleString()} matches</span>
            <button disabled={safePage <= 1} onClick={() => setPage(safePage - 1)}>
              Previous
            </button>
            <span>
              {safePage} / {pageCount}
            </span>
            <button
              disabled={safePage >= pageCount}
              onClick={() => setPage(safePage + 1)}
            >
              Next
            </button>
          </section>

          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Year</th>
                  <th>Work</th>
                  <th>Medium</th>
                  <th>Concepts</th>
                  <th>Rating</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((work) => (
                  <tr key={work.id} onClick={() => setSelected(work.id)}>
                    <td>{work.year ?? ""}</td>
                    <td className="label">{work.label}</td>
                    <td>{work.medium}</td>
                    <td>
                      <div className="chips">
                        {work.concepts.slice(0, 5).map((concept) => (
                          <span className="chip" key={concept.id}>
                            {concept.label}
                          </span>
                        ))}
                      </div>
                    </td>
                    <td>
                      <RatingButtons work={work} ratings={ratings} onRate={rate} />
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </main>
      ) : view === "recommendations" ? (
        <main>
          <h2>Recommendations</h2>
          {!recommended.length ? (
            <p className="state">Like several works first.</p>
          ) : (
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Score</th>
                    <th>Year</th>
                    <th>Work</th>
                    <th>Why</th>
                    <th>Rating</th>
                  </tr>
                </thead>
                <tbody>
                  {recommended.slice(0, 100).map((item) => (
                    <tr key={item.work.id} onClick={() => setSelected(item.work.id)}>
                      <td>{item.score.toFixed(1)}</td>
                      <td>{item.work.year ?? ""}</td>
                      <td className="label">{item.work.label}</td>
                      <td>
                        <div className="chips">
                          {item.reasons.map((reason) => (
                            <span className="chip" key={reason}>
                              {reason}
                            </span>
                          ))}
                        </div>
                      </td>
                      <td>
                        <RatingButtons
                          work={item.work}
                          ratings={ratings}
                          onRate={rate}
                        />
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </main>
      ) : (
        <main className="state">
          <h2>{view[0].toUpperCase() + view.slice(1)}</h2>
          <p>
            The data adapter is ready. This view is the next migration slice from
            the old React implementation.
          </p>
        </main>
      )}

      {selectedWork ? (
        <Details work={selectedWork} close={() => setSelected(null)} />
      ) : null}
    </div>
  );
}
