#include "catch.hpp"
#include "romm/config.hpp"

TEST_CASE("parseEnvString parses required fields") {
    const std::string env =
        "server_url=http://example.com\n"
        "download_dir=sdmc:/romm_cache/switch\n"
        "log_level=debug\n"
        "http_timeout_seconds=15\n"
        "speed_test_url=http://speed.test/file\n";

    romm::Config cfg;
    std::string err;
    bool ok = romm::parseEnvString(env, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.serverUrl == "http://example.com");
    REQUIRE(cfg.downloadDir == "sdmc:/romm_cache/switch");
    REQUIRE(cfg.logLevel == "debug");
    REQUIRE(cfg.httpTimeoutSeconds == 15);
    REQUIRE(cfg.speedTestUrl == "http://speed.test/file");
}

TEST_CASE("parseEnvString accepts speed_test_url optional") {
    const std::string env =
        "server_url=http://example.com\n"
        "download_dir=sdmc:/romm_cache/switch\n"
        "speed_test_url=\n";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseEnvString(env, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.speedTestUrl.empty());
}

TEST_CASE("parseEnvString rejects missing required fields") {
    const std::string env = "server_url=http://example.com\n";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseEnvString(env, cfg, err);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("parseEnvString rejects https scheme") {
    const std::string env =
        "server_url=https://bad\n"
        "download_dir=sdmc:/romm_cache/switch\n";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseEnvString(env, cfg, err);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("parseEnvString normalizes booleans and log level") {
    const std::string env =
        "server_url=http://ok\n"
        "download_dir=sdmc:/romm_cache/switch\n"
        "fat32_safe=Yes\n"
        "log_level=DeBuG\n";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseEnvString(env, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.fat32Safe == true);
    REQUIRE(cfg.logLevel == "debug");
}

TEST_CASE("parseEnvString ignores comments (full-line and inline)") {
    const std::string env =
        "# full line comment\n"
        "  ; also a comment with leading whitespace\n"
        "export server_url=http://example.com   # trailing comment\n"
        "download_dir=sdmc:/romm_cache/switch ; another trailing comment\n"
        "password=abc#123\n"
        "username=\"user;name\" # comment after quoted value\n"
        "log_level=info\n";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseEnvString(env, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.serverUrl == "http://example.com");
    REQUIRE(cfg.downloadDir == "sdmc:/romm_cache/switch");
    REQUIRE(cfg.password == "abc#123");          // no whitespace before '#', so keep it
    REQUIRE(cfg.username == "user;name");        // ';' inside quotes is part of the value
    REQUIRE(cfg.logLevel == "info");
}

TEST_CASE("parseJsonString parses canonical schema v1") {
    const std::string json =
        "{"
        "\"schema_version\":1,"
        "\"server_url\":\"http://example.com\","
        "\"download_dir\":\"sdmc:/romm_cache/switch\","
        "\"log_level\":\"DeBuG\","
        "\"http_timeout_seconds\":17"
        "}";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseJsonString(json, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.schemaVersion == 1);
    REQUIRE(cfg.serverUrl == "http://example.com");
    REQUIRE(cfg.downloadDir == "sdmc:/romm_cache/switch");
    REQUIRE(cfg.logLevel == "debug");
    REQUIRE(cfg.httpTimeoutSeconds == 17);
}

TEST_CASE("parseJsonString migrates legacy keys when schema is missing") {
    const std::string json =
        "{"
        "\"SERVER_URL\":\"http://example.com\","
        "\"DOWNLOAD_DIR\":\"sdmc:/romm_cache/switch\","
        "\"LOG_LEVEL\":\"INFO\""
        "}";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseJsonString(json, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.schemaVersion == 1);
    REQUIRE(cfg.serverUrl == "http://example.com");
    REQUIRE(cfg.downloadDir == "sdmc:/romm_cache/switch");
    REQUIRE(cfg.logLevel == "info");
}

TEST_CASE("parseJsonString migrates legacy alias fields") {
    const std::string json =
        "{"
        "\"serverUrl\":\"http://example.com\","
        "\"download_path\":\"sdmc:/romm_cache/switch\","
        "\"platform_id\":\"switch\","
        "\"timeout_seconds\":42"
        "}";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseJsonString(json, cfg, err);
    REQUIRE(ok);
    REQUIRE(err.empty());
    REQUIRE(cfg.schemaVersion == 1);
    REQUIRE(cfg.serverUrl == "http://example.com");
    REQUIRE(cfg.downloadDir == "sdmc:/romm_cache/switch");
    REQUIRE(cfg.platform == "switch");
    REQUIRE(cfg.httpTimeoutSeconds == 42);
}

TEST_CASE("parseJsonString rejects unsupported schema version") {
    const std::string json =
        "{"
        "\"schema_version\":999,"
        "\"server_url\":\"http://example.com\","
        "\"download_dir\":\"sdmc:/romm_cache/switch\""
        "}";
    romm::Config cfg;
    std::string err;
    bool ok = romm::parseJsonString(json, cfg, err);
    REQUIRE_FALSE(ok);
    REQUIRE(err.find("Unsupported config schema_version") != std::string::npos);
}
