#include "options_remastered.hpp"

#include "remastered_settings.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace eawr::presentation::godot_backend::options_remastered {
namespace model = presentation::ui;
namespace data = eawr::data::ui;

namespace {

constexpr std::array<Row, 10> page_rows{{
    {"IDC_REM_PROFILE", "Render Profile", 0, 2, 1},
    {"IDC_REM_SCALE", "Render Scale", 50, 200, 25},
    {"IDC_REM_AA", "Anti-Aliasing", 0, model::remastered_antialiasing_count - 1, 1},
    {"IDC_REM_STUDIO", "Studio Look", 0, 100, 10},
    {"IDC_REM_EXPLOSIONS", "Explosions", 0, 400, 25},
    {"IDC_REM_LASERS", "Laser Cores", 0, 400, 25},
    {"IDC_REM_ENGINES", "Engine Glow", 0, 400, 25},
    {"IDC_REM_GLOW", "Window Glow", 50, 800, 25},
    {"IDC_REM_EXPOSURE", "Exposure", 25, 400, 5},
    {"IDC_REM_REFLECTIONS", "Reflections", 0, 400, 25},
}};

constexpr std::string_view note =
    "Scale and anti-aliasing apply now, the profile after a restart, the rest from the next battle.";

[[nodiscard]] const data::DialogControl* find(const data::Dialog& dialog, const std::string_view id) {
    const auto found = std::find_if(dialog.controls.begin(), dialog.controls.end(),
                                    [id](const data::DialogControl& control) { return control.id_name == id; });
    return found != dialog.controls.end() ? &*found : nullptr;
}

[[nodiscard]] int* field(model::RemasteredSettings& settings, const std::string_view row) {
    if (row == "IDC_REM_SCALE") return &settings.render_scale_percent;
    if (row == "IDC_REM_AA") return &settings.antialiasing;
    if (row == "IDC_REM_STUDIO") return &settings.studio_percent;
    if (row == "IDC_REM_EXPLOSIONS") return &settings.explosions_percent;
    if (row == "IDC_REM_LASERS") return &settings.lasers_percent;
    if (row == "IDC_REM_ENGINES") return &settings.engines_percent;
    if (row == "IDC_REM_GLOW") return &settings.glow_percent;
    if (row == "IDC_REM_EXPOSURE") return &settings.exposure_percent;
    if (row == "IDC_REM_REFLECTIONS") return &settings.reflections_percent;
    return nullptr;
}

[[nodiscard]] std::string multiplier(const int percent) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%.2fx", static_cast<double>(percent) / 100.0);
    return buffer;
}

} // namespace

std::span<const Row> rows() noexcept { return page_rows; }

std::string value_label(const std::string_view row) { return std::string(row) + "_VALUE"; }

bool build_page(data::Dialog& dialog) {
    const data::DialogControl* title = find(dialog, "IDC_STATIC_MEDIUM");
    const data::DialogControl* label = find(dialog, "IDC_PARTICLE_DETAIL");
    const data::DialogControl* slider = find(dialog, "IDC_PARTICLE_DETAIL_SLIDER");
    const data::DialogControl* next = find(dialog, "IDC_MESH_DETAIL_SLIDER");
    const data::DialogControl* check = find(dialog, "IDC_SOFT_SHADOWS");
    const data::DialogControl* detect = find(dialog, "IDC_AUTO_DETECT");
    if (!title || !label || !slider || !next || !check || !detect || next->rect.y <= slider->rect.y) return false;
    // The row's right-hand caption ("High") becomes the value label.
    const auto high = std::find_if(dialog.controls.begin(), dialog.controls.end(), [slider](const auto& control) {
        return control.id_name == "IDC_STATIC" && control.rect.x > slider->rect.x
            && std::abs(control.rect.y - slider->rect.y) < slider->rect.height;
    });
    if (high == dialog.controls.end()) return false;

    std::vector<data::DialogControl> controls;
    for (const data::DialogControl& control : dialog.controls) {
        if (control.id_name == "IDC_OK" || control.id_name == "IDC_CANCEL" || control.id_name == "IDC_APPLY") {
            controls.push_back(control);
        }
    }
    data::DialogControl heading = *title;
    heading.text = "Remastered Graphics";
    controls.push_back(std::move(heading));

    const int pitch = next->rect.y - slider->rect.y;
    for (std::size_t index = 0; index < page_rows.size(); ++index) {
        const int down = static_cast<int>(index) * pitch;
        data::DialogControl caption = *label;
        caption.id_name = std::string(page_rows[index].id) + "_LABEL";
        caption.text = std::string(page_rows[index].caption);
        caption.rect.y += down;
        // The row's "Low" caption is dropped, so the name may use its room.
        caption.rect.width = std::max(caption.rect.width, slider->rect.x - label->rect.x - 2);
        // The slider gives up a sixth of its length so "Remastered" fits beside it.
        const int room = slider->rect.width / 6;
        data::DialogControl bar = *slider;
        bar.id_name = std::string(page_rows[index].id);
        bar.rect.y += down;
        bar.rect.width -= room;
        data::DialogControl value = *high;
        value.id_name = value_label(page_rows[index].id);
        value.text = "";
        value.rect.y += down;
        value.rect.x -= room;
        value.rect.width = std::max(value.rect.width, dialog.rect.width - value.rect.x - label->rect.x / 2);
        controls.push_back(std::move(caption));
        controls.push_back(std::move(bar));
        controls.push_back(std::move(value));
    }

    const int below_rows = slider->rect.y + static_cast<int>(page_rows.size()) * pitch + pitch / 4;
    data::DialogControl fps = *check;
    fps.id_name = std::string(fps_check);
    fps.text = "Show FPS Counter";
    fps.rect.x = label->rect.x;
    fps.rect.y = below_rows;
    fps.rect.width = std::max(fps.rect.width, slider->rect.x - label->rect.x);
    data::DialogControl defaults = *detect;
    defaults.id_name = std::string(defaults_button);
    defaults.text = "Defaults";
    defaults.rect.y = below_rows + (fps.rect.height - defaults.rect.height) / 2;
    data::DialogControl explain = *label;
    explain.id_name = "IDC_REM_NOTE";
    explain.text = std::string(note);
    explain.rect.x = label->rect.x;
    explain.rect.y = below_rows + std::max(fps.rect.height, defaults.rect.height) + pitch / 3;
    explain.rect.width = dialog.rect.width - 2 * label->rect.x;
    explain.rect.height = label->rect.height * 5 / 2;
    const int content_bottom = explain.rect.y + explain.rect.height + pitch * 2 / 3;
    controls.push_back(std::move(fps));
    controls.push_back(std::move(defaults));
    controls.push_back(std::move(explain));

    // The buttons move down below the rows and the frame grows with them.
    int buttons_top = dialog.rect.height;
    for (const data::DialogControl& control : controls) {
        if (control.id_name == "IDC_OK" || control.id_name == "IDC_CANCEL" || control.id_name == "IDC_APPLY") {
            buttons_top = std::min(buttons_top, control.rect.y);
        }
    }
    const int shift = std::max(0, content_bottom - buttons_top);
    for (data::DialogControl& control : controls) {
        if (control.id_name == "IDC_OK" || control.id_name == "IDC_CANCEL" || control.id_name == "IDC_APPLY") {
            control.rect.y += shift;
        }
    }
    dialog.rect.height += shift;
    dialog.controls = std::move(controls);
    return true;
}

