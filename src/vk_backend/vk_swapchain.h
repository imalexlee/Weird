#include "vk_backend/vk_device.h"
#include "vk_backend/vk_utils.h"
#include <cstdint>
#include <vector>
#include <vk_backend/vk_types.h>
#include <vulkan/vulkan_core.h>

struct SwapChainSupportDetails {
  VkSurfaceCapabilitiesKHR capabilities;
  std::vector<VkSurfaceFormatKHR> formats;
  std::vector<VkPresentModeKHR> present_modes;
};

class SwapchainContext {
public:
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkExtent2D extent;
  VkFormat format;
  VkSurfaceKHR surface;
  std::vector<VkImage> images;
  std::vector<VkImageView> image_views;

  void create(VkInstance& instance, DeviceContext& device_controller, VkSurfaceKHR surface, uint32_t width,
              uint32_t height, VkPresentModeKHR desired_present_mode = VK_PRESENT_MODE_FIFO_KHR);
  void destroy();
  void create_swapchain(DeviceContext& device_controller, uint32_t width, uint32_t height);
  void destroy_swapchain(VkDevice device);

private:
  SwapChainSupportDetails _support_details;
  VkPresentModeKHR _present_mode;
  DeletionQueue _deletion_queue;

  SwapChainSupportDetails query_support_details(VkPhysicalDevice physical_device);
};
