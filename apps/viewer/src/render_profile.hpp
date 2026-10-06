#pragma once

#include <cstdint>
#include <array>
#include <charconv>
#include <optional>
#include <string_view>

// Render profiles (#184, owner decision D2 = B on #189; docs/rendering.md,
// render profiles). Players get the enhanced profile: 4x MSAA, SMAA and 16x
// anisotropic filtering. Captures and tests stay pinned to the retail look:
// no MSAA, no screen-space AA and no anisotropic filtering, which also makes
// the terrain sample linear like the retail TerrainRenderBump samplers. There
// is no player setting until M6 (D1 = C); `--eawr-render-profile retail|enhanced`
// is an internal switch for evidence runs.
//
// remastered (opt-in, never a default) is the enhanced profile with a linear
// frame (output_mode.hpp): Godot's tonemapper, glow and lit materials instead
// of the stored-value retail colour policy. It is not a retail look.
namespace eawr::presentation::godot_backend {

enum class RenderProfile { retail, enhanced, remastered };

// Fixed view-depth budgets: camera motion must not change cascade scale.
// Godot 4.7.2 fits each split to a sphere and snaps its light-space bounds.
struct ShadowSettings final {
    std::int32_t atlas_size;
    std::array<float, 3> split_offsets;
    float max_distance;
    bool high_filter;
};

[[nodiscard]] constexpr ShadowSettings shadow_settings(const RenderProfile profile, const bool space) noexcept {
    // Keep the near three split depths identical in land and space. Space's
    // longer last cascade must not spend close-up texels on its distant sky.
    const bool enhanced = profile != RenderProfile::retail;
    auto offsets = enhanced ? std::array{0.0625F, 0.1875F, 0.5F} : std::array{0.125F, 0.25F, 0.5F};
    if (space) for (float& offset : offsets) offset *= 0.5F;
    // Space's enhanced atlas is 16k: its biases scale with the texel size and
    // erased zoomed-out self-shadowing at 8k (space_populate.cpp), for 0.5 GiB.
    return {enhanced ? (space ? 16384 : 8192) : 4096, offsets, space ? 8192.0F : 4096.0F, enhanced};
}

// What a profile asks of the root viewport. Zero turns a feature off.
struct RenderSettings final {
    std::uint32_t msaa_samples{};
    bool smaa{};
    std::uint32_t anisotropy{};

    friend constexpr bool operator==(const RenderSettings&, const RenderSettings&) = default;
};

[[nodiscard]] constexpr RenderSettings render_settings(const RenderProfile profile) noexcept {
    return profile != RenderProfile::retail ? RenderSettings{4, true, 16} : RenderSettings{};
}

[[nodiscard]] constexpr std::string_view render_profile_name(const RenderProfile profile) noexcept {
    switch (profile) {
    case RenderProfile::enhanced: return "enhanced";
    case RenderProfile::remastered: return "remastered";
    case RenderProfile::retail: break;
    }
    return "retail";
}

[[nodiscard]] constexpr std::optional<RenderProfile> parse_render_profile(const std::string_view text) noexcept {
    if (text == "retail") return RenderProfile::retail;
    if (text == "enhanced") return RenderProfile::enhanced;
    if (text == "remastered") return RenderProfile::remastered;
    return std::nullopt;
}

// A plain decimal number within [low, high], or nullopt.
[[nodiscard]] inline std::optional<float> parse_bounded(
    const std::string_view text, const float low, const float high) noexcept {
    float value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !(value >= low && value <= high)) {
        return std::nullopt;
    }
    return value;
}

// --eawr-render-scale: the 3D resolution as a multiple of the window's.
// Above 1 supersamples, below 1 trades sharpness for speed on weak GPUs.
[[nodiscard]] inline std::optional<float> parse_render_scale(const std::string_view text) noexcept {
    return parse_bounded(text, 0.5F, 2.0F);
}

// --eawr-reflections: the remastered hulls' backdrop reflection strength.
[[nodiscard]] inline std::optional<float> parse_reflections(const std::string_view text) noexcept {
    return parse_bounded(text, 0.0F, 4.0F);
}

// A render scale from which supersampling replaces MSAA (apply_render_scale):
// 1.5 already draws 2.25 samples per window pixel.
inline constexpr float supersampling_replaces_msaa = 1.5F;

[[nodiscard]] constexpr bool msaa_replaced_by_supersampling(const float scale) noexcept {
    return scale >= supersampling_replaces_msaa;
}

// --eawr-glow: the remastered additive glows' brightness.
[[nodiscard]] inline std::optional<float> parse_glow(const std::string_view text) noexcept {
    return parse_bounded(text, 0.5F, 8.0F);
}

// --eawr-studio: the remastered hulls' studio-model look (0 off, 1 full).
[[nodiscard]] inline std::optional<float> parse_studio(const std::string_view text) noexcept {
    return parse_bounded(text, 0.0F, 1.0F);
}

// --eawr-exposure: the remastered frame's tonemapper exposure.
[[nodiscard]] inline std::optional<float> parse_exposure(const std::string_view text) noexcept {
    return parse_bounded(text, 0.25F, 4.0F);
}

// The run facts that pick the default profile.
struct RenderProfileRun final {
    bool interactive{}; // --eawr-camera-interactive
    bool self_test{};   // --eawr-camera-input-selftest
    bool capture{};     // --eawr-capture
    bool resize_test{}; // --eawr-window-resize-test
};

// An interactive run that captures and tests nothing is a player's view;
// every other run (fixed captures, probes, self-tests, reports) is evidence
// and keeps the retail look unless --eawr-render-profile says otherwise.
[[nodiscard]] constexpr RenderProfile default_render_profile(const RenderProfileRun& run) noexcept {
    return run.interactive && !run.self_test && !run.capture && !run.resize_test
        ? RenderProfile::enhanced : RenderProfile::retail;
}

} // namespace eawr::presentation::godot_backend
