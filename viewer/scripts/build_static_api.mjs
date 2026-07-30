import { copyFile, mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const viewerDirectory = dirname(scriptDirectory);
const distDirectory = join(viewerDirectory, "dist");
const baseUrl = normalizeBaseUrl(
  process.env.ARACHNE_VIEWER_BASE ?? "/arachne/viewer/",
);
const defaultPageSize = 50;
const viewNames = [
  "browse",
  "recommendations",
  "evolution",
  "islands",
  "research",
];

function normalizeBaseUrl(value) {
  const withLeadingSlash = value.startsWith("/") ? value : `/${value}`;
  return withLeadingSlash.endsWith("/")
    ? withLeadingSlash
    : `${withLeadingSlash}/`;
}

function apiPath(relative) {
  return `${baseUrl}api/v1/${relative}`;
}

function uiPath(view, query = "") {
  return `${baseUrl}${view}/${query}`;
}

function escapeHtml(value) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

async function writeJson(path, value) {
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, `${JSON.stringify(value, null, 2)}\n`, "utf8");
}

function linksForView(view) {
  return {
    self: apiPath(`views/${view}.json`),
    ui: uiPath(view),
    catalog: `${baseUrl}data/catalog.json`,
    research: `${baseUrl}data/research.json`,
  };
}

function viewDocument(view, catalog, research) {
  const common = {
    formatVersion: 1,
    apiVersion: "v1",
    kind: "arachne.viewer.view",
    name: view,
    links: linksForView(view),
  };

  if (view === "browse") {
    const pageCount = Math.max(1, Math.ceil(catalog.works.length / defaultPageSize));
    return {
      ...common,
      source: {
        href: `${baseUrl}data/catalog.json`,
        itemPath: "works",
      },
      staticPagination: {
        pageSize: defaultPageSize,
        pageCount,
        total: catalog.works.length,
        pageTemplate: apiPath("browse/pages/{page}.json"),
      },
      uiQuery: {
        q: "full-text filter",
        from: "minimum year",
        to: "maximum year",
        medium: "exact medium",
        concept: "concept id",
        sort: ["date", "label", "medium", "relevance"],
        page: "positive integer",
        pageSize: [25, 50, 100],
      },
    };
  }

  if (view === "research") {
    return {
      ...common,
      source: {
        href: `${baseUrl}data/research.json`,
        itemPath: "items",
        available: Boolean(research),
      },
    };
  }

  return {
    ...common,
    source: {
      href: `${baseUrl}data/catalog.json`,
      itemPath: "works",
    },
    clientState:
      view === "recommendations" || view === "islands"
        ? {
            ratings: "browser-local like/dislike values",
            deterministicWithoutRatings: false,
          }
        : undefined,
  };
}

function renderHtml(template, title, apiUrl, document) {
  const canonicalLink = `<link id="arachne-canonical" rel="canonical" href="${document.links?.ui ?? baseUrl}">`;
  const jsonLink = `<link id="arachne-json" rel="alternate" type="application/json" href="${apiUrl}" title="Arachne JSON document">`;
  const openapiLink = `<link id="arachne-openapi" rel="service-desc" type="application/json" href="${apiPath("openapi.json")}" title="Arachne OpenAPI document">`;

  const fallback = `
    <main id="api-fallback" class="api-fallback">
      <h1>${title}</h1>
      <p>
        Canonical JSON: <a href="${apiUrl}">${apiUrl}</a>
      </p>
      <p>
        JavaScript is optional. When available, it replaces this API document with the interactive viewer.
      </p>
      <pre>${escapeHtml(JSON.stringify(document, null, 2))}</pre>
    </main>`;

  const documentLinks = [canonicalLink, jsonLink, openapiLink].join("\n    ");

  let html = template;
  if (/<meta id="arachne-document-links"[^>]*>/.test(html)) {
    html = html.replace(
      /<meta id="arachne-document-links"[^>]*>/,
      documentLinks,
    );
  } else {
    // Compatibility with an early patch revision. These source links must not
    // be present during the Vite build because some targets are directories or
    // are generated only after Vite finishes.
    html = html
      .replace(/<link id="arachne-canonical"[^>]*>/, canonicalLink)
      .replace(/<link id="arachne-json"[^>]*>/, jsonLink)
      .replace(/<link id="arachne-openapi"[^>]*>/, openapiLink);
  }

  return html
    .replace(/<main id="api-fallback"[\s\S]*?<\/main>/, fallback.trim())
    .replace(/<title>.*?<\/title>/, `<title>${title}</title>`);
}

const template = await readFile(join(distDirectory, "index.html"), "utf8");
const catalog = JSON.parse(
  await readFile(join(distDirectory, "data", "catalog.json"), "utf8"),
);
let research = null;
try {
  research = JSON.parse(
    await readFile(join(distDirectory, "data", "research.json"), "utf8"),
  );
} catch (error) {
  if (error?.code !== "ENOENT") throw error;
}

