#include "App.hpp"

#include <array>
#include <filesystem>

#include <etna/Etna.hpp>
#include <etna/GlobalContext.hpp>
#include <etna/PipelineManager.hpp>

#ifndef VK_USE_PLATFORM_MACOS_MVK
// nothing
#endif

// Your CMake for the task usually defines this like in other samples.
// If your build fails saying LOCAL_SHADERTOY1_SHADERS_ROOT is not defined,
// see the note after the code.
#ifndef LOCAL_SHADERTOY1_SHADERS_ROOT
#define LOCAL_SHADERTOY1_SHADERS_ROOT ""
#endif

static bool isValidImage(const etna::Image& img)
{
  return img.get() != vk::Image{};
}

App::App()
{
  // Create OS window first (needed for required Vulkan instance extensions)
  osWindow = windowing.createWindow(OsWindow::CreateInfo{
    .resolution = resolution,
    .resizeable = true,
    .refreshCb = [this]() { this->drawFrame(); },
    .resizeCb = [this](glm::uvec2 newRes) { this->onResize(newRes); },
  });

  // Initialize Etna exactly once
  if (!etna::is_initilized())
  {
    auto instExts = windowing.getRequiredVulkanInstanceExtensions();

    static const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    etna::InitParams p{};
    p.applicationName = "local_shadertoy1";
    p.applicationVersion = vk::makeApiVersion(0, 1, 0, 0);
    p.instanceExtensions = instExts;
    p.deviceExtensions = std::span<char const* const>(devExts, std::size(devExts));
    p.numFramesInFlight = 2;
    p.generateBarriersAutomatically = true;

    etna::initialize(p);
  }

  startTime = std::chrono::steady_clock::now();

  // Create Vulkan surface using Etna instance
  auto& ctx = etna::get_context();
  vk::UniqueSurfaceKHR surface = osWindow->createVkSurface(ctx.getInstance());

  // Create Etna Window (swapchain wrapper)
  etna::Window::Dependencies wdeps{
    .workCount = ctx.getMainWorkCount(),
    .physicalDevice = ctx.getPhysicalDevice(),
    .device = ctx.getDevice(),
    .presentQueue = ctx.getQueue(),
    .queueFamily = ctx.getQueueFamilyIdx(),
  };

  vkWindow = std::make_unique<etna::Window>(wdeps, etna::Window::CreateInfo{ .surface = std::move(surface) });

  // Create swapchain (must not be 0x0)
  {
    const auto r = osWindow->getResolution();
    if (r.x > 0 && r.y > 0)
      resolution = r;

    vkWindow->recreateSwapchain(etna::Window::DesiredProperties{
      .resolution = vk::Extent2D{resolution.x, resolution.y},
      .vsync = useVsync,
      .autoGamma = true,
      .numFramesInFlight = 2,
    });
  }

  // Create command manager
  etna::PerFrameCmdMgr::Dependencies cdeps{
    .workCount = ctx.getMainWorkCount(),
    .device = ctx.getDevice(),
    .submitQueue = ctx.getQueue(),
    .queueFamily = ctx.getQueueFamilyIdx(),
  };
  commandManager = std::make_unique<etna::PerFrameCmdMgr>(cdeps);

  // Create pipeline + storage image
  initToyPipeline();
  createOrResizeToyImage();
}

App::~App()
{
  // Safe to call even if already shut down, but we only do it when initialized.
  if (etna::is_initilized())
    etna::shutdown();
}

void App::onResize(glm::uvec2 newRes)
{
  resolution = newRes;

  if (!vkWindow)
    return;

  // If minimized => (0,0), can't recreate swapchain.
  if (resolution.x == 0 || resolution.y == 0)
    return;

  vkWindow->recreateSwapchain(etna::Window::DesiredProperties{
    .resolution = vk::Extent2D{resolution.x, resolution.y},
    .vsync = useVsync,
    .autoGamma = true,
    .numFramesInFlight = static_cast<uint32_t>(commandManager ? commandManager->getCmdBufferCount() : 2),
  });

  createOrResizeToyImage();
}

float App::getTimeSeconds() const
{
  const auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::duration<float>>(now - startTime).count();
}

void App::initToyPipeline()
{
  // Load SPIR-V program
  etna::create_program("toy", { std::filesystem::path(LOCAL_SHADERTOY1_SHADERS_ROOT) / "toy.comp.spv" });

  // Create compute pipeline from program
  auto& ctx = etna::get_context();
  toyPipeline = ctx.getPipelineManager().createComputePipeline("toy", {});
}

