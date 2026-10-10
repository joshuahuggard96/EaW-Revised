#pragma once

#include "legacy/mesh_additive.hpp"
#include "remastered_hull.hpp"
#include "shader_adapter.hpp"

#include <string>
#include <string_view>

// The remastered profile's Godot-lit materials (output_mode.hpp), beside the
// hull (remastered_hull.hpp). Not a retail look; only the linear output mode
// selects them.
//
// Every remastered material lights the same way (remastered_lighting): the
// environment's sun in light() (Lambert diffuse, GGX specular) under Godot's
// shadow, the fill lights per vertex, the retail shadow floor over both, and
// the captured backdrop as a roughness-blurred, Fresnel-weighted reflection
// with specular anti-aliasing. A material supplies its radiance calibration:
// the stored-value adapters (the hull) raise the retail terms to the power
// 2.2, the linear-policy adapters (MeshGlossColorize, the fixed MeshAlpha)
// already light in linear light and take them as they are, so each keeps the
// brightness its retail adapter has.
namespace eawr::presentation::godot_backend::remastered_materials {

// Declarations every remastered material shares. The shader defines
// eawr_sun_radiance() and eawr_specular_radiance() and fills eawr_fill.
inline constexpr std::string_view common_declarations = R"GODOT(
uniform vec3 eawr_light_diffuse = vec3(2.0, 1.88, 1.72);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
uniform vec3 eawr_shadow_floor = vec3(0.5);
uniform mat4 eawr_sph_fill_r;
uniform mat4 eawr_sph_fill_g;
uniform mat4 eawr_sph_fill_b;
// The backdrop cubemap (GodotRenderer::capture_backdrop); strength 0 until it exists.
uniform samplerCube eawr_backdrop : source_color, filter_linear_mipmap;
uniform float eawr_backdrop_strength = 0.0;
varying vec3 eawr_fill;

vec3 eawr_decode(vec3 stored_rgb) {
    vec3 nonnegative = max(stored_rgb, vec3(0.0));
    return mix(pow((nonnegative + 0.055) / 1.055, vec3(2.4)), nonnegative / 12.92,
        lessThanEqual(nonnegative, vec3(0.04045)));
}

// SPH_LIGHT_FILL irradiance (the fill lights and ambient) at a world normal.
vec3 eawr_fill_irradiance(vec3 normal_world) {
    vec4 normal_h = vec4(normal_world, 1.0);
    return vec3(dot(normal_h, eawr_sph_fill_r * normal_h), dot(normal_h, eawr_sph_fill_g * normal_h),
        dot(normal_h, eawr_sph_fill_b * normal_h));
}

// Specular anti-aliasing (Kaplanyan-Hoffman): widens the GGX lobe by the
// normal's screen-space variance, so detail finer than a pixel does not
// shimmer or form moire grids in highlights and reflections.
float eawr_filtered_roughness(float authored, vec3 normal_dx, vec3 normal_dy) {
    float variance = 0.25 * (dot(normal_dx, normal_dx) + dot(normal_dy, normal_dy));
    float alpha2 = clamp(authored * authored * authored * authored + min(2.0 * variance, 0.18), 0.0, 1.0);
    return sqrt(sqrt(alpha2));
}

// The share of a gloss mask a material keeps: its specular colour's
// strongest channel, at most 1. Hulls author Specular 1; asteroids a
// near-white mask with Specular 0.08, which retail's dim highlight hid.
float eawr_specular_strength(vec3 specular) {
    return clamp(max(max(specular.r, specular.g), specular.b), 0.0, 1.0);
}

// Glossy panels reflect like clear-coated paint (F0 0.25), matte ones like
// plain dielectric (0.04).
float eawr_reflectance(float gloss) {
    return mix(0.04, 0.25, gloss);
}

// The backdrop mirrored in the surface: blurrier with roughness, stronger at
// grazing angles (Schlick, damped by roughness), unshadowed by the sun.
vec3 eawr_backdrop_reflection(vec3 reflected_world, float n_dot_v, float roughness, float gloss) {
    float reflectance = eawr_reflectance(gloss);
    float fresnel = reflectance + (max(1.0 - roughness, reflectance) - reflectance) * pow(1.0 - n_dot_v, 5.0);
    return textureLod(eawr_backdrop, reflected_world, roughness * 7.0).rgb * fresnel * eawr_backdrop_strength;
}
)GODOT";

