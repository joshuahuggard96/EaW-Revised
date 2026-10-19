#pragma once

// The fork's remastered graphics page (presentation::ui::RemasteredSettings) on the engine's
// globals. Start-up (viewer_host_ready.cpp) and the options screen's Accept share these, so a
// saved value means the same at both. A --eawr-* option given on the command line pins its
// value: the saved one is ignored and the page shows the row disabled.
//
// When a change shows: render scale and anti-aliasing at once (the root viewport); the other
// values from the next battle (renderers, materials and particle programs read them when a
// battle loads); the render profile after a restart (the output mode decides how every shader
// compiles, so it is chosen once before any renderer exists).

#include "output_mode.hpp"
#include "render_profile.hpp"
#include "render_profile_viewport.hpp"

#include "eawr/presentation/ui/settings.hpp"

namespace eawr::presentation::godot_backend {

// The --eawr-* graphics options the command line gave (viewer_host_ready.cpp).
struct RemasteredPins final {
    bool render_profile{};
    bool render_scale{};
    bool antialiasing{};
    bool studio{};
    bool explosions{};
    bool lasers{};
    bool engines{};
    bool glow{};
    bool exposure{};
    bool reflections{};
    bool fps_counter{};
};

[[nodiscard]] inline RemasteredPins& remastered_pins() noexcept {
    static RemasteredPins pins;
    return pins;
}

// The profile this run started with; a saved change waits for the next start.
[[nodiscard]] inline RenderProfile& running_render_profile() noexcept {
    static RenderProfile profile = RenderProfile::retail;
    return profile;
}

[[nodiscard]] constexpr float from_percent(const int percent) noexcept { return static_cast<float>(percent) / 100.0F; }

// The anti-aliasing a saved choice stands for at a profile and render scale. FSR 2 needs a
// render scale of 1 or less and retail keeps its own, so either falls back to the profile's.
[[nodiscard]] constexpr AntiAliasing remastered_antialiasing(const int setting, const RenderProfile profile,
                                                             const float scale) noexcept {
    AntiAliasing mode = AntiAliasing::profile;
    switch (setting) {
    case 1: mode = AntiAliasing::profile; break;
    case 2: mode = AntiAliasing::smaa; break;
    case 3: mode = AntiAliasing::taa; break;
    case 4: mode = AntiAliasing::fsr2; break;
    case 5: mode = AntiAliasing::off; break;
    default:
        // Automatic: the owner's pick, FSR 2 for remastered at native resolution or below.
        mode = profile == RenderProfile::remastered && scale <= 1.0F ? AntiAliasing::fsr2 : AntiAliasing::profile;
    }
    if (profile == RenderProfile::retail || (mode == AntiAliasing::fsr2 && scale > 1.0F)) return AntiAliasing::profile;
    return mode;
}

// The saved values the next battle reads, for every option the command line left open.
inline void apply_remastered_globals(const presentation::ui::RemasteredSettings& settings) {
    const RemasteredPins& pins = remastered_pins();
    if (!pins.studio) studio_strength() = from_percent(settings.studio_percent);
    if (!pins.explosions) explosion_strength() = from_percent(settings.explosions_percent);
    if (!pins.lasers) laser_strength() = from_percent(settings.lasers_percent);
    if (!pins.engines) engine_strength() = from_percent(settings.engines_percent);
    if (!pins.glow) glow_strength() = from_percent(settings.glow_percent);
    if (!pins.exposure) linear_exposure() = from_percent(settings.exposure_percent);
    if (!pins.reflections) backdrop_reflections() = from_percent(settings.reflections_percent);
}

// Render scale and anti-aliasing on the root viewport, at once. The running profile is applied
// again first: its MSAA and SMAA are what a lower scale or another mode starts from.
inline void apply_remastered_viewport(godot::Viewport& viewport, const presentation::ui::RemasteredSettings& settings) {
    const RemasteredPins& pins = remastered_pins();
    const RenderProfile profile = running_render_profile();
    if (profile == RenderProfile::retail || (pins.render_scale && pins.antialiasing)) return;
    const float scale = pins.render_scale ? static_cast<float>(viewport.get_scaling_3d_scale())
                                          : from_percent(settings.render_scale_percent);
    const AntiAliasing mode = pins.antialiasing ? active_antialiasing()
                                                : remastered_antialiasing(settings.antialiasing, profile, scale);
    apply_render_profile(viewport, profile);
    apply_render_scale(viewport, scale);
    apply_antialiasing(viewport, mode);
}

// The FPS counter's saved choice, unless --eawr-fps chose.
[[nodiscard]] inline bool saved_fps_counter(const presentation::ui::RemasteredSettings& settings) noexcept {
    return !remastered_pins().fps_counter && settings.fps_counter;
}

} // namespace eawr::presentation::godot_backend
