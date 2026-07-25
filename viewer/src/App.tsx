import { useEffect, useMemo, useState } from "react";
import { buildDomain, loadCatalog, loadResearch } from "./lib/data";
import { buildFeatureIndex } from "./lib/features";
import { buildResearchData } from "./lib/research";
import { DEFAULT_SETTINGS } from "./lib/settings";
import { EMPTY_FILTERS } from "./lib/browse";
import type { BrowseFilters, BrowseSort } from "./lib/browse";
import { loadRatings, saveRatings, toggleRating } from "./lib/ratings";
import type {
  Catalog,
  Domain,
  EntityId,
  RatingValue,
  Ratings,
  ResearchData,
} from "./lib/types";
import { BrowseView } from "./views/BrowseView";
import { RecommendationsView } from "./views/RecommendationsView";
import { EvolutionView } from "./views/EvolutionView";
import { IslandsView } from "./views/IslandsView";
import { ResearchView } from "./views/ResearchView";
import {
  FloatingEntityWindows,
  useEntityWindows,
} from "./components/windows";
import "./styles.css";

type ViewName =
  | "browse"
  | "recommendations"
  | "evolution"
  | "islands"
  | "research";

const VIEWS: Array<{ name: ViewName; label: string }> = [
  { name: "browse", label: "Browse" },
  { name: "recommendations", label: "Recommendations" },
  { name: "evolution", label: "Evolution" },
  { name: "islands", label: "Islands" },
  { name: "research", label: "Research" },
];

export default function App() {
  const [catalog, setCatalog] = useState<Catalog | null>(null);
  const [externalResearch, setExternalResearch] = useState<ResearchData | null>(null);
  const [domain, setDomain] = useState<Domain | null>(null);
  const [error, setError] = useState("");
  const [view, setView] = useState<ViewName>("browse");
  const [ratings, setRatings] = useState<Ratings>(() => loadRatings());
  const [filters, setFilters] = useState<BrowseFilters>(EMPTY_FILTERS);
  const [sort, setSort] = useState<BrowseSort>("date");
  const [page, setPage] = useState(1);
  const [pageSize, setPageSize] = useState(
    DEFAULT_SETTINGS.browse.defaultPageSize,
  );

  const {
    windows,
    openWindow,
    focusWindow,
    closeWindow,
    moveWindow,
  } = useEntityWindows();

  useEffect(() => {
    Promise.all([loadCatalog(), loadResearch()])
      .then(([loadedCatalog, loadedResearch]) => {
        setCatalog(loadedCatalog);
        setExternalResearch(loadedResearch);
        setDomain(buildDomain(loadedCatalog));
      })
      .catch((cause: unknown) =>
        setError(cause instanceof Error ? cause.message : String(cause)),
      );
  }, []);

  useEffect(() => saveRatings(ratings), [ratings]);

  const featureIndex = useMemo(
    () =>
      domain
        ? buildFeatureIndex(domain, DEFAULT_SETTINGS.features)
        : null,
    [domain],
  );

  const research = useMemo(
    () => (catalog ? buildResearchData(catalog, externalResearch) : null),
    [catalog, externalResearch],
  );

  function rate(id: EntityId, value: RatingValue) {
    setRatings((current) => toggleRating(current, id, value));
  }

  function updateFilters(next: BrowseFilters) {
    setFilters(next);
    setPage(1);
  }

  function updateSort(next: BrowseSort) {
    setSort(next);
    setPage(1);
  }

  function updatePageSize(next: number) {
    setPageSize(next);
    setPage(1);
  }

  function clearRatings() {
    if (!Object.keys(ratings).length) return;
    if (window.confirm("Clear all local ratings?")) setRatings({});
  }

  if (error) {
    return (
      <main className="state error-panel">
        <h2>Catalog failed to load</h2>
        <p>{error}</p>
      </main>
    );
  }

  if (!catalog || !domain || !featureIndex || !research) {
    return <main className="state">Loading catalog…</main>;
  }

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <h1>Arachne</h1>
          <span>
            {domain.works.length.toLocaleString()} works · {catalog.productSnapshotId}
          </span>
        </div>

        <nav className="view-tabs" aria-label="Main views">
          {VIEWS.map(({ name, label }) => (
            <button
              type="button"
              key={name}
              className={view === name ? "tab active" : "tab"}
              aria-pressed={view === name}
              onClick={() => setView(name)}
            >
              {label}
              {name === "research" && research.summary.problems > 0 ? (
                <span className="tab-count">{research.summary.problems}</span>
              ) : null}
            </button>
          ))}
        </nav>

        <div className="rating-summary">
          <span>{Object.keys(ratings).length.toLocaleString()} rated</span>
          <button
            type="button"
            disabled={!Object.keys(ratings).length}
            onClick={clearRatings}
          >
            Clear
          </button>
        </div>
      </header>

      <main className={view === "evolution" || view === "islands" ? "graph-main" : ""}>
        {view === "browse" ? (
          <BrowseView
            domain={domain}
            index={featureIndex}
            settings={DEFAULT_SETTINGS}
            ratings={ratings}
            filters={filters}
            sort={sort}
            page={page}
            pageSize={pageSize}
            onFilters={updateFilters}
            onSort={updateSort}
            onPage={setPage}
            onPageSize={updatePageSize}
            onOpen={openWindow}
            onRate={rate}
          />
        ) : view === "recommendations" ? (
          <RecommendationsView
            domain={domain}
            index={featureIndex}
            ratings={ratings}
            settings={DEFAULT_SETTINGS}
            onOpen={openWindow}
            onRate={rate}
          />
        ) : view === "evolution" ? (
          <EvolutionView
            domain={domain}
            index={featureIndex}
            settings={DEFAULT_SETTINGS}
            onOpen={openWindow}
          />
        ) : view === "islands" ? (
          <IslandsView
            domain={domain}
            index={featureIndex}
            ratings={ratings}
            settings={DEFAULT_SETTINGS}
            onOpen={openWindow}
            onRate={rate}
          />
        ) : (
          <ResearchView data={research} domain={domain} onOpen={openWindow} />
        )}
      </main>

      <FloatingEntityWindows
        windows={windows}
        domain={domain}
        ratings={ratings}
        onRate={rate}
        onFocus={focusWindow}
        onClose={closeWindow}
        onMove={moveWindow}
      />
    </div>
  );
}
