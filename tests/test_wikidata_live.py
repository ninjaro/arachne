"""Small opt-in checks against the Wikidata and Commons Action APIs."""

from __future__ import annotations

import json
import unittest
import urllib.parse
import urllib.request


WIKIDATA_API = "https://www.wikidata.org/w/api.php"
COMMONS_API = "https://commons.wikimedia.org/w/api.php"
USER_AGENT = "Arachne/2.0 live tests (+https://github.com/ninjaro/arachne)"


def action_api(endpoint: str, parameters: dict[str, str]) -> dict:
    request = urllib.request.Request(
        endpoint,
        data=urllib.parse.urlencode(parameters).encode("ascii"),
        headers={
            "Accept": "application/json",
            "Content-Type": "application/x-www-form-urlencoded",
            "User-Agent": USER_AGENT,
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def entity_claim_targets(entity: dict, property_id: str) -> set[str]:
    targets: set[str] = set()
    for claim in entity.get("claims", {}).get(property_id, []):
        value = claim.get("mainsnak", {}).get("datavalue", {}).get("value")
        if isinstance(value, dict) and isinstance(value.get("id"), str):
            targets.add(value["id"])
    return targets


class WikidataLiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.entities_response = action_api(
            WIKIDATA_API,
            {
                "action": "wbgetentities",
                "format": "json",
                "formatversion": "2",
                "redirects": "yes",
                "ids": "Q2001|Q103474|Q11559876|Q42|Q999999999",
                "props": "labels|descriptions|claims",
                "languages": "en|ja",
                "languagefallback": "1",
            },
        )

    def test_known_entities_expose_metadata_and_credit_relation(self) -> None:
        entities = self.entities_response["entities"]
        self.assertEqual(entities["Q2001"]["labels"]["en"]["value"], "Stanley Kubrick")
        self.assertIn("P569", entities["Q2001"]["claims"])
        self.assertIn("Q2001", entity_claim_targets(entities["Q103474"], "P57"))
        self.assertIn("missing", entities["Q999999999"])

    def test_multilingual_name_recovers_unmapped_local_fixture(self) -> None:
        result = action_api(
            WIKIDATA_API,
            {
                "action": "wbsearchentities",
                "format": "json",
                "formatversion": "2",
                "type": "item",
                "limit": "20",
                "strictlanguage": "1",
                "search": "深井国",
                "language": "ja",
                "uselang": "ja",
            },
        )
        self.assertIn("Q11559876", {row["id"] for row in result["search"]})

    def test_external_id_search_is_not_label_dependent(self) -> None:
        result = action_api(
            WIKIDATA_API,
            {
                "action": "query",
                "format": "json",
                "formatversion": "2",
                "list": "search",
                "srnamespace": "0",
                "srlimit": "20",
                "srsearch": "haswbstatement:P345=nm0297912",
            },
        )
        self.assertIn(
            "Q11559876", {row["title"] for row in result["query"]["search"]}
        )

    def test_commons_returns_metadata_without_media_bytes(self) -> None:
        q42 = self.entities_response["entities"]["Q42"]
        image_claim = q42["claims"]["P18"][0]
        filename = image_claim["mainsnak"]["datavalue"]["value"]
        result = action_api(
            COMMONS_API,
            {
                "action": "query",
                "format": "json",
                "formatversion": "2",
                "prop": "imageinfo",
                "titles": f"File:{filename}",
                "iiprop": "url|size|mime|extmetadata",
                "iiextmetadatafilter": (
                    "LicenseShortName|LicenseUrl|Artist|Credit|Restrictions|UsageTerms"
                ),
            },
        )
        image_info = result["query"]["pages"][0]["imageinfo"][0]
        self.assertTrue(image_info["url"].startswith("https://"))
        self.assertTrue(image_info["mime"].startswith("image/"))
        self.assertIn("LicenseShortName", image_info["extmetadata"])
        self.assertNotIn("data", image_info)


if __name__ == "__main__":
    unittest.main()
