#include "showcase/module.hpp"

#include <string>
#include <vector>

namespace strata::showcase {
namespace {

struct graph_node {
    u64         id{};
    std::string title;
    vec2        pos;
    u64         in_a{};
    const char* in_a_label{};
    u64         in_b{};
    const char* in_b_label{};
    u64         out_a{};
    const char* out_a_label{};
};

struct nodes_module final : demo_module {
    node_editor            graph;
    std::vector<graph_node> nodes;
    std::vector<node_link>  links;
    u64                     next_id{10};

    nodes_module()
    {
        reset_graph();
    }

    void reset_graph()
    {
        nodes = {
            {1, "Albedo Map",    {16.0f,  26.0f},  0,   nullptr,  0,   nullptr,  101, "RGBA"},
            {2, "Normal Map",    {16.0f,  156.0f}, 0,   nullptr,  0,   nullptr,  102, "Normal"},
            {3, "PBR Surface",   {182.0f, 64.0f},  201, "Base",   202, "Normal", 203, "Lit"},
            {4, "ACES Output",   {360.0f, 88.0f},  301, "HDR",    0,   nullptr,  0,   nullptr},
        };
        links = {
            {101, 201},
            {102, 202},
            {203, 301},
        };
    }

    module_info info() const override
    {
        return {
            .id           = "nodes",
            .title        = "Node Graph",
            .category     = module_category::authoring,
            .summary      = "Interactive shader / DSP node graph with Bezier wiring, drag-to-connect pins, pan, and zoom",
            .default_size = {640.0f, 380.0f},
        };
    }

    void draw(context& ui, demo_env& env) override
    {
        ui.text_dim("Drag nodes, wire pins, wheel to zoom, middle-drag to pan.");
        ui.same_line_right(142.0f);
        if (ui.button("+ Node")) {
            const u64 id = next_id++;
            nodes.push_back({id, std::format("Filter #{}", id), {180.0f + static_cast<f32>(id % 5) * 32.0f, 220.0f},
                             id * 10 + 1, "In", 0, nullptr, id * 10 + 2, "Out"});
            if (env.log != nullptr) { env.log->addf(log_level::info, "Added Filter #{} to Node Graph", id); }
        }
        ui.same_line();
        if (ui.button("Reset")) {
            reset_graph();
        }

        if (graph.begin(ui, "studio_node_graph", {0.0f, 410.0f})) {
            for (auto& n : nodes) {
                graph.begin_node(n.id, n.title, n.pos);
                if (n.in_a != 0)  { graph.input_pin(n.in_a, n.in_a_label); }
                if (n.in_b != 0)  { graph.input_pin(n.in_b, n.in_b_label); }
                if (n.out_a != 0) { graph.output_pin(n.out_a, n.out_a_label); }
                graph.end_node();
            }
            for (const auto& l : links) {
                graph.link(l.from, l.to);
            }
            graph.end();
        }

        if (node_link created; graph.link_created(created)) {
            links.push_back(created);
            if (env.log != nullptr) { env.log->addf(log_level::info, "Connected pin {} -> {}", created.from, created.to); }
        }
        if (node_link removed; graph.link_deleted(removed)) {
            std::erase_if(links, [&](const node_link& l) { return l.from == removed.from && l.to == removed.to; });
        }
    }
};

} // namespace

std::unique_ptr<demo_module> make_nodes_module()
{
    return std::make_unique<nodes_module>();
}

} // namespace strata::showcase
