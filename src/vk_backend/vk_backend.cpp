
#include <array>
#include <iostream>
#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE
#include "global_utils.h"
#include "imgui.h"
#include "vk_backend/resources/vk_descriptor.h"
#include "vk_backend/vk_pipeline.h"
#include "vk_init.h"
#include <GLFW/glfw3.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <string>
#include <vk_backend/vk_command.h>
#include <vk_backend/vk_debug.h>
#include <vk_backend/vk_device.h>
#include <vk_backend/vk_swapchain.h>
#include <vk_backend/vk_types.h>
#include <vk_backend/vk_utils.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_core.h>

#include "vk_backend/vk_backend.h"

#include "../../thirdparty/shaderc/libshaderc/src/shaderc_private.h"
#include "imgui_impl_vulkan.h"
#include "vk_backend/vk_sync.h"
#include "vk_options.h"

// initialization
static void create_allocator(VkBackend* backend);
static void create_default_data(VkBackend* backend);
static void create_desc_layouts(VkBackend* backend);
static void configure_debugger(VkBackend* backend);
static void configure_render_resources(VkBackend* backend);
static void create_grid_pipeline(VkBackend* backend);
static void create_pipeline_layouts(VkBackend* backend);
void        init_sky_box(VkBackend* backend);

static VkShaderModule load_shader_module(const VkBackend* backend, std::span<uint32_t> shader_spv);
// state update
static void           resize(VkBackend* backend);
void                  set_render_state(VkBackend* backend, VkCommandBuffer cmd_buf);
// rendering
static void           render_geometry(VkBackend* backend, VkCommandBuffer cmd_buf,
                                      std::span<const Entity> entities, size_t vert_shader,
                                      size_t frag_shader);
static void           render_ui(VkCommandBuffer cmd_buf);
static void           render_grid(VkBackend* backend, VkCommandBuffer cmd_buf);
static void   render_sky_box(VkBackend* backend, VkCommandBuffer cmd_buf, uint32_t vert_shader_i,
                             uint32_t frag_shader_i);
// utils
static Frame* get_current_frame(VkBackend* backend) {
    return &backend->frames[backend->frame_num % backend->frames.size()];
}
static std::vector<const char*> get_instance_extensions();

using namespace std::chrono;

static VkBackend* active_backend = nullptr;

