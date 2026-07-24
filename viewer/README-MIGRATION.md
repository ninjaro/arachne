# Arachne React viewer migration slice

This directory replaces the current static `viewer/` implementation.

Implemented:

- React + Vite shell
- loading `data/projection.json`
- string IDs
- Browse search, medium filter, sorting and pagination
- details drawer
- local like/dislike ratings
- basic recommendation scoring from concept and credit edges

Not yet migrated:

- full feature weighting from the old repository
- Evolution canvas
- Islands graph
- old floating multi-window behavior
- Vite integration in `viewer_builder::build_site()`

## Local development

Place a generated projection at:

```text
public/data/projection.json
```

Then run:

```sh
npm install
npm run dev
```

## Build

```sh
npm run build
```

For the current C++ site builder, copy `dist/` into the final bundle and place
the generated projection at `dist/data/projection.json`.

The permanent integration should update `viewer_builder::build_site()` to copy
the complete Vite `dist/` tree rather than the old fixed list of
`index.html`, `app.js`, and `styles.css`.
