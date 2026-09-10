#include "BlackFlowRunIntegrity.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#include <monocypher-ed25519.h>
#include <monocypher.h>

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <Security/SecRandom.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

#include "BlackFlowRunLog.h"

namespace asst::blackflow
{
namespace
{
constexpr std::string_view IndexName = "integrity.json";
constexpr std::string_view SignatureDomain = "MAA-BLACKFLOW-ARCHIVE-v1\n";

std::string utf8(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return { reinterpret_cast<const char*>(text.data()), text.size() };
}

std::string hex(const unsigned char* bytes, std::size_t size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}

struct FileDigest
{
    crypto_sha512_ctx context {};
    std::uint64_t size = 0;

    FileDigest() { crypto_sha512_init(&context); }

    void append(std::string_view bytes)
    {
        crypto_sha512_update(&context, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        size += bytes.size();
    }

    std::string hash() const
    {
        auto copy = context;
        std::array<std::uint8_t, 64> digest {};
        crypto_sha512_final(&copy, digest.data());
        return hex(digest.data(), digest.size());
    }
};

bool linked(const std::filesystem::path& path)
{
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }
#endif
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path));
}

FileDigest digest_file(const std::filesystem::path& path)
{
    if (linked(path) || !std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("not a regular unlinked run file: " + utf8(path));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read run file: " + utf8(path));
    }
    FileDigest result;
    std::array<char, 64 * 1024> buffer {};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        result.append({ buffer.data(), static_cast<std::size_t>(input.gcount()) });
    }
    if (!input.eof()) {
        throw std::runtime_error("failed while hashing run file: " + utf8(path));
    }
    return result;
}

void random_seed(std::array<std::uint8_t, 32>& seed)
{
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, seed.data(), static_cast<ULONG>(seed.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("system random generator failed");
    }
#elif defined(__APPLE__)
    if (SecRandomCopyBytes(kSecRandomDefault, seed.size(), seed.data()) != errSecSuccess) {
        throw std::runtime_error("system random generator failed");
    }
#else
    std::size_t filled = 0;
    while (filled < seed.size()) {
        const auto count = getrandom(seed.data() + filled, seed.size() - filled, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("system random generator failed");
        }
        filled += static_cast<std::size_t>(count);
    }
#endif
}

std::mutex registry_mutex;
std::map<std::filesystem::path, std::shared_ptr<RunIntegrity>> registry;

std::filesystem::path normalized(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

std::shared_ptr<RunIntegrity> owner(const std::filesystem::path& path)
{
    std::scoped_lock lock(registry_mutex);
    for (auto parent = normalized(path).parent_path(); !parent.empty();) {
        if (auto found = registry.find(parent); found != registry.end()) {
            return found->second;
        }
        const auto next = parent.parent_path();
        if (next == parent) {
            break;
        }
        parent = next;
    }
    return {};
}
} // namespace

struct RunIntegrity::Impl
{
    std::filesystem::path root;
    std::map<std::string, FileDigest> files;
    std::array<std::uint8_t, 64> secret {};
    std::array<std::uint8_t, 32> public_key {};
    std::mutex mutex;
    std::string failure;
    bool sealed = false;

    [[noreturn]] void fail(const std::string& message)
    {
        if (failure.empty()) {
            failure = message;
        }
        throw std::runtime_error(failure);
    }

    void healthy()
    {
        if (!failure.empty()) {
            throw std::runtime_error(failure);
        }
    }

    std::string relative(const std::filesystem::path& path)
    {
        const auto relative = normalized(path).lexically_relative(root);
        if (relative.empty() || relative.is_absolute()) {
            fail("run file lies outside its run directory");
        }
        for (const auto& component : relative) {
            if (component == "..") {
                fail("run file lies outside its run directory");
            }
        }
        return utf8(relative);
    }

    void check(const std::filesystem::path& path, const FileDigest& expected)
    {
        try {
            const auto actual = digest_file(path);
            if (actual.size != expected.size || actual.hash() != expected.hash()) {
                fail("run file changed after being written: " + relative(path));
            }
        }
        catch (const std::exception& error) {
            fail(error.what());
        }
    }

    void verify()
    {
        healthy();
        std::set<std::string> seen;
        if (linked(root)) {
            fail("run directory is a link");
        }
        for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
            if (linked(item.path())) {
                fail("run directory contains a link");
            }
            if (item.is_directory()) {
                continue;
            }
            const auto name = relative(item.path());
            const auto expected = files.find(name);
            if (expected == files.end()) {
                fail("untracked file added to run directory: " + name);
            }
            check(item.path(), expected->second);
            seen.insert(name);
        }
        if (seen.size() != files.size()) {
            fail("tracked run files are missing");
        }
    }
};

