import { useEffect, useMemo, useState } from "react";
import { buildDomain, loadCatalog, loadResearch } from "./lib/data";
import { buildFeatureIndex } from "./lib/features";
import { buildResearchData } from "./lib/research";
import { DEFAULT_SETTINGS } from "./lib/settings";
import type { BrowseFilters, BrowseSort } from "./lib/browse";
import { buildViewerHref, readViewerLocation } from "./lib/location";
import type { ViewerLocationState, ViewName } from "./lib/location";
import { loadRatings, saveRatings, toggleRating } from "./lib/ratings";
import { appendQueryTerms } from "./lib/query";
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
import "./enhancements.css";

const VIEWS: Array<{ name: ViewName; label: string }> = [
  { name: "browse", label: "Browse" },
  { name: "recommendations", label: "Recommendations" },
  { name: "evolution", label: "Evolution" },
  { name: "islands", label: "Islands" },
  { name: "research", label: "Research" },
];

const LOCATION_DEFAULTS = {
  pageSize: DEFAULT_SETTINGS.browse.defaultPageSize,
  pageSizeOptions: DEFAULT_SETTINGS.browse.pageSizeOptions,
};

export default function App() {
  const [initialLocation] = useState(() =>
    readViewerLocation(
      window.location,
      import.meta.env.BASE_URL,
      LOCATION_DEFAULTS,
    ),
  );
  const [catalog, setCatalog] = useState<Catalog | null>(null);
  const [externalResearch, setExternalResearch] = useState<ResearchData | null>(null);
  const [domain, setDomain] = useState<Domain | null>(null);
  const [error, setError] = useState("");
  const [view, setView] = useState<ViewName>(initialLocation.view);
  const [ratings, setRatings] = useState<Ratings>(() => loadRatings());
  const [filters, setFilters] = useState<BrowseFilters>(
    initialLocation.browse.filters,
  );
  const [sort, setSort] = useState<BrowseSort>(initialLocation.browse.sort);
  const [page, setPage] = useState(initialLocation.browse.page);
  const [pageSize, setPageSize] = useState(initialLocation.browse.pageSize);

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

  useEffect(() => {
    const applyLocation = () => {
      const next = readViewerLocation(
        window.location,
        import.meta.env.BASE_URL,
        LOCATION_DEFAULTS,
      );
      setView(next.view);
      setFilters(next.browse.filters);
      setSort(next.browse.sort);
      setPage(next.browse.page);
      setPageSize(next.browse.pageSize);
    };

    const canonical = buildViewerHref(
      initialLocation,
      import.meta.env.BASE_URL,
      LOCATION_DEFAULTS,
    );
    if (`${window.location.pathname}${window.location.search}` !== canonical) {
      window.history.replaceState(null, "", canonical);
    }

    window.addEventListener("popstate", applyLocation);
    return () => window.removeEventListener("popstate", applyLocation);
  }, [initialLocation]);

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

  function currentLocation(overrides: {
    view?: ViewName;
    filters?: BrowseFilters;
    sort?: BrowseSort;
    page?: number;
    pageSize?: number;
  } = {}): ViewerLocationState {
    return {
      view: overrides.view ?? view,
      browse: {
        filters: overrides.filters ?? filters,
        sort: overrides.sort ?? sort,
        page: overrides.page ?? page,
        pageSize: overrides.pageSize ?? pageSize,
      },
    };
  }

  function writeLocation(
    next: ViewerLocationState,
    mode: "push" | "replace",
  ) {
    const href = buildViewerHref(
      next,
      import.meta.env.BASE_URL,
      LOCATION_DEFAULTS,
    );
    if (mode === "push") window.history.pushState(null, "", href);
    else window.history.replaceState(null, "", href);
  }

  function navigateView(next: ViewName) {
    if (next === view) return;
    setView(next);
    writeLocation(currentLocation({ view: next }), "push");
  }

  function updateFilters(next: BrowseFilters) {
    setFilters(next);
    setPage(1);
    writeLocation(currentLocation({ filters: next, page: 1 }), "replace");
  }

  function updateSort(next: BrowseSort) {
    setSort(next);
    setPage(1);
    writeLocation(currentLocation({ sort: next, page: 1 }), "replace");
  }

  function updatePage(next: number) {
    setPage(next);
    writeLocation(currentLocation({ page: next }), "push");
  }

  function updatePageSize(next: number) {
    setPageSize(next);
    setPage(1);
    writeLocation(currentLocation({ page: 1, pageSize: next }), "replace");
  }

  function searchBrowse(query: string) {
    const nextFilters = {
      ...filters,
      query: appendQueryTerms(filters.query, query),
    };
    setView("browse");
    setFilters(nextFilters);
    setPage(1);
    writeLocation(
      currentLocation({ view: "browse", filters: nextFilters, page: 1 }),
      "push",
    );
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
            <a
              key={name}
              href={buildViewerHref(
                currentLocation({ view: name }),
                import.meta.env.BASE_URL,
                LOCATION_DEFAULTS,
              )}
              className={view === name ? "tab active" : "tab"}
              aria-current={view === name ? "page" : undefined}
              onClick={(event) => {
                if (
                  event.button !== 0 ||
                  event.metaKey ||
                  event.ctrlKey ||
                  event.shiftKey ||
                  event.altKey
                ) {
                  return;
                }
                event.preventDefault();
                navigateView(name);
              }}
            >
              {label}
              {name === "research" && research.summary.problems > 0 ? (
                <span className="tab-count">{research.summary.problems}</span>
              ) : null}
            </a>
          ))}
        </nav>

        <div className="rating-summary">
          <a href={`${import.meta.env.BASE_URL}api/v1/index.json`}>API</a>
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
            onPage={updatePage}
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
        onSearch={searchBrowse}
        onFocus={focusWindow}
        onClose={closeWindow}
        onMove={moveWindow}
      />
    </div>
  );
}
