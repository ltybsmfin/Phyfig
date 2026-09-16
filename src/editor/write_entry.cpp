#include "write_entry.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace physics_editor::write_entry {
namespace {

// Coverage level that separates ink from background. The same level is used for
// the connected regions and for the marching-squares outline, so the stroke and
// the fill describe the same shape.
constexpr int kInkLevel = 128;
// Collinear points closer than this (in mask pixels) to the line through their
// neighbours are dropped. At the 64 px text raster this is far below one screen
// pixel, so it costs no visible accuracy and keeps the per-frame vertex count low.
constexpr float kSimplifyTolerance = 0.22f;
// Loops shorter than this are rasterization noise.
constexpr float kMinimumLoopLength = 1.2f;

void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }

double boundedNumber(const Json& spec, const char* key, double low, double high) {
    require(spec.contains(key) && spec.at(key).is_number(), std::string("write.") + key + " must be a number");
    const double value = spec.at(key).get<double>();
    require(std::isfinite(value) && value >= low && value <= high,
        std::string("write.") + key + " must be a finite number in [" + std::to_string(low) + "," + std::to_string(high) + "]");
    return value;
}

struct Labeled {
    int width = 0, height = 0, count = 0;
    std::vector<int> labels;  // -1 = background, otherwise the component id
};

// 4-connected ink regions. Two diagonal ink pixels are deliberately *not*
// joined: a broken contour must stay broken instead of being bridged.
Labeled labelInk(const Mask& mask) {
    require(mask.alpha != nullptr && mask.width > 0 && mask.height > 0, "Write geometry requires a non-empty mask");
    Labeled result;
    result.width = mask.width;
    result.height = mask.height;
    const size_t total = static_cast<size_t>(mask.width) * static_cast<size_t>(mask.height);
    result.labels.assign(total, -1);
    std::vector<int> stack;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            const size_t seed = static_cast<size_t>(y) * mask.width + static_cast<size_t>(x);
            if (result.labels[seed] >= 0 || mask.alpha[seed] < kInkLevel) continue;
            const int id = result.count++;
            stack.clear();
            stack.push_back(static_cast<int>(seed));
            result.labels[seed] = id;
            while (!stack.empty()) {
                const int current = stack.back();
                stack.pop_back();
                const int cx = current % mask.width, cy = current / mask.width;
                const int neighbours[4][2] = {{cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1}};
                for (const auto& neighbour : neighbours) {
                    if (neighbour[0] < 0 || neighbour[1] < 0 || neighbour[0] >= mask.width || neighbour[1] >= mask.height) continue;
                    const size_t next = static_cast<size_t>(neighbour[1]) * mask.width + static_cast<size_t>(neighbour[0]);
                    if (result.labels[next] >= 0 || mask.alpha[next] < kInkLevel) continue;
                    result.labels[next] = id;
                    stack.push_back(static_cast<int>(next));
                }
            }
        }
    }
    return result;
}

struct Segment { int first = -1, second = -1; bool used = false; };

std::vector<Point> simplify(const std::vector<Point>& input) {
    if (input.size() < 3) return input;
    std::vector<Point> output;
    output.reserve(input.size());
    output.push_back(input.front());
    for (size_t i = 1; i + 1 < input.size(); ++i) {
        const Point& a = output.back();
        const Point& b = input[i];
        const Point& c = input[i + 1];
        const float dx = c.x - a.x, dy = c.y - a.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        const float deviation = length > 1e-6f ? std::abs((b.x - a.x) * dy - (b.y - a.y) * dx) / length : 0.f;
        if (deviation > kSimplifyTolerance) output.push_back(b);
    }
    output.push_back(input.back());
    return output;
}

