#include "core.hpp"
#include "performance_monitor.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
void check(bool value, const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
snow::SurfaceSpec surface(std::uint64_t id = 1, int left = -200, int top = 400, int width = 400) {
    return {id, {left, top, left + width, top + 300}, false, {{left, left + width}}};
}
void settingsTests() {
    snow::Settings a;
    a.weather = snow::Weather::Custom; a.particles = 1234; a.wind = -71.5f;
    a.profile = snow::Profile::Eco; a.accumulation = false;
    const auto result = snow::parseSettings(snow::serializeSettings(a));
    check(result.has_value(), "settings roundtrip parses");
    check(result->particles == 1234 && result->wind == -71.5f && !result->accumulation,
          "settings roundtrip retains values");
    check(!snow::parseSettings("version=99\nparticles=20\n"), "future config rejected without overwrite");
    check(!snow::parseSettings("version=1\nwind=nan\n"), "NaN rejected");
    check(!snow::parseSettings("version=1\nparticles=99999999\n"), "unbounded count rejected");
    check(!snow::parseSettings("version=1\nwind=1e50\n"), "infinity rejected");
    check(!snow::parseSettings("version=1\nwind=5junk\n"), "numeric suffix rejected");
    check(!snow::parseSettings("version=1\naccumulation=2\n"), "bool validated");
    check(!snow::parseSettings("wind=2\n"), "version required");
    check(!snow::parseSettings(std::string(9000, 'x')), "oversized file rejected");
    check(snow::parseSettings("version=1\nunknown=future\n").has_value(), "unknown keys ignored");
}
void heightTests() {
    snow::HeightField field(400);
    check(field.cells().size() == 50, "height field proportional to width");
    check(field.deposit(120, 400, 48) > 0, "snow deposited");
    const float original = field.volume();
    check(original > 399 && original < 401, "deposit conserves volume");
    check(field.height(120) > 0, "deposit raises height");
    const float removed = field.clear(120, 32, 100);
    check(removed > 0 && std::abs(original - field.volume() - removed) < .01f, "clear removes model mass");
    field.deposit(20, 500, 48);
    const float before = field.height(20);
    field.resize(800);
    check(field.height(20) == before, "resize preserves local snow");
    field.resize(16);
    check(field.cells().size() == 2, "shrunk storage resized");
    field.resize(1000000);
    check(field.cells().size() <= snow::MaxSurfaceCells, "width storage bounded");
    check(field.deposit(-5, 50, 48) == 0, "outside deposit rejected");
    check(field.deposit(5, -50, 48) == 0, "negative snow rejected");
    check(field.clear(5, -10, 10) == 0, "negative radius rejected");
    check(field.height(std::numeric_limits<float>::quiet_NaN()) == 0, "invalid coordinate rejected");
    field.deposit(1, 1e9f, 20);
    for (float h : field.cells()) check(h >= 0 && h <= 20, "height bounded");
}
void accumulationTests() {
    snow::SnowAccumulation model;
    auto s = surface(); model.reconcile(std::span(&s, 1));
    check(model.deposit(-100, 390, 450, 80, 48), "crossed edge deposits");
    const float old = model.totalVolume();
    s.bounds.left += 300; s.bounds.right += 300; s.bounds.top += 20;
    s.visible = {{s.bounds.left, s.bounds.right}}; model.reconcile(std::span(&s, 1));
    check(model.totalVolume() == old, "movement retains accumulation");
    check(model.clear(-100, 400, 40, 99) == 0, "moved surface clears at new coordinate only");
    check(model.clear(200, 420, 40, 99) > 0, "moved snow uses physical origin");
    s.visible.clear(); model.reconcile(std::span(&s, 1));
    check(!model.deposit(200, 390, 450, 80, 48), "hidden edge receives no snow");
    s.id = 2; s.visible = {{s.bounds.left, s.bounds.right}}; model.reconcile(std::span(&s, 1));
    check(model.totalVolume() == 0, "new generation never inherits HWND snow");
    model.reconcile({}); check(model.surfaces().empty(), "closed window frees surface");
    auto a = surface(1, 0, 100); auto b = surface(2, 0, 150);
    const snow::SurfaceSpec pair[]{b, a}; model.reconcile(pair);
    check(model.deposit(100, 50, 200, 80, 48), "crossing deposits once");
    check(model.surfaces()[0].field.volume() == 0, "nearest edge wins independent of iteration");
    check(model.surfaces()[1].field.volume() > 0, "upper edge gets snowfall");
    std::vector<snow::SurfaceSpec> many;
    for (std::uint64_t i = 0; i < 1000; ++i) many.push_back(surface(i + 1));
    model.reconcile(many); check(model.surfaces().size() <= snow::MaxSurfaces, "surface count bounded");
}
void simulationTests() {
    snow::SnowSimulation a(42), b(42);
    a.setBounds({-1920, -200, 1920, 1080}); b.setBounds({-1920, -200, 1920, 1080});
    snow::Settings cfg; cfg.weather = snow::Weather::Custom; cfg.particles = 200;
    a.setSettings(cfg); b.setSettings(cfg);
    for (int i = 0; i < 30; ++i) a.advance(1.0 / 30.0);
    for (int i = 0; i < 60; ++i) b.advance(1.0 / 60.0);
    check(a.particles().size() == b.particles().size(), "frame rate independent count");
    check(a.particles()[0].x == b.particles()[0].x && a.particles()[0].y == b.particles()[0].y,
          "fixed step deterministic across frame rates");
    const auto p = a.particles()[0];
    a.setPaused(true); a.advance(8 * 3600); a.setPaused(false);
    check(a.particles()[0].x == p.x && a.particles()[0].y == p.y, "pause discards elapsed time");
    const auto ticks = a.steps(); a.advance(1000);
    check(a.steps() - ticks <= 8, "catch-up bounded");
    cfg.particles = 1000000; cfg.wind = std::numeric_limits<float>::infinity();
    a.setSettings(cfg); a.advance(0.016);
    check(a.particles().size() <= snow::MaxParticles, "particle pool bounded");
    for (const auto& f : a.particles()) check(std::isfinite(f.x) && std::isfinite(f.y), "invalid settings sanitized");
    std::vector<snow::DrawItem> scene; scene.reserve(snow::MaxDrawItems);
    a.buildScene(scene); const auto capacity = scene.capacity();
    for (int i = 0; i < 30; ++i) { a.advance(1.0 / 30.0); a.buildScene(scene); }
    check(scene.capacity() == capacity && scene.size() <= snow::MaxDrawItems, "scene capacity stable");
    check(sizeof(snow::Particle) <= 32, "compact particle storage");
    a.setBounds({0, 0, 0, 0}); a.advance(.01); a.buildScene(scene);
    check(scene.empty(), "empty desktop has no scene");
}
void sceneClipTests() {
    snow::SnowSimulation sim(11); sim.setBounds({0, 0, 1000, 800});
    snow::Settings cfg; cfg.weather = snow::Weather::Custom; cfg.particles = 0; cfg.maxDepth = 96;
    sim.setSettings(cfg);
    for (int i = 0; i < 200; ++i) sim.advance(1.0 / 60.0);
    auto s = surface(7, 0, 600, 1000); s.visible = {{100, 200}};
    sim.accumulation().reconcile(std::span(&s, 1));
    for (int i = 0; i < 50; ++i) sim.accumulation().deposit(150, 500, 610, 1000, 96);
    std::vector<snow::DrawItem> scene; scene.reserve(snow::MaxDrawItems); sim.buildScene(scene);
    for (const auto& item : scene) {
        check(item.x >= 100 && item.x + item.w <= 200, "snow crest stays inside visible interval");
        check(item.y >= 504, "snow stays inside tracked 96px band");
    }
    check(sim.useTool(snow::Tool::Snowman, 150, 600), "snowman consumes deep snow");
    sim.buildScene(scene);
    for (const auto& item : scene) check(item.y >= 504, "snowman stays inside tracked band");
    s.visible = {{148, 152}}; sim.accumulation().reconcile(std::span(&s, 1)); sim.buildScene(scene);
    for (const auto& item : scene) check(item.x >= 148 && item.x + item.w <= 152, "partly occluded snowman is hidden");
}
void performanceTests() {
    snow::PerformanceMonitor monitor;
    check(monitor.interval(snow::Profile::Balanced, false, false, 0) == 33, "balanced is the default 30 FPS budget");
    check(monitor.interval(snow::Profile::Quality, true, false, 0) == 67, "battery reduces frame rate");
    check(monitor.interval(snow::Profile::Quality, false, true, 0) == 67, "energy saver reduces frame rate");
    check(monitor.interval(snow::Profile::Quality, false, false, .95) >= 33, "high system load reduces frame rate");
    for (int i = 0; i < 80; ++i) monitor.recordFrame(.04);
    check(monitor.interval(snow::Profile::Quality, false, false, 0) == 67, "expensive frames lower budget");
    for (int i = 0; i < 1000; ++i) monitor.recordFrame(.001);
    check(monitor.interval(snow::Profile::Quality, false, false, 0) == 16, "sustained cheap frames recover with hysteresis");
}
void toolsTests() {
    snow::SnowSimulation sim(11); sim.setBounds({0, 0, 1000, 800});
    auto s = surface(7, 0, 600, 1000); sim.accumulation().reconcile(std::span(&s, 1));
    check(!sim.useTool(snow::Tool::Snowball, 100, 600), "cannot create snow from nothing");
    for (int i = 0; i < 20; ++i) sim.accumulation().deposit(100, 590, 610, 1000, 48);
    const float old = sim.accumulation().totalVolume();
    check(sim.useTool(snow::Tool::Snowball, 100, 600), "snowball made from model snow");
    check(sim.accumulation().totalVolume() < old && !sim.decorations().empty(), "snowball consumes snow");
    const auto count = sim.decorations().size();
    sim.accumulation().reconcile({}); sim.advance(.01);
    check(count > 0 && sim.decorations().empty(), "decorations removed with supporting window");
}
}
int main() {
    try { settingsTests(); heightTests(); accumulationTests(); simulationTests(); sceneClipTests(); performanceTests(); toolsTests(); }
    catch (const std::exception& e) { std::cerr << "FAIL after " << checks << ": " << e.what() << '\n'; return 1; }
    std::cout << "PASS: " << checks << " assertions (settings, height fields, accumulation, simulation, tools)\n";
}
