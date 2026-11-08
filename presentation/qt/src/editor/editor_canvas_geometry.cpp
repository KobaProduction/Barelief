#include <core_app/scheme_editor/editor_canvas_geometry.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace presentation::editor_canvas {
namespace {
constexpr double gap = 40;
QSizeF usable_viewport(QSizeF size) {
    return {std::max(1.0, size.width()), std::max(1.0, size.height())};
}
QRectF content_or_origin(QRectF nodes) {
    return nodes.isEmpty() ? QRectF(-100, -100, 200, 200) : nodes;
}
struct LayerNode {
    QRectF bounds;
    int incoming = 0, layer = 0;
    double order = 0;
    std::vector<std::uint32_t> next, previous;
};
using Nodes = std::map<std::uint32_t, LayerNode>;
using Columns = std::vector<std::vector<std::uint32_t>>;
void connect_nodes(Nodes& nodes, const std::vector<Edge>& edges) {
    for (const auto& edge : edges) {
        if (!nodes.contains(edge.from) || !nodes.contains(edge.to)) continue;
        nodes.at(edge.from).next.push_back(edge.to);
        nodes.at(edge.to).previous.push_back(edge.from);
        ++nodes.at(edge.to).incoming;
    }
}
void advance_layer(Nodes& nodes, std::uint32_t id, std::queue<std::uint32_t>& ready) {
    for (auto next : nodes.at(id).next) {
        auto& node = nodes.at(next);
        node.layer = std::max(node.layer, nodes.at(id).layer + 1);
        if (--node.incoming == 0) ready.push(next);
    }
}
void assign_layers(Nodes& nodes) {
    std::queue<std::uint32_t> ready;
    for (const auto& [id, node] : nodes) if (!node.incoming) ready.push(id);
    while (!ready.empty()) {
        auto id = ready.front();
        ready.pop();
        advance_layer(nodes, id, ready);
    }
    // Cyclic drafts remain editable and receive finite, nonoverlapping positions.
    for (auto& [id, node] : nodes) if (node.incoming) node.layer = 0;
}
double predecessor_order(const Nodes& nodes, std::uint32_t id) {
    const auto& previous = nodes.at(id).previous;
    if (previous.empty()) return nodes.at(id).order;
    double total = 0;
    for (auto parent : previous) total += nodes.at(parent).order;
    return total / previous.size();
}
void order_column(Nodes& nodes, std::vector<std::uint32_t>& column) {
    std::stable_sort(column.begin(), column.end(), [&nodes](auto a, auto b) {
        return predecessor_order(nodes, a) < predecessor_order(nodes, b);
    });
    for (std::size_t i = 0; i < column.size(); ++i) nodes.at(column[i]).order = double(i);
}
Columns layered_columns(Nodes& nodes) {
    std::map<int, std::vector<std::uint32_t>> layers;
    for (const auto& [id, node] : nodes) layers[node.layer].push_back(id);
    Columns columns;
    for (auto& [layer, ids] : layers) {
        order_column(nodes, ids);
        columns.push_back(std::move(ids));
    }
    return columns;
}
Columns split_columns(const Nodes& nodes, const Columns& columns, double height) {
    Columns result;
    for (const auto& column : columns) {
        double used = 0;
        result.emplace_back();
        for (auto id : column) {
            const double next = nodes.at(id).bounds.height() + gap;
            if (used > 0 && used + next > height) { result.emplace_back(); used = 0; }
            result.back().push_back(id);
            used += next;
        }
    }
    return result;
}
QSizeF column_size(const Nodes& nodes, const std::vector<std::uint32_t>& column) {
    double width = 0, height = 0;
    for (auto id : column) {
        const auto bounds = nodes.at(id).bounds;
        width = std::max(width, bounds.width());
        height += bounds.height() + gap;
    }
    return {width, std::max(0.0, height - gap)};
}
void place_column(const Nodes& nodes, const std::vector<std::uint32_t>& column,
                  QPointF origin, Positions& positions) {
    for (auto id : column) {
        const auto bounds = nodes.at(id).bounds;
        positions[id] = origin - bounds.topLeft();
        origin.ry() += bounds.height() + gap;
    }
}
struct Packing {
    Positions positions;
    QSizeF extent;
    bool wrapped = false;
};
Packing pack_columns(const Nodes& nodes, const Columns& columns, double width) {
    Packing result;
    double x = 0, y = 0, row_height = 0, used_width = 0;
    for (const auto& column : columns) {
        const auto size = column_size(nodes, column);
        if (x > 0 && x + size.width() > width) {
            x = 0; y += row_height + gap * 2; row_height = 0; result.wrapped = true;
        }
        place_column(nodes, column, {x, y}, result.positions);
        used_width = std::max(used_width, x + size.width());
        row_height = std::max(row_height, size.height());
        x += size.width() + gap;
    }
    result.extent = {used_width, y + row_height};
    return result;
}
double packing_cost(const Packing& packing, QSizeF viewport) {
    const double extent = std::max(packing.extent.width() / viewport.width(),
                                   packing.extent.height() / viewport.height());
    // Keep short routes flowing right when wrapping offers only a marginal gain.
    return extent * (packing.wrapped ? 1.2 : 1.0);
}
double padded_area(const Nodes& nodes) {
    double area = 0;
    for (const auto& [id, node] : nodes)
        area += (node.bounds.width() + gap) * (node.bounds.height() + gap);
    return area;
}
Packing compact_packing(const Nodes& nodes, const Columns& columns, QSizeF viewport) {
    auto best = pack_columns(nodes, columns, std::numeric_limits<double>::max());
    if (columns.size() <= 3 && nodes.size() <= 6) return best;
    const double aspect = viewport.width() / viewport.height();
    const double width = std::sqrt(padded_area(nodes) * aspect);
    for (double factor : {0.65, 0.85, 1.0, 1.25, 1.6, 2.0}) {
        auto split = split_columns(nodes, columns, width * factor / aspect);
        auto candidate = pack_columns(nodes, split, width * factor);
        if (packing_cost(candidate, viewport) < packing_cost(best, viewport)) best = std::move(candidate);
    }
    return best;
}
}

