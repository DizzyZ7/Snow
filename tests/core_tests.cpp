#include "core.hpp"
#include "performance_monitor.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
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
    auto aStorage = std::make_unique<snow::SnowSimulation>(42);
    auto bStorage = std::make_unique<snow::SnowSimulation>(42);
    auto& a = *aStorage; auto& b = *bStorage;
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
    auto storage = std::make_unique<snow::SnowSimulation>(11); auto& sim = *storage;
    sim.setBounds({0, 0, 1000, 800});
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
void saturatedInputTests() {
    auto storage = std::make_unique<snow::SnowSimulation>(17); auto& sim = *storage;
    sim.setBounds({0, 0, 7680, 4320});
    std::vector<snow::SurfaceSpec> surfaces;
    for (std::uint64_t i = 0; i < 21; ++i) surfaces.push_back(surface(i + 1, 0, 120 + static_cast<int>(i) * 200, 7680));
    sim.accumulation().reconcile(surfaces);
    for (auto& state : sim.accumulation().surfaces())
        for (int x = 0; x < 7680; x += 24) state.field.deposit(static_cast<float>(x), 400, 48);
    check(sim.useTool(snow::Tool::Snowball, 200, 120), "dense scene creates a real supported figure");
    std::vector<snow::DrawItem> scene; scene.reserve(snow::MaxDrawItems);
    sim.buildScene(scene); check(scene.size() == snow::MaxDrawItems, "dense 8K scene reaches cap");
    bool flake = false, figure = false;
    for (const auto& item : scene) {
        if (item.r == .96f && item.g == .98f && item.shape >= 1) flake = true;
        if (item.r == .92f && item.g == .96f && item.shape == 1) figure = true;
    }
    check(flake && figure, "dense ledges reserve scene capacity for snowfall and figures");
    sim.buildScene(scene, true);
    check(scene.size() == snow::MaxDrawItems, "interactive scene respects same cap");
    const auto& plane = scene.back();
    check(plane.x == 0 && plane.y == 0 && plane.w == 7680 && plane.h == 4320 && plane.a > 0,
          "input plane always present in saturated scene");
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
    auto storage = std::make_unique<snow::SnowSimulation>(11); auto& sim = *storage;
    sim.setBounds({0, 0, 1000, 800});
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
void depthAndSnapshotTests() {
    auto storage = std::make_unique<snow::SnowSimulation>(19); auto& sim = *storage;
    sim.setBounds({-400, -100, 1000, 800});
    auto s = surface(11, -200, 500, 800); sim.accumulation().reconcile(std::span(&s, 1));
    for (auto& state : sim.accumulation().surfaces())
        for (int x = 4; x < state.field.width(); x += snow::CellWidth)
            state.field.deposit(static_cast<float>(x), 10000, 80);
    snow::Settings cfg; cfg.maxDepth = 12; sim.setSettings(cfg);
    for (const auto& state : sim.accumulation().surfaces())
        for (float height : state.field.cells()) check(height <= 12, "lower depth setting immediately clamps every existing bin");
    s.visible.assign(200000, {-180, 200});
    sim.accumulation().reconcile(std::span(&s, 1));
    const auto& retained = sim.accumulation().surfaces().front().spec.visible;
    check(retained.size() == 32 && retained.capacity() <= 32, "snapshot copies only bounded visible intervals");
    s.visible.assign(200000, {-100, 300}); sim.accumulation().reconcile(std::span(&s, 1));
    check(sim.accumulation().surfaces().front().spec.visible.capacity() <= 32, "updated snapshot never copies oversized interval storage");
}
void fillSurface(snow::SnowSimulation& sim, float depth) {
    for (auto& state : sim.accumulation().surfaces())
        for (int x = 4; x < state.field.width(); x += snow::CellWidth)
            state.field.deposit(static_cast<float>(x), 100000, depth);
}
std::unique_ptr<snow::SnowSimulation> interactiveSnow(float depth = 24) {
    auto sim = std::make_unique<snow::SnowSimulation>(23);
    snow::Settings cfg; cfg.weather = snow::Weather::Custom; cfg.particles = 0; cfg.maxDepth = 96;
    sim->setSettings(cfg); sim->setBounds({-400, -100, 1400, 900});
    auto spec = surface(41, 0, 600, 1000); sim->accumulation().reconcile(std::span(&spec, 1));
    fillSurface(*sim, depth);
    return sim;
}
float snowMass(const snow::SnowSimulation& sim) {
    float mass = 0;
    // accumulation() is intentionally mutable-only; figure mass is independently
    // added to the real field volume by each caller below.
    for (const auto& decoration : sim.decorations()) mass += decoration.volume;
    return mass;
}
const snow::Decoration& ball(const snow::SnowSimulation& sim, std::uint64_t id) {
    for (const auto& decoration : sim.decorations()) if (decoration.id == id) return decoration;
    throw std::runtime_error("expected existing decoration identity");
}
void interactionTests() {
    auto storage = interactiveSnow(0); auto& sim = *storage;
    check(!sim.beginInteraction(snow::Tool::Snowball, 100, 600), "interaction never creates free snow");
    fillSurface(sim, 24);
    const float initialMass = sim.accumulation().totalVolume();
    check(sim.beginInteraction(snow::Tool::Snowball, 100, 600), "mouse down forms a ball from field snow");
    check(sim.decorations().size() == 1 && sim.decorations()[0].volume > 399.99f,
          "new ball stores the snow volume that was removed");
    const std::uint64_t id = sim.decorations()[0].id;
    check(id != 0, "balls have stable identities independent of vector positions");
    check(std::abs(sim.accumulation().totalVolume() + snowMass(sim) - initialMass) < .02f,
          "ball creation conserves field plus figure mass");
    check(sim.dragInteraction(148, 600), "drag rolls along the supported edge");
    check(ball(sim, id).localX == 148 && ball(sim, id).radius > 11 && ball(sim, id).radius <= 18,
          "rolling follows input and grows a bounded radius");
    check(std::abs(sim.accumulation().totalVolume() + snowMass(sim) - initialMass) < .02f,
          "rolling growth comes entirely from swept field snow");
    const float beforeLift = ball(sim, id).rise;
    check(sim.dragInteraction(148, 570) && ball(sim, id).rise > beforeLift, "a ball can be lifted inside the tracked band");
    const float liftedMass = snowMass(sim);
    check(sim.dragInteraction(170, 570) && snowMass(sim) == liftedMass, "lifted motion does not vacuum ground snow");
    const float goodX = ball(sim, id).localX;
    const float goodRise = ball(sim, id).rise;
    check(!sim.beginInteraction(snow::Tool::Snowball, std::numeric_limits<float>::quiet_NaN(), 570) &&
          ball(sim, id).rise == goodRise && sim.dragInteraction(170, 570),
          "invalid new interaction does not release the current lifted ball");
    check(!sim.beginInteraction(snow::Tool::Snowball, -20, 570) &&
          ball(sim, id).rise == goodRise && sim.dragInteraction(170, 570),
          "outside-owner mouse down preserves an existing valid drag");
    check(!sim.dragInteraction(600, 570), "one-event teleport beyond the bounded sweep is rejected");
    check(!sim.dragInteraction(170, 490), "lifting beyond the tracked 96px band is rejected");
    check(!sim.dragInteraction(std::numeric_limits<float>::quiet_NaN(), 570), "NaN drag is rejected");
    check(!sim.dragInteraction(170, std::numeric_limits<float>::infinity()), "infinite drag is rejected");
    check(ball(sim, id).localX == goodX && snowMass(sim) == liftedMass, "rejected drag changes neither placement nor mass");
    sim.endInteraction();
    check(ball(sim, id).rise <= 24, "release without support settles the ball onto its owner");
    const auto count = sim.decorations().size();
    const float centerY = 600 - ball(sim, id).rise - ball(sim, id).radius;
    check(sim.beginInteraction(snow::Tool::Snowball, goodX, centerY) && sim.decorations().size() == count,
          "clicking an existing ball selects it rather than creating another");
    sim.endInteraction();
    for (int i = 0; i < 80; ++i) {
        const auto current = ball(sim, id);
        check(sim.beginInteraction(snow::Tool::Snowball, current.localX, 600 - current.rise - current.radius),
              "grown ball remains selectable");
        check(sim.dragInteraction(i % 2 == 0 ? 250.f : 170.f, 600), "bounded repeated rolling remains supported");
        sim.endInteraction();
    }
    check(ball(sim, id).radius <= 18 && ball(sim, id).volume > 400, "sustained rolling respects growth cap");
    check(std::abs(sim.accumulation().totalVolume() + snowMass(sim) - initialMass) < .1f,
          "repeated capped rolling preserves all remaining snow mass");
    auto hidden = surface(41, 0, 600, 1000); hidden.visible = {{0, 290}, {340, 1000}};
    sim.accumulation().reconcile(std::span(&hidden, 1));
    const auto current = ball(sim, id);
    check(sim.beginInteraction(snow::Tool::Snowball, current.localX, 600 - current.rise - current.radius), "visible ball begins gap test");
    check(!sim.dragInteraction(380, 600) && ball(sim, id).localX == current.localX,
          "drag cannot jump across an occluded interval to another visible span");
    sim.endInteraction();
    check(!sim.beginInteraction(snow::Tool::Snowball, 320, 590), "occluded creation coordinate is rejected");
    check(!sim.beginInteraction(snow::Tool::Snowball, -20, 590), "outside-owner creation coordinate is rejected");
    check(!sim.beginInteraction(snow::Tool::None, 100, 590), "inactive tool cannot begin manipulation");
    const float beforeClear = sim.accumulation().totalVolume();
    check(sim.beginInteraction(snow::Tool::Shovel, 500, 600), "shovel begins a swept interaction");
    sim.dragInteraction(540, 600); sim.endInteraction();
    check(sim.accumulation().totalVolume() < beforeClear, "shovel drag changes actual model snow");
    check(sim.beginInteraction(snow::Tool::Snowball, current.localX, 600 - current.rise - current.radius), "begin before clearing snow");
    sim.clearSnow();
    check(sim.decorations().empty() && sim.accumulation().totalVolume() == 0 && !sim.dragInteraction(150, 590),
          "clear removes all figures, drifts and active drag");
    check(sim.accumulation().surfaces().size() == 1 && sim.accumulation().surfaces()[0].spec.id == 41,
          "clear retains tracker generations and supporting surfaces");
}
void stackingTests() {
    auto storage = interactiveSnow(8); auto& sim = *storage;
    const float initialMass = sim.accumulation().totalVolume();
    check(sim.useTool(snow::Tool::Snowball, 100, 600) && sim.useTool(snow::Tool::Snowball, 300, 600) &&
          sim.useTool(snow::Tool::Snowball, 500, 600), "three independent balls consume real snow");
    const auto base = sim.decorations()[0].id, middle = sim.decorations()[1].id, head = sim.decorations()[2].id;
    check(sim.beginInteraction(snow::Tool::Snowball, 300, 581), "pick middle ball at its visible center");
    check(sim.dragInteraction(100, 559), "lift middle ball onto the base");
    sim.endInteraction();
    check(ball(sim, middle).parentId == base && ball(sim, middle).localX == ball(sim, base).localX,
          "release records real vertical support instead of a side-by-side fake stack");
    check(std::abs(ball(sim, middle).rise - ball(sim, base).rise - ball(sim, base).radius * 2) < .01f,
          "stacked balls visibly touch their supporting ball");
    check(sim.beginInteraction(snow::Tool::Snowball, 500, 581), "pick the third actual ball");
    check(sim.dragInteraction(300, 537) && sim.dragInteraction(100, 537), "bounded lifted path reaches stack top");
    sim.endInteraction();
    check(ball(sim, head).parentId == middle && ball(sim, head).decorated,
          "three supported balls form a decorated snowman without duplicating body mass");
    check(std::abs(sim.accumulation().totalVolume() + snowMass(sim) - initialMass) < .02f,
          "stacking conserves every ball and remaining drift volume");
    check(!sim.beginInteraction(snow::Tool::Snowball, 100, 581), "supporting base cannot be dragged into its own descendants");
    check(sim.decorations().size() == 3 && ball(sim, middle).parentId == base && ball(sim, head).parentId == middle,
          "cycle attempt leaves the support graph unchanged");
    std::vector<snow::DrawItem> scene; sim.buildScene(scene);
    bool face = false;
    for (const auto& item : scene) {
        check(item.y >= 504 && item.y + item.h <= 600, "whole stack stays inside tracked vertical band");
        if (item.r < .2f && item.g < .3f) face = true;
    }
    check(face, "stacked snowman has visible facial details");
    check(sim.beginInteraction(snow::Tool::Snowball, 100, 537), "top ball can be detached from a stack");
    check(sim.dragInteraction(180, 537), "top ball can be carried away without moving lower supports");
    sim.endInteraction();
    check(ball(sim, head).parentId == 0 && !ball(sim, head).decorated && ball(sim, middle).parentId == base,
          "detaching head preserves the remaining supported pair");
    check(sim.beginInteraction(snow::Tool::Snowball, 180, 600 - ball(sim, head).rise - ball(sim, head).radius), "begin lifecycle drag");
    sim.accumulation().reconcile({});
    check(!sim.dragInteraction(200, 580) && sim.decorations().empty(), "owner destruction removes stack and invalidates dragging");
}
void interactionLifecycleTests() {
    auto storage = interactiveSnow(16); auto& sim = *storage;
    check(sim.useTool(snow::Tool::Snowball, 100, 600), "create ball for owner movement");
    const auto id = sim.decorations()[0].id;
    auto moved = surface(41, -200, 400, 1000); sim.accumulation().reconcile(std::span(&moved, 1));
    check(sim.beginInteraction(snow::Tool::Snowball, -100, 400 - ball(sim, id).rise - ball(sim, id).radius),
          "figure follows negative-coordinate owner movement");
    check(sim.dragInteraction(-60, 400), "drag uses the moved physical owner origin"); sim.endInteraction();
    check(ball(sim, id).localX == 140, "owner movement preserves local figure coordinate");
    moved.bounds.right = moved.bounds.left + 80; moved.visible = {{moved.bounds.left, moved.bounds.right}};
    sim.accumulation().reconcile(std::span(&moved, 1)); sim.setPaused(true); sim.advance(0);
    check(sim.decorations().empty(), "resize truncation frees unsupported figures even while paused");
    fillSurface(sim, 24); check(sim.useTool(snow::Tool::Snowball, -160, 400), "surface still accepts figures after resize");
    moved.id = 42; sim.accumulation().reconcile(std::span(&moved, 1)); sim.advance(0);
    check(sim.decorations().empty(), "new window generation cannot inherit figures");
    auto closed = interactiveSnow(16);
    check(closed->useTool(snow::Tool::Snowball, 100, 600), "create figure before close without animation");
    closed->accumulation().reconcile({});
    check(!closed->beginInteraction(snow::Tool::Snowball, 100, 580) && closed->decorations().empty(),
          "interaction on a closed owner frees figures before rejecting its coordinate");
    auto a = interactiveSnow(20), b = interactiveSnow(20);
    for (auto* candidate : {a.get(), b.get()}) {
        check(candidate->beginInteraction(snow::Tool::Snowball, 120, 600), "deterministic interaction starts");
        check(candidate->dragInteraction(170, 600) && candidate->dragInteraction(200, 580), "deterministic bounded drag sequence");
        candidate->endInteraction();
    }
    check(a->decorations()[0].id == b->decorations()[0].id && a->decorations()[0].radius == b->decorations()[0].radius &&
          a->decorations()[0].rise == b->decorations()[0].rise && a->accumulation().totalVolume() == b->accumulation().totalVolume(),
          "same input sequence produces deterministic mass and geometry");
    auto many = interactiveSnow(48);
    for (int i = 0; i < 32; ++i) {
        fillSurface(*many, 48);
        check(many->useTool(snow::Tool::Snowball, 100, 600), "bounded figure slot accepts actual snow");
    }
    check(!many->useTool(snow::Tool::Snowball, 200, 600) && many->decorations().size() == 32, "figure pool remains bounded at 32");
    check(many->beginInteraction(snow::Tool::Snowball, 100, 600 - many->decorations().back().rise - 11),
          "full figure pool still permits selecting existing snow"); many->endInteraction();
    many->clearSnow(); check(many->decorations().empty() && many->accumulation().totalVolume() == 0, "clear releases a full figure pool");
}
void foregroundFlakeTests() {
    auto storage = std::make_unique<snow::SnowSimulation>(29); auto& sim = *storage;
    sim.setBounds({0, 0, 1200, 800});
    std::vector<snow::DrawItem> scene; sim.buildScene(scene);
    std::size_t crystals = 0, ellipses = 0;
    for (const auto& item : scene) {
        if (item.shape == 2) { ++crystals; check(item.w >= 6 && item.w <= 6.8f, "foreground crystals have bounded visible size at normal settings"); }
        if (item.shape == 1) ++ellipses;
    }
    check(crystals > 0 && crystals < scene.size() / 4 && ellipses > crystals,
          "normal weather has a small foreground crystal population among small ellipse flakes");
    check(scene.size() == sim.particles().size(), "crystal detail uses no extra draw items");
}
}
int main() {
    try { settingsTests(); heightTests(); accumulationTests(); simulationTests(); sceneClipTests(); saturatedInputTests(); performanceTests(); toolsTests(); depthAndSnapshotTests(); interactionTests(); stackingTests(); interactionLifecycleTests(); foregroundFlakeTests(); }
    catch (const std::exception& e) { std::cerr << "FAIL after " << checks << ": " << e.what() << '\n'; return 1; }
    std::cout << "PASS: " << checks << " assertions (settings, height fields, accumulation, simulation, tools)\n";
}
