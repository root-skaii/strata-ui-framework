#include "demo4.hpp"

#include <strata/icons.hpp>

#include <array>
#include <format>

#include <windows.h>

using namespace strata;

namespace {

// generated once: 8 namespaces x 8 children x 8 grandchildren x 10 leaves = 5 192 rows over 4 levels
constexpr int level_fanout[4] = {8, 8, 8, 10};

void build_tree(demo4_state& s)
{
    s.nodes.clear();
    s.nodes.push_back({"<root>", 0, 0, -1});

    // breadth first, so a node's children are contiguous
    std::vector<int> level{0};
    for (int depth = 0; depth < 4; ++depth) {
        std::vector<int> next;
        for (const int parent : level) {
            const int first = static_cast<int>(s.nodes.size());
            for (int i = 0; i < level_fanout[depth]; ++i) {
                const char* const kind = depth == 3 ? "Type" : "ns";
                s.nodes.push_back({std::format("{}_{}_{}", kind, depth, static_cast<int>(s.nodes.size())), 0, 0, depth});
                next.push_back(static_cast<int>(s.nodes.size()) - 1);
            }
            s.nodes[static_cast<std::size_t>(parent)].first_child = first;
            s.nodes[static_cast<std::size_t>(parent)].children    = level_fanout[depth];
        }
        level = std::move(next);
    }
}

void build_components(demo4_state& s)
{
    s.components = {
        {"Transform", "UnityEngine.Transform", true},
        {"MeshFilter", "UnityEngine.MeshFilter", true},
        {"MeshRenderer", "UnityEngine.MeshRenderer", true},
        {"Rigidbody", "UnityEngine.Rigidbody", false},
        {"BoxCollider", "UnityEngine.BoxCollider", true},
        {"PlayerController", "Assets.Scripts.Gameplay.PlayerController", true},
        {"AudioSource", "UnityEngine.AudioSource", false},
        {"NavMeshAgent", "UnityEngine.AI.NavMeshAgent", true},
    };
}

} // namespace

demo4_state::demo4_state()
{
    build_tree(*this);
    build_components(*this);
    for (int i = 0; i < 40; ++i) {
        objects.push_back(std::format("GameObject_{:02}", i));
        visible.push_back(1);
    }
    sel.select_one(0);
    // a long list, which is what combo_filtered is for
    static const char* const kinds[] = {"Transform", "Mesh", "Collider", "Rigidbody", "Light", "Camera",
                                        "AudioSource", "Animator", "Canvas", "Text", "Image", "Button"};
    for (int i = 0; i < 300; ++i) {
        types.push_back(std::format("UnityEngine.{}{:03}", kinds[i % 12], i));
    }
    type_views.reserve(types.size());
    for (const std::string& t : types) { type_views.emplace_back(t); }
}

// icons ---------------------------------------------------------------------------------------

void demo4_icons(context& ui, demo4_state& s)
{
    const bool    have_icons = s.font_icons >= 0;
    const font_id icon_font  = static_cast<font_id>(have_icons ? s.font_icons : 0);

    if (auto w = ui.window("icons", {24.0f, 24.0f}, {1180.0f, 940.0f}, window_flags::resizable)) {
        if (!have_icons) {
            ui.text_dim("the icon font is not loaded (--icons NAME)");
            return;
        }
        ui.text_dim("every icons:: constant. a name in red means the loaded icon font has no glyph for it,");
        ui.text_dim("which is how a wrong code point shows up instead of silently drawing a blank box.");
        ui.separator();

        if (s.icon_page >= 0) { // a raw code-point page, for finding the right value for a new name
            ui.textf("code points U+{:04X} .. U+{:04X}", s.icon_page, s.icon_page + 255);
            for (int row = 0; row < 16; ++row) {
                for (int col = 0; col < 16; ++col) {
                    const char32_t cp = static_cast<char32_t>(s.icon_page + row * 16 + col);
                    if (col > 0) { ui.same_line(static_cast<float>(col) * 72.0f); }
                    const glyph_string g{cp};
                    ui.icon_label(icon_font, g, std::format("{:04X}", static_cast<unsigned>(cp)));
                }
            }
            return;
        }

        int shown = 0;
        for (const auto& entry : icons::all) {
            const int col = shown % 4;
            if (col > 0) { ui.same_line(static_cast<float>(col) * 285.0f); }
            const glyph_string g{entry.code};
            const bool         present = ui.font().has_glyph(icon_font, entry.code);
            if (!present) { ui.push_color(style_color::text, color{255, 110, 110, 255}); }
            ui.icon_label(icon_font, g, std::format("{}  U+{:04X}", entry.name, static_cast<unsigned>(entry.code)));
            if (!present) { ui.pop_color(); }
            ++shown;
        }
    }
}

