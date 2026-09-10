#pragma once
#include "Task/Roguelike/BlackFlow/BlackFlowRunIntegrity.h"
#include <filesystem>
#include <fstream>
#include <meojson/json.hpp>

inline void prepare_archive_fixture(const std::filesystem::path& run, bool fresh = true)
{
    std::filesystem::create_directories(run);
    std::ofstream(run / "manifest.json", std::ios::binary) << R"({"collector_version":"test","schema_version":1})";
    std::ofstream(run / "run.log", std::ios::binary) << "fixture human log\n";
    std::ofstream(run / "replay.html", std::ios::binary) << "<html>fixture</html>";
    std::ofstream(run / "replay-data.js", std::ios::binary) << "const BLACKFLOW_RUN_EVENTS=[];\n";
    {
        std::ofstream events(run / "run-events.jsonl", std::ios::binary);
        int sequence = 0;
        for (const auto& action : { "run.started", fresh ? "run.start_confirmed" : "map.observed", "run.ended" }) {
            ++sequence;
            events << json::value(
                          json::object {
                              { "schema_version", 1 },
                              { "sequence", sequence },
                              { "elapsed_ms", sequence },
                              { "timestamp", "2026-09-11T00:00:00Z" },
                              { "level", "INFO" },
                              { "action", action },
                          })
                          .to_string()
                   << '\n';
        }
    }
    asst::blackflow::begin_run_integrity(run);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(run)) {
        if (entry.is_regular_file()) {
            asst::blackflow::run_integrity_record_file(entry.path());
        }
    }
}
