#include "eawr/presentation/godot/renderer.hpp"

#include "renderer_internal.hpp"
#include "particle_culling.hpp"

namespace eawr::presentation::godot_backend {
namespace {

// The remastered frame (output_mode.hpp): Reinhard with white 6 rolls
// overbright specular and additive terms off instead of clipping them, and,
// having no toe, keeps dark hulls and the backdrop where the stored-value
// frame has them. On M2 Coruscant ACES needed an exposure that made lit hulls
// a quarter brighter to lift the backdrop, AgX greyed the nebulae and Filmic
// lifted the blacks. The exposure is the player's (--eawr-exposure).
void apply_linear_tonemap(RenderingServer& rendering, const RID& environment) {
    rendering.environment_set_tonemap(
        environment, RenderingServer::ENV_TONE_MAPPER_REINHARD, linear_exposure(), 6.0F);
}

// Godot's glow standing in for the retail SceneBloom: the scene's cutoff is
// the HDR threshold and 1.5 times its strength the intensity, since the
// tonemapper's shoulder dims a halo the retail pass added after saturation.
void apply_linear_glow(
    RenderingServer& rendering, const RID& environment, const std::optional<lighting::bloom::SceneBloom>& bloom) {
    PackedFloat32Array levels;
    for (const float level : {0.0F, 0.0F, 1.0F, 0.6F, 0.3F, 0.0F, 0.0F}) levels.push_back(level);
    constexpr float scale = 1.5F;
    rendering.environment_set_glow(environment, bloom.has_value(), levels,
        bloom ? bloom->strength * scale : 0.0F, 1.0F, 0.0F, 0.0F, RenderingServer::ENV_GLOW_BLEND_MODE_SOFTLIGHT,
        bloom ? bloom->cutoff : 1.0F, 1.0F, 12.0F, 0.0F, RID());
}

// Cubemap face cameras in Godot's layer order (+X, -X, +Y, -Y, +Z, -Z): the
// forward axis and the up that puts each face's t axis down the image. A
// face also mirrors left to right (the cube is seen from inside), so each
// image is flipped before upload.
struct CubeFace final {
    Vector3 forward;
    Vector3 up;
};
constexpr std::array<CubeFace, 6> cube_faces{{
    {Vector3(1, 0, 0), Vector3(0, 1, 0)},
    {Vector3(-1, 0, 0), Vector3(0, 1, 0)},
    {Vector3(0, 1, 0), Vector3(0, 0, -1)},
    {Vector3(0, -1, 0), Vector3(0, 0, 1)},
    {Vector3(0, 0, 1), Vector3(0, 1, 0)},
    {Vector3(0, 0, -1), Vector3(0, 1, 0)},
}};
constexpr std::int32_t backdrop_face_size = 512;

} // namespace


[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

[[nodiscard]] Transform3D transform_from(const std::array<float, 16>& matrix) {
    return Transform3D(
        Basis(
            Vector3(matrix[0], matrix[1], matrix[2]),
            Vector3(matrix[4], matrix[5], matrix[6]),
            Vector3(matrix[8], matrix[9], matrix[10])),
        Vector3(matrix[12], matrix[13], matrix[14]));
}

[[nodiscard]] Projection godot_matrix(const SphChannelMatrix& matrix) {
    return Projection(
        Vector4(matrix.columns[0][0], matrix.columns[0][1], matrix.columns[0][2], matrix.columns[0][3]),
        Vector4(matrix.columns[1][0], matrix.columns[1][1], matrix.columns[1][2], matrix.columns[1][3]),
        Vector4(matrix.columns[2][0], matrix.columns[2][1], matrix.columns[2][2], matrix.columns[2][3]),
        Vector4(matrix.columns[3][0], matrix.columns[3][1], matrix.columns[3][2], matrix.columns[3][3]));
}

[[nodiscard]] Projection godot_matrix(const std::array<float, 16>& matrix) {
    return Projection(
        Vector4(matrix[0], matrix[1], matrix[2], matrix[3]),
        Vector4(matrix[4], matrix[5], matrix[6], matrix[7]),
        Vector4(matrix[8], matrix[9], matrix[10], matrix[11]),
        Vector4(matrix[12], matrix[13], matrix[14], matrix[15]));
}

CachedShader::~CachedShader() {
    if (auto* rendering = RenderingServer::get_singleton(); rendering && shader.is_valid())
        rendering->free_rid(shader);
}

GodotShaderCache::GodotShaderCache() : impl_(std::make_unique<Impl>()) {}
GodotShaderCache::~GodotShaderCache() = default;
GodotShaderCache::Stats GodotShaderCache::stats() const noexcept {
    return {impl_->entries.size(), impl_->source_bytes};
}

GodotRenderer::Impl::Impl(Node3D& owner, std::shared_ptr<GodotShaderCache> shaders)
    : owner_(owner), shader_cache_(std::move(shaders)) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || owner_.get_world_3d().is_null() || owner_.get_viewport() == nullptr) {
        fail(diagnostic_codes::backend_unavailable,
            "Godot RenderingServer, world, or viewport is unavailable");
        return;
    }
    scenario_ = owner_.get_world_3d()->get_scenario();
    viewport_ = owner_.get_viewport()->get_viewport_rid();
    camera_ = rendering->camera_create();
    rendering->camera_set_perspective(camera_, 45.0F, 1.0F, 20000.0F);
    Transform3D fixed_camera;
    fixed_camera.origin = Vector3(0.0F, 420.0F, 1050.0F);
    fixed_camera = fixed_camera.looking_at(Vector3(), Vector3(0.0F, 1.0F, 0.0F));
    rendering->camera_set_transform(camera_, fixed_camera);
    view_transform_ = fixed_camera;
    rendering->viewport_attach_camera(viewport_, camera_);
    environment_ = rendering->environment_create();
    rendering->environment_set_background(environment_, RenderingServer::ENV_BG_COLOR);
    // The stored-value pass decodes the colour buffer once more after the
    // RenderingDevice clear, so there the clear colour is encoded once more.
    // A linear frame is never decoded and takes the linear value.
    const auto background = [](const float linear) {
        if (stored_output::linear()) return linear;
        const float stored = linear_to_srgb_component(linear);
        return stored_output::active() ? linear_to_srgb_component(stored) : stored;
    };
    rendering->environment_set_bg_color(environment_, Color(
        background(0.006F), background(0.008F), background(0.015F), 1.0));
    // Colour policy (docs/rendering.md): the linear tonemapper at exposure 1 /
    // white 1 and the environment's defaults, without glow, adjustments or
    // auto-exposure, so the output pixel is the stored value.
    rendering->environment_set_tonemap(
        environment_, RenderingServer::ENV_TONE_MAPPER_LINEAR, 1.0, 1.0);
    if (stored_output::linear()) apply_linear_tonemap(*rendering, environment_);
    rendering->scenario_set_environment(scenario_, environment_);
    stored_compositor_ = stored_output::create(*rendering);
    if (stored_compositor_.compositor.is_valid()) {
        rendering->scenario_set_compositor(scenario_, stored_compositor_.compositor);
    }
}

