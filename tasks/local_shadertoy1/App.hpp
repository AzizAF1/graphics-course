#pragma once

#include <chrono>
#include <memory>

#include <glm/glm.hpp>

#include <etna/Window.hpp>
#include <etna/PerFrameCmdMgr.hpp>
#include <etna/ComputePipeline.hpp>
#include <etna/Image.hpp>

#include "wsi/OsWindowingManager.hpp"

class App
{
public:
  App();
  ~App();

  void run();

private:
  void drawFrame();
  void onResize(glm::uvec2 newRes);

  void initToyPipeline();
  void createOrResizeToyImage();
  float getTimeSeconds() const;

private:
  OsWindowingManager windowing;
  std::unique_ptr<OsWindow> osWindow;

  glm::uvec2 resolution{1280, 720};
  bool useVsync = true;

  std::unique_ptr<etna::Window> vkWindow;
  std::unique_ptr<etna::PerFrameCmdMgr> commandManager;

  // ---- Local Shadertoy resources ----
  etna::ComputePipeline toyPipeline{};
  etna::Image toyImage{}; // storage image
  vk::Format toyFormat = vk::Format::eR8G8B8A8Unorm;

  std::chrono::steady_clock::time_point startTime{};

  struct Push
  {
    glm::vec2 iResolution; // w,h
    float iTime;
    float _pad0;
    glm::vec4 iMouse;
  };
};