void App::createOrResizeToyImage()
{
  if (resolution.x == 0 || resolution.y == 0)
    return;

  if (isValidImage(toyImage))
  {
    const auto ext = toyImage.getExtent();
    if (ext.width == resolution.x && ext.height == resolution.y)
      return;
  }

  etna::Image::CreateInfo ci{};
  ci.extent = vk::Extent3D{ resolution.x, resolution.y, 1 };
  ci.format = toyFormat;

  // Etna uses "imageUsage" (NOT "usage") in this version style (similar to Buffer::CreateInfo::bufferUsage).
  ci.imageUsage =
    vk::ImageUsageFlagBits::eStorage |
    vk::ImageUsageFlagBits::eTransferSrc;

  ci.name = "toy_storage_image";

  toyImage = etna::get_context().createImage(ci);
}

void App::run()
{
  while (!osWindow->isBeingClosed())
  {
    windowing.poll();
    drawFrame();
  }
}

void App::drawFrame()
{
  if (!vkWindow || !commandManager)
    return;

  // skip minimized
  const auto curRes = osWindow->getResolution();
  if (curRes.x == 0 || curRes.y == 0)
    return;

  if (curRes != resolution)
    onResize(curRes);

  auto cmd = commandManager->acquireNext();

  etna::begin_frame();

  auto next = vkWindow->acquireNext();
  if (!next)
  {
    etna::end_frame();
    return;
  }

  auto [backbuffer, backbufferView, backbufferAvailableSem, backbufferReadyForPresentSem] = *next;

  ETNA_CHECK_VK_RESULT(cmd.begin(vk::CommandBufferBeginInfo{}));
  {
    // Backbuffer -> TransferDst for blit destination
    etna::set_state(
      cmd,
      backbuffer,
      vk::PipelineStageFlagBits2::eTransfer,
      vk::AccessFlagBits2::eTransferWrite,
      vk::ImageLayout::eTransferDstOptimal,
      vk::ImageAspectFlagBits::eColor);

    // Storage image -> General for compute writes
    createOrResizeToyImage();

    etna::set_state(
      cmd,
      toyImage.get(),
      vk::PipelineStageFlagBits2::eComputeShader,
      vk::AccessFlagBits2::eShaderWrite,
      vk::ImageLayout::eGeneral,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(cmd);

    // Descriptor set: set=0 binding=0 = storage image
    auto progInfo = etna::get_shader_program("toy");

    auto set = etna::create_descriptor_set(
      progInfo.getDescriptorLayoutId(0),
      cmd,
      {
        etna::Binding{0, toyImage.genBinding(vk::Sampler{}, vk::ImageLayout::eGeneral, {})},
      });

    vk::DescriptorSet vkSet = set.getVkSet();

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, toyPipeline.getVkPipeline());
    cmd.bindDescriptorSets(
      vk::PipelineBindPoint::eCompute,
      toyPipeline.getVkPipelineLayout(),
      0,
      1,
      &vkSet,
      0,
      nullptr);

    // Push constants
    Push pc{};
    pc.iResolution = glm::vec2(float(resolution.x), float(resolution.y));
    pc.iTime = getTimeSeconds();
    pc._pad0 = 0.0f;
    pc.iMouse = glm::vec4(0, 0, 0, 0); // stubbed

    cmd.pushConstants(
      toyPipeline.getVkPipelineLayout(),
      vk::ShaderStageFlagBits::eCompute,
      0,
      sizeof(Push),
      &pc);

    // Dispatch (local_size = 32x32)
    const uint32_t groupX = (resolution.x + 31u) / 32u;
    const uint32_t groupY = (resolution.y + 31u) / 32u;
    cmd.dispatch(groupX, groupY, 1);

    // Storage image -> TransferSrc for blit source
    etna::set_state(
      cmd,
      toyImage.get(),
      vk::PipelineStageFlagBits2::eTransfer,
      vk::AccessFlagBits2::eTransferRead,
      vk::ImageLayout::eTransferSrcOptimal,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(cmd);

    // Blit storage -> backbuffer
    vk::ImageBlit blit{};
    blit.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    blit.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    blit.srcOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.srcOffsets[1] = vk::Offset3D{int32_t(resolution.x), int32_t(resolution.y), 1};
    blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.dstOffsets[1] = vk::Offset3D{int32_t(resolution.x), int32_t(resolution.y), 1};

    cmd.blitImage(
      toyImage.get(),
      vk::ImageLayout::eTransferSrcOptimal,
      backbuffer,
      vk::ImageLayout::eTransferDstOptimal,
      1,
      &blit,
      vk::Filter::eNearest);

    // Backbuffer -> Present
    etna::set_state(
      cmd,
      backbuffer,
      vk::PipelineStageFlagBits2::eColorAttachmentOutput,
      {},
      vk::ImageLayout::ePresentSrcKHR,
      vk::ImageAspectFlagBits::eColor);

    etna::flush_barriers(cmd);
  }
  ETNA_CHECK_VK_RESULT(cmd.end());

  // Submit and present
  auto renderingDone = commandManager->submit(
    cmd,
    backbufferAvailableSem,
    std::move(backbufferReadyForPresentSem));

  const bool presented = vkWindow->present(renderingDone, backbufferView);
  (void)presented;

  etna::end_frame();
}