// a deep tree -----------------------------------------------------------------------------------

namespace {

// one subtree; rows are identified by index, so nothing is concatenated per row
void draw_subtree(context& ui, demo4_state& s, int index)
{
    const demo4_state::tree_node_data& n = s.nodes[static_cast<std::size_t>(index)];
    const std::string_view id{reinterpret_cast<const char*>(&index), sizeof(index)};
    if (n.children == 0) {
        if (ui.tree_leaf(n.name, id, s.selected == index)) { s.selected = index; }
        return;
    }
    if (ui.tree_node(n.name, id, tree_flags::default_open)) {
        for (int i = 0; i < n.children; ++i) {
            draw_subtree(ui, s, n.first_child + i);
        }
        ui.tree_pop();
    }
}

} // namespace

void demo4_bigtree(context& ui, demo4_state& s)
{
    if (auto w = ui.window("namespace tree", {24.0f, 24.0f}, {520.0f, 860.0f}, window_flags::resizable)) {
        ui.textf("{} nodes over 4 levels, all expanded", s.nodes.size() - 1);
        if (ui.button("expand all")) { ui.open_all_tree_nodes(); }
        ui.same_line();
        if (ui.button("collapse all")) { ui.close_all_tree_nodes(); }
        ui.same_line();
        if (ui.button("reveal selection") && s.selected >= 0) { s.culling_note = true; }
        ui.separator();

        if (auto c = ui.child("tree", {0.0f, 0.0f}, child_flags::frame)) {
            auto nav = ui.navigation("tree");
            const demo4_state::tree_node_data& root = s.nodes[0];
            for (int i = 0; i < root.children; ++i) {
                draw_subtree(ui, s, root.first_child + i);
            }
        }
    }

    if (auto w = ui.window("frame stats", {560.0f, 24.0f}, {420.0f, 320.0f}, window_flags::resizable)) {
        const frame_stats& st = ui.stats();
        const auto mono = ui.with_font(static_cast<font_id>(s.font_mono >= 0 ? s.font_mono : 0));
        ui.textf("items submitted   {}", st.items_submitted);
        ui.textf("items culled      {}", st.items_culled);
        ui.textf("items drawn       {}", st.items_submitted - st.items_culled);
        ui.separator();
        ui.textf("vertices          {}", st.vertices);
        ui.textf("indices           {}", st.indices);
        ui.textf("draw calls        {}", st.draw_calls);
        ui.separator();
        ui.textf("text measures     {}", st.text_measures);
        ui.textf("measure cache hit {}", st.measure_hits);
        ui.textf("anim slots        {} / {}", st.anim_slots_used, st.anim_slots_total);
        if (!s.deterministic) {
            ui.separator();
            ui.textf("begin_frame  {:.3f} ms", st.begin_frame_ms);
            ui.textf("end_frame    {:.3f} ms", st.end_frame_ms);
        }
    }
}

// the app-shaped scene ---------------------------------------------------------------------------

