# Arachne Viewer — redesign prompt

Create the next iteration of Arachne Viewer without reinventing the product.

The primary screen and primary product is **Evolution**: a large interactive metro-like map of cultural trajectories over time. Browse, Research, Taste, entity cards, and the API are secondary surfaces.

Keep the successful parts of the current dark redesign: compact left sidebar, dense typography, strong visual hierarchy, compact controls, and a narrow pinned inspector.

## Main screen

- persistent sidebar on the left: `Evolution`, `Browse`, `Research`, `Taste`, with pinned/saved items below;
- center: Evolution canvas takes almost all available space;
- above the canvas: seed trajectories, separate `Earlier` and `Later`, filters/visibility/zoom;
- right: narrow collapsible/resizable inspector;
- the graph must always have higher spatial priority than the inspector and other panels.

## What Evolution shows

- line = trajectory: currently primarily a concept (`genre`, `style`, `movement`, `theme`, `technique`, `motif`, etc.), later also an agent;
- station = a work or a simple derived aggregate of several similar child works;
- interchange = several trajectories in one atomic work;
- ordinary trajectory continuity means shared membership over time, **not influence and not causality**;
- documented work-to-work relations (`influenced_by`, `adapted_from`, `derived_from`, etc.) are shown with a separate directional/dashed overlay.

The horizontal axis defines **chronological order**, not a linear metric time scale. Year width depends on the amount and complexity of visible data; dense years are wider, and large historical gaps may be compressed.

Exact dates constrain the layout more strongly. Tied or incomplete dates may be reordered within the valid chronological constraints to reduce clutter, crossings, and trajectory wiggle.

`Earlier` and `Later` are two separate exploration controls. Do not merge them back into one generic `Reach`.

## Aggregation

The data contains hierarchy:

- series → season → episode;
- album → track;
- collection/book → contained works / chapters / parts.

Do not draw this hierarchy as nested boxes or a tree inside the graph.

Hierarchy is primarily useful for projection and aggregation:

- a semantically homogeneous album can become one station;
- a homogeneous series can also be collapsed even if it spans many years;
- a heterogeneous anthology-like series should stay more detailed;
- a rare exceptional episode/track may appear separately when the current focus makes it important.

An aggregate should still look like an ordinary station with a short label such as `10 tracks`, `12 episodes`, `3 works`.

## Trajectory width

Line thickness represents **significance / centrality** of a trajectory for a particular station or aggregate.

- strong membership → thick line;
- peripheral membership → thin line;
- if a tag is present in only a small fraction of an aggregate's children, the line through that aggregate should be thinner;
- a line may smoothly narrow or widen between stations.

Do not use the same thickness for confidence, popularity, traversal depth, number of children, or selected state.

## Graph complexity

Do not try to show everything with equal visual emphasis at once.

Use:

- seed/focus;
- Earlier/Later;
- filtering;
- pinning;
- trajectory ranking;
- bundling;
- hierarchy-aware aggregation;
- semantic zoom.

Keep the metro-like visual grammar small: station, trajectory, interchange, aggregate, explicit relation overlay.

## Visual direction

Keep the dark analytical workspace, density, strong typography, left rail, compact controls, and clear selected states.

Do not turn the screen into a dashboard of large rounded cards.

Focus first on **one convincing full-screen Evolution state with a realistically dense graph**. Browse should be at most a secondary example of the same shell.

Do not invent fake historical relations, dates, centrality, or credits. If real data is unavailable, use clearly neutral placeholders.