GodotRenderer::Impl::~Impl() {
    particle_culling::clear_camera(scenario_);
    // Fog bindings go first, while every material RID is still valid.
    disable_fog();
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return;
    for (const auto& [entity, instance] : instances_) {
        static_cast<void>(entity);
        if (instance.rid.is_valid()) rendering->free_rid(instance.rid);
        if (instance.skeleton.is_valid()) rendering->free_rid(instance.skeleton);
    }
    for (const auto& [asset, resource] : resources_) {
        static_cast<void>(asset);
        free_resource(*rendering, resource);
    }
    for (const auto& [name, texture] : shared_textures_) {
        static_cast<void>(name);
        if (texture.is_valid()) rendering->free_rid(texture);
    }
    if (camera_.is_valid()) rendering->free_rid(camera_);
    if (environment_.is_valid()) rendering->free_rid(environment_);
    stored_output::release(*rendering, stored_compositor_);
    if (backdrop_capture_) {
        for (const RID& viewport : backdrop_capture_->viewports) rendering->free_rid(viewport);
        for (const RID& camera : backdrop_capture_->cameras) rendering->free_rid(camera);
        rendering->free_rid(backdrop_capture_->environment);
        rendering->free_rid(backdrop_capture_->compositor);
    }
    if (backdrop_cubemap_.is_valid()) rendering->free_rid(backdrop_cubemap_);
    if (sun_instance_.is_valid()) rendering->free_rid(sun_instance_);
    if (sun_light_.is_valid()) rendering->free_rid(sun_light_);
}