void demo4_app(context& ui, demo4_state& s)
{
    const bool    have_icons = s.font_icons >= 0;
    const font_id icon_font  = static_cast<font_id>(have_icons ? s.font_icons : 0);

    if (auto w = ui.window("scene", {24.0f, 24.0f}, {420.0f, 600.0f}, window_flags::resizable)) {
        // window-scoped shortcuts: Delete destroys, F2 renames, Ctrl+D duplicates; only while this panel is focused and
        // no text field has the keyboard.
        if (ui.window_focused()) {
            if (ui.key_pressed(VK_DELETE) && !s.sel.empty()) {
                ui.ask_confirm("destroy", std::format("Destroy {} object(s)?", s.sel.size()),
                               static_cast<strata::u64>(s.sel.size()));
            }
            if (ui.key_pressed(VK_F2))          { s.status = "rename"; }
            if (ui.key_pressed('D', true))      { s.status = "duplicate"; }
        }

        {   // bulk actions are disabled while nothing is selected, and still say why
            auto d = ui.disabled_if(s.sel.empty());
            if (ui.button("Destroy selected")) {
                ui.ask_confirm("destroy", std::format("Destroy {} object(s)?", s.sel.size()),
                               static_cast<strata::u64>(s.sel.size()));
            }
            if (s.sel.empty()) { ui.tooltip("select something first"); }
            ui.same_line();
            if (ui.button("Copy names")) {
                std::string text;
                for (const int i : s.sel.items()) { text += s.objects[static_cast<std::size_t>(i)] + "\n"; }
                ui.copy_text(text);
                s.status = "copied";
            }
        }
        ui.separator();

        if (auto c = ui.child("list", {0.0f, 0.0f}, child_flags::frame)) {
            auto nav = ui.navigation("objects");
            for (std::size_t i = 0; i < s.objects.size(); ++i) {
                const int idx = static_cast<int>(i);
                // the row leaves room for its accessories and elides its own label there
                ui.set_next_item_gutter(56.0f);
                if (ui.selectable(s.objects[i], {reinterpret_cast<const char*>(&idx), sizeof(idx)}, s.sel.contains(idx))) {
                    ui.selection_click(s.sel, idx);
                }
                if (ui.item_truncated()) { ui.tooltip(s.objects[i]); }
                // ... which strata places, clips and hit-tests inside the row
                if (have_icons && ui.row_accessory_button(icon_font, icons::trash)) {
                    ui.ask_confirm("destroy", std::format("Destroy {}?", s.objects[i]), static_cast<strata::u64>(idx));
                }
                bool shown = s.visible[i] != 0;
                if (ui.row_accessory_checkbox("vis", shown)) { s.visible[i] = shown ? 1 : 0; }
            }
        }
    }

    if (auto w = ui.window("inspector##app", {470.0f, 24.0f}, {400.0f, 600.0f}, window_flags::resizable)) {
        ui.text_dim(std::format("{} selected   |   {}", s.sel.size(), s.status));
        ui.tooltip("plain text can own a tooltip now");
        ui.separator();

        ui.text("component type (300 entries, type to filter)");
        ui.set_next_item_width(ui.content_width());
        (void)ui.combo_filtered("##type", s.type_index, s.type_views.data(), s.type_views.size());
        ui.separator();

        ui.textf("ui scale {} %", ui.scale_percent());
        if (ui.button("-")) { s.pending_scale_percent = std::max(50, ui.scale_percent() - 10); }
        ui.same_line();
        if (ui.button("+")) { s.pending_scale_percent = std::min(400, ui.scale_percent() + 10); }
        ui.same_line();
        if (ui.button("100 %")) { s.pending_scale_percent = 100; }
        ui.text_dim("the whole ui grows and shrinks; the window the game owns does not change");
        ui.separator();

        {   // the viewport marker an object picker draws over what is under the cursor
            const item_result box = ui.custom_item("picked", {0.0f, 90.0f});
            ui.draw().rect_filled(box.bounds, ui.theme().widget_bg.scaled_alpha(0.5f));
            ui.draw().corner_brackets(box.bounds.expanded(-6.0f), strata::color{92, 230, 230, 255}, 2.0f);
            ui.draw().text({box.bounds.min.x + 14.0f, box.bounds.min.y + 10.0f}, ui.theme().text_dim,
                           "draw().corner_brackets()", 0);
        }
        ui.separator();

        ui.textf("content rect {:.0f} x {:.0f}, scrollbar {:.0f} px",
                 ui.content_rect().width(), ui.content_rect().height(), strata::context::scrollbar_width());
    }

    // the confirmation owns its state; the "don't ask again" tick is stored for the caller
    switch (ui.confirm("destroy", {"Destroy", "Cancel"}, {.title = "Destroy?", .remember = &s.never_ask, .danger = 1})) {
    case 1:  s.status = std::format("destroyed {}", ui.confirm_data()); break;
    case 2:  s.status = "cancelled"; break;
    case -1: s.status = "dismissed"; break;
    default: break;
    }
}

