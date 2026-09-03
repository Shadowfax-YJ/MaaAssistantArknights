#include "BlackFlowRunArchive.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <zlib.h>

namespace asst::blackflow
{
namespace
{
constexpr std::uint32_t ZipLocalHeaderSignature = 0x04034b50U;
constexpr std::uint32_t ZipDataDescriptorSignature = 0x08074b50U;
constexpr std::uint32_t ZipCentralHeaderSignature = 0x02014b50U;
constexpr std::uint32_t ZipEndSignature = 0x06054b50U;
constexpr std::uint16_t ZipVersion = 20;
constexpr std::uint16_t ZipDataDescriptorAndUtf8Flags = 0x0808U;
constexpr std::uint16_t ZipStoreMethod = 0;
constexpr std::uint16_t ZipDeflateMethod = 8;
constexpr std::uint16_t ZipEpochDate = 33;
constexpr std::size_t IoBufferSize = 64U * 1024U;

void set_error(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::string path_to_utf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return { reinterpret_cast<const char*>(value.data()), value.size() };
}

bool write_u16(std::ostream& output, std::uint16_t value)
{
    const std::array<char, 2> bytes {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
    };
    output.write(bytes.data(), bytes.size());
    return static_cast<bool>(output);
}

bool write_u32(std::ostream& output, std::uint32_t value)
{
    const std::array<char, 4> bytes {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    output.write(bytes.data(), bytes.size());
    return static_cast<bool>(output);
}

bool read_exact(std::istream& input, void* destination, std::size_t size)
{
    input.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

std::uint16_t read_u16(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t read_u32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

struct ZipEntry
{
    std::filesystem::path source;
    std::string name;
    std::uint16_t method = ZipDeflateMethod;
    std::uint32_t crc = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t local_offset = 0;
};

bool should_store_without_deflating(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".webp";
}

bool copy_stored_file(
    std::ifstream& input,
    std::ofstream& output,
    std::uint32_t& crc,
    std::uint64_t& written,
    std::string* error)
{
    std::array<char, IoBufferSize> buffer {};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            continue;
        }
        crc = static_cast<std::uint32_t>(::crc32(
            crc,
            reinterpret_cast<const Bytef*>(buffer.data()),
            static_cast<uInt>(count)));
        output.write(buffer.data(), count);
        written += static_cast<std::uint64_t>(count);
        if (!output) {
            set_error(error, "failed to write stored ZIP entry data");
            return false;
        }
    }
    if (input.bad()) {
        set_error(error, "failed to read run log file");
        return false;
    }
    return true;
}

bool copy_deflated_file(
    std::ifstream& input,
    std::ofstream& output,
    std::uint32_t& crc,
    std::uint64_t& uncompressed,
    std::uint64_t& compressed,
    std::string* error)
{
    z_stream stream {};
    if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        set_error(error, "failed to initialize ZIP deflater");
        return false;
    }
    std::array<std::uint8_t, IoBufferSize> input_buffer {};
    std::array<std::uint8_t, IoBufferSize> output_buffer {};
    bool ok = true;
    while (input && ok) {
        input.read(reinterpret_cast<char*>(input_buffer.data()), static_cast<std::streamsize>(input_buffer.size()));
        const std::streamsize count = input.gcount();
        if (count <= 0) {
            continue;
        }
        crc = static_cast<std::uint32_t>(::crc32(crc, input_buffer.data(), static_cast<uInt>(count)));
        uncompressed += static_cast<std::uint64_t>(count);
        stream.next_in = input_buffer.data();
        stream.avail_in = static_cast<uInt>(count);
        while (stream.avail_in > 0) {
            stream.next_out = output_buffer.data();
            stream.avail_out = static_cast<uInt>(output_buffer.size());
            if (deflate(&stream, Z_NO_FLUSH) != Z_OK) {
                set_error(error, "failed to deflate run log file");
                ok = false;
                break;
            }
            const std::size_t produced = output_buffer.size() - stream.avail_out;
            output.write(reinterpret_cast<const char*>(output_buffer.data()), static_cast<std::streamsize>(produced));
            compressed += produced;
            if (!output) {
                set_error(error, "failed to write deflated ZIP entry data");
                ok = false;
                break;
            }
        }
    }
    if (input.bad() && ok) {
        set_error(error, "failed to read run log file");
        ok = false;
    }
    int status = Z_OK;
    while (ok && status != Z_STREAM_END) {
        stream.next_out = output_buffer.data();
        stream.avail_out = static_cast<uInt>(output_buffer.size());
        status = deflate(&stream, Z_FINISH);
        if (status != Z_OK && status != Z_STREAM_END) {
            set_error(error, "failed to finish deflated ZIP entry");
            ok = false;
            break;
        }
        const std::size_t produced = output_buffer.size() - stream.avail_out;
        output.write(reinterpret_cast<const char*>(output_buffer.data()), static_cast<std::streamsize>(produced));
        compressed += produced;
        if (!output) {
            set_error(error, "failed to finish ZIP entry data");
            ok = false;
        }
    }
    deflateEnd(&stream);
    return ok;
}

bool write_zip_entry(std::ofstream& output, ZipEntry& entry, std::string* error)
{
    const auto offset = output.tellp();
    if (offset < 0 || static_cast<std::uint64_t>(offset) > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "run archive exceeds the ZIP32 size limit");
        return false;
    }
    entry.local_offset = static_cast<std::uint32_t>(offset);
    if (!write_u32(output, ZipLocalHeaderSignature) || !write_u16(output, ZipVersion) ||
        !write_u16(output, ZipDataDescriptorAndUtf8Flags) || !write_u16(output, entry.method) ||
        !write_u16(output, 0) || !write_u16(output, ZipEpochDate) || !write_u32(output, 0) ||
        !write_u32(output, 0) || !write_u32(output, 0) ||
        !write_u16(output, static_cast<std::uint16_t>(entry.name.size())) || !write_u16(output, 0)) {
        set_error(error, "failed to write ZIP local header");
        return false;
    }
    output.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    if (!output) {
        set_error(error, "failed to write ZIP entry name");
        return false;
    }