void GodotRenderer::Impl::apply_lighting_params(RenderingServer& rendering, const RID& material) const {
    if (!lighting_) return;
    rendering.material_set_param(material, StringName("eawr_sph_r"), godot_matrix(lighting_->sph[0]));
    rendering.material_set_param(material, StringName("eawr_sph_g"), godot_matrix(lighting_->sph[1]));
    rendering.material_set_param(material, StringName("eawr_sph_b"), godot_matrix(lighting_->sph[2]));
    rendering.material_set_param(material, StringName("eawr_sph_fill_r"), godot_matrix(lighting_->sph_fill[0]));
    rendering.material_set_param(material, StringName("eawr_sph_fill_g"), godot_matrix(lighting_->sph_fill[1]));
    rendering.material_set_param(material, StringName("eawr_sph_fill_b"), godot_matrix(lighting_->sph_fill[2]));
    rendering.material_set_param(material, StringName("eawr_light_diffuse"),
        Vector3(lighting_->sun_diffuse[0], lighting_->sun_diffuse[1], lighting_->sun_diffuse[2]));
    rendering.material_set_param(material, StringName("eawr_light_direction"),
        Vector3(lighting_->toward_light[0], lighting_->toward_light[1], lighting_->toward_light[2]));
    rendering.material_set_param(material, StringName("eawr_light_specular"),
        Vector3(lighting_->specular[0], lighting_->specular[1], lighting_->specular[2]));
    rendering.material_set_param(material, StringName("eawr_shadow_floor"),
        Vector3(lighting_->shadow_floor[0], lighting_->shadow_floor[1], lighting_->shadow_floor[2]));
    rendering.material_set_param(material, StringName("eawr_glow"), glow_strength());
    if (backdrop_cubemap_.is_valid()) {
        rendering.material_set_param(material, StringName(backdrop_parameter.data()), backdrop_cubemap_);
        rendering.material_set_param(material, StringName("eawr_backdrop_strength"), backdrop_reflections());
    }
}

void GodotRenderer::Impl::apply_wind_params(
    RenderingServer& rendering, const RID& material, const MaterialDescription& source) const {
    if (!wind_ || !legacy::reads_wind(source)) return;
    rendering.material_set_param(material, StringName("eawr_wind"),
        Vector3(wind_->wind[0], wind_->wind[1], wind_->wind[2]));
    rendering.material_set_param(material, StringName("eawr_scene_time"), wind_->scene_time);
}

void GodotRenderer::Impl::set_wind(const GodotRenderer::WindState& wind) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || (wind_ && *wind_ == wind)) return;
    wind_ = wind;
    for (const auto& [asset, resource] : resources_) {
        if (resource.material.is_valid()) apply_wind_params(*rendering, resource.material, resource.description);
        const auto consumer = fog_consumers_.find(asset);
        if (consumer != fog_consumers_.end() && consumer->second.shader.is_valid()) {
            apply_wind_params(*rendering, consumer->second.material, resource.description);
        }
    }
}

void GodotRenderer::Impl::set_scene_bloom(const std::optional<lighting::bloom::SceneBloom>& bloom) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering && stored_output::linear()) {
        // A linear frame glows with Godot's HDR glow at the scene's strength
        // and cutoff; the retail pass stays off.
        apply_linear_glow(*rendering, environment_, bloom);
        scene_bloom_active_ = bloom.has_value();
        return;
    }
    if (!rendering || !stored_compositor_.bloom.is_valid()) return;
    scene_bloom::configure(*rendering, stored_compositor_.bloom, bloom);
    scene_bloom_active_ = bloom.has_value();
}

