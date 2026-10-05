#pragma once

#include "legacy/bump_colorize.hpp"
#include "shader_adapter.hpp"

#include <string>
#include <string_view>

// The remastered profile's hull material (output_mode.hpp): MeshBumpColorize
// and RSkinBumpColorize lit by Godot in linear light instead of the retail
// sph_t2 equation (legacy/bump_colorize.hpp). Not a retail look; only the
// linear output mode selects it.
//
// It keeps the legacy adapter's uniforms, textures and authored tangent frame,
// so bindings, lighting parameters and skinning reach it unchanged:
// - Albedo is the colorized base texel, decoded to linear light.
// - The NormalTexture perturbs the authored frame per pixel; its alpha (the
//   retail specular mask) is the gloss, which drives roughness and the
//   specular strength. Retail textures carry no metalness, so hulls stay
//   dielectric: with no reflected environment a metal would turn black.
// - The sun is a Lambert diffuse and a GGX specular term in light(), under
//   Godot's shadow attenuation. Its radiance is (2 x diffuse)^2.2, so a texel
//   facing the sun keeps the stored-value brightness 2 x surface x diffuse;
//   the specular colour takes the same power.
// - The environment's fill lights and ambient (SPH_LIGHT_FILL, per vertex) add
//   to the sun's diffuse with the same power, so an unlit side keeps its
//   retail brightness. As in retail, the shadow floor darkens them where the
//   sun is shadowed; unshadowed fill left every hull's far side too bright.
namespace eawr::presentation::godot_backend::remastered_hull {

inline constexpr std::string_view shader_head = R"GODOT(
shader_type spatial;
// eawr:linear-output
render_mode fog_disabled, depth_draw_opaque, depth_test_default, cull_back, ambient_light_disabled;

uniform sampler2D BaseTexture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D NormalTexture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec3 eawr_emissive = vec3(0.0);
uniform vec3 eawr_diffuse = vec3(1.0);
uniform vec3 eawr_specular = vec3(1.0);
uniform vec3 eawr_colorization = vec3(0.0, 1.0, 0.0);
uniform vec2 eawr_uv_offset = vec2(0.0);
uniform mat4 eawr_sph_fill_r;
uniform mat4 eawr_sph_fill_g;
uniform mat4 eawr_sph_fill_b;
uniform vec4 eawr_light_scale = vec4(1.0);
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
instance uniform float eawr_unit_opacity = 1.0;
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_diffuse = vec3(2.0, 1.88, 1.72);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
uniform vec3 eawr_shadow_floor = vec3(0.5);
varying vec3 eawr_fill;

vec3 eawr_decode(vec3 stored_rgb) {
    vec3 nonnegative = max(stored_rgb, vec3(0.0));
    return mix(pow((nonnegative + 0.055) / 1.055, vec3(2.4)), nonnegative / 12.92,
        lessThanEqual(nonnegative, vec3(0.04045)));
}

void vertex() {
    mat3 model_basis = mat3(MODEL_MATRIX);
    vec3 normal_model = normalize(NORMAL);
    vec3 tangent_model = normalize(TANGENT);
    vec3 binormal_model = CUSTOM0.x * tangent_model + CUSTOM0.y * normal_model
        + CUSTOM0.z * cross(normal_model, tangent_model);
    vec3 normal_world = normalize(model_basis * normal_model);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 fill = vec3(
        dot(normal_h, eawr_sph_fill_r * normal_h),
        dot(normal_h, eawr_sph_fill_g * normal_h),
        dot(normal_h, eawr_sph_fill_b * normal_h));
    vec3 stored_fill = clamp(eawr_diffuse * fill * eawr_light_scale.rgb * eawr_unit_light_scale + eawr_emissive,
        0.0, 1.0);
    eawr_fill = pow(2.0 * stored_fill, vec3(2.2));
    // Godot carries these to view space; the authored binormal replaces cross(N, T).
    NORMAL = normal_model;
    TANGENT = tangent_model;
    BINORMAL = binormal_model;
    UV += eawr_uv_offset;
}

void fragment() {
    if (eawr_unit_opacity < 1.0) {
        float eawr_dither = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
        if (eawr_dither >= eawr_unit_opacity) discard;
    }
    vec4 base = texture(BaseTexture, UV);
    vec4 normal_texel = texture(NormalTexture, UV);
    vec3 surface = mix(base.rgb, eawr_colorization * base.rgb, base.a);
    vec3 n = 2.0 * (normal_texel.rgb - 0.5);
    NORMAL = normalize(TANGENT * n.x + BINORMAL * n.y + NORMAL * n.z);
    ALBEDO = eawr_decode(surface);
    ROUGHNESS = mix(0.7, 0.35, normal_texel.a);
    METALLIC = 0.0;
    // Carries the gloss mask to light() as SPECULAR_AMOUNT (0.16 x SPECULAR).
    SPECULAR = normal_texel.a;
}

void light() {
    float n_dot_l = clamp(dot(NORMAL, LIGHT), 0.0, 1.0);
    vec3 sun = pow(2.0 * eawr_diffuse * eawr_light_diffuse * eawr_light_scale.rgb * eawr_unit_light_scale,
        vec3(2.2));
    // Retail darkens everything a shadow volume covers, fill included, by the
    // shadow floor (a stored-value scale, so its power 2.2 here).
    vec3 shadow = pow(mix(eawr_shadow_floor, vec3(1.0), ATTENUATION), vec3(2.2));
    DIFFUSE_LIGHT += (sun * n_dot_l * ATTENUATION + eawr_fill) * shadow;
    float gloss = SPECULAR_AMOUNT / 0.16;
    vec3 half_vector = normalize(VIEW + LIGHT);
    float n_dot_h = clamp(dot(NORMAL, half_vector), 0.0, 1.0);
    float n_dot_v = max(dot(NORMAL, VIEW), 1e-4);
    float alpha = ROUGHNESS * ROUGHNESS;
    float alpha2 = alpha * alpha;
    float denominator = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / (PI * denominator * denominator);
    float k = alpha * 0.5;
    float visibility = 0.25 / ((n_dot_l * (1.0 - k) + k) * (n_dot_v * (1.0 - k) + k));
    // Glossy panels reflect like clear-coated paint (F0 0.25), matte ones
    // like plain dielectric (0.04).
    float reflectance = mix(0.04, 0.25, gloss);
    float fresnel = reflectance + (1.0 - reflectance) * pow(1.0 - clamp(dot(half_vector, VIEW), 0.0, 1.0), 5.0);
    vec3 specular_light = pow(eawr_light_specular * eawr_specular, vec3(2.2));
    SPECULAR_LIGHT += specular_light * fresnel * distribution * visibility * n_dot_l * ATTENUATION;
}
)GODOT";

// The remastered source for a MeshBumpColorize or RSkinBumpColorize material,
// or an empty view for every other program. Both programs share it: Godot
// moves the authored frame through the skin before vertex() reads it.
[[nodiscard]] inline std::string_view shader_source(const MaterialDescription& material) noexcept {
    const bool hull = material.program == legacy::bump_colorize::mesh_family.program
        || material.program == legacy::bump_colorize::rskin_family.program;
    if (!hull || material.technique != "sph_t2" || material.pass_name != "sph_t2_p0"
        || material.pass != RenderPass::opaque) {
        return {};
    }
    return shader_head;
}

} // namespace eawr::presentation::godot_backend::remastered_hull
