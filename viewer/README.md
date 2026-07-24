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
ignored file `public/data/catalog.json`.

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

The SQLite database remains the source of truth. `catalog.json` is disposable.

## Arachne production integration

The migration package includes `scripts/apply_production_integration.py`. The
installer runs it automatically. It updates the C++ viewer builder to:

- create the compact catalog directly from the verified product JSONL export;
- copy the complete Vite `viewer/dist/` tree into the immutable site bundle;
- add `data/catalog.json` to the bundle;
- keep the full projection as a separate build artifact rather than shipping it
  to every browser.

It also updates the publication workflow to run `npm ci` and `npm run build`,
and updates the static bundle test for hashed Vite assets.