void add_video_button(data::Dialog& dialog) {
    const data::DialogControl* advanced = find(dialog, "IDC_ADVANCED");
    if (advanced == nullptr) return;
    data::DialogControl button = *advanced;
    button.id_name = std::string(video_button);
    button.id.reset();
    button.text = "Remastered...";
    button.rect.y += advanced->rect.height + advanced->rect.height / 3;
    dialog.controls.push_back(std::move(button));
}

int value_of(const model::PlayerSettings& draft, const std::string_view row, const int running_profile) {
    if (row == "IDC_REM_PROFILE") {
        switch (draft.video.render_profile) {
        case model::RenderProfileSetting::retail: return 0;
        case model::RenderProfileSetting::enhanced: return 1;
        case model::RenderProfileSetting::remastered: return 2;
        case model::RenderProfileSetting::automatic: break;
        }
        return running_profile;
    }
    model::RemasteredSettings copy = draft.remastered;
    const int* value = field(copy, row);
    return value != nullptr ? *value : 0;
}

void store(model::PlayerSettings& draft, const std::string_view row, const int value, const bool profile_touched) {
    if (row == "IDC_REM_PROFILE") {
        if (!profile_touched) return;
        draft.video.render_profile = value <= 0 ? model::RenderProfileSetting::retail
            : value == 1 ? model::RenderProfileSetting::enhanced : model::RenderProfileSetting::remastered;
        return;
    }
    if (int* target = field(draft.remastered, row)) *target = value;
}

std::string value_text(const std::string_view row, const int value) {
    if (row == "IDC_REM_PROFILE") return value <= 0 ? "Retail" : value == 1 ? "Enhanced" : "Remastered";
    if (row == "IDC_REM_AA") {
        constexpr std::array<std::string_view, 6> names{"Auto", "MSAA", "SMAA", "TAA", "FSR 2", "Off"};
        return std::string(names[static_cast<std::size_t>(std::clamp(value, 0, 5))]);
    }
    if (row == "IDC_REM_STUDIO") return value <= 0 ? "Off" : std::to_string(value) + "%";
    if (value <= 0) return "Off";
    return multiplier(value);
}

std::optional<std::string> pinned_reason(const std::string_view row) {
    const RemasteredPins& pins = remastered_pins();
    const auto reason = [](const char* option) {
        return std::optional<std::string>{std::string("Set by the ") + option + " launch option."};
    };
    if (row == "IDC_REM_PROFILE" && pins.render_profile) return reason("--eawr-render-profile");
    if (row == "IDC_REM_SCALE" && pins.render_scale) return reason("--eawr-render-scale");
    if (row == "IDC_REM_AA" && pins.antialiasing) return reason("--eawr-aa");
    if (row == "IDC_REM_STUDIO" && pins.studio) return reason("--eawr-studio");
    if (row == "IDC_REM_EXPLOSIONS" && pins.explosions) return reason("--eawr-explosions");
    if (row == "IDC_REM_LASERS" && pins.lasers) return reason("--eawr-lasers");
    if (row == "IDC_REM_ENGINES" && pins.engines) return reason("--eawr-engines");
    if (row == "IDC_REM_GLOW" && pins.glow) return reason("--eawr-glow");
    if (row == "IDC_REM_EXPOSURE" && pins.exposure) return reason("--eawr-exposure");
    if (row == "IDC_REM_REFLECTIONS" && pins.reflections) return reason("--eawr-reflections");
    if (row == fps_check && pins.fps_counter) return reason("--eawr-fps");
    return std::nullopt;
}

} // namespace eawr::presentation::godot_backend::options_remastered
