#pragma once

#include "core/window.h"
#include "vk_backend/resources/vk_image.h"
#include "vk_backend/vk_command.h"
#include "vk_backend/vk_debug.h"
#include "vk_backend/vk_device.h"
#include "vk_backend/vk_frame.h"
#include "vk_backend/vk_utils.h"
#include <core/camera.h>
#include <core/ctpl_stl.h>
#include <core/thread_pool.h>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <vk_backend/vk_scene.h>
#include <vk_backend/vk_swapchain.h>
#include <vk_backend/vk_types.h>
#include <vulkan/vulkan_core.h>

constexpr uint64_t FRAME_NUM = 3;

constexpr uint32_t CHECKER_WIDTH = 32;
[[maybe_unused]] static constexpr std::array<uint32_t, CHECKER_WIDTH * CHECKER_WIDTH> white_image = []() {
  std::array<uint32_t, CHECKER_WIDTH * CHECKER_WIDTH> result{};
  uint32_t black = __builtin_bswap32(0xFFFFFFFF);
  for (uint32_t& el : result) {
    el = black;
  }
  return result;
}();

class VkBackend {
public:
  VkBackend(){};
  ~VkBackend() { destroy(); };

  void create(Window& window, Camera& camera);
  void draw();
  void resize();

private:
  VkInstance _instance;
  DeviceContext _device_context;
  VkSurfaceKHR _surface;
  Debugger _debugger;
  SwapchainContext _swapchain_context;
  VmaAllocator _allocator;

  Scene _scene;
  Camera* _camera;

  CommandContext _imm_cmd_context;
  VkFence _imm_fence;

  AllocatedImage _draw_image;
  AllocatedImage _depth_image;

  uint64_t _frame_num{1};
  std::array<Frame, FRAME_NUM> _frames;
  SceneData _scene_data;

  // defaults
  VkSampler _default_linear_sampler;
  VkSampler _default_nearest_sampler;
  AllocatedImage _default_texture;

  // ThreadPool _thread_pool;

  ctpl::thread_pool _thread_pool;
  DeletionQueue _deletion_queue;

  // initialization
  void create_instance();
  void create_allocator();
  void create_pipelines();
  void create_default_data();

  // core functions
  void draw_geometry(VkCommandBuffer cmd_buf, VkExtent2D extent, uint32_t swapchain_img_idx);
  void update_scene();

  // deinitialization
  void destroy();

  // utils
  inline Frame& get_current_frame() { return _frames[_frame_num % FRAME_NUM]; }
  std::vector<const char*> get_instance_extensions();
  void immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function);
  MeshBuffers upload_mesh_buffers(std::span<uint32_t> indices, std::span<Vertex> vertices);
  AllocatedImage upload_texture_image(void* data, VkImageUsageFlags usage, uint32_t height, uint32_t width);

  friend Scene load_scene(VkBackend* backend, std::filesystem::path path);
  friend void destroy_scene(VkBackend* backend, Scene& scene);
  friend AllocatedImage generate_texture(VkBackend* backend, fastgltf::Asset& asset, fastgltf::Texture& gltf_texture);
};