void GodotRenderer::Impl::set_lighting(const GodotRenderer::LightingState& lighting) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !scenario_.is_valid()) return;
    // RenderingServer cannot restore an unset light parameter. Replace a
    // previously tuned light when a later scene requests the defaults.
    if (lighting_ && ((lighting_->shadow_bias && !lighting.shadow_bias)
                      || (lighting_->shadow_normal_bias && !lighting.shadow_normal_bias)
                      || (lighting_->shadow_blur && !lighting.shadow_blur))) {
        if (sun_instance_.is_valid()) rendering->free_rid(sun_instance_);
        if (sun_light_.is_valid()) rendering->free_rid(sun_light_);
        sun_instance_ = RID();
        sun_light_ = RID();
    }
    lighting_ = lighting;
    for (const auto& [asset, resource] : resources_) {
        static_cast<void>(asset);
        if (resource.material.is_valid()) apply_lighting_params(*rendering, resource.material);
    }
    for (const auto& [asset, consumer] : fog_consumers_) {
        static_cast<void>(asset);
        if (consumer.shader.is_valid()) apply_lighting_params(*rendering, consumer.material);
    }
    if (!sun_light_.is_valid()) {
        sun_light_ = rendering->directional_light_create();
        sun_instance_ = rendering->instance_create2(sun_light_, scenario_);
    }
    rendering->light_set_color(sun_light_, Color(1.0F, 1.0F, 1.0F));
    rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_ENERGY, 1.0);
    switch (lighting.shadow_layout) {
    case GodotRenderer::ShadowLayout::orthogonal:
        rendering->light_directional_set_shadow_mode(
            sun_light_, RenderingServer::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL);
        break;
    case GodotRenderer::ShadowLayout::parallel_2_splits:
        rendering->light_directional_set_shadow_mode(
            sun_light_, RenderingServer::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_2_SPLITS);
        break;
    case GodotRenderer::ShadowLayout::parallel_4_splits:
        rendering->light_directional_set_shadow_mode(
            sun_light_, RenderingServer::LIGHT_DIRECTIONAL_SHADOW_PARALLEL_4_SPLITS);
        break;
    }
    if (lighting.shadow_layout != GodotRenderer::ShadowLayout::orthogonal) {
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_SPLIT_1_OFFSET,
            lighting.shadow_split_offsets[0]);
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_SPLIT_2_OFFSET,
            lighting.shadow_split_offsets[1]);
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_SPLIT_3_OFFSET,
            lighting.shadow_split_offsets[2]);
    }
    rendering->light_directional_set_blend_splits(sun_light_, lighting.shadow_blend_splits);
    rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_MAX_DISTANCE,
        lighting.shadow_max_distance);
    rendering->directional_shadow_atlas_set_size(lighting.shadow_atlas_size, true);
    switch (lighting.shadow_filter) {
    case GodotRenderer::ShadowFilter::soft_low:
        rendering->directional_soft_shadow_filter_set_quality(RenderingServer::SHADOW_QUALITY_SOFT_LOW);
        break;
    case GodotRenderer::ShadowFilter::soft_medium:
        rendering->directional_soft_shadow_filter_set_quality(RenderingServer::SHADOW_QUALITY_SOFT_MEDIUM);
        break;
    case GodotRenderer::ShadowFilter::soft_high:
        rendering->directional_soft_shadow_filter_set_quality(RenderingServer::SHADOW_QUALITY_SOFT_HIGH);
        break;
    case GodotRenderer::ShadowFilter::soft_ultra:
        rendering->directional_soft_shadow_filter_set_quality(RenderingServer::SHADOW_QUALITY_SOFT_ULTRA);
        break;
    }
    if (lighting.shadow_bias) {
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_BIAS, *lighting.shadow_bias);
    }
    if (lighting.shadow_normal_bias) {
        rendering->light_set_param(
            sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_NORMAL_BIAS, *lighting.shadow_normal_bias);
    }
    if (lighting.shadow_blur) {
        rendering->light_set_param(sun_light_, RenderingServer::LIGHT_PARAM_SHADOW_BLUR, *lighting.shadow_blur);
    }
    rendering->light_set_shadow(sun_light_, lighting.shadows);
    const Vector3 toward(lighting.toward_light[0], lighting.toward_light[1], lighting.toward_light[2]);
    // A directional light shines along its local -Z.
    const Vector3 travel = -toward.normalized();
    const Vector3 up = std::abs(travel.y) > 0.99F ? Vector3(0.0F, 0.0F, 1.0F) : Vector3(0.0F, 1.0F, 0.0F);
    Transform3D transform;
    transform = transform.looking_at(travel, up);
    rendering->instance_set_transform(sun_instance_, transform);
}

