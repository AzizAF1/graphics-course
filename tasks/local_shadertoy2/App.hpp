#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <etna/Etna.hpp>
#include <etna/Window.hpp>
#include <etna/PerFrameCmdMgr.hpp>

#include "wsi/OsWindowingManager.hpp"

class App
{
public:
  App();
  ~App();

  void run();

private:
  // ---- init ----
  void initEtna();
  void recreateSwapchainAndOffscreen();

  // ---- resources ----
  void createOffscreenProcImage();
  void createOrReloadTexture();
  void createDescriptors();
  void createPipelines();

  // ---- frame ----
  void drawFrame();
  float getTimeSeconds() const;

  // ---- helpers ----
  static std::vector<uint32_t> readSpv(const char* path);
  static uint32_t findMemoryType(uint32_t typeBits, vk::MemoryPropertyFlags props);

private:
  OsWindowingManager windowing;
  std::unique_ptr<OsWindow> osWindow;

  glm::uvec2 resolution{1280, 720};
  bool useVsync = true;

  std::unique_ptr<etna::Window> vkWindow;
  std::unique_ptr<etna::PerFrameCmdMgr> commandManager;

  vk::Extent2D swapExtent{};
  vk::Format procFormat{vk::Format::eB8G8R8A8Unorm};

  std::chrono::steady_clock::time_point startTime;

  struct Push
  {
    glm::vec2 iResolution;
    float iTime;
    float _pad0;
    glm::vec4 iMouse;
  };

  // ---------- Offscreen proc RT ----------
  vk::UniqueImage procImage{};
  vk::UniqueDeviceMemory procMemory{};
  vk::UniqueImageView procView{};
  vk::UniqueSampler procSampler{};

  // ---------- Loaded texture ----------
  uint32_t texW = 0, texH = 0;
  vk::UniqueImage texImage{};
  vk::UniqueDeviceMemory texMemory{};
  vk::UniqueImageView texView{};
  vk::UniqueSampler texSampler{};

  // ---------- Descriptor set (proc + tex) ----------
  vk::UniqueDescriptorSetLayout dsetLayout{};
  vk::UniqueDescriptorPool dpool{};
  vk::DescriptorSet dset{};

  // ---------- Shaders / pipelines ----------
  vk::UniqueShaderModule vertModule{};
  vk::UniqueShaderModule procFragModule{};
  vk::UniqueShaderModule mainFragModule{};

  vk::UniquePipelineLayout procPipelineLayout{};
  vk::UniquePipelineLayout mainPipelineLayout{};

  vk::UniquePipeline procPipeline{};
  vk::UniquePipeline mainPipeline{};
};
