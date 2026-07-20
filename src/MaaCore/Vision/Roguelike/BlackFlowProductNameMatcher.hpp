#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace asst
{
enum class BlackFlowProductNameMatchKind
{
    Exact,
    Fuzzy,
    Unmatched,
};

struct BlackFlowProductNameMatch
{
    BlackFlowProductNameMatchKind kind = BlackFlowProductNameMatchKind::Unmatched;
    std::string standard_product_name;
};

class BlackFlowProductNameMatcher
{
public:
    explicit BlackFlowProductNameMatcher(std::span<const std::string> standard_product_names);

    BlackFlowProductNameMatch match(std::string_view ocr_text, double ocr_score) const;

private:
    struct StandardProductName
    {
        std::string text;
        std::u32string code_points;
    };

    std::vector<StandardProductName> m_standard_product_names;
};
} // namespace asst
