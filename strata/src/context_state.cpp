// save_state / load_state: what the user arranged, in one section of a config

#include "strata/config.hpp"
#include "strata/context.hpp"

#include "context_impl.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <string>

namespace strata {

namespace {

constexpr std::string_view state_version = "1";
constexpr std::size_t      tree_chunk    = 96; // tree nodes per config line

[[nodiscard]] bool parse_hex(std::string_view t, u64& out) noexcept
{
    const auto r = std::from_chars(t.data(), t.data() + t.size(), out, 16);
    return !t.empty() && r.ec == std::errc{} && r.ptr == t.data() + t.size();
}

[[nodiscard]] bool parse_float(std::string_view t, f32& out) noexcept
{
    const auto r = std::from_chars(t.data(), t.data() + t.size(), out);
    return !t.empty() && r.ec == std::errc{} && r.ptr == t.data() + t.size() && std::isfinite(out);
}

// the next space-separated word of `s`
[[nodiscard]] std::string_view next_word(std::string_view& s) noexcept
{
    while (!s.empty() && s.front() == ' ') { s.remove_prefix(1); }
    const std::size_t n = std::min(s.find(' '), s.size());
    const std::string_view w = s.substr(0, n);
    s.remove_prefix(n);
    return w;
}

} // namespace

void context::save_state(config& cfg, std::string_view section) const
{
    // replaced, not merged: gone tables / windows must not return from stale lines
    std::vector<std::string> old;
    for (const config::entry& e : cfg.entries(section)) { old.push_back(e.key); }
    for (const std::string& k : old) { (void)cfg.erase(section, k); }

    cfg.set(section, "version", state_version);

    // the dock layout (includes every window's place, size and collapse), one line per key
    const std::string dock = dock_save_layout();
    std::size_t line_no = 0;
    for (std::size_t i = 0; i < dock.size();) {
        std::size_t e = dock.find('\n', i);
        if (e == std::string::npos) { e = dock.size(); }
        if (e > i) { cfg.set(section, std::format("dock_{:04}", line_no++), std::string_view{dock}.substr(i, e - i)); }
        i = e + 1;
    }

    for (const table_state& t : m_->tables_) {
        if (t.key != 0 && t.inited) { cfg.set(section, std::format("table_{:x}", t.key), table_layout_text(t)); }
    }

    std::string line;
    std::size_t in_line = 0, chunk = 0;
    for (const tree_state& t : m_->tree_states_) {
        line += std::format("{}{:x}:{}", line.empty() ? "" : " ", t.key, t.open ? 1 : 0);
        if (++in_line == tree_chunk) {
            cfg.set(section, std::format("tree_{:04}", chunk++), line);
            line.clear();
            in_line = 0;
        }
    }
    if (!line.empty()) { cfg.set(section, std::format("tree_{:04}", chunk), line); }

    for (const window_state& w : m_->windows_) {
        if (w.key != 0 && w.scroll > 0.0f) { cfg.set(section, std::format("scroll_{:x}", w.key), std::format("{}", w.scroll)); }
    }
    for (const child_state& c : m_->children_) {
        if (c.key != 0 && (c.scroll > 0.0f || c.scroll_x > 0.0f)) {
            cfg.set(section, std::format("scroll_{:x}", c.key), std::format("{} {}", c.scroll, c.scroll_x));
        }
    }
}

bool context::load_state(const config& cfg, std::string_view section)
{
    if (cfg.get(section, "version") != state_version) {
        return false;
    }
    std::string dock;
    for (const config::entry& e : cfg.entries(section)) {
        const std::string_view key = e.key;
        const std::string_view value = e.value;
        if (key.starts_with("dock_")) {
            dock += value;
            dock += '\n';
        } else if (key.starts_with("table_")) {
            u64 k{};
            if (!parse_hex(key.substr(6), k) || k == 0) { continue; }
            const auto it = std::ranges::find_if(m_->table_pending_, [k](const auto& p) { return p.first == k; });
            if (it != m_->table_pending_.end()) { it->second.assign(value); } else { m_->table_pending_.emplace_back(k, std::string{value}); }
        } else if (key.starts_with("tree_")) {
            std::string_view rest = value;
            for (std::string_view w = next_word(rest); !w.empty(); w = next_word(rest)) {
                const std::size_t colon = w.find(':');
                u64 k{};
                if (colon == std::string_view::npos || !parse_hex(w.substr(0, colon), k) || k == 0) { continue; }
                const bool open = w.substr(colon + 1) == "1";
                const auto it = std::lower_bound(m_->tree_states_.begin(), m_->tree_states_.end(), k,
                                                 [](const tree_state& s, id x) { return s.key < x; });
                if (it != m_->tree_states_.end() && it->key == k) { it->open = open; }
                else { m_->tree_states_.insert(it, tree_state{k, 0, open}); } // (its parent scope is learnt when it is shown)
            }
        } else if (key.starts_with("scroll_")) {
            u64 k{};
            if (!parse_hex(key.substr(7), k) || k == 0) { continue; }
            std::string_view rest = value;
            vec2 s{};
            if (!parse_float(next_word(rest), s.y)) { continue; }
            const std::string_view x = next_word(rest);
            if (!x.empty() && !parse_float(x, s.x)) { continue; }
            // a window that exists already takes it now, the rest when they appear
            bool applied = false;
            for (window_state& w : m_->windows_) {
                if (w.key == k) { w.scroll = s.y; applied = true; }
            }
            if (!applied) { m_->scroll_pending_.emplace_back(k, s); }
        }
    }
    if (!dock.empty()) { (void)dock_load_layout(dock); }
    return true;
}

void context::apply_pending_scroll(id key, f32& scroll_y, f32* scroll_x) noexcept
{
    if (m_->scroll_pending_.empty()) {
        return;
    }
    for (auto it = m_->scroll_pending_.begin(); it != m_->scroll_pending_.end(); ++it) {
        if (it->first != key) { continue; }
        scroll_y = it->second.y; // (a window clamps it to its content on the frame it is drawn)
        if (scroll_x != nullptr) { *scroll_x = it->second.x; }
        m_->scroll_pending_.erase(it);
        return;
    }
}

} // namespace strata
