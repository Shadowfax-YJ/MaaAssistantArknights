#ifdef _WIN32

#include "Utils/Platform/Platform.h"

#include <malloc.h>

namespace asst::platform
{
static size_t get_page_size()
{
    SYSTEM_INFO sys_info {};
    GetSystemInfo(&sys_info);
    return sys_info.dwPageSize;
}

const size_t page_size = get_page_size();

void* aligned_alloc(size_t len, size_t align)
{
    return _aligned_malloc(len, align);
}

void aligned_free(void* ptr)
{
    _aligned_free(ptr);
}

os_string to_osstring(const std::string& utf8_str)
{
    if (utf8_str.empty()) {
        return {};
    }

    int len = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), static_cast<int>(utf8_str.size()), nullptr, 0);
    os_string result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), static_cast<int>(utf8_str.size()), result.data(), len);
    return result;
}

std::string from_osstring(const os_string& os_str)
{
    if (os_str.empty()) {
        return {};
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, os_str.c_str(), static_cast<int>(os_str.size()), nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, os_str.c_str(), static_cast<int>(os_str.size()), result.data(), len, nullptr, nullptr);
    return result;
}

std::string path_to_crt_string(const std::filesystem::path& path)
{
    return from_osstring(path.native());
}

std::string path_to_ansi_string(const std::filesystem::path& path)
{
    return from_osstring(path.native());
}

std::string call_command(const std::string& cmdline [[maybe_unused]], bool* exit_flag [[maybe_unused]])
{
    return {};
}
} // namespace asst::platform

#endif
