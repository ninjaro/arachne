# Arachne web viewer

React/Vite static viewer migrated from `ninjaro/art-islands` and adapted to the
current Arachne product database.

## Features

- Browse with search, date, medium and concept filters
- pagination and relevance sorting
- browser-local like/dislike ratings
- weighted recommendations with positive and negative explanations
- [TimeNets-inspired](https://idl.uw.edu/papers/timenets) Evolution lineages with
  metric time, focus-plus-context expansion, and d3-dag layout and edge routing
- rated/recommended Islands graph with disconnected components preserved
- draggable multi-window work details
- concepts, credits, content-guide assertions, measurements, identifiers,
  manifestations and financial facts from the current database

## Local development

From `viewer/`:

```sh
npm run data
npm run dev
```

The data command reads `../database/art-islands.sqlite` and writes the generated,
ignored files `public/data/catalog.json` and `public/data/research.json`.
Research data comes directly from the schema-v5 `ingest_issues` and
`merge_hints` tables; only open rows are included in the review queue.

Open the Vite URL printed by the command, normally `http://localhost:5173/`.

## Production build

```sh
npm run data
npm run build
npm run preview
```

The static site is written to `viewer/dist/`.

## Generated data

`public/data/catalog.json` is deliberately not committed. The full old
`projection.json` contains provenance nodes that the catalog UI does not need and
was about 79 MiB. The compact browser read model is generated directly from the
canonical SQLite database and is about 12 MiB for the current corpus.

The SQLite database remains the source of truth. `catalog.json` and
`research.json` are disposable viewer projections.

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
