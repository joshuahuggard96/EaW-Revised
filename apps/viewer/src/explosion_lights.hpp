#pragma once

#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace godot {
class Node3D;
}

namespace eawr::presentation::godot_backend {

// Remastered only, not a retail look: a short point light at each explosion, so a blast
// lights the hulls around it. A fixed pool of RenderingServer omni lights without shadows;
// when it is full, a new flash replaces the dimmest one. Only Godot-lit materials (the
// remastered ones) receive the light; retail-path shaders are unshaded.
class ExplosionLights final {
public:
    static constexpr std::size_t max_lights = 16;

    explicit ExplosionLights(godot::Node3D& host);
    ~ExplosionLights();
    ExplosionLights(const ExplosionLights&) = delete;
    ExplosionLights& operator=(const ExplosionLights&) = delete;

    struct Flash final {
        std::array<double, 3> position{};  // source (ALO/sim) basis
        std::array<float, 3> colour{1.0F, 1.0F, 1.0F};
        float range{};
        float energy{};
        double frames{};  // effect-clock samples (30 Hz) until dark
    };
    // `born` and `now` are effect-clock samples, fractional between samples.
    void add(const Flash& flash, double born);
    void update(double now);
    void release();
    [[nodiscard]] std::size_t live() const noexcept;
    [[nodiscard]] std::uint64_t flashes() const noexcept { return flashes_; }
    [[nodiscard]] std::uint64_t replaced() const noexcept { return replaced_; }

private:
    struct Light final {
        godot::RID light;
        godot::RID instance;
        Flash flash;
        double born{};
        bool active{};
        float level{};
    };
    [[nodiscard]] static float envelope(double age, double frames) noexcept;
    godot::RID scenario_;
    std::vector<Light> lights_;
    std::uint64_t flashes_{}, replaced_{};
};

} // namespace eawr::presentation::godot_backend
