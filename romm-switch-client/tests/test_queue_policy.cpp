#include "catch.hpp"
#include "romm/status.hpp"
#include "romm/queue_policy.hpp"

TEST_CASE("canEnqueueGame blocks active queue") {
    romm::Status st;
    romm::Game g; g.id = "1";
    st.downloadQueue.push_back(romm::QueueItem{g, romm::QueueState::Pending, ""});
    romm::Game attempt; attempt.id = "1";
    REQUIRE_FALSE(romm::canEnqueueGame(st, attempt));
}

TEST_CASE("canEnqueueGame blocks history this session") {
    romm::Status st;
    romm::Game g; g.id = "2";
    st.downloadHistory.push_back(romm::QueueItem{g, romm::QueueState::Completed, ""});
    romm::Game attempt; attempt.id = "2";
    REQUIRE_FALSE(romm::canEnqueueGame(st, attempt));
}

TEST_CASE("canEnqueueGame allows failed history") {
    romm::Status st;
    romm::Game g; g.id = "3";
    st.downloadHistory.push_back(romm::QueueItem{g, romm::QueueState::Failed, "net"});
    romm::Game attempt; attempt.id = "3";
    REQUIRE(romm::canEnqueueGame(st, attempt));
}

TEST_CASE("canEnqueueGame allows new id") {
    romm::Status st;
    romm::Game attempt; attempt.id = "fresh";
    REQUIRE(romm::canEnqueueGame(st, attempt));
}