// Marching squares on the coverage values at the ink level. Crossing points are
// linearly interpolated, so the outline follows the real glyph silhouette with
// sub-pixel accuracy instead of stepping along pixel corners. A one pixel zero
// border closes every contour and lets ink that touches the bitmap edge finish.
//
// Coordinates are texel corners: a mask pixel (x, y) is sampled at its centre and
// drawn by ImGui/math_text over [x, x+1) x [y, y+1), so the centre grid is
// shifted by half a pixel. Corners of a filled block are chamfered by half a
// pixel, which is what the original antialiased coverage looks like anyway.
std::vector<Loop> traceLoops(const Mask& mask) {
    const int w = mask.width, h = mask.height;
    const int pw = w + 2, ph = h + 2;
    std::vector<unsigned char> pad(static_cast<size_t>(pw) * static_cast<size_t>(ph), 0);
    for (int y = 0; y < h; ++y)
        std::memcpy(&pad[static_cast<size_t>(y + 1) * pw + 1], mask.alpha + static_cast<size_t>(y) * w, static_cast<size_t>(w));
    auto vertex = [&](int x, int y) { return static_cast<int>(pad[static_cast<size_t>(y) * pw + x]); };
    auto horizontal = [&](int x, int y) { return 2 * (y * pw + x); };    // grid edge (x,y)-(x+1,y)
    auto vertical = [&](int x, int y) { return 2 * (y * pw + x) + 1; };  // grid edge (x,y)-(x,y+1)
    const int edgeCount = 2 * pw * ph;
    std::vector<unsigned char> crossing(static_cast<size_t>(edgeCount), 0);
    std::vector<Point> position(static_cast<size_t>(edgeCount));
    auto interpolate = [](int a, int b) {
        if (a == b) return 0.5f;
        return std::clamp(static_cast<float>(kInkLevel - a) / static_cast<float>(b - a), 0.f, 1.f);
    };
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x + 1 < pw; ++x) {
            const int a = vertex(x, y), b = vertex(x + 1, y);
            if ((a >= kInkLevel) == (b >= kInkLevel)) continue;
            const int id = horizontal(x, y);
            crossing[static_cast<size_t>(id)] = 1;
            position[static_cast<size_t>(id)] = {static_cast<float>(x) + interpolate(a, b) - 0.5f, static_cast<float>(y) - 0.5f};
        }
    }
    for (int y = 0; y + 1 < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const int a = vertex(x, y), b = vertex(x, y + 1);
            if ((a >= kInkLevel) == (b >= kInkLevel)) continue;
            const int id = vertical(x, y);
            crossing[static_cast<size_t>(id)] = 1;
            position[static_cast<size_t>(id)] = {static_cast<float>(x) - 0.5f, static_cast<float>(y) + interpolate(a, b) - 0.5f};
        }
    }
    std::vector<Segment> segments;
    std::vector<int> firstEdge(static_cast<size_t>(edgeCount), -1), secondEdge(static_cast<size_t>(edgeCount), -1);
    auto addSegment = [&](int a, int b) {
        if (a == b || !crossing[static_cast<size_t>(a)] || !crossing[static_cast<size_t>(b)]) return;
        const int index = static_cast<int>(segments.size());
        segments.push_back({a, b, false});
        if (firstEdge[static_cast<size_t>(a)] < 0) firstEdge[static_cast<size_t>(a)] = index; else secondEdge[static_cast<size_t>(a)] = index;
        if (firstEdge[static_cast<size_t>(b)] < 0) firstEdge[static_cast<size_t>(b)] = index; else secondEdge[static_cast<size_t>(b)] = index;
    };
    for (int y = 0; y + 1 < ph; ++y) {
        for (int x = 0; x + 1 < pw; ++x) {
            const int v0 = vertex(x, y), v1 = vertex(x + 1, y), v2 = vertex(x + 1, y + 1), v3 = vertex(x, y + 1);
            const int code = (v0 >= kInkLevel ? 1 : 0) | (v1 >= kInkLevel ? 2 : 0) | (v2 >= kInkLevel ? 4 : 0) | (v3 >= kInkLevel ? 8 : 0);
            const int top = horizontal(x, y), right = vertical(x + 1, y), bottom = horizontal(x, y + 1), left = vertical(x, y);
            switch (code) {
            case 0: case 15: break;
            case 1: case 14: addSegment(top, left); break;
            case 2: case 13: addSegment(top, right); break;
            case 3: case 12: addSegment(left, right); break;
            case 4: case 11: addSegment(bottom, right); break;
            case 6: case 9: addSegment(top, bottom); break;
            case 7: case 8: addSegment(bottom, left); break;
            case 5:  // TL and BR ink: the saddle is resolved by the cell centre
                if ((v0 + v1 + v2 + v3) / 4 >= kInkLevel) { addSegment(top, right); addSegment(bottom, left); }
                else { addSegment(top, left); addSegment(bottom, right); }
                break;
            case 10: // TR and BL ink
                if ((v0 + v1 + v2 + v3) / 4 >= kInkLevel) { addSegment(top, left); addSegment(bottom, right); }
                else { addSegment(top, right); addSegment(bottom, left); }
                break;
            default: break;
            }
        }
    }
    std::vector<Loop> loops;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].used) continue;
        int current = static_cast<int>(i);
        segments[i].used = true;
        const int startEdge = segments[i].first;
        int edge = segments[i].second;
        std::vector<Point> points;
        points.push_back(position[static_cast<size_t>(startEdge)]);
        bool closed = false;
        for (size_t guard = 0; guard <= segments.size(); ++guard) {
            points.push_back(position[static_cast<size_t>(edge)]);
            if (edge == startEdge) { closed = true; break; }
            int next = firstEdge[static_cast<size_t>(edge)];
            if (next < 0 || next == current || segments[static_cast<size_t>(next)].used) next = secondEdge[static_cast<size_t>(edge)];
            if (next < 0 || next == current || segments[static_cast<size_t>(next)].used) break;
            segments[static_cast<size_t>(next)].used = true;
            current = next;
            edge = segments[static_cast<size_t>(next)].first == edge ? segments[static_cast<size_t>(next)].second : segments[static_cast<size_t>(next)].first;
        }
        if (!closed || points.size() < 4) continue;
        points.pop_back();  // the closing point repeats the first
        std::vector<Point> cleaned;
        cleaned.reserve(points.size());
        for (const Point& point : points) {
            if (!cleaned.empty() && std::abs(cleaned.back().x - point.x) < 1e-4f && std::abs(cleaned.back().y - point.y) < 1e-4f) continue;
            cleaned.push_back(point);
        }
        if (cleaned.size() < 3) continue;
        cleaned = simplify(cleaned);
        Loop loop;
        loop.points = std::move(cleaned);
        double area = 0.0, length = 0.0;
        for (size_t p = 0; p < loop.points.size(); ++p) {
            const Point& a = loop.points[p];
            const Point& b = loop.points[(p + 1) % loop.points.size()];
            area += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
            length += std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y));
        }
        loop.area = static_cast<float>(area * 0.5);
        loop.length = static_cast<float>(length);
        if (loop.length < kMinimumLoopLength) continue;
        loops.push_back(std::move(loop));
    }
    return loops;
}

