#include "core.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>

namespace snow {
namespace {
constexpr double FixedStep = 1.0 / 120.0;
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
            surfaces_.push_back({spec, HeightField(spec.bounds.width())});
            state = &surfaces_.back();
        } else { it->field.resize(spec.bounds.width()); it->spec = spec; state = &*it; }
        if (state->spec.visible.size() > 32) state->spec.visible.resize(32);
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
SnowSimulation::SnowSimulation(std::uint32_t seed) : rng_(seed ? seed : 1) { decorations_.reserve(32); }
float SnowSimulation::random() noexcept {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.f / 16777216.f);
}
void SnowSimulation::spawn(Particle& p, bool initial) noexcept {
    p.x = static_cast<float>(bounds_.left) + random() * static_cast<float>(bounds_.width());
    p.y = initial ? static_cast<float>(bounds_.top) + random() * static_cast<float>(bounds_.height()) : static_cast<float>(bounds_.top) - 8;
    p.speed = 24 + random() * 65; p.drift = (random() - .5f) * 30;
    p.radius = .8f + random() * 1.9f; p.phase = random() * 6.2831853f;
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
    constexpr std::size_t counts[]{300, 800, 1600, 2600};
    targetCount_ = settings_.weather == Weather::Custom ? static_cast<std::size_t>(settings_.particles) : counts[static_cast<std::size_t>(settings_.weather)];
    if (settings_.profile == Profile::Eco) targetCount_ = std::min<std::size_t>(targetCount_, 700);
}
void SnowSimulation::setPaused(bool value) noexcept { paused_ = value; remainder_ = 0; }
void SnowSimulation::advance(double elapsed) {
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
    std::erase_if(decorations_, [&](Decoration& d) {
        d.age = std::min(d.age + dt, 1.f);
        const auto* state = accumulation_.find(d.surfaceId);
        return !state || d.localX < 0 || d.localX >= static_cast<float>(state->field.width());
    });
}
bool SnowSimulation::useTool(Tool tool, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    const float strength = settings_.interaction;
    if (tool == Tool::Mitten || tool == Tool::Shovel)
        return accumulation_.clear(x, y, (tool == Tool::Shovel ? 90 : 35) * strength, 14 * strength) > 0;
    if ((tool != Tool::Snowball && tool != Tool::Snowman) || decorations_.size() >= 32) return false;
    auto* state = accumulation_.hit(x, y, 80);
    if (!state) return false;
    const float local = x - static_cast<float>(state->spec.bounds.left);
    const float needed = tool == Tool::Snowball ? 400.f : 1800.f;
    if (state->field.available(local, 80) + .001f < needed) return false;
    state->field.collect(local, 80, needed);
    decorations_.push_back({state->spec.id, local, tool == Tool::Snowball ? 11.f : 16.f, 0, tool});
    return true;
}
void SnowSimulation::buildScene(std::vector<DrawItem>& scene, bool interactive) const {
    scene.clear(); if (!bounds_.valid()) return;
    const std::size_t budget = MaxDrawItems - (interactive ? 1 : 0);
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
    for (std::size_t i = 0; i < count_; ++i) {
        const auto& p = particles_[i]; const float r = p.radius * size_;
        add({p.x - r, p.y - r, r * 2, r * 2, .96f, .98f, 1, .45f + p.radius * .18f, 1});
    }
    for (const auto& d : decorations_) {
        const auto* s = accumulation_.find(d.surfaceId); if (!s) continue;
        const float x = static_cast<float>(s->spec.bounds.left) + d.localX;
        const bool fits = std::any_of(s->spec.visible.begin(), s->spec.visible.end(), [&](Span span) {
            return x - d.radius >= static_cast<float>(span.left) && x + d.radius <= static_cast<float>(span.right);
        });
        if (!fits) continue;
        // Keep figures inside the same 96px band used by occlusion tracking.
        // Deep drifts partly bury their base rather than letting them cross a higher window.
        const float figureHeight = d.radius * (d.kind == Tool::Snowman ? 4.17f : 2.f);
        const float rise = std::min(s->field.height(d.localX), std::max(0.f, 96 - figureHeight - 8));
        const float y = static_cast<float>(s->spec.bounds.top) - rise;
        const float bounce = d.age < .5f ? std::sin(d.age * 6.2831853f) * 8 * (1 - d.age * 2) : 0;
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
        }
    }
    if (interactive) scene.push_back({static_cast<float>(bounds_.left), static_cast<float>(bounds_.top),
        static_cast<float>(bounds_.width()), static_cast<float>(bounds_.height()), 0, 0, 0, 1.f / 255.f, 0});
}
}
