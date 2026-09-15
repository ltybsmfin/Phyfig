#include "editor_write.hpp"

#include "editor.hpp"
#include "font_support.hpp"
#include "formula.hpp"
#include "write_entry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace physics_editor {
namespace {

// The text outline is traced from a CPU raster of this size and then scaled to
// the drawn pixel size. A fixed raster keeps the cache independent of zoom and
// gives Chinese glyphs a smooth silhouette instead of the 18 px atlas steps.
constexpr float kTextRasterPx = write_entry::kDefaultRasterPx;

// One decoded character of the text. `layoutX/layoutY` is where the traced
// 64 px mask's top-left sits in the cached layout, and `maskWidth/maskHeight` is
// that mask's pixel size: together they map the high-resolution outline onto the
// real glyph rectangle of the current atlas (see write_draw::text below).
struct TextCharacter {
    uint32_t codepoint = 0;
    float layoutX = 0.f, layoutY = 0.f;
    float maskWidth = 0.f, maskHeight = 0.f;
};
struct TextGeometry { std::string text; write_entry::Layout layout; std::vector<TextCharacter> characters; };
struct FormulaGeometry { std::string name; write_entry::Layout layout; };

// Small main-thread caches. Rebuilding is expensive (a throwaway font atlas or a
// full mask decode), drawing is not, so entries are keyed by the text and by the
// formula resource identity and evicted least-recently-used.
std::shared_ptr<const TextGeometry> textGeometry(const std::string& text) {
    static std::deque<std::shared_ptr<const TextGeometry>> cache;
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if ((*it)->text != text) continue;
        std::shared_ptr<const TextGeometry> found = *it;
        cache.erase(it);
        cache.push_front(found);
        return found;
    }
    auto geometry = std::make_shared<TextGeometry>();
    geometry->text = text;
    const auto glyphs = graphics::font_support::rasterizeText(text, kTextRasterPx, graphics::font_support::systemFontCandidates());
    std::vector<write_entry::UnitRequest> requests;
    requests.reserve(glyphs.size());
    geometry->characters.reserve(glyphs.size());
    for (const auto& glyph : glyphs) {
        write_entry::UnitRequest request;
        request.x = glyph.penX + glyph.x;
        request.y = glyph.penY + glyph.y;
        request.mode = write_entry::UnitMode::SingleUnit;
        request.mask = {glyph.hasBitmap ? glyph.alpha.data() : nullptr, glyph.width, glyph.height};
        requests.push_back(request);
        TextCharacter character;
        character.codepoint = glyph.codepoint;
        character.layoutX = request.x;
        character.layoutY = request.y;
        character.maskWidth = glyph.hasBitmap ? static_cast<float>(glyph.width) : 0.f;
        character.maskHeight = glyph.hasBitmap ? static_cast<float>(glyph.height) : 0.f;
        geometry->characters.push_back(character);
    }
    // The masks point into `glyphs`, which stays alive for this call.
    geometry->layout = write_entry::buildLayout(requests);
    cache.push_front(geometry);
    if (cache.size() > 24) cache.pop_back();
    return geometry;
}

std::shared_ptr<const FormulaGeometry> formulaGeometry(const std::string& name, uint64_t resources) {
    static std::deque<std::shared_ptr<const FormulaGeometry>> cache;
    static uint64_t cacheGeneration = ~uint64_t(0);
    if (cacheGeneration != resources) { cache.clear(); cacheGeneration = resources; }
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if ((*it)->name != name) continue;
        std::shared_ptr<const FormulaGeometry> found = *it;
        cache.erase(it);
        cache.push_front(found);
        return found;
    }
    const auto formula = math_text::entry(static_cast<std::size_t>(math_text::find(name.c_str())));
    std::vector<unsigned char> alpha(static_cast<std::size_t>(formula.width) * static_cast<std::size_t>(formula.height), 0);
    std::size_t position = 0;
    for (std::size_t p = 0; p < formula.bytes; p += 2) {
        if (p + 1 >= formula.bytes) throw std::runtime_error("Invalid formula RLE");
        for (int repeat = 0; repeat < static_cast<int>(formula.rle[p]); ++repeat) {
            if (position >= alpha.size()) throw std::runtime_error("Formula mask overflow");
            alpha[position++] = formula.rle[p + 1];
        }
    }
    if (position != alpha.size()) throw std::runtime_error("Incomplete formula mask");
    write_entry::UnitRequest request;
    request.mask = {alpha.data(), formula.width, formula.height};
    request.mode = write_entry::UnitMode::ComponentPerUnit;
    auto geometry = std::make_shared<FormulaGeometry>();
    geometry->name = name;
    geometry->layout = write_entry::buildLayout({request});
    cache.push_front(geometry);
    if (cache.size() > 24) cache.pop_back();
    return geometry;
}