    std::ifstream input(entry.source, std::ios::binary);
    if (!input) {
        set_error(error, "failed to open run log file: " + path_to_utf8(entry.source.filename()));
        return false;
    }
    std::uint32_t crc = static_cast<std::uint32_t>(::crc32(0L, Z_NULL, 0));
    std::uint64_t uncompressed = 0;
    std::uint64_t compressed = 0;
    const bool copied = entry.method == ZipStoreMethod
        ? copy_stored_file(input, output, crc, uncompressed, error)
        : copy_deflated_file(input, output, crc, uncompressed, compressed, error);
    if (!copied) {
        return false;
    }
    if (entry.method == ZipStoreMethod) {
        compressed = uncompressed;
    }
    if (uncompressed > std::numeric_limits<std::uint32_t>::max() ||
        compressed > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "a run log file exceeds the ZIP32 entry size limit");
        return false;
    }
    entry.crc = crc;
    entry.uncompressed_size = static_cast<std::uint32_t>(uncompressed);
    entry.compressed_size = static_cast<std::uint32_t>(compressed);
    if (!write_u32(output, ZipDataDescriptorSignature) || !write_u32(output, entry.crc) ||
        !write_u32(output, entry.compressed_size) || !write_u32(output, entry.uncompressed_size)) {
        set_error(error, "failed to write ZIP data descriptor");
        return false;
    }
    return true;
}