// The shared light(): SPECULAR carries the gloss mask (SPECULAR_AMOUNT is
// 0.16 x SPECULAR for a dielectric).
inline constexpr std::string_view common_light = R"GODOT(
void light() {
    if (!LIGHT_IS_DIRECTIONAL) {
        // Remastered explosion flashes (point lights): Lambert and a soft
        // highlight on the gloss mask, without the sun's fill or shadow floor.
        float point_n_dot_l = clamp(dot(NORMAL, LIGHT), 0.0, 1.0);
        float point_n_dot_h = clamp(dot(NORMAL, normalize(VIEW + LIGHT)), 0.0, 1.0);
        DIFFUSE_LIGHT += LIGHT_COLOR * ATTENUATION * point_n_dot_l;
        SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * point_n_dot_l * pow(point_n_dot_h, 24.0) * (SPECULAR_AMOUNT / 0.16) * 0.5;
    } else {
        float n_dot_l = clamp(dot(NORMAL, LIGHT), 0.0, 1.0);
        // Retail darkens everything a shadow volume covers, fill included, by the
        // shadow floor (a stored-value scale, so its power 2.2 here).
        vec3 shadow = pow(mix(eawr_shadow_floor, vec3(1.0), ATTENUATION), vec3(2.2));
        DIFFUSE_LIGHT += (eawr_sun_radiance() * n_dot_l * ATTENUATION + eawr_fill) * shadow;
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
        float reflectance = eawr_reflectance(gloss);
        float fresnel = reflectance + (1.0 - reflectance) * pow(1.0 - clamp(dot(half_vector, VIEW), 0.0, 1.0), 5.0);
        SPECULAR_LIGHT += eawr_specular_radiance() * fresnel * distribution * visibility * n_dot_l * ATTENUATION;
    }
}
)GODOT";

// The shared tail of fragment(): roughness from the gloss with specular
// anti-aliasing, the gloss for light(), and the backdrop reflection.
inline constexpr std::string_view common_surface = R"GODOT(
    ROUGHNESS = eawr_filtered_roughness(mix(0.7, 0.35, eawr_gloss), dFdx(NORMAL), dFdy(NORMAL));
    METALLIC = 0.0;
    SPECULAR = eawr_gloss;
    EMISSION += eawr_backdrop_reflection((INV_VIEW_MATRIX * vec4(reflect(-VIEW, NORMAL), 0.0)).xyz,
        clamp(dot(NORMAL, VIEW), 0.0, 1.0), ROUGHNESS, eawr_gloss);
}
)GODOT";

// MeshGlossColorize sph_t0 (legacy/mesh_gloss_colorize.hpp), opaque: the
// colorized base texel lit per pixel by the vertex normal; the GlossTexture's
// red channel is the gloss. Linear-policy calibration.
inline constexpr std::string_view gloss_colorize_head = R"GODOT(
shader_type spatial;
// eawr:linear-output
render_mode fog_disabled, depth_draw_opaque, depth_test_default, cull_back, ambient_light_disabled;

uniform sampler2D BaseTexture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D GlossTexture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec3 eawr_emissive = vec3(0.0);
uniform vec3 eawr_diffuse = vec3(1.0);
uniform vec3 eawr_specular = vec3(1.0);
uniform vec3 eawr_colorization = vec3(0.0, 1.0, 0.0);
uniform vec4 eawr_light_scale = vec4(1.0);
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
)GODOT";

inline constexpr std::string_view gloss_colorize_body = R"GODOT(
vec3 eawr_sun_radiance() {
    return 2.0 * eawr_diffuse * eawr_light_diffuse * eawr_light_scale.rgb;
}

vec3 eawr_specular_radiance() {
    return eawr_light_specular * eawr_specular;
}

void vertex() {
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    eawr_fill = 2.0 * eawr_diffuse * eawr_fill_irradiance(normal_world) * eawr_light_scale.rgb
        * eawr_unit_light_scale;
}

void fragment() {
    vec4 base = texture(BaseTexture, UV);
    vec3 base_linear = eawr_decode(base.rgb);
    ALBEDO = mix(base_linear, eawr_colorization * base_linear, base.a) * eawr_unit_light_scale;
    EMISSION = 2.0 * eawr_emissive * ALBEDO;
    float eawr_gloss = texture(GlossTexture, UV).r * eawr_specular_strength(eawr_specular);
)GODOT";

