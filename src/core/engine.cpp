#include "engine.h"
#include "core_options.h"
#include "loaders/gltf_loader.h"

#include <GLFW/glfw3.h>
#include <array>
#include <core/camera.h>
#include <core/editor.h>
#include <core/scene.h>
#include <core/window.h>
#include <stb_image.h>

#include <vk_backend/vk_backend.h>

static Engine* active_engine = nullptr;

void engine_init(Engine* engine, EngineMode mode) {
    assert(active_engine == nullptr);
    active_engine = engine;

    engine->mode = mode;

    constexpr glm::vec4 init_cam_pos = {0, 0, 8, 1};

    window_init(&engine->window, core_opts::initial_width, core_opts::initial_height, "Triangle Machine");

    camera_init(&engine->camera, &engine->window, init_cam_pos);

    audio_ctx_init(&engine->audio_ctx);

    const VkInstance   instance = vk_instance_create("triangle machine", "my engine");
    const VkSurfaceKHR surface  = vk_surface_get(&engine->window, instance);

    backend_init(&engine->backend, instance, surface, engine->window.width, engine->window.height, mode);
    if (mode == EngineMode::EDIT) {
        editor_init(&engine->editor, &engine->backend, &engine->camera, engine->window.glfw_window);
    }

    backend_upload_vert_shader(&engine->backend, "../shaders/vertex/indexed_draw.vert", "vert shader");
    backend_upload_frag_shader(&engine->backend, "../shaders/fragment/pbr.frag", "frag shader");
    backend_upload_sky_box_shaders(&engine->backend, "../shaders/vertex/skybox.vert", "../shaders/fragment/skybox.frag", "skybox shaders");

    std::array file_names = {
        "../assets/skybox/right.jpg",  "../assets/skybox/left.jpg",  "../assets/skybox/top.jpg",
        "../assets/skybox/bottom.jpg", "../assets/skybox/front.jpg", "../assets/skybox/back.jpg",
    };

    int width, height, nr_channels;
    stbi_info(file_names[0], &width, &height, &nr_channels);

    std::vector<uint8_t> skybox_data;
    skybox_data.resize(width * height * 4 * 6); // 4 channels. 6 total images

    for (size_t i = 0; i < 6; i++) {
        uint8_t* data      = stbi_load(file_names[i], &width, &height, &nr_channels, 4);
        size_t   data_size = width * height * 4;
        size_t   offset    = data_size * i;
        memcpy(skybox_data.data() + offset, data, data_size);
    }

    backend_upload_sky_box(&engine->backend, skybox_data.data(), 4, width, height);

    backend_upload_cursor_shaders(&engine->backend);

    window_register_key_callback(&engine->window, [=](int key, int scancode, int action, int mods) {
        camera_key_callback(&engine->camera, key, scancode, action, mods);
        // scene_key_callback(&engine->scene, key, action);
    });

    window_register_cursor_callback(&engine->window, [=](double x_pos, double y_pos) { camera_cursor_callback(&engine->camera, x_pos, y_pos); });

    window_register_mouse_button_callback(
        &engine->window, [=](int button, int action, int mods) { camera_mouse_button_callback(&engine->camera, button, action, mods); });

    if (mode == EngineMode::RELEASE) {
        // const std::string gltf_path = "../assets/glb/structure.glb";
        // scene_load_gltf_path(&engine->scene, &engine->backend, gltf_path);
    }
}

void engine_run(Engine* engine) {
    WorldData world_data{};
    while (!glfwWindowShouldClose(engine->window.glfw_window)) {
        glfwPollEvents();

        uint32_t viewport_width  = engine->window.width;
        uint32_t viewport_height = engine->window.height;

        if (engine->mode == EngineMode::EDIT) {
            viewport_width  = engine->editor.viewport_width;
            viewport_height = engine->editor.viewport_height;
        }

        world_data = camera_update(&engine->camera, viewport_width, viewport_height);

        if (engine->mode == EngineMode::EDIT) {
            editor_update(&engine->editor, &engine->backend, &engine->window, &engine->camera, &engine->scene);
        }

        if (engine->mode == EngineMode::EDIT) {
            if (engine->editor.quit) {
                break;
            }
        }
        scene_update(&engine->scene, &engine->backend);
        backend_draw(&engine->backend, engine->scene.entities, &world_data, 0, 0);
    }
}

void engine_deinit(Engine* engine) {
    backend_finish_pending_vk_work(&engine->backend);

    if (engine->mode == EngineMode::EDIT) {
        editor_deinit(&engine->editor);
    }
    window_deinit(&engine->window);
    backend_deinit(&engine->backend);
}
uint16_t engine_select_entity_at(const Engine* engine, int32_t x, int32_t y) { return backend_entity_id_at_pos(&engine->backend, x, y); }