// Which connected region lies on either side of a loop. Sampling just off the
// outline on both sides is orientation independent, so the tracer does not have
// to promise a winding direction.
int componentBesideLoop(const Labeled& labeled, const Loop& loop) {
    const size_t sample = loop.points.size() / 3;
    const Point& current = loop.points[sample];
    const Point& previous = loop.points[(sample + loop.points.size() - 1) % loop.points.size()];
    const Point& next = loop.points[(sample + 1) % loop.points.size()];
    const float dx = next.x - previous.x, dy = next.y - previous.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-6f) return -1;
    const float nx = -dy / length, ny = dx / length;
    auto lookup = [&](float offset) {
        const int x = static_cast<int>(std::lround(current.x + nx * offset));
        const int y = static_cast<int>(std::lround(current.y + ny * offset));
        if (x < 0 || y < 0 || x >= labeled.width || y >= labeled.height) return -1;
        return labeled.labels[static_cast<size_t>(y) * labeled.width + x];
    };
    int best = -1;
    for (float offset : {0.6f, -0.6f, 1.2f, -1.2f}) {
        const int candidate = lookup(offset);
        if (candidate >= 0) { best = candidate; if (std::abs(offset) <= 0.6f) break; }
    }
    return best;
}

// Which of the eight neighbours an unowned texel follows: the one with the
// strongest coverage, ties going to the lower region index. That makes the
// attribution a function of the mask alone instead of the order it is walked in.
int bestNeighbour(const Mask& mask, const std::vector<int>& owner, int x, int y) {
    int best = -1, weight = -1;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= mask.width || ny >= mask.height) continue;
            const size_t neighbour = static_cast<size_t>(ny) * mask.width + nx;
            const int id = owner[neighbour];
            if (id < 0) continue;
            const int candidate = static_cast<int>(mask.alpha[neighbour]);
            if (candidate > weight || (candidate == weight && id < best)) { best = id; weight = candidate; }
        }
    }
    return best;
}

