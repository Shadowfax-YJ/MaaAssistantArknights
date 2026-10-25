#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <meojson/json.hpp>

namespace asst::blackflow
{
inline std::int64_t evidence_unix_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// An observation reference, not an item acquisition or a run-log sequence.
inline json::object collection_popup_evidence_sample(
    std::uint64_t run_revision,
    std::uint64_t ordinal,
    json::object state,
    std::int64_t sampled_at = evidence_unix_time_ms())
{
    return {
        { "evidence_id", "popup-" + std::to_string(run_revision) + "-" + std::to_string(ordinal) },
        { "sample_ordinal", ordinal },
        { "sampled_at_unix_ms", sampled_at },
        { "sample_state", std::move(state) },
    };
}
}
