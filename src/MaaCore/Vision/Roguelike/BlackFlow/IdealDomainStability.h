#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace asst::blackflow::perception
{
// 理想源和理想域属于一张地图，而不是某一帧截图。该状态对象与拓扑缓存采用
// 相同生命周期：换地图时 reset；同图一旦确认，后续帧只能复用，不能改写位置。
template <typename Coordinate>
class SameMapIdealDomainState
{
public:
    void observe(
        std::string_view status,
        const std::optional<Coordinate>& source,
        const std::vector<Coordinate>& domain)
    {
        if (status != "recognized" || !source.has_value()) {
            return;
        }
        if (!m_source.has_value()) {
            m_source = source;
            m_domain = domain;
        }
    }

    void reset()
    {
        m_source.reset();
        m_domain.clear();
    }

    [[nodiscard]] const std::optional<Coordinate>& source() const noexcept { return m_source; }
    [[nodiscard]] const std::vector<Coordinate>& domain() const noexcept { return m_domain; }

private:
    std::optional<Coordinate> m_source;
    std::vector<Coordinate> m_domain;
};
} // namespace asst::blackflow::perception
