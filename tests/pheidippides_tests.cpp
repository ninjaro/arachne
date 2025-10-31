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

pheidippides& shared_client() {
    static pheidippides client;
    return client;
}

TEST(Pheidippides, FetchJsonItems) {
    auto& client = shared_client();
    std::unordered_set<std::string> ids
        = { "Q190082", "Q165769", "Q184874", "Q313728" };
    const std::unordered_map<std::string, std::string> expected_labels {
        { "Q190082", "Arachne" },
        { "Q165769", "Penelope" },
        { "Q184874", "Ariadne" },
        { "Q313728", "Pheidippides" },
    };
    const auto json = client.fetch_json(ids, entity_kind::item);
    ASSERT_TRUE(json.contains("entities"));
    const auto& entities = json.at("entities");
    EXPECT_EQ(entities.size(), expected_labels.size());
    for (const auto& [id, expected_label] : expected_labels) {
        ASSERT_TRUE(entities.contains(id));
        const auto& entity = entities.at(id);
        ASSERT_TRUE(entity.contains("labels"));
        ASSERT_TRUE(entity["labels"].contains("en"));
        ASSERT_TRUE(entity["labels"]["en"].contains("value"));
        EXPECT_EQ(entity["labels"]["en"]["value"], expected_label);
    }
}

TEST(Pheidippides, FetchJsonProperty) {
    auto& client = shared_client();
    std::unordered_set<std::string> ids = { "P1049", "P2925", "P4185" };
    const std::unordered_map<std::string, std::string> expected_labels {
        { "P1049", "worshipped by" },
        { "P2925", "domain of saint or deity" },
        { "P4185", "iconographic symbol" },
    };
    const auto json = client.fetch_json(ids, entity_kind::property);
    ASSERT_TRUE(json.contains("entities"));
    const auto& entities = json.at("entities");
    EXPECT_EQ(entities.size(), expected_labels.size());
    for (const auto& [id, expected_label] : expected_labels) {
        ASSERT_TRUE(entities.contains(id));
        const auto& entity = entities.at(id);
        ASSERT_TRUE(entity.contains("labels"));
        ASSERT_TRUE(entity["labels"].contains("en"));
        ASSERT_TRUE(entity["labels"]["en"].contains("value"));
        EXPECT_EQ(entity["labels"]["en"]["value"], expected_label);
    }
}

TEST(Pheidippides, FetchJsonLexeme) {
    auto& client = shared_client();
    std::unordered_set<std::string> ids = { "L17828", "L327555" };
    const std::unordered_map<std::string, std::string> expected_lemmas {
        { "L17828", "loom" },
        { "L327555", "sewing" },
    };
    const auto json = client.fetch_json(ids, entity_kind::lexeme);
    ASSERT_TRUE(json.contains("entities"));
    const auto& entities = json.at("entities");
    EXPECT_EQ(entities.size(), expected_lemmas.size());
    for (const auto& [id, expected_lemma] : expected_lemmas) {
        ASSERT_TRUE(entities.contains(id));
        const auto& entity = entities.at(id);
        ASSERT_TRUE(entity.contains("lemmas"));
        ASSERT_TRUE(entity["lemmas"].contains("en"));
        ASSERT_TRUE(entity["lemmas"]["en"].contains("value"));
        EXPECT_EQ(entity["lemmas"]["en"]["value"], expected_lemma);
        ASSERT_TRUE(entity.contains("lexicalCategory"));
        ASSERT_TRUE(entity.contains("forms"));
        ASSERT_TRUE(entity.contains("senses"));
        ASSERT_TRUE(entity.contains("claims"));
    }
}

TEST(Pheidippides, FetchJsonMediainfo) {
    auto& client = shared_client();
    // "Velázquez, Diego - The Fable of Arachne (Las Hilanderas) - c. 1657.jpg"
    // "Statue of Pheidippides along the Marathon Road.jpg"
    std::unordered_set<std::string> ids = { "M6940375", "M10678815" };
    const std::unordered_map<std::string, std::string> expected_depicts {
        { "M6940375", "Q984058" }, // Las Hilanderas
        { "M10678815", "Q313728" }, // Pheidippides
    };
    const auto json = client.fetch_json(ids, entity_kind::mediainfo);
    ASSERT_TRUE(json.contains("entities"));
    const auto& entities = json.at("entities");
    EXPECT_EQ(entities.size(), ids.size());

    for (const auto& [id, expected_qid] : expected_depicts) {
        ASSERT_TRUE(entities.contains(id));
        const auto& entity = entities.at(id);

        ASSERT_TRUE(entity.contains("type"));
        EXPECT_EQ(entity.at("type"), "mediainfo");
        ASSERT_TRUE(entity.contains("id"));
        EXPECT_EQ(entity.at("id"), id);
        ASSERT_TRUE(entity.contains("statements"));
        const auto& st = entity.at("statements");
        ASSERT_TRUE(st.contains("P180"));

        bool found = false;
        for (const auto& stmt : st.at("P180")) {
            if (!stmt.contains("mainsnak")) {
                continue;
            }
            const auto& snak = stmt.at("mainsnak");
            if (!snak.contains("datavalue")) {
                continue;
            }
            const auto& dv = snak.at("datavalue");
            if (dv.contains("type") && dv.at("type") == "wikibase-entityid"
                && dv.contains("value") && dv.at("value").contains("id")) {
                if (dv.at("value").at("id") == expected_qid) {
                    found = true;
                    break;
                }
            }
        }
        EXPECT_TRUE(found) << id;
    }
}

TEST(Pheidippides, FetchJsonEntitySchema) {
    auto& client = shared_client();
    std::unordered_set<std::string> ids = { "E10", "E42" };
    const std::unordered_map<std::string, std::string> expected_labels {
        { "E10", "human" },
        { "E42", "autor" },
    };
    const auto json = client.fetch_json(ids, entity_kind::entity_schema);
    ASSERT_TRUE(json.contains("query"));
    ASSERT_TRUE(json["query"].contains("pages"));

    const auto& pages = json["query"]["pages"];
    ASSERT_TRUE(pages.is_array());
    EXPECT_EQ(pages.size(), ids.size());

    std::unordered_set<std::string> found;

    for (const auto& page : pages) {
        ASSERT_TRUE(page.contains("title"));
        const auto& title = page.at("title").get_ref<const std::string&>();
        ASSERT_TRUE(title.rfind("EntitySchema:", 0) == 0) << title;
        const std::string eid
            = title.substr(std::string("EntitySchema:").size());
        found.emplace(eid);
    }

    for (const auto& id : ids) {
        EXPECT_TRUE(found.contains(id)) << "missing EntitySchema:" << id;
    }
}