// The fixed-function MeshAlpha sph_t1 (shader_adapter.hpp
// fixed_mesh_shader_alpha), transparent: the base texel lit per pixel by the
// vertex normal and blended by its alpha. Retail authors no gloss, so it is
// matte (gloss 0). Linear-policy calibration.
inline constexpr std::string_view alpha_head = R"GODOT(
shader_type spatial;
// eawr:linear-output
render_mode fog_disabled, blend_mix, depth_draw_never, depth_test_default, cull_back, ambient_light_disabled;

uniform sampler2D BaseTexture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform vec4 eawr_light_scale = vec4(1.0);
)GODOT";

inline constexpr std::string_view alpha_body = R"GODOT(
vec3 eawr_sun_radiance() {
    return 2.0 * Diffuse.rgb * eawr_light_diffuse * eawr_light_scale.rgb;
}

vec3 eawr_specular_radiance() {
    return vec3(0.0);
}

void vertex() {
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    eawr_fill = 2.0 * Diffuse.rgb * eawr_fill_irradiance(normal_world) * eawr_light_scale.rgb;
}

void fragment() {
    vec4 base = texture(BaseTexture, UV);
    ALBEDO = eawr_decode(base.rgb);
    ALPHA = base.a * Diffuse.a * eawr_light_scale.a;
    EMISSION = 2.0 * Emissive.rgb * ALBEDO;
    float eawr_gloss = 0.0;
)GODOT";

// MeshGloss sph_t0 (shader_adapter.hpp meshgloss_shader_opaque), opaque:
// the base texel lit per pixel by the vertex normal; the base alpha is the
// gloss. Linear-policy calibration.
inline constexpr std::string_view gloss_head = R"GODOT(
shader_type spatial;
// eawr:linear-output
render_mode fog_disabled, depth_draw_opaque, depth_test_default, cull_back, ambient_light_disabled;

uniform sampler2D BaseTexture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform vec4 Specular = vec4(1.0);
uniform vec4 eawr_light_scale = vec4(1.0);
)GODOT";

inline constexpr std::string_view gloss_body = R"GODOT(
vec3 eawr_sun_radiance() {
    return 2.0 * Diffuse.rgb * eawr_light_diffuse * eawr_light_scale.rgb;
}

vec3 eawr_specular_radiance() {
    return eawr_light_specular * Specular.rgb;
}

void vertex() {
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    eawr_fill = 2.0 * Diffuse.rgb * eawr_fill_irradiance(normal_world) * eawr_light_scale.rgb;
}

void fragment() {
    vec4 base = texture(BaseTexture, UV);
    ALBEDO = eawr_decode(base.rgb);
    EMISSION = 2.0 * Emissive.rgb * ALBEDO;
    float eawr_gloss = base.a * eawr_specular_strength(Specular.rgb);
)GODOT";

// The opaque RSKIN adapter (shader_adapter.hpp rskin_shader_opaque) for
// RSkinGloss, RSkinGlossColorize and the lower bump techniques: skinned by
// Godot before vertex(); the base alpha both colorizes toward Colorization
// and is the gloss. Keeps the unit's light scale and screen-door opacity.
// Linear-policy calibration.
inline constexpr std::string_view rskin_head = R"GODOT(
shader_type spatial;
// eawr:linear-output
render_mode fog_disabled, depth_draw_opaque, depth_test_default, cull_back, ambient_light_disabled;

uniform sampler2D BaseTexture : filter_linear_mipmap_anisotropic, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform vec4 Specular = vec4(1.0);
uniform vec4 Colorization = vec4(0.0, 1.0, 0.0, 1.0);
uniform vec4 eawr_light_scale = vec4(1.0);
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
instance uniform float eawr_unit_opacity = 1.0;
)GODOT";

inline constexpr std::string_view rskin_body = R"GODOT(
vec3 eawr_sun_radiance() {
    return 2.0 * Diffuse.rgb * eawr_light_diffuse * eawr_light_scale.rgb * eawr_unit_light_scale;
}

vec3 eawr_specular_radiance() {
    return eawr_light_specular * Specular.rgb;
}

