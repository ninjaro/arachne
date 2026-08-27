# Non-authoritative provider reference

This is a research shortlist, not an activation plan, supported-provider list,
or statement of semantic trust. A source should enter configuration only after
its current access, licensing, attribution, and rate-limit terms are reviewed
and an Ariadne adapter exists. Pheidippides transports bytes and receipts; no
provider response enters canonical state without the normal human-reviewed
batch boundary.

The current implemented and optional bulk providers are documented in
[Architecture](ARCHITECTURE.md#candidate-graph-and-transient-semantic-projections) and
[`config/README.md`](../config/README.md). The following visual-art sources are
useful leads not otherwise recorded there:

| Source | Potential research use | Preferred acquisition lead | Caveat to recheck |
|---|---|---|---|
| [Getty Vocabularies](https://www.getty.edu/research/tools/vocabularies/obtain/) | ULAN agents; AAT materials, techniques, roles, and object types; TGN places; CONA works and iconography | Published linked-open-data downloads for corpus work; SPARQL or record representations for lookup | Attribution and vocabulary-specific terms |
| [Getty collection and provenance data](https://data.getty.edu/) | Collection objects, bibliography, dealers, auctions, inventories, and ownership leads | Provider linked data and targeted queries | Provenance observations must not become inferred ownership automatically |
| [Rijksmuseum Data Services](https://data.rijksmuseum.nl/docs/) | Objects, creators, materials, references, and IIIF assets | OAI-PMH or LDES for harvesting; resolver/search for lookup | Preserve provider identity and rights metadata |
| [Metropolitan Museum collection API](https://metmuseum.github.io/) | Object IDs, creators, dates, media, dimensions, and public images | Public dataset for broad processing; API for lookup | Field and image availability varies by object |
| [Art Institute of Chicago API](https://api.artic.edu/docs/) | Works, artists, exhibitions, publications, and IIIF images | Published data dumps for broad processing; API for lookup | Follow per-field rights and attribution |
| [Library of Congress API](https://www.loc.gov/apis/json-and-yaml/) | Books, prints, photographs, audio, and archival leads | Dataset exports where offered; JSON API for lookup | Heterogeneous responses do not represent the complete catalogue |
| [Smithsonian Open Access](https://www.si.edu/openaccess/devtools) | Cross-domain museum and archive records plus eligible media | Published dataset for broad work; keyed API for lookup | Rights vary; distinguish explicitly designated open assets |
| [Europeana APIs](https://europeana.atlassian.net/wiki/spaces/EF/pages/2462351393/Accessing+the+APIs) | Cross-provider European museum, library, and archive discovery | OAI-PMH/SPARQL where suitable; record APIs for lookup | Aggregated records vary and may duplicate original providers |

Official dumps, snapshots, OAI-PMH, or event streams are preferable for
periodic corpus processing; point APIs are for targeted enrichment. Listing a
source here creates no requirement to implement or activate it.
