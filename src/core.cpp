#include "core.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>

namespace snow {
namespace {
constexpr double FixedStep = 1.0 / 120.0;
constexpr float SnowBand = 96;
constexpr float BallMassScale = 400.f / (11.f * 11.f);
constexpr float MaximumBallRadius = 18;
constexpr float MaximumDragDistance = 256;
constexpr std::size_t MaximumDecorations = 32;
constexpr std::size_t MaximumStack = 3;
bool footprintVisible(const SurfaceSpec& spec, float left, float right) noexcept {
    return left >= static_cast<float>(spec.bounds.left) && right <= static_cast<float>(spec.bounds.right) &&
        std::any_of(spec.visible.begin(), spec.visible.end(), [left, right](Span span) {
            return left >= static_cast<float>(span.left) && right <= static_cast<float>(span.right);
        });
}
float figureHeight(const Decoration& decoration) noexcept {
    return decoration.radius * (decoration.kind == Tool::Snowman ? 4.17f : 2.f);
}
float collectionRadius(const SurfaceSpec& spec, float x, float requested) noexcept {
    for (Span span : spec.visible) {
        if (x >= static_cast<float>(span.left) && x < static_cast<float>(span.right)) {
            // Whole 8px bins only: do not collect a bin that straddles occlusion.
            const float margin = static_cast<float>(CellWidth) * .5f;
            return std::max(0.f, std::min({requested, x - static_cast<float>(span.left) - margin,
                static_cast<float>(span.right) - x - margin}));
        }
    }
    return 0;
}
bool visible(const SurfaceSpec& spec, float x) noexcept {
    return std::any_of(spec.visible.begin(), spec.visible.end(), [x](Span span) {
        return x >= static_cast<float>(span.left) && x < static_cast<float>(span.right);
    });
}
float bounded(float value, float low, float high, float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
bool safeRect(Rect r) noexcept {
    constexpr int Limit = 1000000;
    return r.left >= -Limit && r.top >= -Limit && r.right <= Limit && r.bottom <= Limit &&
           r.left <= Limit && r.top <= Limit && r.right >= -Limit && r.bottom >= -Limit && r.valid();
}
template<class T> bool number(std::string_view text, T& out) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool same(const Settings& a, const Settings& b) noexcept {
    return a.weather == b.weather && a.profile == b.profile && a.particles == b.particles &&
           a.speed == b.speed && a.wind == b.wind && a.size == b.size &&
           a.accumulationRate == b.accumulationRate && a.maxDepth == b.maxDepth &&
           a.interaction == b.interaction;
}
}
Settings validated(Settings s) noexcept {
    if (static_cast<unsigned>(s.weather) > 4U) s.weather = Weather::Normal;
    if (static_cast<unsigned>(s.profile) > 2U) s.profile = Profile::Balanced;
    s.particles = std::clamp(s.particles, 0, static_cast<int>(MaxParticles));
    s.speed = bounded(s.speed, .1f, 4, 1); s.wind = bounded(s.wind, -250, 250, 0);
    s.size = bounded(s.size, .5f, 3, 1); s.accumulationRate = bounded(s.accumulationRate, .1f, 5, 1);
    s.maxDepth = bounded(s.maxDepth, 4, 96, 48); s.interaction = bounded(s.interaction, .25f, 4, 1);
    return s;
}
std::string serializeSettings(Settings s) {
    s = validated(s);
    return "version=1\nweather=" + std::to_string(static_cast<int>(s.weather)) +
        "\nprofile=" + std::to_string(static_cast<int>(s.profile)) +
        "\nparticles=" + std::to_string(s.particles) + "\nspeed=" + std::to_string(s.speed) +
        "\nwind=" + std::to_string(s.wind) + "\nsize=" + std::to_string(s.size) +
        "\naccumulationRate=" + std::to_string(s.accumulationRate) +
        "\nmaxDepth=" + std::to_string(s.maxDepth) + "\ninteraction=" + std::to_string(s.interaction) +
        "\naccumulation=" + (s.accumulation ? "1\n" : "0\n");
}
std::optional<Settings> parseSettings(std::string_view text) {
    if (text.size() > 8192) return {};
    Settings s; bool versionSeen = false;
    while (!text.empty()) {
        const auto end = text.find('\n'); auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const auto split = line.find('='); if (split == line.npos) return {};
        const auto key = line.substr(0, split), value = line.substr(split + 1);
        if (key == "version") { int v{}; if (!number(value, v) || v != 1 || versionSeen) return {}; versionSeen = true; }
        else if (key == "weather") { int v{}; if (!number(value, v)) return {}; s.weather = static_cast<Weather>(v); }
        else if (key == "profile") { int v{}; if (!number(value, v)) return {}; s.profile = static_cast<Profile>(v); }
        else if (key == "particles") { if (!number(value, s.particles)) return {}; }
        else if (key == "speed") { if (!number(value, s.speed)) return {}; }
        else if (key == "wind") { if (!number(value, s.wind)) return {}; }
        else if (key == "size") { if (!number(value, s.size)) return {}; }
        else if (key == "accumulationRate") { if (!number(value, s.accumulationRate)) return {}; }
        else if (key == "maxDepth") { if (!number(value, s.maxDepth)) return {}; }
        else if (key == "interaction") { if (!number(value, s.interaction)) return {}; }
        else if (key == "accumulation") { if (value != "0" && value != "1") return {}; s.accumulation = value == "1"; }
    }
    if (!versionSeen || !same(s, validated(s))) return {};
    return s;
}
void HeightField::resize(int width) {
    width_ = std::clamp(width, 0, static_cast<int>(MaxSurfaceCells) * CellWidth);
    const auto cells = static_cast<std::size_t>((width_ + CellWidth - 1) / CellWidth);
    heights_.resize(cells, 0);
    // Topology events only; release oversized retained storage after a substantial shrink.
    if (heights_.capacity() > std::max<std::size_t>(64, cells * 2)) heights_.shrink_to_fit();
}
void HeightField::clampDepth(float maximum) noexcept {
    maximum = std::isfinite(maximum) ? std::clamp(maximum, 0.f, 96.f) : 0.f;
    for (float& height : heights_) height = std::min(height, maximum);
}
float HeightField::height(float x) const noexcept {
    if (!std::isfinite(x) || x < 0 || x >= static_cast<float>(width_) || heights_.empty()) return 0;
    return heights_[static_cast<std::size_t>(x / static_cast<float>(CellWidth))];
}
float HeightField::volume() const noexcept {
    float sum = 0;
    for (std::size_t i = 0; i < heights_.size(); ++i)
        sum += heights_[i] * static_cast<float>(std::min(CellWidth, width_ - static_cast<int>(i) * CellWidth));
    return sum;
}
float HeightField::deposit(float x, float amount, float maxDepth) noexcept {
    if (!std::isfinite(x) || !std::isfinite(amount) || !std::isfinite(maxDepth) || amount <= 0 ||
        maxDepth <= 0 || x < 0 || x >= static_cast<float>(width_)) return 0;
    maxDepth = std::min(maxDepth, 96.f);
    const int center = static_cast<int>(x) / CellWidth;
    float added = 0;
    // Five compact bins form a small mound, no heap allocation or per-pixel field.
    constexpr float weights[]{1, 2, 3, 2, 1};
    float totalWeight = 0;
    for (int d = -2; d <= 2; ++d) if (center + d >= 0 && center + d < static_cast<int>(heights_.size())) totalWeight += weights[d + 2];
    for (int d = -2; d <= 2; ++d) {
        const int index = center + d;
        if (index < 0 || index >= static_cast<int>(heights_.size())) continue;
        auto& cell = heights_[static_cast<std::size_t>(index)];
        // Reducing maximum depth clamps existing snow in affected bins as well.
        cell = std::min(cell, maxDepth);
        const float binWidth = static_cast<float>(std::min(CellWidth, width_ - index * CellWidth));
        const float delta = std::min(maxDepth - cell, amount * weights[d + 2] / totalWeight / binWidth);
        cell += delta; added += delta * binWidth;
    }
    return added;
}
float HeightField::available(float x, float radius) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(radius) || radius <= 0) return 0;
    float result = 0;
    for (std::size_t i = 0; i < heights_.size(); ++i) {
        const float center = static_cast<float>(i * CellWidth) + static_cast<float>(CellWidth) * .5f;
        if (std::abs(center - x) <= radius) result += heights_[i] * static_cast<float>(std::min(CellWidth, width_ - static_cast<int>(i) * CellWidth));
    }
    return result;
}
float HeightField::clear(float x, float radius, float strength) noexcept {
    if (!std::isfinite(x) || !std::isfinite(radius) || !std::isfinite(strength) || radius <= 0 || strength <= 0) return 0;
    float removed = 0;
    for (std::size_t i = 0; i < heights_.size(); ++i) {
        const float center = static_cast<float>(i * CellWidth) + static_cast<float>(CellWidth) * .5f;
        const float distance = std::abs(center - x);
        if (distance > radius) continue;
        const float delta = std::min(heights_[i], strength * (1 - distance / radius));
        heights_[i] -= delta;
        removed += delta * static_cast<float>(std::min(CellWidth, width_ - static_cast<int>(i) * CellWidth));
    }
    return removed;
}
float HeightField::collect(float x, float radius, float amount) noexcept {
    if (!std::isfinite(amount) || amount <= 0 || !std::isfinite(x) || !std::isfinite(radius) || radius <= 0) return 0;
    float taken = 0;
    for (std::size_t i = 0; i < heights_.size() && taken < amount; ++i) {
        const float center = static_cast<float>(i * CellWidth) + static_cast<float>(CellWidth) * .5f;
        if (std::abs(center - x) > radius) continue;
        const float width = static_cast<float>(std::min(CellWidth, width_ - static_cast<int>(i) * CellWidth));
        const float delta = std::min(heights_[i], (amount - taken) / width);
        heights_[i] -= delta; taken += delta * width;
    }
    return taken;
}
SnowAccumulation::SnowAccumulation() { surfaces_.reserve(MaxSurfaces); }
void SnowAccumulation::reconcile(std::span<const SurfaceSpec> snapshot) {
    std::erase_if(surfaces_, [&](const SurfaceState& state) {
        return std::none_of(snapshot.begin(), snapshot.end(), [&](const SurfaceSpec& s) { return s.id == state.spec.id && safeRect(s.bounds); });
    });
    for (const auto& spec : snapshot) {
        if (!safeRect(spec.bounds) || !spec.id) continue;
        auto it = std::find_if(surfaces_.begin(), surfaces_.end(), [&](const SurfaceState& s) { return s.spec.id == spec.id; });
        SurfaceState* state = nullptr;
        if (it == surfaces_.end()) {
            if (surfaces_.size() == MaxSurfaces) break;
            // Never copy an untrusted source vector before applying its limit.
            surfaces_.push_back({{spec.id, spec.bounds, spec.taskbar, {}}, HeightField(spec.bounds.width())});
            state = &surfaces_.back();
        } else {
            it->field.resize(spec.bounds.width()); it->spec.id = spec.id;
            it->spec.bounds = spec.bounds; it->spec.taskbar = spec.taskbar; state = &*it;
        }
        const auto count = std::min<std::size_t>(spec.visible.size(), 32);
        state->spec.visible.assign(spec.visible.begin(), spec.visible.begin() + static_cast<std::ptrdiff_t>(count));
    }
    std::iota(sorted_.begin(), sorted_.begin() + static_cast<std::ptrdiff_t>(surfaces_.size()), std::size_t{});
    std::sort(sorted_.begin(), sorted_.begin() + static_cast<std::ptrdiff_t>(surfaces_.size()), [&](std::size_t a, std::size_t b) { return surfaces_[a].spec.bounds.top < surfaces_[b].spec.bounds.top; });
}
bool SnowAccumulation::deposit(float x, float oldY, float newY, float amount, float maxDepth) noexcept {
    if (!std::isfinite(x) || !std::isfinite(oldY) || !std::isfinite(newY) || newY <= oldY) return false;
    SurfaceState* hitSurface = nullptr; float closest = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < surfaces_.size(); ++i) {
        auto& s = surfaces_[sorted_[i]];
        const float top = static_cast<float>(s.spec.bounds.top);
        if (top - 96 > newY) break;
        if (top < oldY || !visible(s.spec, x)) continue;
        const float localX = x - static_cast<float>(s.spec.bounds.left);
        if (localX < 0 || localX >= static_cast<float>(s.field.width())) continue;
        const float edge = top - s.field.height(localX);
        if (oldY <= edge && newY >= edge && edge < closest) { hitSurface = &s; closest = edge; }
    }
    if (!hitSurface) return false;
    hitSurface->field.deposit(x - static_cast<float>(hitSurface->spec.bounds.left), amount, maxDepth);
    return true;
}
SurfaceState* SnowAccumulation::hit(float x, float y, float radius) noexcept {
    SurfaceState* best = nullptr; float distance = radius + 96;
    for (auto& s : surfaces_) {
        if (!visible(s.spec, x)) continue;
        const float local = x - static_cast<float>(s.spec.bounds.left);
        if (local < 0 || local >= static_cast<float>(s.field.width())) continue;
        const float top = static_cast<float>(s.spec.bounds.top);
        const float delta = std::abs(y - (top - s.field.height(local)));
        if (y <= top + radius && y >= top - 96 - radius && delta < distance) { best = &s; distance = delta; }
    }
    return best;
}
float SnowAccumulation::clear(float x, float y, float radius, float strength) noexcept {
    auto* state = hit(x, y, radius);
    return state ? state->field.clear(x - static_cast<float>(state->spec.bounds.left), radius, strength) : 0;
}
const SurfaceState* SnowAccumulation::find(std::uint64_t id) const noexcept {
    for (const auto& s : surfaces_) if (s.spec.id == id) return &s;
    return nullptr;
}
float SnowAccumulation::totalVolume() const noexcept {
    float sum = 0; for (const auto& s : surfaces_) sum += s.field.volume(); return sum;
}
SnowSimulation::SnowSimulation(std::uint32_t seed) : rng_(seed ? seed : 1) { decorations_.reserve(MaximumDecorations); }
float SnowSimulation::random() noexcept {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.f / 16777216.f);
}
void SnowSimulation::spawn(Particle& p, bool initial) noexcept {
    p.x = static_cast<float>(bounds_.left) + random() * static_cast<float>(bounds_.width());
    p.y = initial ? static_cast<float>(bounds_.top) + random() * static_cast<float>(bounds_.height()) : static_cast<float>(bounds_.top) - 8;
    p.speed = 24 + random() * 65; p.drift = (random() - .5f) * 30;
    p.radius = .8f + random() * 1.9f; p.phase = random() * 6.2831853f;
    // A small deterministic foreground population reads as crystals at normal
    // size; the rest keep the compact ellipse treatment and the same item count.
    if (p.phase < .31415927f) p.radius = 3 + p.phase * (.4f / .31415927f);
}
void SnowSimulation::setBounds(Rect bounds) {
    if (!safeRect(bounds)) bounds = {};
    bounds_ = bounds;
    if (!bounds_.valid()) { count_ = 0; remainder_ = 0; return; }
    if (!count_) { count_ = targetCount_; for (std::size_t i = 0; i < count_; ++i) spawn(particles_[i], true); }
    else for (std::size_t i = 0; i < count_; ++i) {
        const auto& p = particles_[i];
        if (p.x < static_cast<float>(bounds_.left) || p.x >= static_cast<float>(bounds_.right) || p.y < static_cast<float>(bounds_.top) - 10 || p.y >= static_cast<float>(bounds_.bottom)) spawn(particles_[i], true);
    }
}
void SnowSimulation::setSettings(Settings s) noexcept {
    settings_ = validated(s);
    for (auto& state : accumulation_.surfaces()) state.field.clampDepth(settings_.maxDepth);
    constexpr std::size_t counts[]{300, 800, 1600, 2600};
    targetCount_ = settings_.weather == Weather::Custom ? static_cast<std::size_t>(settings_.particles) : counts[static_cast<std::size_t>(settings_.weather)];
    if (settings_.profile == Profile::Eco) targetCount_ = std::min<std::size_t>(targetCount_, 700);
    pruneDecorations();
}
void SnowSimulation::setPaused(bool value) noexcept { paused_ = value; remainder_ = 0; }
void SnowSimulation::advance(double elapsed) {
    // Lifecycle cleanup also runs on a paused/zero-time model update.
    pruneDecorations();
    if (paused_ || !bounds_.valid() || !std::isfinite(elapsed) || elapsed <= 0) return;
    remainder_ += std::min(elapsed, FixedStep * 8);
    unsigned loops = 0;
    while (remainder_ + 1e-12 >= FixedStep && loops < 8) {
        step(static_cast<float>(FixedStep)); remainder_ -= FixedStep; ++loops;
    }
    if (remainder_ < 0) remainder_ = 0;
}
void SnowSimulation::step(float dt) {
    ++steps_; time_ += dt;
    if (time_ > 1000) time_ -= 1000;
    float targetWind = settings_.wind;
    if (settings_.weather == Weather::Blizzard) targetWind += 100;
    const float blend = std::min(1.f, dt * 2);
    wind_ += (targetWind - wind_) * blend;
    speed_ += (settings_.speed * (settings_.weather == Weather::Blizzard ? 1.7f : 1.f) - speed_) * blend;
    size_ += (settings_.size - size_) * blend;
    densityRemainder_ += dt * 600;
    const auto change = static_cast<std::size_t>(densityRemainder_); densityRemainder_ -= static_cast<float>(change);
    if (count_ < targetCount_) {
        const auto next = std::min(targetCount_, count_ + change);
        while (count_ < next) { spawn(particles_[count_], false); ++count_; }
    } else count_ -= std::min(change, count_ - targetCount_);
    for (std::size_t i = 0; i < count_; ++i) {
        auto& p = particles_[i]; const float oldY = p.y;
        const float gust = std::sin(time_ * .6f + p.phase) * 9;
        p.x += (p.drift + wind_ + gust) * dt; p.y += p.speed * speed_ * dt;
        if (p.x < static_cast<float>(bounds_.left) - 8) p.x = static_cast<float>(bounds_.right) + 7;
        else if (p.x > static_cast<float>(bounds_.right) + 8) p.x = static_cast<float>(bounds_.left) - 7;
        const bool landed = settings_.accumulation && accumulation_.deposit(p.x, oldY, p.y, p.radius * p.radius * 7 * settings_.accumulationRate, settings_.maxDepth);
        if (landed || p.y > static_cast<float>(bounds_.bottom) + 8) spawn(p, false);
    }
    for (auto& decoration : decorations_) decoration.age = std::min(decoration.age + dt, 1.f);
    pruneDecorations();
}
Decoration* SnowSimulation::findDecoration(std::uint64_t id) noexcept {
    for (auto& decoration : decorations_) if (decoration.id == id) return &decoration;
    return nullptr;
}
const Decoration* SnowSimulation::findDecoration(std::uint64_t id) const noexcept {
    for (const auto& decoration : decorations_) if (decoration.id == id) return &decoration;
    return nullptr;
}
const Decoration* SnowSimulation::childOf(std::uint64_t id) const noexcept {
    for (const auto& decoration : decorations_) if (decoration.parentId == id) return &decoration;
    return nullptr;
}
float SnowSimulation::chainHeight(std::uint64_t id) const noexcept {
    float height = 0;
    for (std::size_t count = 0; count < MaximumDecorations; ++count) {
        const auto* decoration = findDecoration(id);
        if (!decoration) return height;
        height += figureHeight(*decoration);
        const auto* child = childOf(id);
        if (!child) return height;
        id = child->id;
    }
    return SnowBand + 1; // A cycle never becomes a drawable or valid placement.
}
float SnowSimulation::decorationRise(const Decoration& decoration, std::size_t depth) const noexcept {
    if (depth >= MaximumDecorations) return SnowBand + 1;
    const auto* state = accumulation_.find(decoration.surfaceId);
    if (!state) return SnowBand + 1;
    if (interaction_.ballId == decoration.id) return decoration.rise;
    if (decoration.parentId != 0) {
        const auto* parent = findDecoration(decoration.parentId);
        return parent ? decorationRise(*parent, depth + 1) + figureHeight(*parent) : SnowBand + 1;
    }
    // A growing drift may bury the base, but never pushes a whole stack through
    // the tracking band's ceiling. All contacts use this same derived position.
    return std::min(state->field.height(decoration.localX), std::max(0.f, SnowBand - chainHeight(decoration.id) - .01f));
}
bool SnowSimulation::decorationVisible(const Decoration& decoration, std::size_t depth) const noexcept {
    if (depth >= MaximumDecorations) return false;
    const auto* state = accumulation_.find(decoration.surfaceId);
    if (!state) return false;
    const float x = static_cast<float>(state->spec.bounds.left) + decoration.localX;
    const float rise = decorationRise(decoration);
    if (!footprintVisible(state->spec, x - decoration.radius, x + decoration.radius) ||
        rise < 0 || rise + figureHeight(decoration) > SnowBand + .001f) return false;
    const auto* parent = findDecoration(decoration.parentId);
    return decoration.parentId == 0 || (parent && decorationVisible(*parent, depth + 1));
}
void SnowSimulation::pruneDecorations() noexcept {
    for (std::size_t pass = 0; pass < MaximumDecorations; ++pass) {
        const auto size = decorations_.size();
        std::erase_if(decorations_, [&](const Decoration& decoration) {
            const auto* state = accumulation_.find(decoration.surfaceId);
            if (!state || !decoration.id || !std::isfinite(decoration.radius) || decoration.radius <= 0 ||
                !std::isfinite(decoration.volume) || decoration.volume <= 0 || !std::isfinite(decoration.localX) ||
                decoration.localX - decoration.radius < 0 ||
                decoration.localX + decoration.radius > static_cast<float>(state->field.width())) return true;
            const Decoration* node = &decoration;
            for (std::size_t depth = 0; node->parentId != 0; ++depth) {
                const auto* parent = findDecoration(node->parentId);
                if (depth >= MaximumStack - 1 || !parent || parent->id == decoration.id ||
                    parent->surfaceId != decoration.surfaceId || parent->kind != Tool::Snowball ||
                    node->kind != Tool::Snowball) return true;
                node = parent;
            }
            return chainHeight(node->id) > SnowBand + .001f;
        });
        if (size == decorations_.size()) break;
    }
    if (interaction_.ballId && !findDecoration(interaction_.ballId)) interaction_ = {};
    for (auto& decoration : decorations_) {
        decoration.rise = decorationRise(decoration);
        decoration.decorated = false;
        if (decoration.kind != Tool::Snowball || decoration.parentId == 0) continue;
        const auto* parent = findDecoration(decoration.parentId);
        if (parent && parent->parentId != 0) decoration.decorated = true;
    }
}
bool SnowSimulation::useTool(Tool tool, float x, float y) {
    pruneDecorations();
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    const float strength = settings_.interaction;
    if (tool == Tool::Mitten || tool == Tool::Shovel)
        return accumulation_.clear(x, y, (tool == Tool::Shovel ? 90 : 35) * strength, 14 * strength) > 0;
    if ((tool != Tool::Snowball && tool != Tool::Snowman) || decorations_.size() >= MaximumDecorations) return false;
    auto* state = accumulation_.hit(x, y, 80);
    if (!state) return false;
    const float local = x - static_cast<float>(state->spec.bounds.left);
    const float top = static_cast<float>(state->spec.bounds.top);
    const float radius = tool == Tool::Snowball ? 11.f : 16.f;
    if (y < top - SnowBand || y > top || !footprintVisible(state->spec, x - radius, x + radius)) return false;
    const float needed = tool == Tool::Snowball ? 400.f : 1800.f;
    const float reach = collectionRadius(state->spec, x, 80);
    if (state->field.available(local, reach) < needed) return false;
    const float taken = state->field.collect(local, reach, needed);
    if (taken <= 0) return false;
    const float actualRadius = tool == Tool::Snowball ? std::sqrt(taken / BallMassScale) : radius;
    decorations_.push_back({state->spec.id, local, actualRadius, 0, tool, nextDecorationId_++, 0, taken, 0, false});
    if (nextDecorationId_ == 0) ++nextDecorationId_;
    pruneDecorations();
    return true;
}
bool SnowSimulation::beginInteraction(Tool tool, float x, float y) {
    pruneDecorations();
    if (!std::isfinite(x) || !std::isfinite(y) || tool == Tool::None ||
        static_cast<unsigned>(tool) > static_cast<unsigned>(Tool::Snowman)) return false;
    const bool supported = std::any_of(accumulation_.surfaces().begin(), accumulation_.surfaces().end(),
        [x, y](const SurfaceState& state) {
            const float top = static_cast<float>(state.spec.bounds.top);
            const float local = x - static_cast<float>(state.spec.bounds.left);
            return local >= 0 && local < static_cast<float>(state.field.width()) && visible(state.spec, x) &&
                y >= top - SnowBand && y <= top;
        });
    if (!supported) return false;
    endInteraction();
    if (tool == Tool::Mitten || tool == Tool::Shovel) {
        if (!accumulation_.hit(x, y, 80)) return false;
        interaction_.tool = tool;
        useTool(tool, x, y);
        return true;
    }
    if (tool != Tool::Snowball) return useTool(tool, x, y);
    Decoration* selected = nullptr;
    float closest = std::numeric_limits<float>::max();
    bool blocked{};
    for (auto& decoration : decorations_) {
        if (decoration.kind != Tool::Snowball || !decorationVisible(decoration)) continue;
        const auto* state = accumulation_.find(decoration.surfaceId);
        if (!state) continue;
        const float centerX = static_cast<float>(state->spec.bounds.left) + decoration.localX;
        const float centerY = static_cast<float>(state->spec.bounds.top) - decoration.rise - decoration.radius;
        const float dx = x - centerX, dy = y - centerY;
        const float distance = dx * dx + dy * dy;
        const float reach = decoration.radius + 4;
        if (distance > reach * reach || distance > closest) continue;
        closest = distance;
        blocked = childOf(decoration.id) != nullptr;
        selected = &decoration;
    }
    if (selected && blocked) return false;
    if (!selected) {
        if (!useTool(tool, x, y)) return false;
        selected = &decorations_.back();
    }
    const auto* state = accumulation_.find(selected->surfaceId);
    if (!state) return false;
    const float top = static_cast<float>(state->spec.bounds.top);
    if (y < top - SnowBand || y > top) return false;
    const float bottom = top - selected->rise;
    selected->parentId = 0;
    interaction_ = {tool, selected->id, static_cast<float>(state->spec.bounds.left) + selected->localX - x, bottom - y};
    pruneDecorations();
    return true;
}
bool SnowSimulation::dragInteraction(float x, float y) {
    pruneDecorations();
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (interaction_.tool == Tool::Mitten || interaction_.tool == Tool::Shovel)
        return useTool(interaction_.tool, x, y);
    auto* decoration = findDecoration(interaction_.ballId);
    if (interaction_.tool != Tool::Snowball || !decoration || !decorationVisible(*decoration)) return false;
    SurfaceState* state = nullptr;
    for (auto& surface : accumulation_.surfaces()) if (surface.spec.id == decoration->surfaceId) { state = &surface; break; }
    if (!state) return false;
    const float top = static_cast<float>(state->spec.bounds.top);
    if (y < top - SnowBand || y > top) return false;
    const float worldX = x + interaction_.offsetX;
    const float local = worldX - static_cast<float>(state->spec.bounds.left);
    const float distance = std::abs(local - decoration->localX);
    if (distance > MaximumDragDistance) return false;
    const float oldWorld = static_cast<float>(state->spec.bounds.left) + decoration->localX;
    const float low = std::min(oldWorld, worldX), high = std::max(oldWorld, worldX);
    const Span* path = nullptr;
    for (const auto& span : state->spec.visible) {
        if (low - decoration->radius >= static_cast<float>(span.left) &&
            high + decoration->radius <= static_cast<float>(span.right)) { path = &span; break; }
    }
    if (!path || !footprintVisible(state->spec, low - decoration->radius, high + decoration->radius)) return false;
    const float requestedRise = std::max(0.f, top - (y + interaction_.bottomOffset));
    const float radiusLimit = std::min({MaximumBallRadius, low - static_cast<float>(path->left),
        static_cast<float>(path->right) - high, (SnowBand - requestedRise) * .5f});
    if (radiusLimit + .001f < decoration->radius) return false;
    const float ground = std::min(state->field.height(local), SnowBand - decoration->radius * 2);
    const bool rolling = requestedRise <= ground + 2;
    if (rolling && distance > 0) {
        const float room = std::max(0.f, radiusLimit * radiusLimit * BallMassScale - decoration->volume);
        float remaining = std::min(room, distance * 2);
        const auto samples = static_cast<unsigned>(std::clamp(std::ceil(distance / static_cast<float>(CellWidth)), 1.f, 32.f));
        const float portion = remaining / static_cast<float>(samples);
        for (unsigned sample = 1; sample <= samples && remaining > 0; ++sample) {
            const float at = decoration->localX + (local - decoration->localX) *
                static_cast<float>(sample) / static_cast<float>(samples);
            const float physical = static_cast<float>(state->spec.bounds.left) + at;
            const float reach = collectionRadius(state->spec, physical, decoration->radius);
            const float taken = state->field.collect(at, reach, std::min(remaining, portion));
            decoration->volume += taken; remaining -= taken;
        }
        decoration->radius = std::sqrt(decoration->volume / BallMassScale);
    }
    decoration->localX = local;
    decoration->rise = std::max(requestedRise, std::min(state->field.height(local), SnowBand - decoration->radius * 2));
    return true;
}
void SnowSimulation::endInteraction() noexcept {
    pruneDecorations();
    auto* moving = findDecoration(interaction_.ballId);
    if (moving) {
        Decoration* support = nullptr;
        float closest = std::numeric_limits<float>::max();
        for (auto& candidate : decorations_) {
            if (candidate.id == moving->id || candidate.surfaceId != moving->surfaceId ||
                candidate.kind != Tool::Snowball || childOf(candidate.id) ||
                moving->radius > candidate.radius + .001f || !decorationVisible(candidate)) continue;
            bool cycle{};
            std::size_t depth = 1;
            const Decoration* ancestor = &candidate;
            for (; ancestor; ancestor = findDecoration(ancestor->parentId)) {
                if (ancestor->id == moving->id || depth++ >= MaximumStack) { cycle = true; break; }
                if (ancestor->parentId == 0) break;
            }
            if (cycle) continue;
            const float target = decorationRise(candidate) + candidate.radius * 2;
            const float dx = std::abs(moving->localX - candidate.localX), dy = std::abs(moving->rise - target);
            if (dx > std::max(6.f, moving->radius * .6f) || dy > 12 || target + moving->radius * 2 > SnowBand) continue;
            const auto* state = accumulation_.find(moving->surfaceId);
            if (!state) continue;
            const float left = static_cast<float>(state->spec.bounds.left);
            if (!footprintVisible(state->spec, left + std::min(moving->localX, candidate.localX) - moving->radius,
                    left + std::max(moving->localX, candidate.localX) + moving->radius)) continue;
            const float score = dx * dx + dy * dy;
            if (score < closest) { closest = score; support = &candidate; }
        }
        if (support) { moving->parentId = support->id; moving->localX = support->localX; }
    }
    interaction_ = {};
    pruneDecorations();
}
void SnowSimulation::clearSnow() noexcept {
    interaction_ = {};
    decorations_.clear();
    for (auto& state : accumulation_.surfaces()) state.field.clampDepth(0);
}
void SnowSimulation::buildScene(std::vector<DrawItem>& scene, bool interactive) const {
    scene.clear(); if (!bounds_.valid()) return;
    const std::size_t completeBudget = MaxDrawItems - (interactive ? 1 : 0);
    std::size_t figureBudget = 0;
    for (const auto& decoration : decorations_) if (decorationVisible(decoration))
        figureBudget += decoration.kind == Tool::Snowman ? 7 : decoration.decorated ? 4 : 1;
    // Ledges may be dense on a large desktop. Reserve the known finite output
    // for weather and visible figures before using the remaining height budget.
    std::size_t budget = completeBudget - count_ - figureBudget;
    const auto add = [&](DrawItem item) { if (scene.size() < budget) scene.push_back(item); };
    if (settings_.accumulation) for (const auto& s : accumulation_.surfaces()) {
        const auto cells = s.field.cells();
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const float x = static_cast<float>(s.spec.bounds.left) + static_cast<float>(i * CellWidth);
            const float h = cells[i]; if (h < .15f) continue;
            const float right = std::min(x + static_cast<float>(CellWidth), static_cast<float>(s.spec.bounds.right));
            for (const auto span : s.spec.visible) {
                const float left = std::max(x, static_cast<float>(span.left));
                const float end = std::min(right, static_cast<float>(span.right));
                if (end <= left) continue;
                add({left, static_cast<float>(s.spec.bounds.top) - h, end - left, h, .91f, .95f, 1, .94f, 0});
                // A soft bright crest makes coarse height bins read as snow rather than columns.
                const float top = static_cast<float>(s.spec.bounds.top);
                const float crest = std::max(top - 96, top - h - 2);
                add({left, crest, end - left, std::min(5.f, top - crest), .98f, .99f, 1, .9f, 1});
            }
        }
    }
    budget = completeBudget;
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& p = particles_[i]; const float r = p.radius * size_;
        const float shape = r * 2 >= 6 && p.phase < 1.5707963f ? 2.f : 1.f;
        add({p.x - r, p.y - r, r * 2, r * 2, .96f, .98f, 1, std::min(1.f, .45f + p.radius * .18f), shape});
    }
    for (const auto& d : decorations_) {
        const auto* s = accumulation_.find(d.surfaceId); if (!s) continue;
        if (!decorationVisible(d)) continue;
        const float x = static_cast<float>(s->spec.bounds.left) + d.localX;
        const float rise = decorationRise(d);
        const float y = static_cast<float>(s->spec.bounds.top) - rise;
        const float bounce = d.kind == Tool::Snowman && d.age < .5f ?
            std::min(std::max(0.f, SnowBand - rise - figureHeight(d)), std::sin(d.age * 6.2831853f) * 8 * (1 - d.age * 2)) : 0;
        const float radius = d.radius;
        auto circle = [&](float cx, float cy, float r, float red, float green, float blue) { add({cx - r, cy - r - bounce, r * 2, r * 2, red, green, blue, 1, 1}); };
        circle(x, y - radius, radius, .92f, .96f, 1);
        if (d.kind == Tool::Snowman) {
            circle(x, y - radius * 2.5f, radius * .72f, .96f, .98f, 1);
            circle(x, y - radius * 3.65f, radius * .52f, 1, 1, 1);
            circle(x - 3, y - radius * 3.8f, 1.2f, .1f, .15f, .2f);
            circle(x + 3, y - radius * 3.8f, 1.2f, .1f, .15f, .2f);
            add({x, y - radius * 3.55f - bounce, 8, 3, 1, .4f, .12f, 1, 0});
            add({x - 10, y - radius * 3.05f - bounce, 20, 4, .65f, .1f, .16f, 1, 0});
        } else if (d.decorated) {
            const float center = y - radius;
            circle(x - radius * .3f, center - radius * .2f, 1.2f, .1f, .15f, .2f);
            circle(x + radius * .3f, center - radius * .2f, 1.2f, .1f, .15f, .2f);
            add({x, center, std::min(8.f, radius), 3, 1, .4f, .12f, 1, 0});
        }
    }
    if (interactive) scene.push_back({static_cast<float>(bounds_.left), static_cast<float>(bounds_.top),
        static_cast<float>(bounds_.width()), static_cast<float>(bounds_.height()), 0, 0, 0, 1.f / 255.f, 0});
}
}
