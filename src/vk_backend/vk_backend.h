#pragma once

#include "core/window.h"
#include "vk_backend/resources/vk_image.h"
#include "vk_backend/vk_debug.h"
#include "vk_backend/vk_device.h"
#include "vk_backend/vk_frame.h"
#include <cstdint>
#include <vector>
#include <vk_backend/vk_swapchain.h>
#include <vk_backend/vk_types.h>

constexpr uint64_t FRAME_NUM = 3;

class VkBackend {
public:
  VkBackend(){};
  ~VkBackend() { destroy(); };

  void create(Window& window);
  void draw();

private:
  VkInstance _instance;
  VkSurfaceKHR _surface;
  Debugger _debugger;
  DeviceContext _device_context;
  SwapchainContext _swapchain_context;
  VmaAllocator _allocator;
  AllocatedImage _draw_image;
  std::array<Frame, FRAME_NUM> _frames;
  uint64_t _frame_num{1};

  // initialization
  void create_instance(GLFWwindow* window);
  void create_allocator();

  // core functions
  void draw_geometry(VkCommandBuffer cmd_buf, VkExtent2D extent, uint32_t swapchain_img_idx);

  // deinitialization
  void destroy();

  // utils
  inline uint64_t get_frame_index() { return _frame_num % FRAME_NUM; }
  std::vector<const char*> get_instance_extensions(GLFWwindow* window);
};
