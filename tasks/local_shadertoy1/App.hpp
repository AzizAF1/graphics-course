#pragma once

#include <memory>

#include <glm/glm.hpp>

#include <wsi/OsWindow.hpp>
#include <wsi/OsWindowingManager.hpp>

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

private:
  glm::uvec2 resolution{1280, 720};
  bool useVsync{true};

  OsWindowingManager windowing;
  std::unique_ptr<OsWindow> osWindow;

  std::unique_ptr<etna::Window> vkWindow;
  std::unique_ptr<etna::PerFrameCmdMgr> commandManager;
};
