#pragma once

#include <cstdint>

// The frame's colour space (docs/rendering.md, colour policy).
//
// stored: the retail policy. Shaders hand Godot stored 8-bit values and the
//   stored-value compositor pass decodes the frame once (stored_output.hpp).
// linear: the remastered render profile. Every spatial shader decodes its own
//   result (linear_output_source in shader_adapter.hpp), the frame holds
//   linear light and the decode pass is off, so Godot's tonemapper, glow and
//   lit materials work as designed. Not a retail look; captures never use it.
//
// The viewer host picks the mode once, before any renderer or shader exists.
namespace eawr::presentation::godot_backend {

enum class OutputMode : std::uint8_t { stored, linear };

[[nodiscard]] inline OutputMode& output_mode() noexcept {
    static OutputMode mode = OutputMode::stored;
    return mode;
}

// The linear frame's tonemapper exposure (--eawr-exposure). 1.25 keeps the
// space backdrop and lit and unlit hulls within a few levels of the
// stored-value frame on M2 Coruscant. The stored policy ignores it; its
// linear tonemapper stays at exposure 1.
inline constexpr float default_linear_exposure = 1.25F;

[[nodiscard]] inline float& linear_exposure() noexcept {
    static float exposure = default_linear_exposure;
    return exposure;
}

// The remastered hulls' backdrop reflection strength (--eawr-reflections):
// 1 is the physically based amount, 0 turns the reflections off. The default
// is three times physical, the owner's pick: on dark backdrops such as M2
// Coruscant the physical amount barely shows.
inline constexpr float default_backdrop_reflections = 3.0F;

[[nodiscard]] inline float& backdrop_reflections() noexcept {
    static float strength = default_backdrop_reflections;
    return strength;
}

// The remastered additive glows' brightness (--eawr-glow): 1 is the retail
// colour in linear light; above 1 engines and lights bloom.
inline constexpr float default_glow = 3.0F;

[[nodiscard]] inline float& glow_strength() noexcept {
    static float strength = default_glow;
    return strength;
}

} // namespace eawr::presentation::godot_backend