void GodotRenderer::Impl::set_shadows_enabled(const bool enabled) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !sun_light_.is_valid()) return;
    rendering->light_set_shadow(sun_light_, enabled);
}

void GodotRenderer::Impl::set_casts_shadows(const sim::AssetId asset_id, const bool casts) {
    if (casts) non_casting_.erase(asset_id);
    else non_casting_.insert(asset_id);
}

void GodotRenderer::Impl::set_backdrop(const sim::AssetId asset_id) {
    backdrop_assets_.insert(asset_id);
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return;
    for (const auto& [entity, instance] : instances_) {
        static_cast<void>(entity);
        if (instance.asset_id == asset_id) rendering->instance_set_layer_mask(instance.rid, 1U | backdrop_layer);
    }
}

void GodotRenderer::Impl::capture_backdrop(const std::array<float, 3>& eye) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    // No reflection strength, nothing to capture.
    if (!rendering || !stored_output::linear() || backdrop_reflections() <= 0.0F || backdrop_capture_
        || !scenario_.is_valid()) {
        return;
    }
    BackdropCapture capture;
    // Linear light out, untonemapped: the 8-bit viewport stores it sRGB
    // encoded and the hull's source_color sampler decodes it. An empty
    // compositor keeps the scene bloom out of the faces.
    capture.environment = rendering->environment_create();
    rendering->environment_set_background(capture.environment, RenderingServer::ENV_BG_COLOR);
    rendering->environment_set_bg_color(capture.environment, Color(0.006F, 0.008F, 0.015F, 1.0F));
    rendering->environment_set_tonemap(capture.environment, RenderingServer::ENV_TONE_MAPPER_LINEAR, 1.0, 1.0);
    capture.compositor = rendering->compositor_create();
    const Vector3 origin(eye[0], eye[1], eye[2]);
    for (std::size_t face = 0; face < cube_faces.size(); ++face) {
        const RID camera = rendering->camera_create();
        rendering->camera_set_perspective(camera, 90.0F, 1.0F, camera_far_);
        Transform3D transform;
        transform.origin = origin;
        rendering->camera_set_transform(camera, transform.looking_at(origin + cube_faces[face].forward, cube_faces[face].up));
        rendering->camera_set_cull_mask(camera, backdrop_layer);
        rendering->camera_set_environment(camera, capture.environment);
        rendering->camera_set_compositor(camera, capture.compositor);
        const RID viewport = rendering->viewport_create();
        rendering->viewport_set_size(viewport, backdrop_face_size, backdrop_face_size);
        rendering->viewport_set_scenario(viewport, scenario_);
        rendering->viewport_attach_camera(viewport, camera);
        rendering->viewport_set_update_mode(viewport, RenderingServer::VIEWPORT_UPDATE_ONCE);
        rendering->viewport_set_active(viewport, true);
        capture.cameras[face] = camera;
        capture.viewports[face] = viewport;
    }
    backdrop_capture_ = capture;
}

void GodotRenderer::Impl::update_backdrop() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !backdrop_capture_) return;
    // A ONCE viewport renders with the next frame; read it two frames later.
    if (++backdrop_capture_->frames < 3) return;
    TypedArray<Image> faces;
    for (const RID& viewport : backdrop_capture_->viewports) {
        Ref<Image> image = rendering->texture_2d_get(rendering->viewport_get_texture(viewport));
        if (image.is_null() || image->is_empty()) break;
        image->convert(Image::FORMAT_RGBA8);
        image->flip_x();
        image->generate_mipmaps();
        faces.push_back(image);
    }
    for (const RID& viewport : backdrop_capture_->viewports) rendering->free_rid(viewport);
    for (const RID& camera : backdrop_capture_->cameras) rendering->free_rid(camera);
    rendering->free_rid(backdrop_capture_->environment);
    rendering->free_rid(backdrop_capture_->compositor);
    backdrop_capture_.reset();
    if (faces.size() != static_cast<int64_t>(cube_faces.size())) return;
    if (backdrop_cubemap_.is_valid()) rendering->free_rid(backdrop_cubemap_);
    backdrop_cubemap_ = rendering->texture_2d_layered_create(faces, RenderingServer::TEXTURE_LAYERED_CUBEMAP);
    if (!lighting_) return;
    for (const auto& [asset, resource] : resources_) {
        static_cast<void>(asset);
        if (resource.material.is_valid()) apply_lighting_params(*rendering, resource.material);
    }
}

