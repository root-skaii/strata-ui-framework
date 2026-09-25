#include "strata/keybinds.hpp"

#include <algorithm>
#include <cctype>

namespace strata {

u32 keybinds::add(std::string_view name, std::string_view default_chord, std::string_view description, std::string_view context)
{
    for (std::size_t i = 0; i < actions_.size(); ++i) {
        if (actions_[i].name == name) { return static_cast<u32>(i); }
    }
    key_chord chord;
    if (!chord_from_string(default_chord, chord)) { chord = {}; }
    actions_.push_back({std::string{name}, std::string{description}, std::string{context}, chord, chord});
    return static_cast<u32>(actions_.size() - 1);
}

const keybinds::action* keybinds::find(std::string_view name) const noexcept
{
    for (const action& a : actions_) {
        if (a.name == name) { return &a; }
    }
    return nullptr;
}

bool keybinds::bind(std::string_view name, const key_chord& chord)
{
    for (action& a : actions_) {
        if (a.name == name) {
            a.chord = chord;
            return true;
        }
    }
    return false;
}

bool keybinds::reset(std::string_view name)
{
    for (action& a : actions_) {
        if (a.name == name) {
            a.chord = a.default_chord;
            return true;
        }
    }
    return false;
}

void keybinds::reset_all() noexcept
{
    for (action& a : actions_) { a.chord = a.default_chord; }
}

void keybinds::set_context(std::string_view context, bool active)
{
    if (context.empty()) { return; }
    const auto it = std::find(active_.begin(), active_.end(), context);
    if (active && it == active_.end()) { active_.emplace_back(context); }
    if (!active && it != active_.end()) { active_.erase(it); }
}

bool keybinds::context_active(std::string_view context) const noexcept
{
    return context.empty() || std::find(active_.begin(), active_.end(), context) != active_.end();
}

const keybinds::action* keybinds::conflict(std::string_view name) const noexcept
{
    const action* self = find(name);
    if (self == nullptr || !self->chord.bound()) { return nullptr; }
    for (const action& a : actions_) {
        if (&a == self || a.chord != self->chord) { continue; }
        if (a.context.empty() || self->context.empty() || a.context == self->context) { return &a; } // they can be on together
    }
    return nullptr;
}

bool keybinds::pressed(const context& ui, std::string_view name) const
{
    const action* a = find(name);
    if (a == nullptr || !a->chord.bound() || !available(*a)) {
        return false;
    }
    if (a->context.empty()) { // a context action on the same chord that is on takes the key from a global one
        for (const action& o : actions_) {
            if (!o.context.empty() && available(o) && o.chord == a->chord) { return false; }
        }
    }
    return ui.chord_pressed(a->chord);
}

std::string keybinds::text(std::string_view name) const
{
    const action* a = find(name);
    return a != nullptr ? chord_to_string(a->chord) : std::string{};
}

void keybinds::store(config& cfg, std::string_view section) const
{
    for (const action& a : actions_) { cfg.set(section, a.name, chord_to_string(a.chord)); }
}

std::size_t keybinds::load(const config& cfg, std::string_view section)
{
    std::size_t changed = 0;
    for (action& a : actions_) {
        if (!cfg.has(section, a.name)) { continue; }
        key_chord chord;
        if (!chord_from_string(cfg.get(section, a.name), chord)) { continue; }
        if (chord != a.chord) { ++changed; }
        a.chord = chord;
    }
    return changed;
}

bool keybind_editor(context& ui, keybinds& binds, std::string_view id, f32 height)
{
    bool changed = false;
    if (!ui.begin_table(id, 3, table_default, height)) {
        return false;
    }
    ui.table_setup_column("Action");
    ui.table_setup_column("Key", 150.0f);
    ui.table_setup_column("", 64.0f);
    (void)ui.table_headers_row();
    for (const keybinds::action& a : binds.actions()) {
        ui.push_id(a.name);
        if (ui.table_next_row()) {
            ui.table_next_column();
            ui.text(a.name);
            if (!a.description.empty()) { ui.tooltip(a.description); }
            if (!a.context.empty()) {
                ui.same_line();
                ui.text_dim("[" + a.context + "]");
            }
            if (const keybinds::action* other = binds.conflict(a.name)) {
                ui.same_line();
                ui.text_colored(color{255, 176, 64, 255}, "!");
                ui.tooltip("the same key is used by " + other->name);
            }

            ui.table_next_column();
            key_chord chord = a.chord;
            if (ui.hotkey_chord("##chord", chord)) {
                (void)binds.bind(a.name, chord);
                changed = true;
            }

            ui.table_next_column();
            if (a.chord != a.default_chord && ui.button("reset")) {
                (void)binds.reset(a.name);
                changed = true;
            }
        }
        ui.pop_id();
    }
    ui.end_table();
    return changed;
}

// the command palette ---------------------------------------------------------------------------

int fuzzy_score(std::string_view query, std::string_view text) noexcept
{
    const auto low = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    int  score = 0;
    std::size_t at = 0;                 // where the search for the next letter starts
    std::size_t last = std::string_view::npos;
    bool any = false;
    for (const char qc : query) {
        if (qc == ' ') { continue; }
        std::size_t found = std::string_view::npos;
        for (std::size_t i = at; i < text.size(); ++i) {
            if (low(text[i]) == low(qc)) { found = i; break; }
        }
        if (found == std::string_view::npos) { return -1; }
        any = true;
        score += 10;
        if (last != std::string_view::npos && found == last + 1) { score += 15; }        // letters that follow each other
        if (found == 0 || std::string_view{" _-./"}.find(text[found - 1]) != std::string_view::npos) { score += 12; } // a word start
        if (last != std::string_view::npos) { score -= static_cast<int>(std::min<std::size_t>(found - last - 1, 8)); }
        last = found;
        at   = found + 1;
    }
    if (!any) { return 0; }
    // the query as one piece is the best match; shorter texts beat longer ones
    std::string q;
    for (const char c : query) { if (c != ' ') { q.push_back(low(c)); } }
    std::string t;
    for (const char c : text) { t.push_back(low(c)); }
    if (t.find(q) != std::string::npos) { score += 40; }
    return score - static_cast<int>(text.size() / 8);
}

std::string action_title(std::string_view name)
{
    std::string out{name};
    for (char& c : out) { if (c == '_') { c = ' '; } }
    if (!out.empty()) { out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0]))); }
    return out;
}

