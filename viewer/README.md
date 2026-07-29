# Arachne web viewer

React/Vite static viewer migrated from `ninjaro/art-islands` and adapted to the
current Arachne product database.

## Features

- Browse with search, date, medium and concept filters
- pagination and relevance sorting
- browser-local like/dislike ratings
- weighted recommendations with positive and negative explanations
- inferred temporal Evolution forest
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
