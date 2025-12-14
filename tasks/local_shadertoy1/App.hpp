#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include <glm/glm.hpp>

#include <wsi/OsWindow.hpp>
#include <wsi/OsWindowingManager.hpp>

#include <etna/Image.hpp>
#include <etna/PipelineManager.hpp>
#include <etna/PerFrameCmdMgr.hpp>
#include <etna/Window.hpp>

class App
{
public:
  App();
  ~App();

  void run();

private:
  void drawFrame();
  void recreateStorageImage();

private:
  glm::uvec2 resolution;
  bool useVsync;

  OsWindowingManager windowing;
  std::unique_ptr<OsWindow> osWindow;

  std::unique_ptr<etna::Window> vkWindow;
  std::unique_ptr<etna::PerFrameCmdMgr> commandManager;

  std::chrono::steady_clock::time_point startTime{};

  std::optional<etna::Image> storageImage;
  std::optional<etna::ComputePipeline> toyPipeline;
};
