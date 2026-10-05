// Render profile selection (#184, apps/viewer/src/render_profile.hpp).
#include "render_profile.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using eawr::presentation::godot_backend::default_render_profile;
using eawr::presentation::godot_backend::parse_render_profile;
using eawr::presentation::godot_backend::parse_exposure;
using eawr::presentation::godot_backend::parse_reflections;
using eawr::presentation::godot_backend::parse_render_scale;
using eawr::presentation::godot_backend::render_profile_name;
using eawr::presentation::godot_backend::render_settings;
using eawr::presentation::godot_backend::RenderProfile;
using eawr::presentation::godot_backend::RenderProfileRun;
using eawr::presentation::godot_backend::RenderSettings;

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void settings_follow_the_owner_decision() {
    check(render_settings(RenderProfile::retail) == RenderSettings{0, false, 0},
          "retail has no MSAA, no screen-space AA and no anisotropic filtering");
    check(render_settings(RenderProfile::enhanced) == RenderSettings{4, true, 16},
          "enhanced is 4x MSAA, SMAA and 16x anisotropic filtering");
    check(render_settings(RenderProfile::remastered) == render_settings(RenderProfile::enhanced),
          "remastered keeps the enhanced viewport settings; only the output mode differs");
}

void names_round_trip() {
    for (const RenderProfile profile : {RenderProfile::retail, RenderProfile::enhanced, RenderProfile::remastered}) {
        check(parse_render_profile(render_profile_name(profile)) == profile, "a profile name parses back");
    }
    for (const std::string_view bad : {"", "Retail", "ENHANCED", "off", "retail ", "Remastered"}) {
        check(!parse_render_profile(bad), "only the exact lower-case names parse");
    }
}

void render_scales_parse_within_bounds() {
    check(parse_render_scale("1") == 1.0F, "1 is the window resolution");
    check(parse_render_scale("1.5") == 1.5F, "a fractional scale parses");
    check(parse_render_scale("0.5") == 0.5F && parse_render_scale("2") == 2.0F, "both bounds are accepted");
    for (const std::string_view bad : {"", "0.49", "2.01", "-1", "x", "1.5x", " 1", "nan", "inf"}) {
        check(!parse_render_scale(bad), "only a plain number from 0.5 to 2 parses");
    }
    check(parse_exposure("1.5") == 1.5F && parse_exposure("0.25") == 0.25F && parse_exposure("4") == 4.0F,
          "exposures from 0.25 to 4 parse");
    for (const std::string_view bad : {"", "0.2", "4.5", "-1", "bright"}) {
        check(!parse_exposure(bad), "only a plain number from 0.25 to 4 is an exposure");
    }
    check(parse_reflections("0") == 0.0F && parse_reflections("1") == 1.0F && parse_reflections("4") == 4.0F,
          "reflection strengths from 0 to 4 parse");
    for (const std::string_view bad : {"", "-0.5", "4.5", "shiny"}) {
        check(!parse_reflections(bad), "only a plain number from 0 to 4 is a reflection strength");
    }
}

void shadow_budgets_are_bounded_and_ordered() {
    using eawr::presentation::godot_backend::shadow_settings;
    for (const auto profile : {RenderProfile::retail, RenderProfile::enhanced}) {
        for (const bool space : {false, true}) {
            const auto settings = shadow_settings(profile, space);
            float previous = 0.0F;
            for (const float split : settings.split_offsets) {
                check(split > previous && split < 1.0F, "cascades have increasing nonempty depth intervals");
                previous = split;
            }
            check(settings.max_distance >= 4096.0F && settings.max_distance <= 8192.0F,
                  "tactical reach stays within the fixed land/space budget");
            // Godot's directional atlas limit; only enhanced space uses 16k.
            check(settings.atlas_size <= (space && profile != RenderProfile::retail ? 16384 : 8192)
                      && settings.atlas_size >= 4096,
                  "atlas stays within the supported GPU budget");
        }
    }
    check(shadow_settings(RenderProfile::enhanced, false).atlas_size
              > shadow_settings(RenderProfile::retail, false).atlas_size,
          "enhanced spends more texels on shadow edges");
    for (const auto profile : {RenderProfile::retail, RenderProfile::enhanced}) {
        const auto land = shadow_settings(profile, false);
        const auto space = shadow_settings(profile, true);
        for (std::size_t split = 0; split < land.split_offsets.size(); ++split) {
            check(land.split_offsets[split] * land.max_distance
                      == space.split_offsets[split] * space.max_distance,
                  "space's far reach must not enlarge the three near cascades");
        }
    }
}

RenderProfileRun run(const bool interactive, const bool self_test, const bool capture, const bool resize_test) {
    return RenderProfileRun{.interactive = interactive, .self_test = self_test, .capture = capture,
        .resize_test = resize_test};
}

void only_a_players_live_view_is_enhanced() {
    check(default_render_profile(run(true, false, false, false)) == RenderProfile::enhanced,
          "an interactive run without capture or test is a player's view");
    check(default_render_profile(run(false, false, false, false)) == RenderProfile::retail,
          "a fixed run keeps the retail look");
    check(default_render_profile(run(false, false, true, false)) == RenderProfile::retail,
          "a capture keeps the retail look");
    check(default_render_profile(run(true, true, false, false)) == RenderProfile::retail,
          "a camera self-test keeps the retail look");
    check(default_render_profile(run(true, false, true, false)) == RenderProfile::retail,
          "a capture-locked interactive run keeps the retail look");
    check(default_render_profile(run(true, false, false, true)) == RenderProfile::retail,
          "a window-resize test keeps the retail look");
}

} // namespace

int main() {
    settings_follow_the_owner_decision();
    names_round_trip();
    render_scales_parse_within_bounds();
    shadow_budgets_are_bounded_and_ordered();
    only_a_players_live_view_is_enhanced();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "render profile contracts passed\n";
    return EXIT_SUCCESS;
}