struct Partition {
    std::vector<int> owner;  // -1 = not drawn, otherwise the component id
    int count = 0;           // components, the ones without an ink level label included
};

// Decides which texels of a mask the animated fill draws, and which region draws
// each of them. The regions themselves come from the ink level (`labels`): those
// are the shapes the outline is traced from and the units a formula staggers
// through. Two things are added on top of that threshold.
//
// Coverage below the ink level — the antialiased fringe — is attributed to the
// region beside it, so a fill samples the original alpha instead of a thresholded
// copy. Two scan-order rounds settle an ordinary edge; a worklist sweep then
// completes the attribution, because an edge that runs against the scan order, or
// one that is more than two texels wide, would otherwise stay unowned and vanish
// from every animated frame while the finished draw still shows it.
//
// A region whose coverage never reaches the ink level — a hairline, a very light
// glyph — has no label at all, so it is not a shape the outline can trace. It
// still carries ink: it becomes its own component and the fill shows it, with no
// outline phase to play. Four-connected, exactly like the ink level labelling.
//
// Finally one ring of zero coverage texels is added around the whole partition.
// A fill draws a texel span by sampling the original texture, and the bilinear
// filter of the pixels half a texel outside the outermost ink reads part of that
// ink: those pixels have to be covered too, or the last frame before the entry
// finishes — still drawn by the fill — would show a thinner edge than the frame
// after it, which is the whole quad. Drawing a zero coverage texel adds nothing
// by itself, because the sampled alpha is zero. The ring is deliberately cut off
// at the mask border: a UV outside the mask would read whatever the atlas stores
// next to the formula.
Partition partitionMask(const Mask& mask, const std::vector<int>& labels, int count) {
    Partition partition;
    partition.owner = labels;
    partition.count = count;
    const int total = mask.width * mask.height;
    // Two scan-order rounds settle an ordinary one or two texel antialiased edge.
    for (int pass = 0; pass < 2; ++pass) {
        bool changed = false;
        for (int y = 0; y < mask.height; ++y) {
            for (int x = 0; x < mask.width; ++x) {
                const size_t index = static_cast<size_t>(y) * mask.width + x;
                if (mask.alpha[index] == 0 || partition.owner[index] >= 0) continue;
                const int best = bestNeighbour(mask, partition.owner, x, y);
                if (best >= 0) { partition.owner[index] = best; changed = true; }
            }
        }
        if (!changed) break;
    }
    // Complete the attribution. Every texel with coverage must end up in exactly
    // one region, otherwise the fill would drop ink the finished draw shows.
    std::vector<int> worklist;
    worklist.reserve(static_cast<size_t>(total));
    for (int index = 0; index < total; ++index) if (partition.owner[index] >= 0) worklist.push_back(index);
    for (size_t head = 0; head < worklist.size(); ++head) {
        const int current = worklist[head];
        const int cx = current % mask.width, cy = current / mask.width;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= mask.width || ny >= mask.height) continue;
                const size_t index = static_cast<size_t>(ny) * mask.width + nx;
                if (mask.alpha[index] == 0 || partition.owner[index] >= 0) continue;
                const int best = bestNeighbour(mask, partition.owner, nx, ny);
                if (best < 0) continue;
                partition.owner[index] = best;
                worklist.push_back(static_cast<int>(index));
            }
        }
    }
    // Give every remaining group of coverage its own component. What is left here
    // is ink no ink level region reaches at all.
    for (int index = 0; index < total; ++index) {
        if (mask.alpha[index] == 0 || partition.owner[index] >= 0) continue;
        const int id = partition.count++;
        partition.owner[index] = id;
        std::vector<int> pending(1, index);
        while (!pending.empty()) {
            const int current = pending.back();
            pending.pop_back();
            const int cx = current % mask.width, cy = current / mask.width;
            const int neighbours[4][2] = {{cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1}};
            for (const auto& neighbour : neighbours) {
                if (neighbour[0] < 0 || neighbour[1] < 0 || neighbour[0] >= mask.width || neighbour[1] >= mask.height) continue;
                const size_t next = static_cast<size_t>(neighbour[1]) * mask.width + neighbour[0];
                if (mask.alpha[next] == 0 || partition.owner[next] >= 0) continue;
                partition.owner[next] = id;
                pending.push_back(static_cast<int>(next));
            }
        }
    }
    // One ring of filter support, computed from the settled partition so that it
    // stays exactly one texel wide instead of flooding the empty part of the mask.
    const std::vector<int> settled = partition.owner;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            const size_t index = static_cast<size_t>(y) * mask.width + x;
            if (mask.alpha[index] != 0 || settled[index] >= 0) continue;
            const int best = bestNeighbour(mask, settled, x, y);
            if (best >= 0) partition.owner[index] = best;
        }
    }
    return partition;
}