RunIntegrity::RunIntegrity(std::filesystem::path root) :
    m_impl(std::make_unique<Impl>())
{
    m_impl->root = normalized(root);
    std::array<std::uint8_t, 32> seed {};
    random_seed(seed);
    crypto_ed25519_key_pair(m_impl->secret.data(), m_impl->public_key.data(), seed.data());
}

RunIntegrity::~RunIntegrity()
{
    crypto_wipe(m_impl->secret.data(), m_impl->secret.size());
}

void RunIntegrity::before_write(const std::filesystem::path& path)
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->healthy();
    if (m_impl->sealed) {
        m_impl->fail("attempted to write a sealed run");
    }
    const auto name = m_impl->relative(path);
    if (auto found = m_impl->files.find(name); found != m_impl->files.end()) {
        m_impl->check(path, found->second);
    }
    else if (std::filesystem::exists(path)) {
        m_impl->fail("untracked file already exists before write: " + name);
    }
}

void RunIntegrity::record_file(const std::filesystem::path& path)
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->healthy();
    if (m_impl->sealed) {
        m_impl->fail("attempted to register a sealed run file");
    }
    m_impl->files.insert_or_assign(m_impl->relative(path), digest_file(path));
}

void RunIntegrity::record_append(const std::filesystem::path& path, std::string_view bytes)
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->healthy();
    if (m_impl->sealed) {
        m_impl->fail("attempted to append to a sealed run");
    }
    // Hash the bytes produced in memory, never reread and bless an altered prefix.
    m_impl->files[m_impl->relative(path)].append(bytes);
}

void RunIntegrity::verify_files()
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->verify();
}

std::string RunIntegrity::expected_hash(const std::filesystem::path& path) const
{
    std::scoped_lock lock(m_impl->mutex);
    return m_impl->files.at(m_impl->relative(path)).hash();
}

void RunIntegrity::prepare_manifest()
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->verify();
    if (m_impl->sealed) {
        m_impl->fail("run integrity manifest already sealed");
    }
    for (const auto& name : { "manifest.json", "run-events.jsonl", "run.log", "replay.html", "replay-data.js" }) {
        if (!m_impl->files.contains(name)) {
            m_impl->fail("required run file is missing: " + std::string(name));
        }
    }
    std::vector<json::value> events;
    std::ifstream input(m_impl->root / "run-events.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (input.eof()) {
            m_impl->fail("run event stream ends with an incomplete line");
        }
        auto event = json::parse(line);
        if (!event.has_value()) {
            m_impl->fail("invalid run event JSON");
        }
        events.emplace_back(std::move(*event));
    }
    std::string error;
    if (events.empty() || !validate_run_log_replay_stream(events, &error)) {
        m_impl->fail("run event stream is incomplete: " + error);
    }
    if (events.front().get("action", "") != "run.started" || events.back().get("action", "") != "run.ended") {
        m_impl->fail("run is missing its start or end event");
    }
    bool fresh_start = false;
    for (const auto& event : events) {
        if (event.get("elapsed_ms", std::int64_t { -1 }) < 0) {
            m_impl->fail("invalid event elapsed time");
        }
        if (event.get("action", "") == "run.start_confirmed") {
            fresh_start = true;
        }
        const auto image = event.get("image", "path", std::string());
        if (!image.empty() && !m_impl->files.contains(image)) {
            m_impl->fail("run event references a missing image");
        }
    }
    if (!fresh_start) {
        m_impl->fail("run was not collected from a confirmed fresh start");
    }
    auto manifest = json::open(m_impl->root / "manifest.json");
    if (!manifest.has_value() || !manifest->is_object() || manifest->get("collector_version", "").empty()) {
        m_impl->fail("invalid collector manifest");
    }
    json::array files;
    for (const auto& [name, digest] : m_impl->files) {
        files.emplace_back(json::object { { "path", name }, { "size", digest.size }, { "sha512", digest.hash() } });
    }
    const json::object index {
        { "schema_version", 1 },
        { "format", "maa-blackflow-local-integrity" },
        { "origin_attested", false },
        { "started_from_beginning", true },
        { "collector_version", manifest->get("collector_version", "") },
        { "run_directory", utf8(m_impl->root.filename()) },
        { "event_count", events.size() },
        { "public_key", hex(m_impl->public_key.data(), m_impl->public_key.size()) },
        { "hash_algorithm", "SHA-512" },
        { "files", std::move(files) },
    };
    const auto path = m_impl->root / IndexName;
    if (std::filesystem::exists(path)) {
        m_impl->fail("run already contains an integrity manifest");
    }
    const std::string bytes = json::value(index).format();
    std::ofstream output(path, std::ios::binary);
    output << bytes;
    output.flush();
    if (!output) {
        m_impl->fail("failed to write run integrity manifest");
    }
    FileDigest digest;
    digest.append(bytes);
    m_impl->files.emplace(std::string(IndexName), std::move(digest));
    m_impl->sealed = true;
}