bool verify_zip_entry(std::ifstream& archive, const ZipEntry& entry, std::string* error)
{
    archive.clear();
    archive.seekg(entry.local_offset);
    std::array<std::uint8_t, 30> header {};
    if (!read_exact(archive, header.data(), header.size()) || read_u32(header.data()) != ZipLocalHeaderSignature ||
        read_u16(header.data() + 6) != ZipDataDescriptorAndUtf8Flags ||
        read_u16(header.data() + 8) != entry.method) {
        set_error(error, "run archive has an invalid local ZIP header");
        return false;
    }
    const std::uint16_t name_size = read_u16(header.data() + 26);
    const std::uint16_t extra_size = read_u16(header.data() + 28);
    std::string name(name_size, '\0');
    if (!read_exact(archive, name.data(), name.size()) || name != entry.name) {
        set_error(error, "run archive ZIP entry name verification failed");
        return false;
    }
    archive.seekg(extra_size, std::ios::cur);
    if (!archive) {
        set_error(error, "run archive ZIP entry offset is invalid");
        return false;
    }

    std::array<std::uint8_t, IoBufferSize> input_buffer {};
    std::array<std::uint8_t, IoBufferSize> output_buffer {};
    std::uint64_t remaining = entry.compressed_size;
    std::uint64_t uncompressed = 0;
    std::uint32_t crc = static_cast<std::uint32_t>(::crc32(0L, Z_NULL, 0));
    z_stream stream {};
    const bool deflated = entry.method == ZipDeflateMethod;
    if (deflated && inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        set_error(error, "failed to initialize run archive verifier");
        return false;
    }
    bool ok = true;
    bool stream_finished = !deflated;
    while (remaining > 0 && ok) {
        const std::size_t requested = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, input_buffer.size()));
        if (!read_exact(archive, input_buffer.data(), requested)) {
            set_error(error, "run archive ZIP entry is truncated");
            ok = false;
            break;
        }
        remaining -= requested;
        if (!deflated) {
            crc = static_cast<std::uint32_t>(::crc32(crc, input_buffer.data(), static_cast<uInt>(requested)));
            uncompressed += requested;
            continue;
        }
        stream.next_in = input_buffer.data();
        stream.avail_in = static_cast<uInt>(requested);
        while (stream.avail_in > 0 && ok) {
            stream.next_out = output_buffer.data();
            stream.avail_out = static_cast<uInt>(output_buffer.size());
            const uInt input_before = stream.avail_in;
            const int status = inflate(&stream, Z_NO_FLUSH);
            const std::size_t produced = output_buffer.size() - stream.avail_out;
            if (produced > 0) {
                crc = static_cast<std::uint32_t>(::crc32(crc, output_buffer.data(), static_cast<uInt>(produced)));
                uncompressed += produced;
            }
            if (status == Z_STREAM_END) {
                stream_finished = true;
                if (stream.avail_in != 0 || remaining != 0) {
                    set_error(error, "run archive ZIP entry contains trailing compressed data");
                    ok = false;
                }
                break;
            }
            if (status != Z_OK || (produced == 0 && stream.avail_in == input_before)) {
                set_error(error, "run archive ZIP entry cannot be decompressed");
                ok = false;
            }
        }
    }
    if (deflated) {
        inflateEnd(&stream);
    }
    if (!ok || !stream_finished || uncompressed != entry.uncompressed_size || crc != entry.crc) {
        if (ok) {
            set_error(error, "run archive ZIP entry checksum verification failed");
        }
        return false;
    }
    return true;
}

bool verify_zip_central_directory(
    std::ifstream& archive,
    const std::vector<ZipEntry>& entries,
    std::uint32_t central_offset,
    std::uint32_t central_size,
    std::string* error)
{
    archive.clear();
    archive.seekg(central_offset);
    for (const ZipEntry& entry : entries) {
        std::array<std::uint8_t, 46> header {};
        if (!read_exact(archive, header.data(), header.size()) || read_u32(header.data()) != ZipCentralHeaderSignature ||
            read_u16(header.data() + 8) != ZipDataDescriptorAndUtf8Flags ||
            read_u16(header.data() + 10) != entry.method || read_u32(header.data() + 16) != entry.crc ||
            read_u32(header.data() + 20) != entry.compressed_size ||
            read_u32(header.data() + 24) != entry.uncompressed_size ||
            read_u32(header.data() + 42) != entry.local_offset) {
            set_error(error, "run archive central ZIP directory verification failed");
            return false;
        }
        const std::uint16_t name_size = read_u16(header.data() + 28);
        const std::uint16_t extra_size = read_u16(header.data() + 30);
        const std::uint16_t comment_size = read_u16(header.data() + 32);
        std::string name(name_size, '\0');
        if (!read_exact(archive, name.data(), name.size()) || name != entry.name) {
            set_error(error, "run archive central ZIP entry name verification failed");
            return false;
        }
        archive.seekg(static_cast<std::streamoff>(extra_size) + comment_size, std::ios::cur);
    }
    const auto after_central = archive.tellg();
    if (after_central < 0 || static_cast<std::uint64_t>(after_central) !=
            static_cast<std::uint64_t>(central_offset) + central_size) {
        set_error(error, "run archive central ZIP directory size is invalid");
        return false;
    }
    std::array<std::uint8_t, 22> end {};
    if (!read_exact(archive, end.data(), end.size()) || read_u32(end.data()) != ZipEndSignature ||
        read_u16(end.data() + 8) != entries.size() || read_u16(end.data() + 10) != entries.size() ||
        read_u32(end.data() + 12) != central_size || read_u32(end.data() + 16) != central_offset ||
        read_u16(end.data() + 20) != 0) {
        set_error(error, "run archive ZIP end record verification failed");
        return false;
    }
    return true;
}

bool verify_zip_archive(
    const std::filesystem::path& path,
    const std::vector<ZipEntry>& entries,
    std::uint32_t central_offset,
    std::uint32_t central_size,
    std::string* error)
{
    std::ifstream archive(path, std::ios::binary);
    if (!archive) {
        set_error(error, "failed to reopen completed run archive");
        return false;
    }
    for (const ZipEntry& entry : entries) {
        if (!verify_zip_entry(archive, entry, error)) {
            return false;
        }
    }
    return verify_zip_central_directory(archive, entries, central_offset, central_size, error);
}