std::vector<Component> componentsFromMask(const Mask& mask, float offsetX, float offsetY) {
    const Labeled labeled = labelInk(mask);
    const Partition partition = partitionMask(mask, labeled.labels, labeled.count);
    const std::vector<int>& owner = partition.owner;
    std::vector<Component> components(static_cast<size_t>(partition.count));
    for (int id = 0; id < partition.count; ++id) {
        Component& component = components[static_cast<size_t>(id)];
        component.minX = component.inkMinX = static_cast<float>(mask.width);
        component.minY = component.inkMinY = static_cast<float>(mask.height);
        component.maxX = component.inkMaxX = -1.f;
        component.maxY = component.inkMaxY = -1.f;
    }
    // Fill spans and bounds come straight from the ownership map: a hole is a
    // texel that carries no region id, so nothing has to be subtracted at draw
    // time, and a soft edge belongs to exactly one region. The spans carry the
    // filter ring as well, so the drawn bounds are wider than the coverage; the
    // ink bounds are collected in the same walk and drive the drawing order.
    for (int y = 0; y < mask.height; ++y) {
        int x = 0;
        while (x < mask.width) {
            const int id = owner[static_cast<size_t>(y) * mask.width + x];
            if (id < 0) { ++x; continue; }
            const int begin = x;
            while (x < mask.width && owner[static_cast<size_t>(y) * mask.width + x] == id) ++x;
            Component& component = components[static_cast<size_t>(id)];
            component.minX = std::min(component.minX, static_cast<float>(begin));
            component.maxX = std::max(component.maxX, static_cast<float>(x - 1));
            component.minY = std::min(component.minY, static_cast<float>(y));
            component.maxY = std::max(component.maxY, static_cast<float>(y));
            for (int column = begin; column < x; ++column) {
                if (mask.alpha[static_cast<size_t>(y) * mask.width + column] == 0) continue;
                component.inkMinX = std::min(component.inkMinX, static_cast<float>(column));
                component.inkMaxX = std::max(component.inkMaxX, static_cast<float>(column));
                component.inkMinY = std::min(component.inkMinY, static_cast<float>(y));
                component.inkMaxY = std::max(component.inkMaxY, static_cast<float>(y));
            }
            if (component.rows.empty()) component.rows.resize(static_cast<size_t>(mask.height));
            component.rows[static_cast<size_t>(y)].push_back({static_cast<float>(begin), static_cast<float>(x)});
        }
    }
    for (Loop loop : traceLoops(mask)) {
        const int id = componentBesideLoop(labeled, loop);
        if (id < 0 || id >= partition.count) continue;
        // The contours live in the same coordinates as the spans and the bounds:
        // an unshifted loop would draw every stroke at the mask origin while the
        // fill advances per character.
        for (Point& point : loop.points) { point.x += offsetX; point.y += offsetY; }
        components[static_cast<size_t>(id)].loops.push_back(std::move(loop));
    }
    std::vector<Component> result;
    result.reserve(components.size());
    for (Component& component : components) {
        if (component.maxX < component.minX || component.rows.empty()) continue;
        // Outer boundary first (largest absolute area), then the holes. Rows are
        // re-based on minY and every row without a span is dropped.
        std::sort(component.loops.begin(), component.loops.end(),
            [](const Loop& a, const Loop& b) { return std::abs(a.area) > std::abs(b.area); });
        component.strokeLength = 0.f;
        for (const Loop& loop : component.loops) component.strokeLength += loop.length;
        component.minX += offsetX; component.maxX += offsetX;
        component.minY += offsetY; component.maxY += offsetY;
        component.inkMinX += offsetX; component.inkMaxX += offsetX;
        component.inkMinY += offsetY; component.inkMaxY += offsetY;
        const int firstRow = static_cast<int>(component.minY - offsetY);
        const int lastRow = static_cast<int>(component.maxY - offsetY);
        std::vector<std::vector<Span>> rows;
        rows.reserve(static_cast<size_t>(lastRow - firstRow + 1));
        for (int row = firstRow; row <= lastRow; ++row) {
            std::vector<Span> spans = std::move(component.rows[static_cast<size_t>(row)]);
            for (Span& span : spans) { span.x0 += offsetX; span.x1 += offsetX; }
            rows.push_back(std::move(spans));
        }
        component.rows = std::move(rows);
        result.push_back(std::move(component));
    }
    return result;
}

