#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace asst::blackflow
{
struct RecruitmentEvidenceScope
{
    std::uint64_t run_revision = 0;
    std::uint64_t map_generation = 0;
    std::uint64_t page_revision = 0;
    int floor = 0;
    std::string transaction_id;
    std::uint64_t node = 0;

    bool operator==(const RecruitmentEvidenceScope&) const = default;
};

// Retries of one selection share an ID. Only an opened recruitment page consumes
// it; the next choice gets a new ID even if its buttons and names are identical.
class RecruitmentChoiceLedger
{
public:
    const std::string& prepare(const RecruitmentEvidenceScope& scope)
    {
        if (!m_scope.has_value() || *m_scope != scope || m_opened) {
            m_scope = scope;
            m_id = "BF-V" + std::to_string(scope.run_revision) + "-" + std::to_string(++m_sequence);
            m_opened = false;
            m_clicked = false;
            m_attempt = 0;
            m_clicked_attempt = 0;
        }
        ++m_attempt;
        // A failed retry must not erase an earlier dispatched click whose
        // recruitment transition is still arriving.
        return m_id;
    }

    void clicked() noexcept
    {
        m_clicked = true;
        m_clicked_attempt = m_attempt;
    }

    std::uint64_t attempt() const noexcept { return m_attempt; }

    std::uint64_t clicked_attempt() const noexcept { return m_clicked_attempt; }

    std::optional<std::string> opened(const RecruitmentEvidenceScope& scope)
    {
        if (!m_scope.has_value() || *m_scope != scope || !m_clicked) {
            return std::nullopt;
        }
        m_opened = true;
        return m_id;
    }

    void clear_pending() noexcept
    {
        m_scope.reset();
        m_clicked = false;
        m_opened = false;
    }

    void reset() noexcept
    {
        clear_pending();
        m_sequence = 0;
    }

private:
    std::optional<RecruitmentEvidenceScope> m_scope;
    std::string m_id;
    std::uint64_t m_sequence = 0;
    std::uint64_t m_attempt = 0;
    std::uint64_t m_clicked_attempt = 0;
    bool m_clicked = false;
    bool m_opened = false;
};
} // namespace asst::blackflow