const browseWorks = [...catalog.works].sort(
  (left, right) =>
    (left.yearStart ?? Number.MAX_SAFE_INTEGER) -
      (right.yearStart ?? Number.MAX_SAFE_INTEGER) ||
    left.label.localeCompare(right.label) ||
    left.id.localeCompare(right.id),
);
const browseCatalog = { ...catalog, works: browseWorks };

const viewDocuments = Object.fromEntries(
  viewNames.map((view) => [view, viewDocument(view, browseCatalog, research)]),
);

const apiIndex = {
  formatVersion: 1,
  apiVersion: "v1",
  kind: "arachne.viewer.index",
  links: {
    self: apiPath("index.json"),
    openapi: apiPath("openapi.json"),
    ui: baseUrl,
  },
  datasets: {
    catalog: {
      href: `${baseUrl}data/catalog.json`,
      mediaType: "application/json",
      itemPath: "works",
      count: catalog.works.length,
    },
    research: {
      href: `${baseUrl}data/research.json`,
      mediaType: "application/json",
      itemPath: "items",
      available: Boolean(research),
      count: research?.items?.length ?? 0,
    },
  },
  views: Object.fromEntries(
    viewNames.map((view) => [
      view,
      {
        document: apiPath(`views/${view}.json`),
        ui: uiPath(view),
      },
    ]),
  ),
};

const openapi = {
  openapi: "3.1.0",
  info: {
    title: "Arachne static viewer API",
    version: "1.0.0",
    description:
      "Static, read-only JSON resources published with GitHub Pages. Query-driven filtering remains a client-side concern; default browse pages are precomputed.",
  },
  servers: [{ url: baseUrl.replace(/\/$/, "") }],
  paths: {
    "/api/v1/index.json": {
      get: {
        summary: "API index",
        responses: { "200": { description: "API index document" } },
      },
    },
    "/api/v1/views/{view}.json": {
      get: {
        summary: "Viewer view descriptor",
        parameters: [
          {
            name: "view",
            in: "path",
            required: true,
            schema: { type: "string", enum: viewNames },
          },
        ],
        responses: { "200": { description: "View descriptor" } },
      },
    },
    "/api/v1/browse/pages/{page}.json": {
      get: {
        summary: "Precomputed default browse page",
        parameters: [
          {
            name: "page",
            in: "path",
            required: true,
            schema: { type: "integer", minimum: 1 },
          },
        ],
        responses: {
          "200": { description: "Browse page" },
          "404": { description: "Page was not generated" },
        },
      },
    },
  },
};

await writeJson(join(distDirectory, "api", "v1", "index.json"), apiIndex);
await writeJson(join(distDirectory, "api", "v1", "openapi.json"), openapi);

for (const [view, document] of Object.entries(viewDocuments)) {
  await writeJson(
    join(distDirectory, "api", "v1", "views", `${view}.json`),
    document,
  );

  const routeDirectory = join(distDirectory, view);
  await mkdir(routeDirectory, { recursive: true });
  await writeFile(
    join(routeDirectory, "index.html"),
    renderHtml(template, `Arachne · ${view}`, document.links.self, document),
    "utf8",
  );
}

const pageCount = Math.max(1, Math.ceil(browseWorks.length / defaultPageSize));
for (let page = 1; page <= pageCount; page += 1) {
  const firstIndex = (page - 1) * defaultPageSize;
  const items = browseWorks.slice(firstIndex, firstIndex + defaultPageSize);
  const self = apiPath(`browse/pages/${page}.json`);
  const pageDocument = {
    formatVersion: 1,
    apiVersion: "v1",
    kind: "arachne.viewer.browse.page",
    page,
    pageSize: defaultPageSize,
    pageCount,
    total: browseWorks.length,
    links: {
      self,
      first: apiPath("browse/pages/1.json"),
      previous: page > 1 ? apiPath(`browse/pages/${page - 1}.json`) : null,
      next: page < pageCount ? apiPath(`browse/pages/${page + 1}.json`) : null,
      last: apiPath(`browse/pages/${pageCount}.json`),
      ui: uiPath("browse", page === 1 ? "" : `?page=${page}`),
    },
    items,
  };
  await writeJson(
    join(distDirectory, "api", "v1", "browse", "pages", `${page}.json`),
    pageDocument,
  );
}

await writeFile(
  join(distDirectory, "index.html"),
  renderHtml(template, "Arachne · API index", apiIndex.links.self, apiIndex),
  "utf8",
);

// Convenient non-versioned entry points. They are copies rather than redirects,
// because GitHub Pages cannot configure content negotiation or rewrite rules.
await mkdir(join(distDirectory, "api"), { recursive: true });
await copyFile(
  join(distDirectory, "api", "v1", "index.json"),
  join(distDirectory, "api", "index.json"),
);
await copyFile(
  join(distDirectory, "api", "v1", "openapi.json"),
  join(distDirectory, "api", "openapi.json"),
);
