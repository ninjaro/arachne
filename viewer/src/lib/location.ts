import type { BrowseFilters, BrowseSort } from "./browse";

export const VIEW_NAMES = [
  "browse",
  "recommendations",
  "evolution",
  "islands",
  "research",
] as const;

export type ViewName = (typeof VIEW_NAMES)[number];

export interface BrowseLocationState {
  filters: BrowseFilters;
  sort: BrowseSort;
  page: number;
  pageSize: number;
}

export interface ViewerLocationState {
  view: ViewName;
  browse: BrowseLocationState;
}

export interface BrowseLocationDefaults {
  pageSize: number;
  pageSizeOptions: readonly number[];
}

const SORTS: readonly BrowseSort[] = ["date", "label", "medium", "relevance"];

function normalizeBaseUrl(baseUrl: string): string {
  const withLeadingSlash = baseUrl.startsWith("/") ? baseUrl : `/${baseUrl}`;
  return withLeadingSlash.endsWith("/") ? withLeadingSlash : `${withLeadingSlash}/`;
}

function positiveInteger(value: string | null, fallback: number): number {
  if (!value) return fallback;
  const parsed = Number(value);
  return Number.isInteger(parsed) && parsed > 0 ? parsed : fallback;
}

function viewFromPathname(pathname: string, baseUrl: string): ViewName {
  const base = normalizeBaseUrl(baseUrl);
  const relative = pathname.startsWith(base)
    ? pathname.slice(base.length)
    : pathname.replace(/^\/+/, "");
  const candidate = relative.split("/").filter(Boolean)[0];
  return VIEW_NAMES.includes(candidate as ViewName)
    ? (candidate as ViewName)
    : "browse";
}

export function readViewerLocation(
  location: Pick<Location, "pathname" | "search">,
  baseUrl: string,
  defaults: BrowseLocationDefaults,
): ViewerLocationState {
  const params = new URLSearchParams(location.search);
  const requestedSort = params.get("sort") as BrowseSort | null;
  const requestedPageSize = positiveInteger(params.get("pageSize"), defaults.pageSize);

  return {
    view: viewFromPathname(location.pathname, baseUrl),
    browse: {
      filters: {
        query: params.get("q") ?? "",
        minimumYear: params.get("from") ?? "",
        maximumYear: params.get("to") ?? "",
        medium: params.get("medium") ?? "",
        conceptId: params.get("concept") ?? "",
      },
      sort: requestedSort && SORTS.includes(requestedSort) ? requestedSort : "date",
      page: positiveInteger(params.get("page"), 1),
      pageSize: defaults.pageSizeOptions.includes(requestedPageSize)
        ? requestedPageSize
        : defaults.pageSize,
    },
  };
}

export function buildViewerHref(
  state: ViewerLocationState,
  baseUrl: string,
  defaults: BrowseLocationDefaults,
): string {
  const base = normalizeBaseUrl(baseUrl);
  const path = `${base}${state.view}/`;
  if (state.view !== "browse") return path;

  const params = new URLSearchParams();
  const { filters, sort, page, pageSize } = state.browse;

  if (filters.query) params.set("q", filters.query);
  if (filters.minimumYear) params.set("from", filters.minimumYear);
  if (filters.maximumYear) params.set("to", filters.maximumYear);
  if (filters.medium) params.set("medium", filters.medium);
  if (filters.conceptId) params.set("concept", filters.conceptId);
  if (sort !== "date") params.set("sort", sort);
  if (page !== 1) params.set("page", String(page));
  if (pageSize !== defaults.pageSize) params.set("pageSize", String(pageSize));

  const query = params.toString();
  return query ? `${path}?${query}` : path;
}
