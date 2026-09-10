#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace asst
{
template <typename Role, typename RoleLookup>
std::optional<Role> resolve_recruitment_role(
    const std::unordered_set<std::string>& detected_names,
    RoleLookup&& lookup_roles,
    Role unknown_role)
{
    std::unordered_set<Role> candidates;
    std::size_t known_names = 0;
    for (const auto& name : detected_names) {
        auto roles = lookup_roles(name);
        if (name == "阿米娅") {
            // 招募页不显示转职后缀；合并三种形态后，由同页其他干员确认职业。
            roles.merge(lookup_roles("阿米娅-WARRIOR"));
            roles.merge(lookup_roles("阿米娅-MEDIC"));
        }
        roles.erase(unknown_role);
        if (roles.empty()) {
            continue;
        }
        if (known_names++ == 0) {
            candidates = std::move(roles);
        }
        else {
            std::erase_if(candidates, [&](Role role) { return !roles.contains(role); });
        }
    }
    // 至少两名独立干员互相印证；多职业同名干员保留全部职业参与交集。
    if (known_names < 2 || candidates.size() != 1) {
        return std::nullopt;
    }
    return *candidates.begin();
}
} // namespace asst
