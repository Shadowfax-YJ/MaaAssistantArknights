#include "BlackFlowArchiveTestFixture.h"
#include "Task/Roguelike/BlackFlow/BlackFlowRunArchive.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>

using namespace asst::blackflow;

namespace
{
struct Fixture
{
    std::filesystem::path parent =
        std::filesystem::temp_directory_path() /
        ("maa-integrity-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path run = parent / "run-fixture";

    explicit Fixture(bool fresh = true) { prepare_archive_fixture(run, fresh); }

    ~Fixture()
    {
        forget_run_integrity(run);
        std::error_code ignored;
        std::filesystem::remove_all(parent, ignored);
    }
};
}

TEST_CASE("BlackFlow integrity rejects modified added and removed files")
{
    Fixture fixture;
    SECTION("modified same-size data")
    {
        std::ofstream(fixture.run / "run.log", std::ios::binary) << "fixture HUMAN log\n";
    }
    SECTION("extra file")
    {
        std::ofstream(fixture.run / "extra.txt") << "untracked";
    }
    SECTION("missing file")
    {
        std::filesystem::remove(fixture.run / "replay.html");
    }
    RunArchiveResult result;
    std::string error;
    REQUIRE_FALSE(archive_completed_run_directory(fixture.run, result, &error));
    INFO(error);
    REQUIRE_FALSE(error.empty());
    REQUIRE(std::filesystem::is_directory(fixture.run));
    REQUIRE_FALSE(std::filesystem::exists(fixture.parent / "run-fixture.zip"));
}

TEST_CASE("BlackFlow integrity cannot bless an altered log prefix by appending")
{
    Fixture fixture;
    const auto path = fixture.run / "run.log";
    {
        std::ofstream(path, std::ios::binary) << "altered human log\n";
    }
    {
        std::ofstream(path, std::ios::binary | std::ios::app) << "next\n";
    }
    run_integrity_record_append(path, "next\n");
    RunArchiveResult result;
    std::string error;
    REQUIRE_FALSE(archive_completed_run_directory(fixture.run, result, &error));
    REQUIRE(error.find("changed") != std::string::npos);
}

TEST_CASE("BlackFlow integrity remains failed after an altered file is restored")
{
    Fixture fixture;
    const auto path = fixture.run / "run.log";
    {
        std::ofstream(path, std::ios::binary) << "changed";
    }
    REQUIRE_THROWS(run_integrity_before_write(path));
    {
        std::ofstream(path, std::ios::binary) << "fixture human log\n";
    }
    REQUIRE_THROWS(run_integrity_before_write(path));
    RunArchiveResult result;
    std::string error;
    REQUIRE_FALSE(archive_completed_run_directory(fixture.run, result, &error));
}

TEST_CASE("BlackFlow integrity rejects a run collected from the middle")
{
    Fixture fixture(false);
    RunArchiveResult result;
    std::string error;
    REQUIRE_FALSE(archive_completed_run_directory(fixture.run, result, &error));
    REQUIRE(error.find("fresh start") != std::string::npos);
    REQUIRE(std::filesystem::is_directory(fixture.run));
}

TEST_CASE("BlackFlow archive rejects directories without a live recording ledger")
{
    Fixture fixture;
    forget_run_integrity(fixture.run);
    RunArchiveResult result;
    std::string error;
    REQUIRE_FALSE(archive_completed_run_directory(fixture.run, result, &error));
    REQUIRE(error.find("live integrity ledger") != std::string::npos);
}

TEST_CASE("BlackFlow integrity validates event content even when file digests match")
{
    Fixture fixture;
    const auto path = fixture.run / "run-events.jsonl";
    std::vector<json::value> events;
    {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            events.push_back(json::parse(line).value());
        }
    }
    bool trailing_newline = true;
    SECTION("event sequence gap")
    {
        events[1]["sequence"] = 3;
    }
    SECTION("negative event clock")
    {
        events[0]["elapsed_ms"] = -1;
    }
    SECTION("missing referenced image")
    {
        events[1]["image"] = json::object { { "path", "images/missing.jpg" } };
    }
    SECTION("missing end event")
    {
        events.back()["action"] = "map.observed";
    }
    SECTION("incomplete final line")
    {
        trailing_newline = false;
    }
    run_integrity_before_write(path);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        for (std::size_t index = 0; index < events.size(); ++index) {
            output << events[index].to_string();
            if (index + 1 < events.size() || trailing_newline) {
                output << '\n';
            }
        }
    }
    run_integrity_record_file(path);
    RunArchiveResult result;
    std::string error;
    REQUIRE_FALSE(archive_completed_run_directory(fixture.run, result, &error));
    INFO(error);
    REQUIRE_FALSE(error.empty());
    REQUIRE(std::filesystem::is_directory(fixture.run));
    REQUIRE_FALSE(std::filesystem::exists(fixture.parent / "run-fixture.zip"));
}

TEST_CASE("BlackFlow generates an archive for independent signature verification", "[.archive-fixture]")
{
    const char* destination = std::getenv("BLACKFLOW_ARCHIVE_FIXTURE_OUTPUT");
    REQUIRE(destination != nullptr);
    Fixture fixture;
    RunArchiveResult result;
    std::string error;
    REQUIRE(archive_completed_run_directory(fixture.run, result, &error));
    std::filesystem::copy_file(
        result.archive_path,
        std::filesystem::path(destination),
        std::filesystem::copy_options::overwrite_existing);
}