float medianHeight(const std::vector<Component>& components) {
    std::vector<float> heights;
    heights.reserve(components.size());
    for (const Component& component : components) heights.push_back(component.inkMaxY - component.inkMinY + 1.f);
    std::sort(heights.begin(), heights.end());
    return heights.empty() ? 1.f : heights[heights.size() / 2];
}

// Reading order: vertical bands separated by more than one median glyph height,
// top to bottom, and left to right inside a band. Deterministic for a given
// mask. This is the fallback for a formula without a usable writing plan
// (the plan supplies the real structural order: bar before numerator and
// denominator, base before scripts). The keys are the ink bounds, never
// the drawn bounds: the filter ring widens every region by the same texel, and
// widening the draw range must not reshuffle the animation.
void sortReadingOrder(std::vector<Component>& components) {
    if (components.size() < 2) return;
    const float band = std::max(1.f, medianHeight(components));
    std::sort(components.begin(), components.end(), [](const Component& a, const Component& b) {
        const float ay = (a.inkMinY + a.inkMaxY) * 0.5f, by = (b.inkMinY + b.inkMaxY) * 0.5f;
        if (ay != by) return ay < by;
        return a.inkMinX < b.inkMinX;
    });
    std::vector<std::vector<Component>> bands;
    for (Component& component : components) {
        const float centre = (component.inkMinY + component.inkMaxY) * 0.5f;
        if (bands.empty() || centre - (bands.back().front().inkMinY + bands.back().front().inkMaxY) * 0.5f > band) bands.emplace_back();
        bands.back().push_back(std::move(component));
    }
    std::vector<Component> ordered;
    for (auto& row : bands) {
        std::sort(row.begin(), row.end(), [](const Component& a, const Component& b) {
            if (a.inkMinX != b.inkMinX) return a.inkMinX < b.inkMinX;
            return a.inkMinY < b.inkMinY;
        });
        for (Component& component : row) ordered.push_back(std::move(component));
    }
    components = std::move(ordered);
}
// A plan is trusted only when it describes exactly the mask at hand: same size,
// same number of ink components and every component referenced exactly once. A
// stale, hand-edited or truncated plan then falls back to the reading order
// instead of dropping ink or drawing it twice.
bool planFits(const PlanView& plan, int width, int height, std::size_t components) {
    if (plan.units == nullptr || plan.unitCount <= 0) return false;
    if (plan.width != width || plan.height != height) return false;
    if (plan.componentCount != static_cast<int>(components)) return false;
    std::vector<char> used(components, 0);
    for (int unit = 0; unit < plan.unitCount; ++unit) {
        const PlanUnitView& view = plan.units[unit];
        if (view.components == nullptr || view.count <= 0) return false;
        for (int index = 0; index < view.count; ++index) {
            const int component = view.components[index];
            if (component < 0 || component >= static_cast<int>(components) || used[static_cast<std::size_t>(component)]) return false;
            used[static_cast<std::size_t>(component)] = 1;
        }
    }
    for (char value : used) if (!value) return false;
    return true;
}

} // namespace