void GodotRenderer::Impl::set_camera(const FixedCamera& camera) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !camera_.is_valid()) return;
    rendering->camera_set_perspective(
        camera_, camera.vertical_fov_degrees, camera.near_plane, camera.far_plane);
    camera_far_ = camera.far_plane;
    Transform3D camera_transform;
    camera_transform.origin = Vector3(camera.eye[0], camera.eye[1], camera.eye[2]);
    camera_transform = camera_transform.looking_at(
        Vector3(camera.target[0], camera.target[1], camera.target[2]),
        Vector3(camera.up[0], camera.up[1], camera.up[2]));
    rendering->camera_set_transform(camera_, camera_transform);
    particle_culling::set_camera(scenario_, camera);
    view_transform_ = camera_transform;
    for (auto& [entity, instance] : instances_) {
        const auto resource = resources_.find(instance.asset_id);
        if (resource != resources_.end() && !resource->second.billboard_modes.empty()) {
            refresh_billboards(*rendering, entity, instance, resource->second);
        }
    }
}

[[nodiscard]] core::Result<CaptureResult> GodotRenderer::Impl::capture(const FixedCamera& camera) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering || !camera_.is_valid()) {
        return core::Result<CaptureResult>::failure(make_diagnostic(
            diagnostic_codes::capture_failed, "capture camera is unavailable"));
    }
    set_camera(camera);
    const Ref<ViewportTexture> viewport_texture = owner_.get_viewport()->get_texture();
    const Ref<Image> image = viewport_texture.is_valid() ? viewport_texture->get_image() : Ref<Image>();
    if (image.is_null() || image->is_empty()) {
        return core::Result<CaptureResult>::failure(make_diagnostic(
            diagnostic_codes::capture_failed, "Godot returned an empty viewport image"));
    }
    const PackedByteArray packed = image->save_png_to_buffer();
    if (packed.is_empty()) {
        return core::Result<CaptureResult>::failure(make_diagnostic(
            diagnostic_codes::capture_failed, "Godot failed to encode fixed-camera PNG"));
    }
    std::vector<std::byte> png_bytes(static_cast<std::size_t>(packed.size()));
    std::memcpy(png_bytes.data(), packed.ptr(), png_bytes.size());
    return core::Result<CaptureResult>::success(CaptureResult{
        .png_bytes = std::move(png_bytes),
        .width = static_cast<std::uint32_t>(image->get_width()),
        .height = static_cast<std::uint32_t>(image->get_height()),
    });
}

[[nodiscard]] BackendInfo GodotRenderer::Impl::backend_info() const {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return {"Godot", "unavailable", {}, {}, {}};
    // The running method: --rendering-method overrides the project setting.
    return {
        "Godot 4.7.2-stable",
        utf8(rendering->get_current_rendering_method()),
        utf8(rendering->get_video_adapter_vendor()),
        utf8(rendering->get_video_adapter_name()),
        utf8(rendering->get_video_adapter_api_version()),
    };
}

[[nodiscard]] core::Diagnostic GodotRenderer::Impl::make_diagnostic(
    const std::string_view code, std::string message) const {
    BackendInfo info = backend_info();
    if (!info.adapter_name.empty()) {
        message += " [" + info.rendering_method + "; " + info.adapter_vendor + " "
            + info.adapter_name + "; " + info.driver_api + "]";
    }
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("presentation.godot"),
    };
}

