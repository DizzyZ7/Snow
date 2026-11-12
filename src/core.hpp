#pragma once
#include "scene.hpp"
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace snow {
enum class Weather { Light, Normal, Heavy, Blizzard, Custom };
enum class Profile { Eco, Balanced, Quality };
enum class Tool { None, Mitten, Shovel, Snowball, Snowman };
struct Settings {
    Weather weather{Weather::Normal};
    Profile profile{Profile::Balanced};
    int particles{800};
    float speed{1}, wind{0}, size{1}, accumulationRate{1}, maxDepth{48}, interaction{1};
    bool accumulation{true};
};
Settings validated(Settings value) noexcept;
std::string serializeSettings(Settings value);
std::optional<Settings> parseSettings(std::string_view text);

class HeightField {
public:
    explicit HeightField(int width = 0) { resize(width); }
    void resize(int width);
    void clampDepth(float maximum) noexcept;
    float deposit(float x, float volume, float maxDepth) noexcept;
    float clear(float x, float radius, float strength) noexcept;
    float collect(float x, float radius, float amount) noexcept;
    [[nodiscard]] float available(float x, float radius) const noexcept;
    [[nodiscard]] float height(float x) const noexcept;
    [[nodiscard]] float volume() const noexcept;
    [[nodiscard]] std::span<const float> cells() const noexcept { return heights_; }
    [[nodiscard]] int width() const noexcept { return width_; }
private:
    int width_{};
    std::vector<float> heights_;
};
struct SurfaceState {
    SurfaceSpec spec;
    HeightField field;
};
class SnowAccumulation {
public:
    SnowAccumulation();
    void reconcile(std::span<const SurfaceSpec> snapshot);
    bool deposit(float x, float oldY, float newY, float volume, float maxDepth) noexcept;
    float clear(float x, float y, float radius, float strength) noexcept;
    [[nodiscard]] std::span<const SurfaceState> surfaces() const noexcept { return surfaces_; }
    [[nodiscard]] std::span<SurfaceState> surfaces() noexcept { return surfaces_; }
    [[nodiscard]] float totalVolume() const noexcept;
    [[nodiscard]] SurfaceState* hit(float x, float y, float radius) noexcept;
    [[nodiscard]] const SurfaceState* find(std::uint64_t id) const noexcept;
private:
    std::vector<SurfaceState> surfaces_;
    std::array<std::size_t, MaxSurfaces> sorted_{};
};
struct Particle { float x{}, y{}, speed{}, drift{}, radius{}, phase{}; };
struct Decoration {
    std::uint64_t surfaceId{};
    float localX{}, radius{}, age{};
    Tool kind{Tool::Snowball};
    std::uint64_t id{}, parentId{};
    float volume{}, rise{}; // conserved snow volume; bottom above the owner's edge
    bool decorated{}; // a face on the head of three actual stacked balls
};
class SnowSimulation {
public:
    explicit SnowSimulation(std::uint32_t seed = 0x534e4f57U);
    void setBounds(Rect bounds);
    void setSettings(Settings settings) noexcept;
    void setPaused(bool paused) noexcept;
    void advance(double elapsedSeconds);
    bool useTool(Tool tool, float x, float y);
    bool beginInteraction(Tool tool, float x, float y);
    bool dragInteraction(float x, float y);
    void endInteraction() noexcept;
    void clearSnow() noexcept;
    void buildScene(std::vector<DrawItem>& scene, bool interactive = false) const;
    [[nodiscard]] std::span<const Particle> particles() const noexcept { return {particles_.data(), count_}; }
    [[nodiscard]] std::span<const Decoration> decorations() const noexcept { return decorations_; }
    [[nodiscard]] SnowAccumulation& accumulation() noexcept { return accumulation_; }
    [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }
private:
    float random() noexcept;
    void spawn(Particle& particle, bool initial) noexcept;
    void step(float dt);
    void pruneDecorations() noexcept;
    [[nodiscard]] Decoration* findDecoration(std::uint64_t id) noexcept;
    [[nodiscard]] const Decoration* findDecoration(std::uint64_t id) const noexcept;
    [[nodiscard]] const Decoration* childOf(std::uint64_t id) const noexcept;
    [[nodiscard]] float chainHeight(std::uint64_t id) const noexcept;
    [[nodiscard]] float decorationRise(const Decoration& decoration, std::size_t depth = 0) const noexcept;
    [[nodiscard]] bool decorationVisible(const Decoration& decoration, std::size_t depth = 0) const noexcept;
    Rect bounds_{};
    Settings settings_{};
    std::array<Particle, MaxParticles> particles_{};
    std::size_t count_{}, targetCount_{800};
    SnowAccumulation accumulation_;
    std::vector<Decoration> decorations_;
    struct Interaction {
        Tool tool{Tool::None};
        std::uint64_t ballId{};
        float offsetX{}, bottomOffset{};
    } interaction_;
    std::uint64_t nextDecorationId_{1};
    std::uint32_t rng_;
    double remainder_{};
    std::uint64_t steps_{};
    float wind_{}, speed_{1}, size_{1}, time_{}, densityRemainder_{};
    bool paused_{};
};
}
