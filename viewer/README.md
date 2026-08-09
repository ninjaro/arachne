# Arachne web viewer

React/Vite static viewer migrated from `ninjaro/art-islands` and adapted to the
current Arachne product database.

## Features

- Browse with search, date, medium and concept filters
- pagination and relevance sorting
- browser-local like/dislike ratings
- weighted recommendations with positive and negative explanations
- [TimeNets](https://idl.uw.edu/papers/timenets)/metro-inspired Evolution
  storylines with tags as continuous lines,
  works as earliest-date stations, depth-limited temporal expansion, explicit
  relation overlays, and adaptive compressed chronology
- rated/recommended Islands graph with disconnected components preserved
- draggable multi-window work details
- first-class agents with external identifiers and credit references from works
- concepts, content-guide assertions, measurements, work identifiers,
  manifestations and financial facts from the current database

## Local development

From `viewer/`:

```sh
npm run data
npm run dev
```

Run the viewer model tests with `npm test`; `npm run test:watch` keeps them open
during development.

The data command reads three explicit inputs:

- `../database/art-islands.sqlite`, the schema-v6 product database;
- `../database/merge-hints-review.json`, the disposable bounded hint
  projection; and
- `../database/merge-hint-decisions.json`, the durable ignored-pair decisions.

It then writes the generated, ignored files `public/data/catalog.json` and
`public/data/research.json`. Ingest issues come from the product database, while
merge-hint content comes only from the review artifact. The review's source
identity must match both the product bytes used for the catalog and the exact
decision artifact: its `productSha256` must match the catalog database hash,
and its `decisionsSha256` and `ignoredPairCount` must match the decision file's
byte hash and ignored-pair count. A missing or stale review or decision artifact
is an error rather than a database fallback.

Open the Vite URL printed by the command, normally `http://localhost:5173/`.

### Optional local Wikidata image hints

An HPC-produced `wikidata_image_hints_v1` file can be staged beside the
generated catalog without making it part of that catalog:

```sh
npm run data
npm run stage:image-hints -- \
  /path/to/wikidata-image-hints.json \
  public/data/wikidata-image-hints.json \
  --catalog public/data/catalog.json
npm run dev
```

The staging command fails closed unless the artifact's product content hash
matches the generated catalog. The staged file is ignored, optional, and
disposable; remove or restage it when switching product inputs. Images and image
hints never enter SQLite or the catalog schema.

### Image providers and browser requests

The provider registry currently supports local Wikimedia Commons filenames from
the optional artifact, direct Open Library covers and Cover Art Archive images,
and lazy TVmaze show/person lookups. Resolution starts only when an entity card
is opened, runs sequentially per provider, targets two successful images, and
never displays more than three. Candidates are probed in the browser; failed
candidates reserve no space and produce no heading, placeholder, or broken image.

Results are cached for the browser session. Definite missing responses are held
longer than transient failures, while TVmaze `429` responses establish a
provider-wide `Retry-After` cooldown instead of being retried immediately. The
production CSP is derived from the registry's explicit HTTPS API and image-host
allowlists. Direct loading means the relevant provider or CDN receives the
visitor's IP address even though image requests use a no-referrer policy; Arachne
does not proxy those bytes.

## Production build

```sh
npm run data
npm run build
npm run preview
```

The static site is written to `viewer/dist/`.

`npm run build:assets` performs only type-checking and the Vite asset build. It
exists for the protected publication workflow, where the C++ site builder
injects the catalog from a verified product snapshot and then content-addresses
the complete bundle. A normal local build should continue to use `npm run
build`, which also generates the JSON-first static API from the local catalog.

Publication may receive an `image_hints_artifact` path relative to the reviewed
state repository. When supplied, the workflow verifies all three product
identity fields against the selected product snapshot control, stages the file
before `build:assets`, and includes it in the immutable site bundle. Omitting the
input publishes the same viewer without local hints.

## Generated data

`public/data/catalog.json` is deliberately not committed. The full old
`projection.json` contains provenance nodes that the catalog UI does not need and
was about 79 MiB. The browser read model is generated directly from the
canonical SQLite database and is about 27 MiB for the current corpus, including
its first-class `agents` collection. Each contributor retains its agent identity
fields and resolves by `id` to that collection.

The SQLite database remains the source of truth. `catalog.json` and
`research.json` are disposable viewer projections. The optional
`wikidata-image-hints.json` file is a separate snapshot-bound cache, not product
data and not a source for rebuilds.

## Evolution date contract

Evolution interprets dates behind a viewer-only helper until normalized
machine-readable dates are available. A safely validated exact day is preferred;
month-level dates rank ahead of year-only dates; and a year-only value represents
an interval, not an exact point. Ranged, alternative, approximate, or conflicting
dates remain explicitly ambiguous and are never silently ordered. Only the
earliest accepted value places a station—end dates, manifestations, reissues, and
later refinements never extend its geometry. The database schema and ingest
pipeline are intentionally unchanged by the viewer implementation.

## JSON-first static API

The production build publishes machine-readable resources before the optional
React presentation layer:

- `api/v1/index.json` is the service index.
- `api/v1/openapi.json` describes the static endpoints.
- `api/v1/views/<view>.json` describes Browse, Recommendations, Evolution,
  Islands and Research.
- `api/v1/browse/pages/<page>.json` contains precomputed default browse pages.
- `data/catalog.json` and `data/research.json` remain the canonical datasets.

Human-facing routes (`browse/`, `recommendations/`, `evolution/`, `islands/` and
`research/`) contain the corresponding JSON descriptor in readable HTML. When
JavaScript is available, React replaces that document with the interactive
viewer. Browse filters and pagination are represented in the URL query string.

GitHub Pages cannot perform content negotiation or dynamic filtering, so the
JSON API uses explicit `.json` resources and precomputes only the default browse
ordering. The UI can still apply arbitrary filters locally against the catalog.