ImU32 scaledAlpha(ImU32 colour, float alpha) {
    const float base = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFFu);
    const int value = static_cast<int>(std::lround(base * std::clamp(alpha, 0.f, 1.f)));
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(value, 0, 255)) << IM_COL32_A_SHIFT);
}

// IM_TRUNC() from imgui_internal.h. ImFont::RenderText() truncates the line
// origin once and derives every glyph quad from it, so the animated fill has to
// follow the same arithmetic to land on exactly the same pixels.
float truncateToPixel(float value) { return static_cast<float>(static_cast<int>(value)); }

// ImFont::RenderText() tints atlas glyphs with the text colour and leaves
// coloured glyphs untinted (the fill alpha still applies).
ImU32 fillColour(const ImFontGlyph* glyph, ImU32 tint, float alpha) {
    return scaledAlpha(glyph->Colored ? IM_COL32_WHITE : tint, alpha);
}

// Maps the cached 64 px outline onto the rectangle of the glyph the current atlas
// actually draws. The raster size and the atlas size rarely match, so the two
// axes scale independently.
struct GlyphTransform {
    float originX = 0.f, originY = 0.f, scaleX = 1.f, scaleY = 1.f;
    ImVec2 apply(const write_entry::Point& point) const {
        return {originX + point.x * scaleX, originY + point.y * scaleY};
    }
};

// Draws the first `progress` fraction of a unit's outline length. Every contour
// is stroked on its own, so a gap between two ink regions is never bridged by a
// connecting line; the growing tip gets a dot, which reads as a pen.
void strokeUnit(ImDrawList* list, const write_entry::Unit& unit, float progress, const GlyphTransform& transform,
    ImU32 colour, float width) {
    if (progress <= 0.f) return;
    static std::vector<ImVec2> points;
    float budget = progress * unit.strokeLength;
    for (const auto& component : unit.components) {
        for (const auto& loop : component.loops) {
            if (budget <= 0.f) return;
            const std::size_t count = loop.points.size();
            if (count < 2) continue;
            points.clear();
            points.push_back(transform.apply(loop.points[0]));
            float used = 0.f;
            bool reached = false;
            for (std::size_t i = 1; i <= count && !reached; ++i) {
                const auto& previous = loop.points[i - 1];
                const auto& current = loop.points[i % count];  // closes the loop
                const float segment = std::hypot(current.x - previous.x, current.y - previous.y);
                if (used + segment <= budget) {
                    points.push_back(transform.apply(current));
                    used += segment;
                    continue;
                }
                const float fraction = segment > 1e-6f ? (budget - used) / segment : 0.f;
                points.push_back(transform.apply({previous.x + (current.x - previous.x) * fraction,
                    previous.y + (current.y - previous.y) * fraction}));
                used = budget;
                reached = true;
            }
            budget -= used;
            if (points.size() >= 2) list->AddPolyline(points.data(), static_cast<int>(points.size()), colour, 0, width);
            list->AddCircleFilled(points.back(), std::max(0.7f, width * 0.5f), colour);
        }
    }
}

bool inputNumber(const char* label, double* value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", *value);
    if (!ImGui::InputText(label, buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsScientific)) return false;
    char* end = nullptr;
    const double parsed = std::strtod(buffer, &end);
    if (end == buffer || *end != '\0' || !std::isfinite(parsed)) return false;
    *value = parsed;
    return true;
}
} // namespace

