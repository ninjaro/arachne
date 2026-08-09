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
cd ..
scripts/build.sh
cd viewer
npm run data
npm run dev
```

Run the viewer model tests with `npm test`; `npm run test:watch` keeps them open
during development.

Paths passed to the native executable are repository-relative, even though npm
runs from `viewer/`. `ARACHNE_BINARY` can select a non-default CMake output
(the default is `../build/arachne`). Local data generation needs no graph store
or operations configuration: the catalog script writes a generic working export to
`../.arachne/tmp/viewer-product-local.jsonl` from the canonical database, and
the native commands require its embedded database identity to match the current
SQLite bytes before applying any research or taste semantics. The export stays
outside `public/` and cannot become part of a browser or static-site build.

The data command reads four explicit inputs:

- `../database/art-islands.sqlite`, the schema-v6 product database;
- the generic local JSONL produced from those exact SQLite bytes;
- `../database/merge-hints-review.json`, the disposable bounded hint
  projection; and
- `../database/merge-hint-decisions.json`, the durable ignored-pair decisions.

It writes the generated, ignored files `public/data/catalog.json`,
`public/data/research.json`, and `public/data/taste-index.json`. The native
`product research` command is the only research-semantics implementation: it
combines quality gaps, ingest issues, and merge hints into a readable,
snapshot-bound physical JSON report. The npm workflow uses its compact output
for static delivery; direct CLI output remains readable by default. `product
taste-index` precomputes sparse
work vectors, agent-to-concept affinities, norms, feature metadata, and postings
so React does not scan the full catalog to derive global weights. The review's
source identity must match both the product snapshot and the exact decision
artifact. A missing or stale review, decision, snapshot, or export is an error
rather than a fallback.

Both projections can also be generated or inspected directly over SSH:

```sh
# Reviewed snapshot mode:
build/arachne product research \
  --config config/arachne.local.json \
  --product-snapshot graphs/product/active.json \
  --output research.json
build/arachne product taste-index \
  --config config/arachne.local.json \
  --product-snapshot graphs/product/active.json \
  --output taste-index.json --compact
build/arachne product entity \
  --config config/arachne.local.json \
  --product-snapshot graphs/product/active.json \
  --id work-001234

# Local canonical-database mode (after npm run data:catalog):
build/arachne product taste-index \
  --database database/art-islands.sqlite \
  --product-export .arachne/tmp/viewer-product-local.jsonl \
  --output viewer/public/data/taste-index.json
```

Open the Vite URL printed by the command, normally `http://localhost:5173/`.

### Local Taste and portable profiles

Browse searches and rates both works and first-class agents; its `All` mode
keeps the two families in separate tables. Taste stores explicit `+1`/`-1`
ratings for works, agents, and concepts only in `arachne-viewer-ratings-v2` in
the browser. The previous `arachne-viewer-ratings-v1` value map is migrated as
work ratings. Reset, JSON import, and JSON export are local operations; imports
report records missing from the active snapshot rather than failing the whole
profile.

Portable rating exports include a format version, product snapshot, entity ID,
family, and value. The separate interest-profile export contains signed,
explainable agent and concept signals for possible future CLI mining; exporting
it does not schedule work. `Load demo profile` opens a small static, read-only
example and never merges it into the browser's real ratings.

Inferred concept preferences are a disposable projection, not an explicit
rating. The frontend combines the user's small rating set with the
snapshot-bound `taste_index_v1`; it does not rebuild document frequencies or
agent distributions. Explicit and inferred values remain visible independently,
including when they disagree.

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
injects the catalog and freshly derives both research and taste artifacts from
the selected verified product snapshot. It excludes any pre-existing copies of
those three files from compiled assets, preventing stale local data from entering
a bundle, and then content-addresses the complete result. A normal local build
should continue to use `npm run build`, which also generates the JSON-first
static API from the local catalog.

Publication looks for `derived/wikidata-image-hints.json` in the reviewed state
repository and may receive another relative `image_hints_artifact` path as an
override. When a file is selected, the workflow verifies all three product
identity fields against the selected product snapshot control, stages it before
`build:assets`, and includes it in the immutable site bundle. If neither a
reviewed default nor an override exists, it publishes the same viewer without
local hints.

## Generated data

`public/data/catalog.json` is deliberately not committed. The full old
`projection.json` contains provenance nodes that the catalog UI does not need and
was about 79 MiB. The browser read model is generated directly from the
canonical SQLite database and is about 27 MiB for the current corpus, including
its first-class `agents` collection. Each contributor retains its agent identity
fields and resolves by `id` to that collection.

The SQLite database remains the source of truth. `catalog.json`,
`research.json`, and `taste-index.json` are disposable viewer projections. The optional
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