void RunIntegrity::sign_zip(const std::filesystem::path& archive)
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->healthy();
    if (!m_impl->sealed) {
        m_impl->fail("cannot sign an unsealed run");
    }
    const std::string digest = digest_file(archive).hash();
    const std::string message = std::string(SignatureDomain) + digest;
    std::array<std::uint8_t, 64> signature {};
    crypto_ed25519_sign(
        signature.data(),
        m_impl->secret.data(),
        reinterpret_cast<const std::uint8_t*>(message.data()),
        message.size());
    if (crypto_ed25519_check(
            signature.data(),
            m_impl->public_key.data(),
            reinterpret_cast<const std::uint8_t*>(message.data()),
            message.size()) != 0) {
        m_impl->fail("local archive signature self-check failed");
    }
    const std::string comment = json::value(
                                    json::object {
                                        { "format", "maa-blackflow-auto-archive" },
                                        { "schema_version", 1 },
                                        { "algorithm", "Ed25519" },
                                        { "origin_attested", false },
                                        { "archive_sha512", digest },
                                        { "public_key", hex(m_impl->public_key.data(), m_impl->public_key.size()) },
                                        { "signature", hex(signature.data(), signature.size()) },
                                    })
                                    .to_string();
    if (comment.size() > 65535) {
        m_impl->fail("archive signature comment is too long");
    }
    std::fstream file(archive, std::ios::in | std::ios::out | std::ios::binary);
    std::array<char, 22> end {};
    file.seekg(-22, std::ios::end);
    file.read(end.data(), end.size());
    if (!file || std::string_view(end.data(), 4) != std::string_view("PK\x05\x06", 4) || end[20] != 0 || end[21] != 0) {
        m_impl->fail("archive has no unsigned ZIP end record");
    }
    file.seekp(-2, std::ios::end);
    const std::array<char, 2> length { static_cast<char>(comment.size() & 255),
                                       static_cast<char>(comment.size() >> 8) };
    file.write(length.data(), length.size());
    file.write(comment.data(), static_cast<std::streamsize>(comment.size()));
    file.flush();
    if (!file) {
        m_impl->fail("failed to append archive signature");
    }
    file.close();
    // Verify the persisted signature envelope and every byte covered by it, not just the in-memory signature.
    std::ifstream check(archive, std::ios::binary);
    const auto signed_size = std::filesystem::file_size(archive);
    const auto payload_size = signed_size - comment.size() - 2;
    FileDigest persisted;
    std::array<char, 64 * 1024> buffer {};
    std::uint64_t remaining = payload_size;
    while (remaining != 0) {
        const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
        check.read(buffer.data(), count);
        if (check.gcount() != count) {
            m_impl->fail("signed archive was truncated");
        }
        persisted.append({ buffer.data(), static_cast<std::size_t>(count) });
        remaining -= count;
    }
    std::array<char, 2> saved_length {};
    check.read(saved_length.data(), saved_length.size());
    std::string saved(comment.size(), '\0');
    check.read(saved.data(), static_cast<std::streamsize>(saved.size()));
    persisted.append(std::string_view("\0\0", 2));
    if (!check || saved_length != length || saved != comment || persisted.hash() != digest) {
        m_impl->fail("persisted archive signature verification failed");
    }
}

void begin_run_integrity(const std::filesystem::path& root)
{
    const auto path = normalized(root);
    auto session = std::make_shared<RunIntegrity>(path);
    std::scoped_lock lock(registry_mutex);
    if (!registry.emplace(path, std::move(session)).second) {
        throw std::runtime_error("run integrity already initialized");
    }
}

void forget_run_integrity(const std::filesystem::path& root) noexcept
{
    try {
        std::scoped_lock lock(registry_mutex);
        registry.erase(normalized(root));
    }
    catch (...) {
    }
}

std::shared_ptr<RunIntegrity> take_run_integrity(const std::filesystem::path& root)
{
    std::scoped_lock lock(registry_mutex);
    auto found = registry.find(normalized(root));
    if (found == registry.end()) {
        return {};
    }
    auto session = std::move(found->second);
    registry.erase(found);
    return session;
}

void run_integrity_before_write(const std::filesystem::path& path)
{
    if (auto session = owner(path)) {
        session->before_write(path);
    }
}

void run_integrity_record_file(const std::filesystem::path& path)
{
    if (auto session = owner(path)) {
        session->record_file(path);
    }
}

void run_integrity_record_append(const std::filesystem::path& path, std::string_view bytes)
{
    if (auto session = owner(path)) {
        session->record_append(path, bytes);
    }
}
} // namespace asst::blackflow