void vertex() {
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    eawr_fill = 2.0 * Diffuse.rgb * eawr_fill_irradiance(normal_world) * eawr_light_scale.rgb
        * eawr_unit_light_scale;
}

void fragment() {
    if (eawr_unit_opacity < 1.0) {
        float eawr_dither = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
        if (eawr_dither >= eawr_unit_opacity) discard;
    }
    vec4 base = texture(BaseTexture, UV);
    ALBEDO = mix(eawr_decode(base.rgb), Colorization.rgb, base.a);
    EMISSION = 2.0 * Emissive.rgb * ALBEDO;
    float eawr_gloss = base.a * eawr_specular_strength(Specular.rgb);
)GODOT";

[[nodiscard]] inline const std::string& gloss_source() {
    static const std::string source = std::string(gloss_head) + std::string(common_declarations)
        + std::string(gloss_body) + std::string(common_surface) + std::string(common_light);
    return source;
}

[[nodiscard]] inline const std::string& rskin_source() {
    static const std::string source = std::string(rskin_head) + std::string(common_declarations)
        + std::string(rskin_body) + std::string(common_surface) + std::string(common_light);
    return source;
}

[[nodiscard]] inline const std::string& gloss_colorize_source() {
    static const std::string source = std::string(gloss_colorize_head) + std::string(common_declarations)
        + std::string(gloss_colorize_body) + std::string(common_surface) + std::string(common_light);
    return source;
}

[[nodiscard]] inline const std::string& alpha_source() {
    static const std::string source = std::string(alpha_head) + std::string(common_declarations)
        + std::string(alpha_body) + std::string(common_surface) + std::string(common_light);
    return source;
}

// MeshAdditive t0 (legacy/mesh_additive.hpp): the retail additive glow,
// decoded to linear light and scaled by eawr_glow (--eawr-glow), so engines,
// windows and running lights rise above the glow threshold and bloom. The
// retail adapter clamps its colour to 1, which keeps them below it.
[[nodiscard]] inline const std::string& additive_source() {
    static const std::string source = [] {
        std::string text = linear_output_source(std::string(legacy::mesh_additive::shader_source))
            .value_or(std::string{});
        constexpr std::string_view decode = "    ALBEDO = eawr_linear_output(ALBEDO);\n";
        const std::size_t at = text.rfind(decode);
        if (at == std::string::npos) return std::string{};
        text.replace(at, decode.size(), "    ALBEDO = eawr_linear_output(ALBEDO) * eawr_glow;\n");
        const std::size_t declarations = text.find("uniform ");
        text.insert(declarations, std::string(linear_source_marker) + "\nuniform float eawr_glow = 1.0;\n");
        return text;
    }();
    return source;
}

// The remastered source for a legacy material, or an empty view for one that
// keeps its retail adapter in the linear frame.
[[nodiscard]] inline std::string_view shader_source(const MaterialDescription& material) {
    if (const std::string_view hull = remastered_hull::shader_source(material); !hull.empty()) return hull;
    if (material.program == "MeshGlossColorize.fx" && material.technique == "sph_t0"
        && material.pass_name == "sph_t0_p0" && material.pass == RenderPass::opaque) {
        return gloss_colorize_source();
    }
    if (material.program == "MeshAlpha.fx" && material.technique == "sph_t1" && material.pass_name == "sph_t1_p0"
        && material.pass == RenderPass::transparent) {
        return alpha_source();
    }
    if (material.program == legacy::mesh_additive::family.program && material.technique == "t0"
        && material.pass_name == "t0_p0" && material.pass == RenderPass::transparent) {
        return additive_source();
    }
    if (material.pass != RenderPass::opaque) return {};
    const auto is = [&material](const std::string_view program, const std::string_view technique) {
        return material.program == program && material.technique == technique
            && material.pass_name == std::string(technique) + "_p0";
    };
    if (is("MeshGloss.fx", "sph_t0")) return gloss_source();
    // The selectors GodotRenderer::Impl::legacy_shader_source routes to the RSKIN adapter.
    if (is("RSkinBumpColorize.fx", "sph_t0") || is("RSkinGloss.fx", "sph_t1") || is("RSkinGlossColorize.fx", "sph_t0")
        || is("MeshBumpColorize.fx", "t0")) {
        return rskin_source();
    }
    return {};
}

} // namespace eawr::presentation::godot_backend::remastered_materials
