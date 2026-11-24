#include "showcase/modules/registry.hpp"

namespace strata::showcase {

std::unique_ptr<demo_module> make_explorer_module();
std::unique_ptr<demo_module> make_inspector_module();
std::unique_ptr<demo_module> make_charts_module();
std::unique_ptr<demo_module> make_editor_module();
std::unique_ptr<demo_module> make_nodes_module();
std::unique_ptr<demo_module> make_sequencer_module();
std::unique_ptr<demo_module> make_data_module();
std::unique_ptr<demo_module> make_visuals_module();
std::unique_ptr<demo_module> make_pickers_module();
std::unique_ptr<demo_module> make_typography_module();
std::unique_ptr<demo_module> make_overlays_module();
std::unique_ptr<demo_module> make_console_module();

std::vector<std::unique_ptr<demo_module>> create_builtin_modules()
{
    std::vector<std::unique_ptr<demo_module>> out;
    out.reserve(12);
    out.push_back(make_explorer_module());
    out.push_back(make_inspector_module());
    out.push_back(make_charts_module());
    out.push_back(make_editor_module());
    out.push_back(make_nodes_module());
    out.push_back(make_sequencer_module());
    out.push_back(make_data_module());
    out.push_back(make_visuals_module());
    out.push_back(make_pickers_module());
    out.push_back(make_typography_module());
    out.push_back(make_overlays_module());
    out.push_back(make_console_module());
    return out;
}

} // namespace strata::showcase
