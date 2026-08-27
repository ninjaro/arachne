# Non-authoritative research questions

This note preserves useful analytical questions for future research. It does
not define product semantics, schema, mining requirements, provider policy, or
canonical writes. Computed results remain hypotheses and follow the boundary in
[Architecture](ARCHITECTURE.md#canonical-semantic-write-boundary).

## Keep analytical channels separate

“Concept similarity” is not one measurement. Useful channels answer different
questions and should remain independently inspectable:

- raw and weighted work-set overlap measure support;
- Jaccard measures symmetric overlap;
- directional containment exposes asymmetry but does not prove hierarchy;
- context similarity compares neighbouring concepts even when direct work
  overlap is low;
- rarity-aware association distinguishes informative rare co-occurrence from
  ubiquitous concepts, but needs support counts;
- temporal distributions expose overlap, displacement, gaps, continuity, and
  revival without implying influence;
- sequence, transition, and partial-order comparisons ask whether trajectories
  share repertoire, order, or only a local phase;
- ancestry and structural fingerprints compare graph position, provided that
  chronological, documented-influence, and derived graphs remain separate;
- perturbation and cross-snapshot stability expose sensitivity and corpus debt.

Metric disagreement can itself define a research question. High context
similarity with low direct overlap may suggest sibling roles; a large temporal
offset may suggest succession; symmetric containment plus name similarity may
justify identity review. None of these signatures authorizes a canonical
relation or merge.

## Corpus maturity and the long tail

Corpus size is not corpus maturity. Useful diagnostics include dated and agent
coverage, source and evidence diversity, temporal span, medium balance, support,
and stability under resampling. A small evidence-rich concept may be more useful
than a large collection of stubs.

Instability is not automatically noise. It can indicate sparse mining,
inconsistent annotation, an algorithm-sensitive result, or a genuinely hybrid,
contested, transitional, or boundary object. Research output should retain the
support and perturbation context needed to distinguish those possibilities.

Rare concepts likewise mix meaningful peripheral structure with unsupported
observations. Analyses should expose both rarity and support instead of pruning
the long tail or treating every rare result as equally reliable.

## Cross-media questions

A shared label does not guarantee the same manifestation across media. Compare
concept × medium trajectories as separate channels before asking whether they
share temporal shape, neighbouring concepts, agent structure, revival timing,
or documented transmission.

Cross-media analogy is easier to inspect when decomposed—similar time shape,
similar boundary position, different content, no known transmission—than when
hidden inside one universal feature vector. Adaptation, documented influence,
similarity, shared terminology, and chronology may coexist for the same works
but remain different relations.

The unit of transmission is also open: a complete work, technique, motif,
narrative device, production technology, formal structure, genre convention,
or social practice. Different media may require different units.

## Durable open questions

- When does a descriptor or narrow pseudo-genre develop enough independent
  works, agents, continuity, terminology, descendants, or revivals to merit
  genre research?
- Is hierarchy global, or does prominence depend on medium, period, corpus,
  research focus, and display scale?
- How should agent trajectories inform concept trajectories without turning a
  shared credit into influence?
- What evidence distinguishes documented influence from exposure, scholarly
  argument, retrospective speculation, chronological adjacency, and similarity?
- How should contradictory evidence and algorithm disagreement guide further
  research without being averaged into consensus?
- Can a readable tree remain only a presentation projection while research
  preserves multiple parentage, hybridization, convergence, revival, and
  opposition?
- Which historical context belongs in the art graph, and which should remain
  external researcher context rather than expanding into a general world graph?

The governing research distinction is:

```text
source observation -> human interpretation -> computed hypothesis
```

The layers may inform one another but must remain identifiable.
