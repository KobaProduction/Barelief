#pragma once

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <cstdint>
#include <map>
#include <vector>

namespace presentation::editor_canvas {
struct Node {
    std::uint32_t id;
    QRectF bounds;
};
struct Edge {
    std::uint32_t from, to;
};
using Positions = std::map<std::uint32_t, QPointF>;

double fit_scale(QRectF nodes, QSizeF viewport);
double minimum_scale(QRectF nodes, QSizeF viewport);
QRectF navigation_bounds(QRectF nodes, QSizeF viewport, double scale);
QPointF constrain_center(QRectF canvas, QSizeF viewport, double scale, QPointF center);
Positions arrange(const std::vector<Node>& nodes, const std::vector<Edge>& edges, QSizeF viewport);
}
