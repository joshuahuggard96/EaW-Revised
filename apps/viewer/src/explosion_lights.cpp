#include "explosion_lights.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <algorithm>
#include <cmath>

namespace eawr::presentation::godot_backend {
using namespace godot;

ExplosionLights::ExplosionLights(Node3D& host) {
    if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
}

ExplosionLights::~ExplosionLights() { release(); }

float ExplosionLights::envelope(const double age, const double frames) noexcept {
    // A two-sample rise, then a fall that is quick at first and long in the tail.
    constexpr double rise = 2.0;
    if (age < 0.0 || age >= frames) return 0.0F;
    if (age < rise) return static_cast<float>(age / rise);
    const double fall = 1.0 - (age - rise) / (frames - rise);
    return static_cast<float>(fall * fall);
}

void ExplosionLights::add(const Flash& flash, const double born) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr || !scenario_.is_valid() || flash.energy <= 0.0F || flash.frames <= 2.0) return;
    ++flashes_;
    Light* slot = nullptr;
    for (Light& light : lights_) {
        if (!light.active) { slot = &light; break; }
    }
    if (slot == nullptr && lights_.size() < max_lights) {
        Light created;
        created.light = rendering->omni_light_create();
        rendering->light_set_shadow(created.light, false);
        rendering->light_set_param(created.light, RenderingServer::LIGHT_PARAM_SPECULAR, 1.0);
        // No inverse-distance falloff: at ship scale (hundreds of units) it leaves nothing.
        // The range window alone fades the light, from full at the blast to dark at its range.
        rendering->light_set_param(created.light, RenderingServer::LIGHT_PARAM_ATTENUATION, 0.0);
        created.instance = rendering->instance_create2(created.light, scenario_);
        lights_.push_back(created);
        slot = &lights_.back();
    }
    if (slot == nullptr) {
        slot = &*std::min_element(lights_.begin(), lights_.end(),
            [](const Light& a, const Light& b) { return a.level * a.flash.energy < b.level * b.flash.energy; });
        ++replaced_;
    }
    slot->flash = flash;
    slot->born = born;
    slot->active = true;
    slot->level = 0.0F;
    rendering->light_set_color(slot->light, Color(flash.colour[0], flash.colour[1], flash.colour[2]));
    rendering->light_set_param(slot->light, RenderingServer::LIGHT_PARAM_RANGE, flash.range);
    rendering->light_set_param(slot->light, RenderingServer::LIGHT_PARAM_ENERGY, 0.0);
    // The one documented ALO-to-render conversion, (x, y, z) -> (x, z, -y).
    const auto& at = flash.position;
    rendering->instance_set_transform(slot->instance, Transform3D(Basis(),
        Vector3(static_cast<float>(at[0]), static_cast<float>(at[2]), static_cast<float>(-at[1]))));
    rendering->instance_set_visible(slot->instance, false);
}

void ExplosionLights::update(const double now) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    for (Light& light : lights_) {
        if (!light.active) continue;
        const double age = now - light.born;
        light.level = envelope(age, light.flash.frames);
        if (age >= light.flash.frames) light.active = false;
        rendering->light_set_param(light.light, RenderingServer::LIGHT_PARAM_ENERGY, light.flash.energy * light.level);
        rendering->instance_set_visible(light.instance, light.active && light.level > 0.0F);
    }
}

void ExplosionLights::release() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering != nullptr) {
        for (const Light& light : lights_) {
            rendering->free_rid(light.instance);
            rendering->free_rid(light.light);
        }
    }
    lights_.clear();
}

std::size_t ExplosionLights::live() const noexcept {
    return static_cast<std::size_t>(std::count_if(lights_.begin(), lights_.end(), [](const Light& l) { return l.active; }));
}

} // namespace eawr::presentation::godot_backend