// fail(), returning the recorded diagnostic.
[[nodiscard]] core::Diagnostic GodotRenderer::Impl::pushed(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic = make_diagnostic(code, std::move(message));
    diagnostics_.push(diagnostic);
    return diagnostic;
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic = make_diagnostic(code, std::move(message));
    diagnostics_.push(diagnostic);
    return core::Result<void>::failure(std::move(diagnostic));
}

GodotRenderer::GodotRenderer(Node3D& host, std::shared_ptr<GodotShaderCache> shaders)
    : impl_(std::make_unique<Impl>(host, std::move(shaders))) {}
GodotRenderer::~GodotRenderer() = default;
GodotRenderer::GodotRenderer(GodotRenderer&&) noexcept = default;
GodotRenderer& GodotRenderer::operator=(GodotRenderer&&) noexcept = default;

core::Result<void> GodotRenderer::upload(
    const sim::AssetId asset_id,
    const assets::Model& model,
    const assets::Texture& texture,
    const MaterialDescription& material) {
    return impl_->upload(asset_id, model, texture, material);
}

core::Result<void> GodotRenderer::upload(
    const sim::AssetId asset_id,
    const assets::Model& model,
    const assets::Texture& texture,
    const MaterialDescription& material,
    const std::span<const BindingTexture> binding_textures) {
    return impl_->upload(asset_id, model, texture, material, binding_textures);
}

core::Result<void> GodotRenderer::register_shared_texture(
    const std::string_view name, const assets::Texture& texture) {
    return impl_->register_shared_texture(name, texture);
}

core::Result<void> GodotRenderer::register_shared_texture_array(
    const std::string_view name, const std::span<const assets::Texture> layers, const std::uint32_t edge) {
    return impl_->register_shared_texture_array(name, layers, edge);
}

std::size_t GodotRenderer::shared_texture_count() const noexcept { return impl_->shared_texture_count(); }

core::Result<void> GodotRenderer::retain(const sim::AssetId asset_id) {
    return impl_->retain(asset_id);
}

core::Result<void> GodotRenderer::release(const sim::AssetId asset_id) {
    return impl_->release(asset_id);
}

std::vector<ResourceReference> GodotRenderer::resources() const { return impl_->resources(); }

core::Result<void> GodotRenderer::set_skin_pose(
    const sim::EntityId entity_id,
    const sim::AssetId asset_id,
    const std::span<const animation::BonePose> bones) {
    return impl_->set_skin_pose(entity_id, asset_id, bones);
}

std::vector<GodotRenderer::SkinBindingEvidence> GodotRenderer::skin_bindings() const {
    return impl_->skin_bindings();
}

std::vector<GodotRenderer::SubmissionEvidence> GodotRenderer::submission_evidence() const {
    return impl_->submission_evidence();
}

std::size_t GodotRenderer::instance_count() const noexcept { return impl_->instance_count(); }

GodotRenderer::SubmitWork GodotRenderer::submit_work() const noexcept { return impl_->submit_work(); }

void GodotRenderer::clear_skin_pose(const sim::EntityId entity_id) {
    impl_->clear_skin_pose(entity_id);
}
void GodotRenderer::copy_instance_pose(const sim::EntityId source, const sim::EntityId target,
                                     const float light_factor) {
    impl_->copy_instance_pose(source, target, light_factor);
}
void GodotRenderer::forget_instance_pose(const sim::EntityId entity_id) {
    impl_->forget_instance_pose(entity_id);
}
void GodotRenderer::set_billboard_light(const sim::AssetId asset_id,
                                       const std::array<float, 3>& toward_light) {
    impl_->set_billboard_light(asset_id, toward_light);
}

core::Result<void> GodotRenderer::set_material_scalar(const sim::AssetId asset_id, const std::string_view binding,
                                                      const float value) {
    return impl_->set_material_scalar(asset_id, binding, value);
}

void GodotRenderer::set_light_scale(const sim::EntityId entity_id, const std::array<float, 3>& rgb) {
    impl_->set_light_scale(entity_id, rgb);
}

core::Result<void> GodotRenderer::set_material_priority(const sim::AssetId asset_id, const std::int32_t priority) {
    return impl_->set_material_priority(asset_id, priority);
}

