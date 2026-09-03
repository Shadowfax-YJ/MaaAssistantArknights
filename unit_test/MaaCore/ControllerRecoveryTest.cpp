#include <catch2/catch_test_macros.hpp>

#include "Controller/ControllerRecoveryPolicy.h"

using namespace asst::controller_recovery;

TEST_CASE("screencap enables transport recovery immediately")
{
    CHECK(allow_reconnect_for_screencap_attempt(0));
    CHECK(allow_reconnect_for_screencap_attempt(1));
    CHECK(MaxScreencapAttempts > 1);
}

TEST_CASE("adb command timeout is reconnectable")
{
    CHECK(should_reconnect_after_command(std::nullopt, true, true, false));
    CHECK(should_reconnect_after_command(1, true, true, false));

    CHECK_FALSE(should_reconnect_after_command(0, true, true, false));
    CHECK_FALSE(should_reconnect_after_command(std::nullopt, false, true, false));
    CHECK_FALSE(should_reconnect_after_command(std::nullopt, true, false, false));
    CHECK_FALSE(should_reconnect_after_command(std::nullopt, true, true, true));
}
