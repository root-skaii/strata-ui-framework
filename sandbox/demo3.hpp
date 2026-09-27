#pragma once

// more demos: tabs and popups, drag and drop, date / time pickers, long lists and tables, code editor, passwords
// and masks. state in demo3_state (member of demo2_state); main draws them through demo3_show.

#include <strata/strata.hpp>

#include <array>
#include <string>
#include <vector>

struct demo3_state {
    std::string scene;
    bool        deterministic = false; // a screenshot: the clock is fixed
    int         frame = 0;
    int         mono_font = -1; // font id of the mono font, -1 when not loaded

    // tabs, popups and status widgets
    bool                     show_tabs = false;
    std::vector<std::string> docs{"main.cpp", "context.hpp", "README.md", "notes.txt"};
    int                      doc         = 0;
    int                      doc_counter = 1;
    std::vector<std::string> many;
    int                      many_selected = 0;
    bool                     wrap = true;
    float                    zoom = 1.0f;
    std::vector<std::string> tags{"ui", "d3d12", "tools", "beta"};
    std::array<bool, 16>     tag_on{true, false, true};
    int                      tag_counter = 1;

    // drag and drop, date and time pickers
    bool                     show_dnd = false;
    std::vector<std::string> todo{"write the docs", "fix the tab drag", "add golden images", "ship it"};
    std::vector<std::string> done;
    int                      selected_task = -1;
    std::string              last_drop = "-";
    std::array<strata::color, 3> palette{{{92, 141, 255, 255}, {240, 86, 143, 255}, {74, 222, 128, 255}}};
    strata::color            canvas{60, 66, 90, 255};
    strata::date             day{2026, 9, 25};
    strata::time_of_day      clock{13, 45, 0};
    strata::date             day2{2026, 10, 31};
    strata::time_of_day      clock2{9, 30, 15};

    // long lists and tables
    bool        show_lists = false;
    int         list_selected = -1;
    int         list_rows_drawn = 0;
    int         big_rows_drawn  = 0;
    std::string table_layout;
    int         file_selected = -1;

    // code, passwords and masks
    bool        show_editor = false;
    std::string code;
    std::string password = "hunter2";
    std::string pin      = "1234";
    std::string phone;
    std::string plate = "ab123";
    std::string stamp = "20260925";
    std::string hex_color = "5b8dff";
    std::vector<strata::text_span> spans;

    demo3_state();
};

void demo3_show(strata::context& ui, demo3_state& s);
void demo3_update(strata::context& ui, demo3_state& s);
// what a scene clicks before its screenshot (frame counts from 0)
void demo3_script(const demo3_state& s, int frame, strata::vec2& pos, bool* down);
