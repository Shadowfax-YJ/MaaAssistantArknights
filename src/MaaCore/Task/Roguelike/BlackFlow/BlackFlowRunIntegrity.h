#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace asst::blackflow
{
// Keys are generated per run, held only in memory, and never reused for release signing.
// This is local integrity evidence, not attestation of an official/unmodified client.
class RunIntegrity
{
public:
    explicit RunIntegrity(std::filesystem::path root);
    ~RunIntegrity();
    RunIntegrity(const RunIntegrity&) = delete;
    RunIntegrity& operator=(const RunIntegrity&) = delete;

    void before_write(const std::filesystem::path& path);
    void record_file(const std::filesystem::path& path);
    void record_append(const std::filesystem::path& path, std::string_view bytes);
    void prepare_manifest();
    void verify_files();
    [[nodiscard]] std::string expected_hash(const std::filesystem::path& path) const;
    void sign_zip(const std::filesystem::path& archive);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

void begin_run_integrity(const std::filesystem::path& root);
void forget_run_integrity(const std::filesystem::path& root) noexcept;
std::shared_ptr<RunIntegrity> take_run_integrity(const std::filesystem::path& root);
void run_integrity_before_write(const std::filesystem::path& path);
void run_integrity_record_file(const std::filesystem::path& path);
void run_integrity_record_append(const std::filesystem::path& path, std::string_view bytes);
} // namespace asst::blackflow
