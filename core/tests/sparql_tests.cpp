/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Yaroslav Riabtsev <yaroslav.riabtsev@rwth-aachen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "pheidippides.hpp"
#include <gtest/gtest.h>

using namespace arachnespace;
using namespace corespace;

pheidippides& shared_sparql_client() {
    static pheidippides client;
    return client;
}

TEST(PheidippidesSparql, LasHilanderasIsAtPrado) {
    auto& client = shared_sparql_client();
    const char* q = R"SPARQL(
    PREFIX wd:  <http://www.wikidata.org/entity/>
    PREFIX wdt: <http://www.wikidata.org/prop/direct/>
    ASK {
        wd:Q984058                # Las Hilanderas / The Fable of Arachne
        wdt:P170 wd:Q297 ;      # creator Diego Velázquez
        wdt:P276 wd:Q160112 .   # location Museo del Prado (Madrid)
    }
    )SPARQL";

    const auto json = client.wdqs(q);
    ASSERT_TRUE(json.contains("boolean"));
    EXPECT_TRUE(json["boolean"].get<bool>());
}

TEST(PheidippidesSparql, PropertiesAreWikibaseItemType) {
    auto& client = shared_sparql_client();
    const char* q = R"SPARQL(
    PREFIX wd:  <http://www.wikidata.org/entity/>
    PREFIX wikibase: <http://wikiba.se/ontology#>
    SELECT ?p ?type WHERE {
        VALUES ?p { wd:P1049 wd:P2925 wd:P4185 wd:P180 }
        ?p wikibase:propertyType ?type .
    }
    )SPARQL";

    const auto json = client.wdqs(q);
    std::unordered_map<std::string, std::string> types;
    for (const auto& b : json["results"]["bindings"]) {
        const std::string p_uri = b.at("p").at("value");
        const std::string p_id = p_uri.substr(p_uri.find_last_of('/') + 1);
        const std::string type_uri = b.at("type").at("value");
        types[p_id] = type_uri;
    }
    const std::string item_ty = "http://wikiba.se/ontology#WikibaseItem";
    EXPECT_EQ(types.at("P1049"), item_ty);
    EXPECT_EQ(types.at("P2925"), item_ty);
    EXPECT_EQ(types.at("P4185"), item_ty);
    EXPECT_EQ(types.at("P180"), item_ty);
}

TEST(PheidippidesSparql, TrioAreGreekMythCharacters) {
    auto& client = shared_sparql_client();
    const char* q = R"SPARQL(
    PREFIX wd:  <http://www.wikidata.org/entity/>
    PREFIX wdt: <http://www.wikidata.org/prop/direct/>
    SELECT ?item WHERE {
        VALUES ?item { wd:Q190082 wd:Q165769 wd:Q184874 }
        ?item wdt:P31/wdt:P279* wd:Q22988604.
    }
  )SPARQL";

    const auto json = client.wdqs(q);
    std::unordered_set<std::string> got;
    for (const auto& b : json["results"]["bindings"]) {
        const auto& uri
            = b.at("item").at("value").get_ref<const std::string&>();
        got.emplace(uri.substr(uri.find_last_of('/') + 1));
    }
    EXPECT_TRUE(got.contains("Q190082"));
    EXPECT_TRUE(got.contains("Q165769"));
    EXPECT_TRUE(got.contains("Q184874"));
}

TEST(PheidippidesSparql, SewingLexemeIsNoun) {
    auto& client = shared_sparql_client();
    const char* q = R"SPARQL(
    PREFIX wd:       <http://www.wikidata.org/entity/>
    PREFIX wikibase: <http://wikiba.se/ontology#>
    PREFIX dct:      <http://purl.org/dc/terms/>
    SELECT ?lemma ?lc WHERE {
        VALUES ?l { wd:L327555 }
        ?l wikibase:lemma ?lemma ;
        wikibase:lexicalCategory ?lc ;
        dct:language wd:Q1860 .
        FILTER (LANG(?lemma) = "en")
    }
  )SPARQL";

    const auto json = client.wdqs(q);
    ASSERT_EQ(json["results"]["bindings"].size(), 1u);
    const auto& b = json["results"]["bindings"][0];
    EXPECT_EQ(b.at("lemma").at("value"), "sewing");
    EXPECT_NE(
        b.at("lc").at("value").get<std::string>().find("/Q1084"),
        std::string::npos
    );
}