void command_palette::open() noexcept
{
    open_        = true;
    just_opened_ = true;
    selected_    = 0;
    query_.clear();
}

std::string command_palette::show(context& ui, const keybinds& binds)
{
    std::string chosen;
    if (!open_ && shortcut.bound() && ui.chord_pressed(shortcut)) {
        open();
    }
    if (!open_) {
        return chosen;
    }

    constexpr std::string_view title = "Command palette";
    if (just_opened_) { ui.open_modal(title); }
    auto m = ui.modal(title, {560.0f, 0.0f}, modal_flags::esc_closes | modal_flags::backdrop_closes | modal_flags::no_title_bar);
    if (!m) {
        if (!just_opened_) { open_ = false; } // closed by Esc or a click outside
        just_opened_ = false;
        return chosen;
    }

    // the keys go first: the search field uses up the ones it knows, and Esc would only leave the field
    const bool esc  = ui.key_pressed(key::escape);
    const int  move = (ui.key_pressed(key::down) ? 1 : 0) - (ui.key_pressed(key::up) ? 1 : 0);
    if (just_opened_) {
        ui.request_text_focus("##palette_query");
        just_opened_ = false;
    }
    if (ui.input_text("##palette_query", query_, "Type a command...", input_flags::none, 96)) { selected_ = 0; }

    struct hit {
        const keybinds::action* action;
        int                     score;
    };
    std::vector<hit> hits;
    for (const keybinds::action& a : binds.actions()) {
        if (!binds.available(a)) { continue; }
        int score = 0;
        if (!query_.empty()) {
            score = std::max({fuzzy_score(query_, action_title(a.name)), fuzzy_score(query_, a.name),
                              fuzzy_score(query_, a.description) - 20});
            if (score < 0) { continue; }
        }
        hits.push_back({&a, score});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const hit& x, const hit& y) { return x.score > y.score; });

    const int n     = static_cast<int>(hits.size());
    const int shown = std::min(n, std::max(max_rows, 1));
    selected_ = std::clamp(selected_ + move, 0, std::max(n - 1, 0));
    const int first = std::clamp(selected_ - shown + 1, 0, n - shown);

    ui.spacing(2.0f);
    const style& theme = ui.theme();
    const f32    row_h = ui.frame_height();
    for (int i = first; i < first + shown; ++i) {
        const keybinds::action& a = *hits[static_cast<std::size_t>(i)].action;
        const item_result row = ui.custom_item("row " + a.name, {0.0f, row_h});
        draw_list& dl = ui.draw();
        if (i == selected_ || row.hovered) {
            shape_style bg;
            bg.radius      = radii(theme.rounding * 0.55f);
            bg.fill_top    = i == selected_ ? theme.accent.scaled_alpha(0.28f) : theme.widget_hover.scaled_alpha(0.75f);
            bg.fill_bottom = bg.fill_top;
            dl.shape(row.bounds, bg);
        }
        const font_id f     = ui.current_font();
        const f32     text_y = row.bounds.min.y + (row_h - ui.font().line_height(f)) * 0.5f;
        const std::string name = action_title(a.name);
        dl.text({row.bounds.min.x + 10.0f, text_y}, i == selected_ ? theme.accent_hover : theme.text, name, f);
        const std::string chord = chord_to_string(a.chord);
        const f32 chord_w = chord.empty() ? 0.0f : ui.font().measure(f, chord).x;
        if (!chord.empty()) { dl.text({row.bounds.max.x - 10.0f - chord_w, text_y}, theme.text_dim, chord, f); }
        if (!a.description.empty()) { // as much of the description as fits between the name and the shortcut
            const f32 x = row.bounds.min.x + 10.0f + ui.font().measure(f, name).x + 12.0f;
            const f32 room = row.bounds.max.x - 10.0f - chord_w - 12.0f - x;
            if (room > 40.0f) {
                dl.push_clip({{x, row.bounds.min.y}, {x + room, row.bounds.max.y}});
                dl.text({x, text_y}, theme.text_dim.scaled_alpha(0.75f), a.description, f);
                dl.pop_clip();
            }
        }
        if (row.pressed) { chosen = a.name; }
    }
    if (n == 0) { ui.text_dim("no matching commands"); }
    if (n > shown) { ui.text_dim(std::to_string(n) + " commands"); }

    if (ui.input_submitted() && n > 0) { chosen = hits[static_cast<std::size_t>(selected_)].action->name; }
    if (esc || !chosen.empty()) {
        open_ = false;
        ui.close_modal();
    }
    return chosen;
}

} // namespace strata