// row ergonomics --------------------------------------------------------------------------------

void demo4_rows(context& ui, demo4_state& s)
{
    const bool    have_icons = s.font_icons >= 0;
    const font_id icon_font  = static_cast<font_id>(have_icons ? s.font_icons : 0);

    if (auto w = ui.window("inspector", {24.0f, 24.0f}, {460.0f, 560.0f}, window_flags::resizable)) {
        ui.set_next_item_width(ui.content_width());
        (void)ui.input_text("##filter", s.filter, "filter components",
                            input_flags::clear_button | input_flags::select_all_on_focus);
        ui.separator();

        for (std::size_t i = 0; i < s.components.size(); ++i) {
            demo4_state::component& c = s.components[i];
            ui.push_id(&c); // the row is identified by the object it shows

            // the header keeps a right gutter for the switch and remove button, so its label is ellipsized before them
            const float gutter = 76.0f;
            item_result row;
            bool        row_right = false;
            {
                auto g = ui.right_gutter(gutter);
                row = ui.custom_item("row", {0.0f, 30.0f});
                row_right = ui.item_clicked(mouse_button::right); // asked while the row is still the last item
                ui.draw().rect_filled(row.bounds, ui.theme().widget_bg.scaled_alpha(row.hovered ? 0.9f : 0.55f));
                const float text_y = row.bounds.min.y + (row.bounds.height() - ui.font().line_height(0)) * 0.5f;
                ui.label_clipped({row.bounds.min.x + 8.0f, text_y}, row.bounds.width() - 16.0f, ui.theme().text, c.name);
            }
            // anything after this takes the press from the row it overlaps
            ui.allow_item_overlap();

            ui.same_line_right(gutter);
            if (ui.toggle("##on", c.enabled)) { s.removed = -1; }
            ui.same_line_right(24.0f);
            if (have_icons ? ui.icon_button(icon_font, icons::trash) : ui.button("x")) {
                s.removed = static_cast<int>(i);
            }

            if (row.pressed && !ui.item_claimed()) { s.row_selected = static_cast<int>(i); }
            if (row_right) { ui.open_popup("row menu"); }

            // the full type name, cut to the width instead of running past it
            ui.push_right_gutter(gutter);
            ui.text_ellipsis(c.type);
            ui.pop_right_gutter();

            if (auto p = ui.popup("row menu")) {
                if (ui.selectable("copy type")) { ui.close_popup(); }
                if (ui.selectable("remove")) { s.removed = static_cast<int>(i); ui.close_popup(); }
            }
            ui.pop_id();
        }

        ui.separator();
        ui.textf("selected {}   removed {}", s.row_selected, s.removed);
    }

    if (auto w = ui.window("keyboard list", {510.0f, 24.0f}, {330.0f, 560.0f}, window_flags::resizable)) {
        ui.text_dim("up / down / home / end move, enter picks");
        ui.separator();
        if (auto c = ui.child("rows", {0.0f, 0.0f}, child_flags::frame)) {
            auto nav = ui.navigation("rows");
            for (std::size_t i = 0; i < s.components.size(); ++i) {
                const int idx = static_cast<int>(i);
                if (ui.selectable(s.components[i].name, {reinterpret_cast<const char*>(&idx), sizeof(idx)},
                                  s.row_selected == idx)) {
                    s.row_selected = idx;
                }
                if (s.row_selected == idx) { ui.ensure_item_visible(); }
            }
        }
    }
}
