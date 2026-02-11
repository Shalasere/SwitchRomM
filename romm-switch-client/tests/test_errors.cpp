#include "catch.hpp"
#include "romm/errors.hpp"

TEST_CASE("classifyError maps auth and HTTP status") {
    romm::ErrorInfo info = romm::classifyError("HTTP 401 Unauthorized", romm::ErrorCategory::Network);
    REQUIRE(info.category == romm::ErrorCategory::Auth);
    REQUIRE(info.code == romm::ErrorCode::HttpUnauthorized);
    REQUIRE(info.httpStatus == 401);
    REQUIRE_FALSE(info.retryable);
    REQUIRE_FALSE(info.userMessage.empty());
}

TEST_CASE("classifyError maps unsupported feature") {
    romm::ErrorInfo info = romm::classifyError("Chunked transfer not supported", romm::ErrorCategory::Network);
    REQUIRE(info.category == romm::ErrorCategory::Unsupported);
    REQUIRE(info.code == romm::ErrorCode::UnsupportedFeature);
    REQUIRE_FALSE(info.retryable);
}

TEST_CASE("classifyError maps missing required config") {
    romm::ErrorInfo info = romm::classifyError("Config missing server_url or download_dir.", romm::ErrorCategory::Config);
    REQUIRE(info.category == romm::ErrorCategory::Config);
    REQUIRE(info.code == romm::ErrorCode::MissingRequiredField);
    REQUIRE_FALSE(info.retryable);
}
