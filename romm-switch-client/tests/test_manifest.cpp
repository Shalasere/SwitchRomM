#include "catch.hpp"
#include "romm/manifest.hpp"

TEST_CASE("manifest serialize/deserialize roundtrip") {
    romm::Manifest m;
    m.rommId = "42";
    m.fileId = "99";
    m.fsName = "Test.nsp";
    m.url = "http://host/path";
    m.totalSize = 123456;
    m.partSize = 4096;
    m.parts.push_back({0, 4096, "abcd", true});
    m.parts.push_back({1, 4096, "efgh", false});

    std::string json = romm::manifestToJson(m);

    romm::Manifest parsed;
    std::string err;
    REQUIRE(romm::manifestFromJson(json, parsed, err));
    REQUIRE(err.empty());
    REQUIRE(parsed.rommId == m.rommId);
    REQUIRE(parsed.fileId == m.fileId);
    REQUIRE(parsed.fsName == m.fsName);
    REQUIRE(parsed.url == m.url);
    REQUIRE(parsed.totalSize == m.totalSize);
    REQUIRE(parsed.partSize == m.partSize);
    REQUIRE(parsed.parts.size() == 2);
    REQUIRE(parsed.parts[0].index == 0);
    REQUIRE(parsed.parts[0].size == 4096);
    REQUIRE(parsed.parts[0].sha256 == "abcd");
    REQUIRE(parsed.parts[0].completed == true);
    REQUIRE(parsed.parts[1].completed == false);
}

TEST_CASE("manifestFromJson rejects missing fields") {
    std::string bad = "{\"romm_id\":\"1\"}";
    romm::Manifest m;
    std::string err;
    REQUIRE_FALSE(romm::manifestFromJson(bad, m, err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("planResume counts valid and invalid parts") {
    romm::Manifest m;
    m.totalSize = 3 * 4096;
    m.partSize = 4096;
    m.parts = { {0, 4096, ""}, {1, 4096, ""}, {2, 4096, ""} };

    std::vector<std::pair<int, uint64_t>> observed = {
        {0, 4096}, // valid
        {1, 1000}, // partial
        {3, 4096}  // unknown index
    };

    romm::ResumePlan plan = romm::planResume(m, observed);
    REQUIRE(plan.validParts.size() == 1);
    REQUIRE(plan.invalidParts.size() == 1); // unknown index only
    REQUIRE(plan.partialIndex == 1);
    REQUIRE(plan.partialBytes == 1000);
    REQUIRE(plan.bytesHave == 4096 + 1000);
    REQUIRE(plan.bytesNeed == m.totalSize - plan.bytesHave);
}

TEST_CASE("planResume requires contiguity from part 0") {
    romm::Manifest m;
    m.totalSize = 3 * 4096;
    m.partSize = 4096;
    m.parts = { {0, 4096, ""}, {1, 4096, ""}, {2, 4096, ""} };

    SECTION("missing part 0 invalidates later complete parts") {
        std::vector<std::pair<int, uint64_t>> observed = {
            {1, 4096}, // looks complete but part 0 missing
            {2, 4096}
        };
        romm::ResumePlan plan = romm::planResume(m, observed);
        REQUIRE(plan.validParts.empty());
        REQUIRE(plan.partialIndex == -1);
        REQUIRE(plan.bytesHave == 0);
        REQUIRE(plan.bytesNeed == m.totalSize);
        REQUIRE(plan.invalidParts.size() == 2);
    }

    SECTION("gap after part 0 stops resume boundary") {
        std::vector<std::pair<int, uint64_t>> observed = {
            {0, 4096},
            {2, 4096}
        };
        romm::ResumePlan plan = romm::planResume(m, observed);
        REQUIRE(plan.validParts.size() == 1);
        REQUIRE(plan.validParts[0] == 0);
        REQUIRE(plan.partialIndex == -1);
        REQUIRE(plan.bytesHave == 4096);
        REQUIRE(plan.bytesNeed == m.totalSize - plan.bytesHave);
        REQUIRE(plan.invalidParts.size() == 1);
        REQUIRE(plan.invalidParts[0] == 2);
    }

    SECTION("partial allowed only at first missing index; later parts invalid") {
        std::vector<std::pair<int, uint64_t>> observed = {
            {0, 4096},
            {1, 2048}, // partial at next index
            {2, 4096}  // should be invalid because gap/partial before it
        };
        romm::ResumePlan plan = romm::planResume(m, observed);
        REQUIRE(plan.validParts.size() == 1);
        REQUIRE(plan.validParts[0] == 0);
        REQUIRE(plan.partialIndex == 1);
        REQUIRE(plan.partialBytes == 2048);
        REQUIRE(plan.bytesHave == 4096 + 2048);
        REQUIRE(plan.bytesNeed == m.totalSize - plan.bytesHave);
        REQUIRE(plan.invalidParts.size() == 1);
        REQUIRE(plan.invalidParts[0] == 2);
    }
}
