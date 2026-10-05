#include "legacy_family_support.hpp"
// CPU contracts for the legacy/ family adapters (WP-43): exact selectors,
// fail-closed diagnostics, binding validation and defaults, derived uniforms,
// adapter source render states and the engine-free reference arithmetic.
#include "eawr/presentation/renderer.hpp"

#include "legacy/registry.hpp"
#include "shader_adapter.hpp"
#include "../../../apps/viewer/src/family_textures.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace legacy_family_test_support {


int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

[[nodiscard]] bool near(const float left, const float right, const float tolerance) {
    return std::fabs(left - right) <= tolerance;
}

[[nodiscard]] MaterialDescription selector(const legacy::Family& family, const RenderPass pass) {
    return MaterialDescription{
        .schema_version = MaterialDescription::current_schema_version,
        .route = presentation::MaterialRoute::legacy_effect,
        .pass = pass,
        .program = std::string(family.program),
        .technique = std::string(family.technique),
        .pass_name = std::string(family.pass_name),
        .bindings = {},
    };
}

// The selector with the fewest bindings the family admits: every family
// keeps the effect initializers for unauthored parameters, except that
// MeshGlossColorize and MeshAlphaGloss need a GlossTexture and the bump
// colorize pair a NormalTexture.
[[nodiscard]] MaterialDescription admissible(const legacy::Family& family, const RenderPass pass) {
    MaterialDescription material = selector(family, pass);
    if (family.program == "MeshGlossColorize.fx") {
        material.bindings = {{"BaseTexture", std::string("base.tga")}, {"GlossTexture", std::string("base.tga")}};
    }
    if (family.authored_binormals) {
        material.bindings = {{"BaseTexture", std::string("hull.dds")}, {"NormalTexture", std::string("hull_b.dds")}};
    }
    if (family.program == "MeshAlphaGloss.fx") {
        material.bindings = {{"BaseTexture", std::string("rock.tga")}, {"GlossTexture", std::string("rock_gloss.tga")}};
    }
    return material;
}

[[nodiscard]] RenderPass admitted_pass(const legacy::Family& family) {
    return family.opaque ? RenderPass::opaque : RenderPass::transparent;
}

[[nodiscard]] std::string rejection(const MaterialDescription& material) {
    const auto result = presentation::validate_material(material);
    return result ? std::string{} : result.error().code + " " + result.error().message;
}

[[nodiscard]] std::string_view glsl_type(const legacy::UniformValue& value) {
    switch (value.index()) {
    case 0: return "float";
    case 1: return "vec2";
    case 2: return "vec3";
    default: return "vec4";
    }
}

[[nodiscard]] const legacy::Uniform* uniform(const std::vector<legacy::Uniform>& uniforms, const std::string_view name) {
    for (const legacy::Uniform& item : uniforms) {
        if (item.name == name) return &item;
    }
    return nullptr;
}


} // namespace


using namespace legacy_family_test_support;

// The remastered profile's linear output (output_mode.hpp): every legacy
// source decodes ALBEDO once, at the end of fragment(), and is otherwise as
// written; sources the transform must leave alone stay byte-identical.
void linear_output_contracts() {
    const std::string decode = "ALBEDO = eawr_linear_output(ALBEDO);";
    for (const legacy::Family& family : legacy::registry()) {
        const std::string name(family.program);
        const std::string source(family.shader(admitted_pass(family)));
        const std::optional<std::string> linear = godot_backend::linear_output_source(source);
        check(linear.has_value(), name + " has a linear-output form");
        if (!linear) continue;
        const std::size_t first = linear->find(decode);
        check(first != std::string::npos && linear->find(decode, first + 1) == std::string::npos,
            name + " decodes ALBEDO exactly once");
        check(linear->find("vec3 eawr_linear_output(vec3 stored_rgb)") < linear->find("void fragment()"),
            name + " declares the decoder before fragment()");
        check(first != std::string::npos && linear->find_first_not_of(" \n", first + decode.size()) != std::string::npos
                && (*linear)[linear->find_first_not_of(" \n", first + decode.size())] == '}',
            name + " decodes as the last statement of fragment()");
    }
    const std::string ui = "shader_type canvas_item;\nvoid fragment() { COLOR = vec4(1.0); }\n";
    check(godot_backend::linear_output_source(ui) == ui, "a canvas_item shader is compiled as written");
    const std::string screen = "shader_type spatial;\nuniform sampler2D s : hint_screen_texture;\n"
                               "void fragment() { ALBEDO = texture(s, SCREEN_UV).rgb; }\n";
    check(godot_backend::linear_output_source(screen) == screen, "a screen-texture reader is compiled as written");
    const std::string marked = "shader_type spatial;\n// eawr:linear-output\nvoid fragment() { ALBEDO = vec3(0.5); }\n";
    check(godot_backend::linear_output_source(marked) == marked, "a marked linear source is compiled as written");
    const std::string early = "shader_type spatial;\nvoid fragment() { if (UV.x > 0.5) { ALBEDO = vec3(1.0); return; } "
                              "ALBEDO = vec3(0.0); }\n";
    const auto guarded = godot_backend::linear_output_source(early);
    check(guarded && guarded->find("{ " + decode + " return; }") != std::string::npos,
        "an early return decodes before it leaves fragment()");
    check(!godot_backend::linear_output_source("shader_type spatial;\nvoid fragment() { ALBEDO = vec3(1.0);\n"),
        "an unbalanced fragment() has no linear form");
}

int main() {
    texture_placeholder_contracts();
    ownership_colorization_contracts();
    registry_contracts();
    derived_fog_contracts();
    source_contracts();
    selector_contracts();
    binding_contracts();
    gloss_colorize_contracts();
    vegetation_contracts();
    reference_contracts();
    bump_colorize_contracts();
    dx8_mesh_contracts();
    linear_output_contracts();
    if (failures != 0) {
        std::cerr << failures << " legacy family contract(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "legacy family contracts passed\n";
    return EXIT_SUCCESS;
}