bool write_zip_archive(
    const std::filesystem::path& path,
    std::vector<ZipEntry>& entries,
    std::uint32_t& central_offset,
    std::uint32_t& central_size,
    std::string* error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        set_error(error, "failed to create temporary run archive");
        return false;
    }
    for (ZipEntry& entry : entries) {
        if (!write_zip_entry(output, entry, error)) {
            return false;
        }
    }
    const auto central_begin = output.tellp();
    if (central_begin < 0 || static_cast<std::uint64_t>(central_begin) > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "run archive exceeds the ZIP32 size limit");
        return false;
    }
    central_offset = static_cast<std::uint32_t>(central_begin);
    for (const ZipEntry& entry : entries) {
        if (!write_u32(output, ZipCentralHeaderSignature) || !write_u16(output, ZipVersion) ||
            !write_u16(output, ZipVersion) || !write_u16(output, ZipDataDescriptorAndUtf8Flags) ||
            !write_u16(output, entry.method) || !write_u16(output, 0) || !write_u16(output, ZipEpochDate) ||
            !write_u32(output, entry.crc) || !write_u32(output, entry.compressed_size) ||
            !write_u32(output, entry.uncompressed_size) ||
            !write_u16(output, static_cast<std::uint16_t>(entry.name.size())) || !write_u16(output, 0) ||
            !write_u16(output, 0) || !write_u16(output, 0) || !write_u16(output, 0) || !write_u32(output, 0) ||
            !write_u32(output, entry.local_offset)) {
            set_error(error, "failed to write ZIP central directory");
            return false;
        }
        output.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        if (!output) {
            set_error(error, "failed to write ZIP central entry name");
            return false;
        }
    }
    const auto central_end = output.tellp();
    if (central_end < 0 || static_cast<std::uint64_t>(central_end - central_begin) >
            std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "run archive central directory exceeds the ZIP32 size limit");
        return false;
    }
    central_size = static_cast<std::uint32_t>(central_end - central_begin);
    if (!write_u32(output, ZipEndSignature) || !write_u16(output, 0) || !write_u16(output, 0) ||
        !write_u16(output, static_cast<std::uint16_t>(entries.size())) ||
        !write_u16(output, static_cast<std::uint16_t>(entries.size())) || !write_u32(output, central_size) ||
        !write_u32(output, central_offset) || !write_u16(output, 0)) {
        set_error(error, "failed to write ZIP end record");
        return false;
    }
    output.flush();
    if (!output) {
        set_error(error, "failed to flush completed run archive");
        return false;
    }
    output.close();
    return true;
}

bool collect_zip_entries(
    const std::filesystem::path& run_directory,
    std::vector<ZipEntry>& entries,
    std::uint64_t& uncompressed_bytes,
    std::string* error)
{
    std::error_code iterator_error;
    for (std::filesystem::recursive_directory_iterator iterator(run_directory, iterator_error), end;
         !iterator_error && iterator != end;
         iterator.increment(iterator_error)) {
        const auto& item = *iterator;
        if (item.is_symlink(iterator_error)) {
            set_error(error, "run log directory contains a symbolic link");
            return false;
        }
        if (item.is_directory(iterator_error)) {
            continue;
        }
        if (!item.is_regular_file(iterator_error)) {
            set_error(error, "run log directory contains an unsupported filesystem entry");
            return false;
        }
        const std::uintmax_t size = item.file_size(iterator_error);
        if (iterator_error) {
            break;
        }
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            set_error(error, "a run log file exceeds the ZIP32 entry size limit");
            return false;
        }
        const auto relative = std::filesystem::relative(item.path(), run_directory, iterator_error);
        if (iterator_error || relative.empty()) {
            break;
        }
        const std::string name = path_to_utf8(run_directory.filename() / relative);
        if (name.empty() || name.size() > std::numeric_limits<std::uint16_t>::max()) {
            set_error(error, "a run log ZIP entry name is too long");
            return false;
        }
        entries.emplace_back(ZipEntry {
            .source = item.path(),
            .name = name,
            .method = should_store_without_deflating(item.path()) ? ZipStoreMethod : ZipDeflateMethod,
        });
        uncompressed_bytes += size;
    }
    if (iterator_error) {
        set_error(error, "failed to enumerate completed run directory: " + iterator_error.message());
        return false;
    }
    std::ranges::sort(entries, {}, &ZipEntry::name);
    if (entries.size() > std::numeric_limits<std::uint16_t>::max()) {
        set_error(error, "completed run contains too many files for a ZIP32 archive");
        return false;
    }
    return true;
}