namespace write_draw {

bool text(ImDrawList* list, const Json& object, ImVec2 position, float pixelSize, const char* text, ImU32 tint,
    double time, float canvasScale) {
    if (!write_entry::hasWrite(object) || text == nullptr || text[0] == '\0') return false;
    ImFont* font = ImGui::GetFont();
    if (font == nullptr || font->ContainerAtlas == nullptr || !(font->FontSize > 0.f) || !(pixelSize > 0.f)) return false;
    const std::shared_ptr<const TextGeometry> geometry = textGeometry(text);
    const write_entry::Progress progress = write_entry::evaluate(object, time, geometry->layout.units.size());
    if (progress.total >= 1.f) return false;  // finished: the caller draws the plain text
    if (progress.total <= 0.f) return true;   // not started: nothing may be visible
    const Json spec = write_entry::resolved(object);
    const float outlineWidth = static_cast<float>(spec.at("outlineWidth").get<double>()) * canvasScale;
    // Layout exactly like ImFont::RenderText(): the line origin is truncated once
    // and every glyph follows the real atlas metrics, so the animated text sits on
    // the same pixels as the finished AddText — no jump when the entry ends.
    const float scale = pixelSize / font->FontSize;
    const float lineHeight = font->FontSize * scale;
    const float originX = truncateToPixel(position.x), originY = truncateToPixel(position.y);
    const ImTextureID texture = font->ContainerAtlas->TexID;
    // Bind the atlas explicitly: the fill samples the glyph bitmaps and the pen
    // tip needs the atlas white pixel, so the object must not depend on whatever
    // texture the draw list happened to carry.
    list->PushTextureID(texture);
    float penX = originX, penY = originY;
    for (std::size_t i = 0; i < geometry->characters.size() && i < geometry->layout.units.size(); ++i) {
        const TextCharacter& character = geometry->characters[i];
        if (character.codepoint == '\n') { penX = originX; penY += lineHeight; continue; }
        if (character.codepoint == '\r') continue;  // RenderText() skips the carriage return
        const ImWchar lookup = static_cast<ImWchar>(character.codepoint);
        const ImFontGlyph* glyph = font->FindGlyph(lookup);
        if (glyph == nullptr) continue;  // RenderText() draws nothing and does not advance
        const write_entry::UnitPhase phase = i < progress.units.size() ? progress.units[i] : write_entry::UnitPhase{};
        if (glyph->Visible) {
            const ImVec2 low{penX + glyph->X0 * scale, penY + glyph->Y0 * scale};
            const ImVec2 high{penX + glyph->X1 * scale, penY + glyph->Y1 * scale};
            if (progress.fill && phase.fill > 0.002f) {
                // The glyph's own texture rectangle: the fill is the original
                // antialiased bitmap, not a thresholded copy, and it is placed by
                // exactly the arithmetic AddText uses.
                list->AddImage(texture, low, high, {glyph->U0, glyph->V0}, {glyph->U1, glyph->V1},
                    fillColour(glyph, tint, phase.fill));
            }
            if (progress.outline && phase.fill < 0.998f && character.maskWidth > 0.f && character.maskHeight > 0.f &&
                font->FindGlyphNoFallback(lookup) != nullptr) {
                // Only outline a glyph the live atlas really contains. The traced
                // outline is mapped onto this glyph's real bounding box.
                GlyphTransform transform;
                transform.scaleX = (glyph->X1 - glyph->X0) * scale / character.maskWidth;
                transform.scaleY = (glyph->Y1 - glyph->Y0) * scale / character.maskHeight;
                transform.originX = low.x - character.layoutX * transform.scaleX;
                transform.originY = low.y - character.layoutY * transform.scaleY;
                strokeUnit(list, geometry->layout.units[i], phase.stroke, transform,
                    scaledAlpha(tint, 1.f - phase.fill), outlineWidth);
            }
        }
        penX += glyph->AdvanceX * scale;
    }
    list->PopTextureID();
    return true;
}

bool formula(ImDrawList* list, const Json& object, ImVec2 position, ImU32 tint, float scale, double time,
    float canvasScale, uint64_t resources) {
    if (!write_entry::hasWrite(object)) return false;
    const std::string name = object.at("properties").at("formula").get<std::string>();
    const std::shared_ptr<const FormulaGeometry> geometry = formulaGeometry(name, resources);
    const write_entry::Progress progress = write_entry::evaluate(object, time, geometry->layout.units.size());
    if (progress.total >= 1.f) return false;
    if (progress.total <= 0.f) return true;
    const Json spec = write_entry::resolved(object);
    // Mask pixels to screen pixels: the same factor math_text::draw() uses.
    const float pixelScale = formula_cache::displayScale * scale;
    ImVec2 uv0{}, uv1{};
    math_text::textureRect(name.c_str(), &uv0, &uv1);
    int maskWidth = 1, maskHeight = 1;
    math_text::maskSize(name.c_str(), &maskWidth, &maskHeight);
    const float width = static_cast<float>(std::max(1, maskWidth)), height = static_cast<float>(std::max(1, maskHeight));
    const ImTextureID texture = ImGui::GetIO().Fonts->TexID;
    // The fill samples the mask inside the atlas and the pen tip needs its white
    // pixel, so bind the atlas explicitly for this object.
    list->PushTextureID(texture);
    if (progress.fill) {
        for (std::size_t i = 0; i < progress.units.size(); ++i) {
            const float fill = progress.units[i].fill;
            if (fill <= 0.002f) continue;
            const ImU32 colour = scaledAlpha(tint, fill);
            for (const auto& component : geometry->layout.units[i].components) {
                for (std::size_t row = 0; row < component.rows.size(); ++row) {
                    const float top = position.y + (component.minY + static_cast<float>(row)) * pixelScale;
                    const float v0 = uv0.y + (uv1.y - uv0.y) * ((component.minY + static_cast<float>(row)) / height);
                    const float v1 = uv0.y + (uv1.y - uv0.y) * ((component.minY + static_cast<float>(row) + 1.f) / height);
                    for (const auto& span : component.rows[row]) {
                        // Each texel span is drawn from the original mask texture, so
                        // the antialiased edge keeps its real coverage instead of a
                        // thresholded colour. The spans tile the region's texels
                        // exactly and never overlap, so a texel is blended exactly
                        // once. They also carry one ring of zero coverage texels
                        // around the ink: the fill draws nothing there, but the
                        // bilinear filter reads them for the soft edge half a texel
                        // outside the outermost ink, which is what makes the last
                        // fill frame agree with the finished draw that follows it.
                        const float u0 = uv0.x + (uv1.x - uv0.x) * (span.x0 / width);
                        const float u1 = uv0.x + (uv1.x - uv0.x) * (span.x1 / width);
                        list->AddImage(texture, {position.x + span.x0 * pixelScale, top},
                            {position.x + span.x1 * pixelScale, top + pixelScale}, {u0, v0}, {u1, v1}, colour);
                    }
                }
            }
        }
    }
    if (progress.outline) {
        const float strokeWidth = static_cast<float>(spec.at("outlineWidth").get<double>()) * canvasScale;
        GlyphTransform transform;  // mask pixels -> screen pixels inside the placed mask
        transform.originX = position.x;
        transform.originY = position.y;
        transform.scaleX = pixelScale;
        transform.scaleY = pixelScale;
        for (std::size_t i = 0; i < progress.units.size(); ++i) {
            const write_entry::UnitPhase& phase = progress.units[i];
            strokeUnit(list, geometry->layout.units[i], phase.stroke, transform,
                scaledAlpha(tint, 1.f - phase.fill), strokeWidth);
        }
    }
    list->PopTextureID();
    return true;
}
} // namespace write_draw