Layout buildLayout(const std::vector<UnitRequest>& requests) {
    Layout layout;
    layout.units.reserve(requests.size());
    for (const UnitRequest& request : requests) {
        std::vector<Component> components = componentsFromMask(request.mask, request.x, request.y);
        if (request.mode == UnitMode::SingleUnit) {
            // Inside one character the ink regions are drawn top to bottom and
            // then left to right. This is a raster order, *not* the real stroke
            // order of the character.
            std::sort(components.begin(), components.end(), [](const Component& a, const Component& b) {
                if (a.inkMinY != b.inkMinY) return a.inkMinY < b.inkMinY;
                return a.inkMinX < b.inkMinX;
            });
            Unit unit;
            unit.components = std::move(components);
            for (const Component& component : unit.components) unit.strokeLength += component.strokeLength;
            layout.totalLength += unit.strokeLength;
            layout.units.push_back(std::move(unit));
        } else if (components.empty()) {
            layout.units.emplace_back();  // keep one unit per formula mask
        } else if (planFits(request.plan, request.mask.width, request.mask.height, components.size())) {
            // The formula's own writing order. One plan unit is one staggered
            // step and owns one or more of the mask's ink regions, so `=` is
            // drawn as one sign instead of two unrelated bars.
            for (int index = 0; index < request.plan.unitCount; ++index) {
                const PlanUnitView& planned = request.plan.units[index];
                Unit unit;
                for (int component = 0; component < planned.count; ++component) {
                    Component moved = std::move(components[static_cast<std::size_t>(planned.components[component])]);
                    unit.strokeLength += moved.strokeLength;
                    unit.components.push_back(std::move(moved));
                }
                layout.totalLength += unit.strokeLength;
                layout.units.push_back(std::move(unit));
            }
        } else {
            // No (usable) plan: keep the old, explainable reading order.
            sortReadingOrder(components);
            for (Component& component : components) {
                Unit unit;
                unit.strokeLength = component.strokeLength;
                layout.totalLength += unit.strokeLength;
                unit.components.push_back(std::move(component));
                layout.units.push_back(std::move(unit));
            }
        }
    }
    return layout;
}

bool hasWrite(const Json& object) { return object.contains("write") && !object.at("write").is_null(); }

Json resolved(const Json& object) {
    require(hasWrite(object), "Object has no write entry");
    const Json& spec = object.at("write");
    require(spec.is_object(), "write must be an object");
    return Json{{"start", spec.value("start", 0.0)},
        {"duration", spec.value("duration", 0.0)},
        {"stagger", spec.value("stagger", 0.35)},
        {"phase", spec.value("phase", 0.65)},
        {"outlineWidth", spec.value("outlineWidth", 1.5)},
        {"outline", spec.value("outline", true)},
        {"fill", spec.value("fill", true)},
        {"easing", spec.value("easing", std::string("smooth"))}};
}

Progress evaluate(const Json& object, double time, size_t unitCount) {
    Progress progress;
    if (!hasWrite(object)) { progress.total = 1.f; return progress; }
    const Json spec = resolved(object);
    const double start = spec.at("start").get<double>(), duration = spec.at("duration").get<double>();
    require(std::isfinite(time), "Write time must be finite");
    require(duration > 0.0, "write.duration must be positive");
    progress.outline = spec.at("outline").get<bool>();
    progress.fill = spec.at("fill").get<bool>();
    progress.total = static_cast<float>(std::clamp((time - start) / duration, 0.0, 1.0));
    progress.units.assign(unitCount, UnitPhase{});
    if (unitCount == 0) return progress;
    const double stagger = spec.at("stagger").get<double>();
    // Every unit advances the start by `stagger` unit durations, and the last
    // unit still ends exactly at start + duration.
    const double unitDuration = duration / (1.0 + stagger * (static_cast<double>(unitCount) - 1.0));
    const double offset = stagger * unitDuration;
    // `phase` is bounded below one: a stroke phase that consumed the whole unit
    // window would leave no time for the fill, which must never be shown before
    // the window ends.
    const float phase = static_cast<float>(std::clamp(spec.at("phase").get<double>(), 0.05, 0.95));
    const bool outline = progress.outline, fill = progress.fill;
    const bool smooth = spec.at("easing").get<std::string>() == "smooth";
    for (size_t i = 0; i < unitCount; ++i) {
        const double raw = std::clamp((time - (start + offset * static_cast<double>(i))) / unitDuration, 0.0, 1.0);
        float value = static_cast<float>(raw);
        if (smooth) value = value * value * (3.f - 2.f * value);
        UnitPhase unit;
        if (outline && fill) {
            unit.stroke = std::clamp(value / phase, 0.f, 1.f);
            const float rest = 1.f - phase;
            // The fill only completes at the very end of the unit window; with a
            // degenerate rest it stays empty until then instead of being treated
            // as finished from the first frame.
            unit.fill = rest > 1e-6f ? std::clamp((value - phase) / rest, 0.f, 1.f) : (value >= 1.f ? 1.f : 0.f);
        } else if (outline) {
            unit.stroke = value;
        } else {
            unit.stroke = 1.f;
            unit.fill = value;
        }
        progress.units[i] = unit;
    }
    return progress;
}