double fit_scale(QRectF nodes, QSizeF viewport) {
    const auto bounds = content_or_origin(nodes).adjusted(-gap, -gap, gap, gap);
    const auto size = usable_viewport(viewport);
    return std::min(3.0, std::min(size.width() / bounds.width(), size.height() / bounds.height()));
}
double minimum_scale(QRectF nodes, QSizeF viewport) {
    return std::min(0.2, fit_scale(nodes, viewport) * 0.75);
}
QRectF navigation_bounds(QRectF nodes, QSizeF viewport, double scale) {
    const auto content = content_or_origin(nodes);
    const auto size = usable_viewport(viewport) / scale;
    const double horizontal = size.width() / 2 + 160 / scale;
    const double vertical = size.height() / 2 + 160 / scale;
    return content.adjusted(-horizontal, -vertical, horizontal, vertical);
}
QPointF constrain_center(QRectF canvas, QSizeF viewport, double scale, QPointF center) {
    const auto half = usable_viewport(viewport) / (2 * scale);
    return {std::clamp(center.x(), canvas.left() + half.width(), canvas.right() - half.width()),
            std::clamp(center.y(), canvas.top() + half.height(), canvas.bottom() - half.height())};
}
Positions arrange(const std::vector<Node>& input, const std::vector<Edge>& edges, QSizeF viewport) {
    Nodes nodes;
    for (const auto& node : input) nodes[node.id].bounds = node.bounds;
    if (nodes.empty()) return {};
    connect_nodes(nodes, edges);
    assign_layers(nodes);
    const auto columns = layered_columns(nodes);
    return compact_packing(nodes, columns, usable_viewport(viewport)).positions;
}
}