void GodotRenderer::set_unit_colorization(const sim::EntityId entity_id, const std::array<float, 3>& rgb) {
    impl_->set_unit_colorization(entity_id, rgb);
}

void GodotRenderer::set_unit_opacity(const sim::EntityId entity_id, const float alpha) {
    impl_->set_unit_opacity(entity_id, alpha);
}

GodotRenderer::LifecycleCounts GodotRenderer::lifecycle_counts() const noexcept {
    return impl_->lifecycle_counts();
}

std::vector<GodotRenderer::InstanceEvidence> GodotRenderer::instance_evidence() const {
    return impl_->instance_evidence();
}

void GodotRenderer::set_camera(const FixedCamera& camera) { impl_->set_camera(camera); }

void GodotRenderer::set_lighting(const LightingState& lighting) { impl_->set_lighting(lighting); }
const std::optional<GodotRenderer::LightingState>& GodotRenderer::lighting() const noexcept {
    return impl_->lighting();
}

void GodotRenderer::set_wind(const WindState& wind) { impl_->set_wind(wind); }

void GodotRenderer::set_shadows_enabled(const bool enabled) { impl_->set_shadows_enabled(enabled); }

void GodotRenderer::set_scene_bloom(const std::optional<lighting::bloom::SceneBloom>& bloom) {
    impl_->set_scene_bloom(bloom);
}

bool GodotRenderer::scene_bloom_active() const noexcept { return impl_->scene_bloom_active(); }

void GodotRenderer::set_casts_shadows(const sim::AssetId asset_id, const bool casts) {
    impl_->set_casts_shadows(asset_id, casts);
}

void GodotRenderer::set_backdrop(const sim::AssetId asset_id) { impl_->set_backdrop(asset_id); }
void GodotRenderer::capture_backdrop(const std::array<float, 3>& eye) { impl_->capture_backdrop(eye); }
void GodotRenderer::update_backdrop() { impl_->update_backdrop(); }
bool GodotRenderer::backdrop_ready() const noexcept { return impl_->backdrop_ready(); }

std::size_t GodotRenderer::shadow_receiving_materials() const noexcept {
    return impl_->shadow_receiving_materials();
}

std::size_t GodotRenderer::shadow_variant_failures() const noexcept {
    return impl_->shadow_variant_failures();
}

core::Result<void> GodotRenderer::enable_fog(const FogOptions& options) {
    return impl_->enable_fog(options);
}

void GodotRenderer::disable_fog() { impl_->disable_fog(); }

void GodotRenderer::set_fog_team(const std::uint32_t team) { impl_->set_fog_team(team); }

void GodotRenderer::reset_fog_stream(const std::uint64_t stream) { impl_->reset_fog_stream(stream); }

core::Result<void> GodotRenderer::declare_fog_consumer(const sim::AssetId asset_id) {
    return impl_->declare_fog_consumer(asset_id);
}

core::Result<GodotRenderer::ExternalFogHandle> GodotRenderer::register_external_fog_material(
    const RID& material, const RID& shader) {
    return impl_->register_external_fog_material(material, shader);
}

void GodotRenderer::unregister_external_fog_material(const ExternalFogHandle handle) {
    impl_->unregister_external_fog_material(handle);
}

std::vector<GodotRenderer::ExternalFogEvidence> GodotRenderer::external_fog_consumers() const {
    return impl_->external_fog_evidence();
}

GodotRenderer::FogStatus GodotRenderer::fog_status() const { return impl_->fog_status(); }

std::vector<GodotRenderer::FogConsumerEvidence> GodotRenderer::fog_consumers() const {
    return impl_->fog_consumer_evidence();
}

void GodotRenderer::submit(std::shared_ptr<const sim::RenderSnapshot> snapshot) {
    impl_->submit(std::move(snapshot));
}

core::Result<CaptureResult> GodotRenderer::capture(const FixedCamera& camera) {
    return impl_->capture(camera);
}

BackendInfo GodotRenderer::backend_info() const { return impl_->backend_info(); }
std::span<const core::Diagnostic> GodotRenderer::diagnostics() const {
    return impl_->diagnostics();
}

} // namespace eawr::presentation::godot_backend