void validate(const Json& object, double sceneDuration) {
    if (!object.contains("write")) return;
    const std::string type = object.value("type", std::string());
    require(type == "text" || type == "formula", "Write entry is only supported on text/formula objects");
    const Json& spec = object.at("write");
    require(spec.is_object() && spec.size() <= 8, "write must be an object with at most 8 fields");
    const std::set<std::string> allowed{"start", "duration", "stagger", "phase", "outlineWidth", "outline", "fill", "easing"};
    for (auto it = spec.begin(); it != spec.end(); ++it) require(allowed.count(it.key()) != 0, "Unknown write field: " + it.key());
    require(spec.contains("start") && spec.at("start").is_number(), "write.start is required and must be a number");
    require(spec.contains("duration") && spec.at("duration").is_number(), "write.duration is required and must be a number");
    const double start = boundedNumber(spec, "start", 0.0, 1e6);
    const double duration = boundedNumber(spec, "duration", 1e-6, 3600.0);
    require(start + duration <= sceneDuration + 1e-9, "write.start + write.duration must stay inside the scene duration");
    if (spec.contains("stagger")) boundedNumber(spec, "stagger", 0.0, 1.0);
    // The outline phase must leave time for the fill, otherwise the object would
    // look finished as soon as its window starts.
    if (spec.contains("phase")) boundedNumber(spec, "phase", 0.05, 0.95);
    if (spec.contains("outlineWidth")) boundedNumber(spec, "outlineWidth", 0.5, 8.0);
    for (const char* key : {"outline", "fill"}) if (spec.contains(key)) require(spec.at(key).is_boolean(), std::string("write.") + key + " must be a boolean");
    if (spec.contains("easing")) {
        require(spec.at("easing").is_string(), "write.easing must be a string");
        const std::string easing = spec.at("easing").get<std::string>();
        require(easing == "linear" || easing == "smooth", "write.easing must be linear or smooth");
    }
    require(spec.value("outline", true) || spec.value("fill", true), "write must enable the outline, the fill or both");
    // The sampling loop in animation::validate() already rejects unknown fields
    // on the object; write is validated here because it is not an animation
    // channel but a draw-time entry effect.
}

Json demo() {
    Registry registry;
    Json scene = Document::demo();
    scene["title"] = "Write 手写入场（中文 + 公式）";
    scene["duration"] = 6.0;
    scene["steps"] = 1;
    scene["objects"] = Json::array();
    auto add = [&](const char* id, const char* type, const char* name, Json properties) {
        Json p = registry.get(type).defaults;
        p.update(properties);
        scene["objects"].push_back({{"id", id}, {"type", type}, {"name", name}, {"properties", p}, {"visible", true}, {"locked", false}, {"step", 0}});
    };
    add("title", "text", "手写标题", {{"x", -6.0}, {"y", 5.4}, {"text", "手写入场动画"}, {"size", 56}});
    scene["objects"].back()["write"] = {{"start", 0.0}, {"duration", 2.2}, {"stagger", 0.5}, {"phase", 0.7}};
    add("note", "text", "中文说明", {{"x", -6.0}, {"y", 2.8}, {"text", "沿字形轮廓逐步描绘，再填充"}, {"size", 36}});
    scene["objects"].back()["write"] = {{"start", 0.8}, {"duration", 3.2}, {"stagger", 0.45}, {"phase", 0.65}};
    add("flight", "formula", "飞行时间", {{"x", -6.0}, {"y", -0.2}, {"formula", "flight"}, {"scale", 2.0}});
    scene["objects"].back()["write"] = {{"start", 1.8}, {"duration", 2.6}, {"stagger", 1.0}, {"phase", 0.6}};
    add("position", "formula", "位置公式", {{"x", -6.0}, {"y", -3.4}, {"formula", "position_y"}, {"scale", 2.0}});
    scene["objects"].back()["write"] = {{"start", 3.2}, {"duration", 2.6}, {"stagger", 1.0}, {"phase", 0.6}};
    return scene;
}
}
