#pragma once

#include <array>
#include <string>
#include <string_view>

#include <meojson/json.hpp>

namespace asst::utils
{
template <size_t Size>
bool has_exact_json_fields(const json::value& value, const std::array<std::string_view, Size>& fields)
{
    if (!value.is_object() || value.as_object().size() != Size) {
        return false;
    }

    for (const auto field : fields) {
        if (!value.contains(std::string(field))) {
            return false;
        }
    }
    return true;
}
} // namespace asst::utils