void Editor::writePanel(const Json& object) {
    ImGui::SeparatorText("Write 手写入场");
    const double sceneDuration = active().scene()["duration"].get<double>();
    auto apply = [&](Json write) {
        execute(Json::array({{{"op", "set_write"}, {"id", selected_}, {"write", std::move(write)}}}));
    };
    if (!write_entry::hasWrite(object)) {
        if (ImGui::Button("添加 Write 入场")) {
            const double duration = std::min(1.5, std::max(0.2, sceneDuration));
            apply({{"start", 0.0}, {"duration", duration}, {"stagger", 0.35}, {"phase", 0.65},
                {"outlineWidth", 1.5}, {"outline", true}, {"fill", true}, {"easing", "smooth"}});
        }
        ImGui::TextWrapped("未设置时文字/公式按原样显示。添加后沿字形轮廓逐步描绘，再淡入填充。");
        return;
    }
    const Json spec = write_entry::resolved(object);
    double start = spec.at("start").get<double>(), duration = spec.at("duration").get<double>();
    double stagger = spec.at("stagger").get<double>(), phase = spec.at("phase").get<double>();
    double outlineWidth = spec.at("outlineWidth").get<double>();
    bool outline = spec.at("outline").get<bool>(), fill = spec.at("fill").get<bool>();
    int easing = spec.at("easing").get<std::string>() == "smooth" ? 1 : 0;
    bool changed = false;
    changed |= inputNumber("开始 / s", &start);
    changed |= inputNumber("时长 / s", &duration);
    changed |= inputNumber("错开 stagger", &stagger);
    ImGui::BeginDisabled(!(outline && fill));
    changed |= inputNumber("描边占比 phase (<1)", &phase);
    ImGui::EndDisabled();
    changed |= inputNumber("描边宽度 / px", &outlineWidth);
    changed |= ImGui::Checkbox("描边阶段", &outline);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("填充阶段", &fill);
    changed |= ImGui::Combo("缓动", &easing, "线性 linear\0平滑 smooth\0");
    if (changed) {
        apply({{"start", start}, {"duration", duration}, {"stagger", stagger}, {"phase", phase},
            {"outlineWidth", outlineWidth}, {"outline", outline}, {"fill", fill},
            {"easing", easing == 1 ? "smooth" : "linear"}});
    }
    if (ImGui::Button("移除 Write 入场")) apply(Json());
    ImGui::TextWrapped("描边按当前字体真实字形框映射，填充直接使用原字形位图；phase 必须小于 1，轮廓来自覆盖掩码"
                       "（非字体矢量路径），中文按墨迹连通区域从上到下、从左到右描绘，不是汉字真实笔顺。开始+时长必须落在场景时长内。");
}
}
