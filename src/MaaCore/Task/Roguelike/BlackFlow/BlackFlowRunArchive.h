#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace asst::blackflow
{
struct RunArchiveResult
{
    std::filesystem::path archive_path;
    std::uint64_t entry_count = 0;
    std::uint64_t uncompressed_bytes = 0;
};

bool archive_completed_run_directory(
    const std::filesystem::path& run_directory,
    RunArchiveResult& result,
    std::string* error = nullptr);

using RunArchiveCompletion = std::function<void(
    const std::filesystem::path& run_directory,
    const RunArchiveResult& result,
    const std::string& error)>;

bool enqueue_completed_run_archive(
    std::filesystem::path run_directory,
    RunArchiveCompletion completion = {},
    std::string* error = nullptr);
} // namespace asst::blackflow