void init_backend(VkBackend* backend, VkInstance instance, VkSurfaceKHR surface, uint32_t width,
                  uint32_t height) {

    assert(active_backend == nullptr);
    active_backend = backend;

    backend->instance = instance;

    init_device_context(&backend->device_ctx, backend->instance, surface);
    init_swapchain_context(&backend->swapchain_context, &backend->device_ctx, surface,
                           vk_opts::desired_present_mode);

    create_allocator(backend);
    create_desc_layouts(backend);
    init_sky_box(backend);

    for (Frame& frame : backend->frames) {
        init_frame(&frame, backend->device_ctx.logical_device, backend->allocator,
                   backend->device_ctx.queues.graphics_family_index,
                   backend->global_desc_set_layout);
    }

    backend->image_extent.width  = width;
    backend->image_extent.height = height;

    backend->color_resolve_image = create_image(
        backend->device_ctx.logical_device, backend->allocator,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_VIEW_TYPE_2D, backend->image_extent, VK_FORMAT_R16G16B16A16_SFLOAT, 1);

    backend->color_image =
        create_image(backend->device_ctx.logical_device, backend->allocator,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                     VK_IMAGE_VIEW_TYPE_2D, backend->image_extent, VK_FORMAT_R16G16B16A16_SFLOAT,
                     backend->device_ctx.raster_samples);

    backend->depth_image = create_image(backend->device_ctx.logical_device, backend->allocator,
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                        VK_IMAGE_VIEW_TYPE_2D, backend->image_extent,
                                        VK_FORMAT_D32_SFLOAT, backend->device_ctx.raster_samples);

    // create color attachments and  rendering information from our allocated images
    configure_render_resources(backend);

    backend->imm_fence =
        create_fence(backend->device_ctx.logical_device, VK_FENCE_CREATE_SIGNALED_BIT);

    init_cmd_context(&backend->imm_cmd_context, backend->device_ctx.logical_device,
                     backend->device_ctx.queues.graphics_family_index,
                     VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    create_default_data(backend);

    create_grid_pipeline(backend);

    create_pipeline_layouts(backend);

    init_shader_ctx(&backend->shader_ctx);

    init_vk_ext_context(&backend->ext_ctx, backend->device_ctx.logical_device);

    if constexpr (vk_opts::validation_enabled) {
        configure_debugger(backend);
    }
}

static constexpr std::array skybox_vertices = {
    // positions
    -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f,
    1.0f,  1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f,
    -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f, -1.0f, 1.0f,
    1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, 1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    1.0f,  1.0f,  -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,  -1.0f, 1.0f,  1.0f,
    1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  -1.0f, 1.0f,  -1.0f, -1.0f, 1.0f,
    -1.0f, 1.0f,  -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
    -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,
    1.0f,  -1.0f, -1.0f, 1.0f,  -1.0f, -1.0f, -1.0f, -1.0f, 1.0f,  1.0f,  -1.0f, 1.0f,
};

void init_sky_box(VkBackend* backend) {

    std::vector<PoolSizeRatio> mat_pool_sizes = {
  //   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
    };
    init_desc_allocator(&backend->sky_box_desc_allocator, backend->device_ctx.logical_device, 1,
                        mat_pool_sizes);

    DescriptorLayoutBuilder layout_builder;
    // add_layout_binding(&layout_builder, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    add_layout_binding(&layout_builder, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    backend->sky_box_desc_set_layout =
        build_set_layout(&layout_builder, backend->device_ctx.logical_device,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    backend->sky_box_desc_set =
        allocate_desc_set(&backend->sky_box_desc_allocator, backend->device_ctx.logical_device,
                          backend->sky_box_desc_set_layout);

    backend->sky_box_buffer = create_buffer(
        36 * sizeof(glm::vec3), backend->allocator, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

    vmaCopyMemoryToAllocation(backend->allocator, skybox_vertices.data(),
                              backend->sky_box_buffer.allocation, 0,
                              backend->sky_box_buffer.info.size);

    backend->deletion_queue.push_persistant([=] {
        deinit_desc_allocator(&backend->sky_box_desc_allocator, backend->device_ctx.logical_device);
    });
};

void upload_sky_box(VkBackend* backend, const uint8_t* texture_data, uint32_t color_channels,
                    uint32_t width, uint32_t height) {

    AllocatedImage cube_tex =
        upload_texture(backend, texture_data, VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_VIEW_TYPE_CUBE,
                       6, color_channels, width, height);

    DescriptorWriter desc_writer;
    write_image_desc(&desc_writer, 0, cube_tex.image_view, backend->default_linear_sampler,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    update_desc_set(&desc_writer, backend->device_ctx.logical_device, backend->sky_box_desc_set);

    backend->deletion_queue.push_persistant(
        [=] { vmaDestroyImage(backend->allocator, cube_tex.image, cube_tex.allocation); });
}

void create_pipeline_layouts(VkBackend* backend) {

    std::array geo_set_layouts{backend->global_desc_set_layout, backend->mat_desc_set_layout,
                               backend->draw_obj_desc_set_layout};

    std::array<VkPushConstantRange, 1> geo_push_constant_ranges{{{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(EntityPushConstants),
    }}};

    backend->geo_pipeline_layout = create_pipeline_layout(
        backend->device_ctx.logical_device, geo_set_layouts, geo_push_constant_ranges, 0);

    std::array sky_box_set_layouts{
        backend->global_desc_set_layout,
        backend->sky_box_desc_set_layout,
    };

    backend->sky_box_pipeline_layout =
        create_pipeline_layout(backend->device_ctx.logical_device, sky_box_set_layouts, {}, 0);
}

static void create_grid_pipeline(VkBackend* backend) {
    PipelineBuilder pb;

    std::ifstream v_file("../shaders/vertex/grid.vert.spv", std::ios::ate | std::ios::binary);
    size_t        v_file_size = v_file.tellg();
    std::vector<uint32_t> v_buf(v_file_size / sizeof(uint32_t));

    v_file.seekg(0);
    v_file.read(reinterpret_cast<char*>(v_buf.data()), v_file_size);
    v_file.close();

    std::ifstream f_file("../shaders/fragment/grid.frag.spv", std::ios::ate | std::ios::binary);
    size_t        f_file_size = f_file.tellg();
    std::vector<uint32_t> f_buf(f_file_size / sizeof(uint32_t));

    f_file.seekg(0);
    f_file.read(reinterpret_cast<char*>(f_buf.data()), f_file_size);
    f_file.close();

    const VkShaderModule vert_shader = load_shader_module(backend, v_buf);
    const VkShaderModule frag_shader = load_shader_module(backend, f_buf);

    std::array set_layouts{backend->global_desc_set_layout};

    set_pipeline_shaders(&pb, vert_shader, frag_shader);
    set_pipeline_topology(&pb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    set_pipeline_raster_state(&pb, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE,
                              VK_POLYGON_MODE_FILL);
    set_pipeline_multisampling(
        &pb, static_cast<VkSampleCountFlagBits>(backend->device_ctx.raster_samples));
    set_pipeline_depth_state(&pb, true, true, VK_COMPARE_OP_GREATER_OR_EQUAL);

    set_pipeline_render_state(&pb, backend->color_image.image_format,
                              backend->depth_image.image_format);
    set_pipeline_blending(&pb, BlendMode::alpha);

    std::array<VkPushConstantRange, 0> push_constant_ranges{{}};

    set_pipeline_layout(&pb, set_layouts, push_constant_ranges, 0);
    backend->grid_pipeline_info = build_pipeline(&pb, backend->device_ctx.logical_device);

    backend->deletion_queue.push_persistant([=] {
        vkDestroyShaderModule(backend->device_ctx.logical_device, vert_shader, nullptr);
        vkDestroyShaderModule(backend->device_ctx.logical_device, frag_shader, nullptr);
    });
}

// adding names to these 64 bit handles helps a lot when reading validation errors
void configure_debugger(VkBackend* backend) {
    init_debugger(&backend->debugger, backend->instance, backend->device_ctx.logical_device);
    set_handle_name(&backend->debugger, backend->color_image.image, VK_OBJECT_TYPE_IMAGE,
                    "color image");

    set_handle_name(&backend->debugger, backend->depth_image.image, VK_OBJECT_TYPE_IMAGE,
                    "depth image");

    set_handle_name(&backend->debugger, backend->color_image.image_view, VK_OBJECT_TYPE_IMAGE_VIEW,
                    "color image view");
    set_handle_name(&backend->debugger, backend->depth_image.image_view, VK_OBJECT_TYPE_IMAGE_VIEW,
                    "depth image view");

    set_handle_name(&backend->debugger, backend->color_resolve_image.image, VK_OBJECT_TYPE_IMAGE,
                    "color resolve image");

    set_handle_name(&backend->debugger, backend->color_resolve_image.image_view,
                    VK_OBJECT_TYPE_IMAGE_VIEW, "color resolve image view");

    set_handle_name(&backend->debugger, backend->imm_cmd_context.primary_buffer,
                    VK_OBJECT_TYPE_COMMAND_BUFFER, "imm cmd_buf buf");

    set_handle_name(&backend->debugger, backend->sky_box_buffer.buffer, VK_OBJECT_TYPE_BUFFER,
                    "skybox buffer");

    for (size_t i = 0; i < backend->frames.size(); i++) {
        const Frame& frame = backend->frames[i];
        set_handle_name(&backend->debugger, frame.command_context.primary_buffer,
                        VK_OBJECT_TYPE_COMMAND_BUFFER,
                        "frame " + std::to_string(i) + " cmd_buf buf");
    }

    for (size_t i = 0; i < backend->swapchain_context.images.size(); i++) {
        set_handle_name(&backend->debugger, backend->swapchain_context.images[i],
                        VK_OBJECT_TYPE_IMAGE, "swapchain image " + std::to_string(i));
        set_handle_name(&backend->debugger, backend->swapchain_context.image_views[i],
                        VK_OBJECT_TYPE_IMAGE_VIEW, "swapchain image view " + std::to_string(i));
    }
}

void create_desc_layouts(VkBackend* backend) {

    DescriptorLayoutBuilder layout_builder;
    add_layout_binding(&layout_builder, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    backend->global_desc_set_layout =
        build_set_layout(&layout_builder, backend->device_ctx.logical_device,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    clear_layout_bindings(&layout_builder);
    add_layout_binding(&layout_builder, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    add_layout_binding(&layout_builder, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    add_layout_binding(&layout_builder, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    backend->mat_desc_set_layout =
        build_set_layout(&layout_builder, backend->device_ctx.logical_device,
                         VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT);

    clear_layout_bindings(&layout_builder);
    add_layout_binding(&layout_builder, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    backend->draw_obj_desc_set_layout =
        build_set_layout(&layout_builder, backend->device_ctx.logical_device,
                         VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT);
}

void configure_render_resources(VkBackend* backend) {

    backend->scene_clear_value = {.color = {{0.1f, 0.1f, 0.1f, 0.2f}}};

    backend->scene_color_attachment = create_color_attachment_info(
        backend->color_image.image_view, &backend->scene_clear_value,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
        backend->color_resolve_image.image_view);

    backend->scene_depth_attachment = create_depth_attachment_info(
        backend->depth_image.image_view, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE);

    backend->scene_rendering_info = create_rendering_info(
        &backend->scene_color_attachment, &backend->scene_depth_attachment, backend->image_extent);
}

void create_default_data(VkBackend* backend) {

    backend->stats.total_fps        = 0;
    backend->stats.total_frame_time = 0;
    backend->stats.total_draw_time  = 0;

    backend->default_linear_sampler =
        create_sampler(backend->device_ctx.logical_device, VK_FILTER_LINEAR, VK_FILTER_LINEAR);

    backend->default_nearest_sampler =
        create_sampler(backend->device_ctx.logical_device, VK_FILTER_NEAREST, VK_FILTER_NEAREST);

    uint32_t white = 0xFFFFFFFF;
    backend->default_texture =
        upload_texture(backend, reinterpret_cast<uint8_t*>(&white), VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_VIEW_TYPE_2D, 1, 4, 1, 1);
}

void create_imgui_vk_resources(VkBackend* backend) {

    std::array<VkDescriptorPoolSize, 1> pool_sizes = {
        {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}},
    };

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets       = 1;
    pool_ci.pPoolSizes    = pool_sizes.data();
    pool_ci.poolSizeCount = pool_sizes.size();
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    VK_CHECK(vkCreateDescriptorPool(backend->device_ctx.logical_device, &pool_ci, nullptr,
                                    &backend->imm_descriptor_pool));

    VkPipelineRenderingCreateInfoKHR pipeline_info{};
    pipeline_info.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    pipeline_info.pColorAttachmentFormats = &backend->color_image.image_format;
    pipeline_info.colorAttachmentCount    = 1;
    pipeline_info.depthAttachmentFormat   = backend->depth_image.image_format;

    ImGui_ImplVulkan_InitInfo init_info   = {};
    init_info.Instance                    = backend->instance;
    init_info.PhysicalDevice              = backend->device_ctx.physical_device;
    init_info.Device                      = backend->device_ctx.logical_device;
    init_info.Queue                       = backend->device_ctx.queues.graphics;
    init_info.DescriptorPool              = backend->imm_descriptor_pool;
    init_info.MinImageCount               = 3;
    init_info.ImageCount                  = 3;
    init_info.UseDynamicRendering         = true;
    init_info.PipelineRenderingCreateInfo = pipeline_info;
    init_info.MSAASamples = static_cast<VkSampleCountFlagBits>(backend->device_ctx.raster_samples);

    ImGui_ImplVulkan_Init(&init_info);
}

void immediate_submit(const VkBackend*                               backend,
                      std::function<void(VkCommandBuffer cmd_buf)>&& function) {
    VK_CHECK(vkResetFences(backend->device_ctx.logical_device, 1, &backend->imm_fence));

    begin_primary_buffer(&backend->imm_cmd_context, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    function(backend->imm_cmd_context.primary_buffer);

    submit_primary_buffer(&backend->imm_cmd_context, backend->device_ctx.queues.graphics, nullptr,
                          nullptr, backend->imm_fence);

    VK_CHECK(vkWaitForFences(backend->device_ctx.logical_device, 1, &backend->imm_fence, VK_TRUE,
                             vk_opts::timeout_dur));
}

void create_allocator(VkBackend* backend) {
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.device         = backend->device_ctx.logical_device;
    allocator_info.physicalDevice = backend->device_ctx.physical_device;
    allocator_info.instance       = backend->instance;
    allocator_info.flags          = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VK_CHECK(vmaCreateAllocator(&allocator_info, &backend->allocator));
}

VkInstance create_vk_instance(const char* app_name, const char* engine_name) {

    VkApplicationInfo app_info{};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pNext              = nullptr;
    app_info.pApplicationName   = app_name;
    app_info.pEngineName        = engine_name;
    app_info.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion         = VK_API_VERSION_1_3;

    const std::vector<const char*> instance_extensions = get_instance_extensions();

    VkInstanceCreateInfo instance_ci{};
    instance_ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_ci.pApplicationInfo        = &app_info;
    instance_ci.flags                   = 0;
    instance_ci.ppEnabledExtensionNames = instance_extensions.data();
    instance_ci.enabledExtensionCount   = instance_extensions.size();

    VkDebugUtilsMessengerCreateInfoEXT debug_ci;
    VkValidationFeaturesEXT            validation_features;
    std::array<const char*, 1>         validation_layers;

    if constexpr (vk_opts::validation_enabled) {
        debug_ci            = create_messenger_info();
        validation_features = create_validation_features();
        validation_layers   = create_validation_layers();

        validation_features.pNext       = &debug_ci;
        instance_ci.pNext               = &validation_features;
        instance_ci.enabledLayerCount   = validation_layers.size();
        instance_ci.ppEnabledLayerNames = validation_layers.data();
    }

    VkInstance instance;
    VK_CHECK(vkCreateInstance(&instance_ci, nullptr, &instance));
    return instance;
}

void create_pipeline(VkBackend* backend, std::span<uint32_t> vert_shader_spv,
                     std::span<uint32_t> frag_shader_spv) {
    PipelineBuilder      pb;
    const VkShaderModule vert_shader = load_shader_module(backend, vert_shader_spv);
    const VkShaderModule frag_shader = load_shader_module(backend, frag_shader_spv);

    std::array set_layouts{backend->global_desc_set_layout, backend->mat_desc_set_layout,
                           backend->draw_obj_desc_set_layout};

    std::array<VkPushConstantRange, 1> push_constant_ranges{{{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(EntityPushConstants),
    }}};

    set_pipeline_shaders(&pb, vert_shader, frag_shader);
    set_pipeline_topology(&pb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    set_pipeline_raster_state(&pb, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE,
                              VK_POLYGON_MODE_FILL);
    set_pipeline_multisampling(
        &pb, static_cast<VkSampleCountFlagBits>(backend->device_ctx.raster_samples));
    set_pipeline_depth_state(&pb, true, true, VK_COMPARE_OP_GREATER_OR_EQUAL);
    set_pipeline_render_state(&pb, backend->color_image.image_format,
                              backend->depth_image.image_format);
    set_pipeline_blending(&pb, BlendMode::none);

    set_pipeline_layout(&pb, set_layouts, push_constant_ranges, 0);
    backend->opaque_pipeline_info = build_pipeline(&pb, backend->device_ctx.logical_device);

    set_pipeline_blending(&pb, BlendMode::alpha);
    set_pipeline_depth_state(&pb, true, false, VK_COMPARE_OP_GREATER_OR_EQUAL);
    backend->transparent_pipeline_info = build_pipeline(&pb, backend->device_ctx.logical_device);

    backend->deletion_queue.push_persistant([=] {
        vkDestroyShaderModule(backend->device_ctx.logical_device, vert_shader, nullptr);
        vkDestroyShaderModule(backend->device_ctx.logical_device, frag_shader, nullptr);
    });
}

void upload_vert_shader(VkBackend* backend, const std::filesystem::path& file_path,
                        const std::string& name) {
    ShaderBuilder builder;

    std::array<VkPushConstantRange, 1> push_constant_ranges{{{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(EntityPushConstants),
    }}};
    std::array set_layouts{backend->global_desc_set_layout, backend->mat_desc_set_layout,
                           backend->draw_obj_desc_set_layout};

    stage_shader(&backend->shader_ctx, file_path, name, set_layouts, push_constant_ranges,
                 VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);

    commit_shaders(&backend->shader_ctx, &backend->ext_ctx, backend->device_ctx.logical_device,
                   ShaderType::unlinked);
}

void upload_frag_shader(VkBackend* backend, const std::filesystem::path& file_path,
                        const std::string& name) {
    ShaderBuilder builder;

    std::array<VkPushConstantRange, 1> push_constant_ranges{{{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(EntityPushConstants),
    }}};
    std::array set_layouts{backend->global_desc_set_layout, backend->mat_desc_set_layout,
                           backend->draw_obj_desc_set_layout};

    stage_shader(&backend->shader_ctx, file_path, name, set_layouts, push_constant_ranges,
                 VK_SHADER_STAGE_FRAGMENT_BIT, 0);

    commit_shaders(&backend->shader_ctx, &backend->ext_ctx, backend->device_ctx.logical_device,
                   ShaderType::unlinked);
}

void upload_both_shaders(VkBackend* backend, const std::filesystem::path& vert_path,
                         const std::filesystem::path& frag_path, const std::string& name) {
    std::array<VkPushConstantRange, 1> push_constant_ranges{{{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset     = 0,
        .size       = sizeof(EntityPushConstants),
    }}};
    std::array set_layouts{backend->global_desc_set_layout, backend->mat_desc_set_layout,
                           backend->draw_obj_desc_set_layout};

    stage_shader(&backend->shader_ctx, vert_path, name, set_layouts, push_constant_ranges,
                 VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);

    stage_shader(&backend->shader_ctx, frag_path, name, set_layouts, push_constant_ranges,
                 VK_SHADER_STAGE_FRAGMENT_BIT, 0);

    commit_shaders(&backend->shader_ctx, &backend->ext_ctx, backend->device_ctx.logical_device,
                   ShaderType::unlinked);
}

void upload_sky_box_shaders(VkBackend* backend, const std::filesystem::path& vert_path,
                            const std::filesystem::path& frag_path, const std::string& name) {

    /*
    Due to error in validation layers, creating linked shaders doesn't work during DEBUG.
    Due to this, I'll just create unlinked shaders for now, but I can improve this in the future
    by conditionally using linked shaders during release builds
   */
    std::array set_layouts{backend->global_desc_set_layout, backend->sky_box_desc_set_layout};

    stage_shader(&backend->shader_ctx, vert_path, name, set_layouts, {}, VK_SHADER_STAGE_VERTEX_BIT,
                 VK_SHADER_STAGE_FRAGMENT_BIT);

    commit_shaders(&backend->shader_ctx, &backend->ext_ctx, backend->device_ctx.logical_device,
                   ShaderType::unlinked);

    stage_shader(&backend->shader_ctx, frag_path, name, set_layouts, {},
                 VK_SHADER_STAGE_FRAGMENT_BIT, 0);

    commit_shaders(&backend->shader_ctx, &backend->ext_ctx, backend->device_ctx.logical_device,
                   ShaderType::linked);
}

std::vector<const char*> get_instance_extensions() {
    uint32_t                 count{0};
    const char**             glfw_extensions = glfwGetRequiredInstanceExtensions(&count);
    std::vector<const char*> extensions;
    for (size_t i = 0; i < count; i++) {
        extensions.emplace_back(glfw_extensions[i]);
    }
    if constexpr (vk_opts::validation_enabled) {
        extensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
}

void draw(VkBackend* backend, std::span<const Entity> entities, const SceneData* scene_data,
          size_t vert_shader, size_t frag_shader) {
    auto start_frame_time = system_clock::now();

    Frame*          current_frame = get_current_frame(backend);
    VkCommandBuffer cmd_buffer    = current_frame->command_context.primary_buffer;

    vkWaitForFences(backend->device_ctx.logical_device, 1, &current_frame->render_fence, VK_TRUE,
                    vk_opts::timeout_dur);

    set_scene_data(current_frame, backend->device_ctx.logical_device, backend->allocator,
                   scene_data);

    uint32_t swapchain_image_index;
    VkResult result = vkAcquireNextImageKHR(
        backend->device_ctx.logical_device, backend->swapchain_context.swapchain,
        vk_opts::timeout_dur, current_frame->present_semaphore, nullptr, &swapchain_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        resize(backend);
        return;
    }

    VK_CHECK(vkResetFences(backend->device_ctx.logical_device, 1, &current_frame->render_fence));

    VkImage swapchain_image = backend->swapchain_context.images[swapchain_image_index];

    begin_primary_buffer(&current_frame->command_context,
                         VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    insert_image_memory_barrier(cmd_buffer, backend->color_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    insert_image_memory_barrier(cmd_buffer, backend->color_resolve_image.image,
                                VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    insert_image_memory_barrier(cmd_buffer, backend->depth_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    vkCmdBeginRendering(cmd_buffer, &backend->scene_rendering_info);

    set_render_state(backend, cmd_buffer);

    render_sky_box(backend, cmd_buffer, 1, 1);

    render_geometry(backend, cmd_buffer, entities, vert_shader, frag_shader);

    // render_grid(backend, cmd_buffer);

    render_ui(cmd_buffer);

    vkCmdEndRendering(cmd_buffer);

    insert_image_memory_barrier(cmd_buffer, backend->color_resolve_image.image,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    insert_image_memory_barrier(cmd_buffer, swapchain_image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    blit_image(cmd_buffer, backend->color_resolve_image.image, swapchain_image,
               backend->swapchain_context.extent, backend->image_extent, 1, 1);

    insert_image_memory_barrier(cmd_buffer, swapchain_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    VkSemaphoreSubmitInfo wait_semaphore_si = create_semaphore_submit_info(
        current_frame->present_semaphore, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkSemaphoreSubmitInfo signal_semaphore_si = create_semaphore_submit_info(
        current_frame->render_semaphore, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);

    submit_primary_buffer(&current_frame->command_context, backend->device_ctx.queues.graphics,
                          &wait_semaphore_si, &signal_semaphore_si, current_frame->render_fence);

    VkPresentInfoKHR present_info{};
    present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.pNext              = nullptr;
    present_info.pSwapchains        = &backend->swapchain_context.swapchain;
    present_info.swapchainCount     = 1;
    present_info.pImageIndices      = &swapchain_image_index;
    present_info.pWaitSemaphores    = &current_frame->render_semaphore;
    present_info.waitSemaphoreCount = 1;

    result = vkQueuePresentKHR(backend->device_ctx.queues.present, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        resize(backend);
        return;
    }

    backend->frame_num++;

    auto end_time = system_clock::now();
    auto dur      = duration<float>(end_time - start_frame_time);
    backend->stats.total_fps +=
        1000000.f / static_cast<float>(duration_cast<microseconds>(dur).count());
    backend->stats.total_frame_time += duration_cast<microseconds>(dur).count();
    if (backend->frame_num % 60 == 0) {
        backend->stats.frame_time = duration_cast<microseconds>(dur).count() / 1000.f;
    }
}

void resize(VkBackend* backend) {
    vkDeviceWaitIdle(backend->device_ctx.logical_device);

    reset_swapchain_context(&backend->swapchain_context, &backend->device_ctx);

    destroy_image(backend->device_ctx.logical_device, backend->allocator, &backend->depth_image);
    destroy_image(backend->device_ctx.logical_device, backend->allocator, &backend->color_image);

    backend->image_extent.width  = backend->swapchain_context.extent.width;
    backend->image_extent.height = backend->swapchain_context.extent.height;

    destroy_image(backend->device_ctx.logical_device, backend->allocator,
                  &backend->color_resolve_image);
    backend->color_resolve_image = create_image(
        backend->device_ctx.logical_device, backend->allocator,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_VIEW_TYPE_2D, backend->image_extent, VK_FORMAT_R16G16B16A16_SFLOAT, 1);

    backend->color_image =
        create_image(backend->device_ctx.logical_device, backend->allocator,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                     VK_IMAGE_VIEW_TYPE_2D, backend->image_extent, VK_FORMAT_R16G16B16A16_SFLOAT,
                     backend->device_ctx.raster_samples);

    backend->depth_image = create_image(backend->device_ctx.logical_device, backend->allocator,
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                        VK_IMAGE_VIEW_TYPE_2D, backend->image_extent,
                                        VK_FORMAT_D32_SFLOAT, backend->device_ctx.raster_samples);

    configure_render_resources(backend);

    for (Frame& frame : backend->frames) {
        reset_frame_sync(&frame, backend->device_ctx.logical_device);
    }
}

void render_geometry(VkBackend* backend, VkCommandBuffer cmd_buf, std::span<const Entity> entities,
                     size_t vert_shader, size_t frag_shader) {
    auto buffer_recording_start = system_clock::now();

    VkDescriptorSet current_mat_desc = VK_NULL_HANDLE;

    const auto record_obj = [&](const DrawObject* obj) {
        if (obj->mat_desc_set != current_mat_desc) {
            current_mat_desc = obj->mat_desc_set;
            vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    backend->geo_pipeline_layout, 1, 1, &obj->mat_desc_set, 0,
                                    nullptr);
        }

        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                backend->geo_pipeline_layout, 2, 1, &obj->obj_desc_set, 0, nullptr);

        vkCmdBindIndexBuffer(cmd_buf, obj->index_buffer, 0, VK_INDEX_TYPE_UINT32);

        vkCmdDrawIndexed(cmd_buf, obj->indices_count, 1, obj->indices_start, 0, 0);
    };

    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, backend->geo_pipeline_layout,
                            0, 1, &get_current_frame(backend)->desc_set, 0, nullptr);

    backend->ext_ctx.vkCmdBindShadersEXT(cmd_buf, 1,
                                         &backend->shader_ctx.vert_shaders[vert_shader].stage,
                                         &backend->shader_ctx.vert_shaders[vert_shader].shader);

    backend->ext_ctx.vkCmdBindShadersEXT(cmd_buf, 1,
                                         &backend->shader_ctx.frag_shaders[frag_shader].stage,
                                         &backend->shader_ctx.frag_shaders[frag_shader].shader);

    backend->ext_ctx.vkCmdSetPolygonModeEXT(cmd_buf, VK_POLYGON_MODE_FILL);

    backend->ext_ctx.vkCmdSetVertexInputEXT(cmd_buf, 0, nullptr, 0, nullptr);

    vkCmdSetDepthTestEnable(cmd_buf, VK_TRUE);

    vkCmdSetDepthWriteEnable(cmd_buf, VK_TRUE);

    vkCmdSetDepthCompareOp(cmd_buf, VK_COMPARE_OP_GREATER_OR_EQUAL);

    for (const auto& entity : entities) {

        VkColorBlendEquationEXT blend_equation = {};
        backend->ext_ctx.vkCmdSetColorBlendEquationEXT(cmd_buf, 0, 1, &blend_equation);

        EntityPushConstants constants = {
            .pos = entity.pos,
        };

        vkCmdPushConstants(cmd_buf, backend->geo_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(EntityPushConstants), &constants);

        VkBool32 color_blend_enabled = VK_FALSE;
        backend->ext_ctx.vkCmdSetColorBlendEnableEXT(cmd_buf, 0, 1, &color_blend_enabled);
        for (const DrawObject& obj : entity.opaque_objs) {

            record_obj(&obj);
        }

        blend_equation = {
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp        = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp        = VK_BLEND_OP_ADD,
        };
        backend->ext_ctx.vkCmdSetColorBlendEquationEXT(cmd_buf, 0, 1, &blend_equation);

        color_blend_enabled = VK_TRUE;
        backend->ext_ctx.vkCmdSetColorBlendEnableEXT(cmd_buf, 0, 1, &color_blend_enabled);
        for (const DrawObject& obj : entity.transparent_objs) {
            record_obj(&obj);
        }
    }

    auto end_time = system_clock::now();
    auto dur      = duration<float>(end_time - buffer_recording_start);
    backend->stats.total_draw_time +=
        static_cast<uint32_t>(duration_cast<microseconds>(dur).count());
    if (backend->frame_num % 60 == 0) {
        backend->stats.draw_time = duration_cast<microseconds>(dur).count();
    }
}

void render_ui(VkCommandBuffer cmd_buf) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd_buf);
}

void render_grid(VkBackend* backend, VkCommandBuffer cmd_buf) {

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = backend->image_extent.width;
    viewport.height   = backend->image_extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = backend->image_extent;
    scissor.offset = {0, 0};

    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      backend->grid_pipeline_info.pipeline);

    vkCmdSetViewport(cmd_buf, 0, 1, &viewport);

    vkCmdSetScissor(cmd_buf, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            backend->grid_pipeline_info.pipeline_layout, 0, 1,
                            &get_current_frame(backend)->desc_set, 0, nullptr);

    vkCmdDraw(cmd_buf, 6, 1, 0, 0);
}

void set_render_state(VkBackend* backend, VkCommandBuffer cmd_buf) {

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = backend->image_extent.width;
    viewport.height   = backend->image_extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = backend->image_extent;
    scissor.offset = {0, 0};

    vkCmdSetViewportWithCount(cmd_buf, 1, &viewport);

    vkCmdSetScissorWithCount(cmd_buf, 1, &scissor);

    vkCmdSetRasterizerDiscardEnable(cmd_buf, VK_FALSE);

    VkColorBlendEquationEXT colorBlendEquationEXT{};
    backend->ext_ctx.vkCmdSetColorBlendEquationEXT(cmd_buf, 0, 1, &colorBlendEquationEXT);

    vkCmdSetPrimitiveTopology(cmd_buf, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

    vkCmdSetPrimitiveRestartEnable(cmd_buf, VK_FALSE);

    backend->ext_ctx.vkCmdSetRasterizationSamplesEXT(
        cmd_buf, static_cast<VkSampleCountFlagBits>(backend->device_ctx.raster_samples));

    uint32_t max = ~0;

    const VkSampleMask sample_masks[4] = {max, max, max, max};

    backend->ext_ctx.vkCmdSetSampleMaskEXT(cmd_buf, VK_SAMPLE_COUNT_4_BIT, sample_masks);

    // Do not use alpha to coverage or alpha to one because not using MSAA???
    backend->ext_ctx.vkCmdSetAlphaToCoverageEnableEXT(cmd_buf, VK_FALSE);

    backend->ext_ctx.vkCmdSetPolygonModeEXT(cmd_buf, VK_POLYGON_MODE_FILL);

    backend->ext_ctx.vkCmdSetVertexInputEXT(cmd_buf, 0, nullptr, 0, nullptr);

    backend->ext_ctx.vkCmdSetTessellationDomainOriginEXT(cmd_buf,
                                                         VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT);

    backend->ext_ctx.vkCmdSetPatchControlPointsEXT(cmd_buf, 1);

    vkCmdSetLineWidth(cmd_buf, 1.0f);
    vkCmdSetFrontFace(cmd_buf, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthCompareOp(cmd_buf, VK_COMPARE_OP_GREATER);
    vkCmdSetDepthTestEnable(cmd_buf, VK_FALSE);
    vkCmdSetDepthBoundsTestEnable(cmd_buf, VK_FALSE);
    vkCmdSetDepthBiasEnable(cmd_buf, VK_FALSE);
    vkCmdSetStencilTestEnable(cmd_buf, VK_FALSE);

    backend->ext_ctx.vkCmdSetLogicOpEnableEXT(cmd_buf, VK_FALSE);

    VkBool32 color_blend_enables[] = {VK_FALSE};
    backend->ext_ctx.vkCmdSetColorBlendEnableEXT(cmd_buf, 0, 1, color_blend_enables);

    VkColorComponentFlags color_component_flags[] = {
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_A_BIT};

    backend->ext_ctx.vkCmdSetColorWriteMaskEXT(cmd_buf, 0, 1, color_component_flags);
}

void render_sky_box(VkBackend* backend, VkCommandBuffer cmd_buf, uint32_t vert_shader_i,
                    uint32_t frag_shader_i) {

    vkCmdSetCullMode(cmd_buf, VK_CULL_MODE_NONE);

    vkCmdSetDepthWriteEnable(cmd_buf, VK_FALSE);

    backend->ext_ctx.vkCmdBindShadersEXT(cmd_buf, 1,
                                         &backend->shader_ctx.vert_shaders[vert_shader_i].stage,
                                         &backend->shader_ctx.vert_shaders[vert_shader_i].shader);

    backend->ext_ctx.vkCmdBindShadersEXT(cmd_buf, 1,
                                         &backend->shader_ctx.frag_shaders[frag_shader_i].stage,
                                         &backend->shader_ctx.frag_shaders[frag_shader_i].shader);

    VkVertexInputBindingDescription2EXT input_description{};
    input_description.sType     = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
    input_description.binding   = 0;
    input_description.stride    = sizeof(float) * 3;
    input_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    input_description.divisor   = 1;

    VkVertexInputAttributeDescription2EXT attribute_description{};
    attribute_description.sType   = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
    attribute_description.binding = 0;
    attribute_description.format  = VK_FORMAT_R32G32B32_SFLOAT;
    attribute_description.offset  = 0;

    backend->ext_ctx.vkCmdSetVertexInputEXT(cmd_buf, 1, &input_description, 1,
                                            &attribute_description);

    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            backend->sky_box_pipeline_layout, 0, 1,
                            &get_current_frame(backend)->desc_set, 0, nullptr);

    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            backend->sky_box_pipeline_layout, 1, 1, &backend->sky_box_desc_set, 0,
                            nullptr);

    VkDeviceSize offsets = {0};
    vkCmdBindVertexBuffers(cmd_buf, 0, 1, &backend->sky_box_buffer.buffer, &offsets);

    vkCmdDraw(cmd_buf, 36, 1, 0, 0);
}

VkShaderModule load_shader_module(const VkBackend* backend, std::span<uint32_t> shader_spv) {

    VkShaderModule           shader_module;
    VkShaderModuleCreateInfo shader_module_ci{};
    shader_module_ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_module_ci.codeSize = shader_spv.size() * sizeof(uint32_t);
    shader_module_ci.pCode    = reinterpret_cast<uint32_t*>(shader_spv.data());

    VK_CHECK(vkCreateShaderModule(backend->device_ctx.logical_device, &shader_module_ci, nullptr,
                                  &shader_module));

    return shader_module;
}

void finish_pending_vk_work(const VkBackend* backend) {
    vkDeviceWaitIdle(backend->device_ctx.logical_device);
}

void deinit_backend(VkBackend* backend) {
    DEBUG_PRINT("destroying Vulkan Backend");

    fmt::println("average draw time: {:.3f} us",
                 static_cast<float>(backend->stats.total_draw_time) /
                     static_cast<float>(backend->frame_num));
    fmt::println("average frame time: {:.3f} ms",
                 static_cast<float>(backend->stats.total_frame_time) / 1000.f /
                     static_cast<float>(backend->frame_num));
    fmt::println("average fps: {:.3f}", static_cast<float>(backend->stats.total_fps) /
                                            static_cast<float>(backend->frame_num));

    backend->deletion_queue.flush();

    if constexpr (vk_opts::validation_enabled) {
        deinit_debugger(&backend->debugger, backend->instance);
    }

    for (Frame& frame : backend->frames) {
        deinit_frame(&frame, backend->device_ctx.logical_device);
        destroy_buffer(backend->allocator, &frame.frame_data_buf);
    }

    vkDestroyDescriptorPool(backend->device_ctx.logical_device, backend->imm_descriptor_pool,
                            nullptr);

    destroy_image(backend->device_ctx.logical_device, backend->allocator, &backend->color_image);
    destroy_image(backend->device_ctx.logical_device, backend->allocator, &backend->depth_image);
    destroy_image(backend->device_ctx.logical_device, backend->allocator,
                  &backend->default_texture);
    destroy_image(backend->device_ctx.logical_device, backend->allocator,
                  &backend->color_resolve_image);

    deinit_shader_ctx(&backend->shader_ctx, &backend->ext_ctx, backend->device_ctx.logical_device);

    vkDestroySampler(backend->device_ctx.logical_device, backend->default_nearest_sampler, nullptr);
    vkDestroySampler(backend->device_ctx.logical_device, backend->default_linear_sampler, nullptr);

    vkDestroyPipelineLayout(backend->device_ctx.logical_device,
                            backend->grid_pipeline_info.pipeline_layout, nullptr);
    vkDestroyPipelineLayout(backend->device_ctx.logical_device, backend->geo_pipeline_layout,
                            nullptr);

    vkDestroyDescriptorSetLayout(backend->device_ctx.logical_device,
                                 backend->global_desc_set_layout, nullptr);
    vkDestroyDescriptorSetLayout(backend->device_ctx.logical_device, backend->mat_desc_set_layout,
                                 nullptr);
    vkDestroyDescriptorSetLayout(backend->device_ctx.logical_device,
                                 backend->draw_obj_desc_set_layout, nullptr);

    vkDestroyPipeline(backend->device_ctx.logical_device, backend->grid_pipeline_info.pipeline,
                      nullptr);

    vkDestroyFence(backend->device_ctx.logical_device, backend->imm_fence, nullptr);

    vmaDestroyAllocator(backend->allocator);

    deinit_cmd_context(&backend->imm_cmd_context, backend->device_ctx.logical_device);

    deinit_swapchain_context(&backend->swapchain_context, backend->device_ctx.logical_device,
                             backend->instance);

    deinit_device_context(&backend->device_ctx);

    vkDestroyInstance(backend->instance, nullptr);
}