TEST(PheidippidesSparql, ItemsHaveExpectedEnglishLabels) {
    auto& client = shared_sparql_client();

    const char* q = R"SPARQL(
    PREFIX wd:  <http://www.wikidata.org/entity/>
    PREFIX bd:  <http://www.bigdata.com/rdf#>
    PREFIX rdfs:<http://www.w3.org/2000/01/rdf-schema#>
    PREFIX wikibase:<http://wikiba.se/ontology#>

    SELECT ?item ?enLabel WHERE {
        VALUES ?item { wd:Q190082 wd:Q165769 wd:Q184874 wd:Q313728 }
        SERVICE wikibase:label {
            bd:serviceParam wikibase:language "en".
            ?item rdfs:label ?enLabel
        }
    }
    )SPARQL";

    const auto json = client.wdqs(q);
    std::unordered_map<std::string, std::string> got;
    for (const auto& b : json["results"]["bindings"]) {
        const std::string uri = b.at("item").at("value");
        const std::string id = uri.substr(uri.find_last_of('/') + 1);
        got[id] = b.at("enLabel").at("value");
    }

    EXPECT_EQ(got.at("Q190082"), "Arachne");
    EXPECT_EQ(got.at("Q165769"), "Penelope");
    EXPECT_EQ(got.at("Q184874"), "Ariadne");
    EXPECT_EQ(got.at("Q313728"), "Pheidippides");
}

TEST(PheidippidesSparql, LexemesReturnEnglishLemmas) {
    auto& client = shared_sparql_client();

    const char* q = R"SPARQL(
    PREFIX wd: <http://www.wikidata.org/entity/>
    PREFIX wikibase:<http://wikiba.se/ontology#>

    SELECT ?lexeme ?lemma WHERE {
        VALUES ?lexeme { wd:L17828 wd:L327555 }
        ?lexeme wikibase:lemma ?lemma .
        FILTER ( LANG(?lemma) = "en" )
    }
    )SPARQL";

    const auto json = client.wdqs(q);
    std::unordered_map<std::string, std::string> lemmas;
    for (const auto& b : json["results"]["bindings"]) {
        const std::string uri = b.at("lexeme").at("value");
        const std::string id = uri.substr(uri.find_last_of('/') + 1);
        lemmas[id] = b.at("lemma").at("value");
    }

    EXPECT_EQ(lemmas.at("L17828"), "loom");
    EXPECT_EQ(lemmas.at("L327555"), "sewing");
}

TEST(PheidippidesSparql, PaintingDepictsArachneAndByVelazquez) {
    auto& client = shared_sparql_client();

    const char* q = R"SPARQL(
    PREFIX wd:  <http://www.wikidata.org/entity/>
    PREFIX wdt: <http://www.wikidata.org/prop/direct/>
    SELECT ?work WHERE {
        VALUES ?work { wd:Q984058 }     # Las Hilanderas / The Spinners
        ?work wdt:P180 wd:Q190082 ;     # depicts Arachne
            wdt:P170 wd:Q297 ;        # creator Diego Velázquez
            wdt:P31  wd:Q3305213 .    # instance of painting
    }
    )SPARQL";

    const auto json = client.wdqs(q);
    ASSERT_EQ(json["results"]["bindings"].size(), 1);
    const std::string uri = json["results"]["bindings"][0]["work"]["value"];
    EXPECT_TRUE(uri.size() && uri.find("Q984058") != std::string::npos);
}

TEST(PheidippidesSparql, AskPheidippidesIsHuman) {
    auto& client = shared_sparql_client();

    const char* q = R"SPARQL(
    PREFIX wd:  <http://www.wikidata.org/entity/>
    PREFIX wdt: <http://www.wikidata.org/prop/direct/>
    ASK { wd:Q313728 wdt:P31 wd:Q5 }
    )SPARQL";

    const auto json = client.wdqs(q);
    ASSERT_TRUE(json.contains("boolean"));
    EXPECT_TRUE(json["boolean"].get<bool>());
}