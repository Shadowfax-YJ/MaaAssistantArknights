#pragma once
#include "Config/AbstractConfig.h"

#include <optional>
#include <unordered_set>
#include <vector>

#include "Common/AsstBattleDef.h"

namespace asst
{
class RoguelikeCopilotConfig final : public MAA_NS::SingletonHolder<RoguelikeCopilotConfig>, public AbstractConfig
{
public:
    virtual ~RoguelikeCopilotConfig() override = default;

    virtual bool load(const std::filesystem::path& path) override;

    std::optional<battle::roguelike::CombatData> get_stage_data(const std::string& stage_name) const;
    std::vector<std::string> get_stage_names(const std::string& theme) const;

protected:
    virtual bool parse(const json::value& json) override;
    std::unordered_map<std::string, battle::roguelike::CombatData> m_stage_data;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_stage_names_by_theme;
    std::string m_loading_theme;
};

inline static auto& RoguelikeCopilot = RoguelikeCopilotConfig::get_instance();
}