class RunArchiveQueue
{
public:
    RunArchiveQueue()
    {
        std::thread([this]() { work(); }).detach();
    }

    bool enqueue(std::filesystem::path run_directory, RunArchiveCompletion completion, std::string* error)
    {
        try {
            std::scoped_lock lock(m_mutex);
            const std::string key = path_to_utf8(run_directory.lexically_normal());
            if (m_pending.contains(key)) {
                return true;
            }
            m_pending.emplace(key);
            m_jobs.emplace_back(Job { std::move(run_directory), std::move(completion), key });
            m_ready.notify_one();
            return true;
        }
        catch (const std::exception& exception) {
            set_error(error, "failed to queue completed run archive: " + std::string(exception.what()));
            return false;
        }
        catch (...) {
            set_error(error, "failed to queue completed run archive");
            return false;
        }
    }

private:
    struct Job
    {
        std::filesystem::path run_directory;
        RunArchiveCompletion completion;
        std::string key;
    };

    void work() noexcept
    {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_ready.wait(lock, [this]() { return !m_jobs.empty(); });
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }

            RunArchiveResult result;
            std::string error;
            try {
                archive_completed_run_directory(job.run_directory, result, &error);
            }
            catch (const std::exception& exception) {
                error = "completed run archive threw an exception: " + std::string(exception.what());
            }
            catch (...) {
                error = "completed run archive threw an unknown exception";
            }
            if (job.completion) {
                try {
                    job.completion(job.run_directory, result, error);
                }
                catch (...) {
                }
            }
            {
                std::scoped_lock lock(m_mutex);
                m_pending.erase(job.key);
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Job> m_jobs;
    std::set<std::string> m_pending;
};

RunArchiveQueue& archive_queue()
{
    // 进程结束时无需等待可能仍在进行的诊断压缩；未完成时原目录会被保留，
    // 下次启动不会因静态析构顺序或 join 阻塞主进程退出。
    static RunArchiveQueue* queue = new RunArchiveQueue();
    return *queue;
}
} // namespace

bool archive_completed_run_directory(
    const std::filesystem::path& run_directory,
    RunArchiveResult& result,
    std::string* error)
{
    result = {};
    if (error != nullptr) {
        error->clear();
    }
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(run_directory, filesystem_error) || filesystem_error ||
        !run_directory.filename().string().starts_with("run-")) {
        set_error(error, "completed run archive source is not a run directory");
        return false;
    }

    std::filesystem::path archive_path = run_directory;
    archive_path += ".zip";
    if (std::filesystem::exists(archive_path, filesystem_error)) {
        set_error(error, "completed run archive already exists");
        return false;
    }
    std::filesystem::path temporary_archive = archive_path;
    temporary_archive += ".tmp";
    std::filesystem::remove(temporary_archive, filesystem_error);

    std::vector<ZipEntry> entries;
    std::uint64_t uncompressed_bytes = 0;
    if (!collect_zip_entries(run_directory, entries, uncompressed_bytes, error)) {
        return false;
    }
    std::uint32_t central_offset = 0;
    std::uint32_t central_size = 0;
    if (!write_zip_archive(temporary_archive, entries, central_offset, central_size, error) ||
        !verify_zip_archive(temporary_archive, entries, central_offset, central_size, error)) {
        std::filesystem::remove(temporary_archive, filesystem_error);
        return false;
    }

    std::filesystem::rename(temporary_archive, archive_path, filesystem_error);
    if (filesystem_error) {
        set_error(error, "failed to publish completed run archive: " + filesystem_error.message());
        std::filesystem::remove(temporary_archive, filesystem_error);
        return false;
    }

    result.archive_path = archive_path;
    result.entry_count = entries.size();
    result.uncompressed_bytes = uncompressed_bytes;
    const std::uintmax_t removed = std::filesystem::remove_all(run_directory, filesystem_error);
    if (filesystem_error || removed == 0) {
        set_error(
            error,
            "completed run was archived but its source directory could not be removed" +
                (filesystem_error ? ": " + filesystem_error.message() : std::string {}));
        return false;
    }
    return true;
}

bool enqueue_completed_run_archive(
    std::filesystem::path run_directory,
    RunArchiveCompletion completion,
    std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    try {
        return archive_queue().enqueue(std::move(run_directory), std::move(completion), error);
    }
    catch (const std::exception& exception) {
        set_error(error, "failed to initialize completed run archive queue: " + std::string(exception.what()));
        return false;
    }
    catch (...) {
        set_error(error, "failed to initialize completed run archive queue");
        return false;
    }
}
} // namespace asst::blackflow
