#include "App.hpp"

#include <fstream>
#include <stdexcept>
#include <cstring>

#include <spdlog/spdlog.h>

// stb_image: include header only (implementation should come from linked target)
#include <stb_image.h>

static etna::GlobalContext& ctx() { return etna::get_context(); }

std::vector<uint32_t> App::readSpv(const char* path)
{
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open())
    throw std::runtime_error(std::string("Failed to open SPIR-V file: ") + path);

  const std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  if (size <= 0 || (size % 4) != 0)
    throw std::runtime_error(std::string("Invalid SPIR-V size: ") + path);

  std::vector<uint32_t> data(static_cast<size_t>(size / 4));
  if (!file.read(reinterpret_cast<char*>(data.data()), size))
    throw std::runtime_error(std::string("Failed to read SPIR-V file: ") + path);

  return data;
}

uint32_t App::findMemoryType(uint32_t typeBits, vk::MemoryPropertyFlags props)
{
  auto mp = ctx().getPhysicalDevice().getMemoryProperties();
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
  {
    if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
      return i;
  }
  throw std::runtime_error("No suitable memory type");
}

App::App()
{
  startTime = std::chrono::steady_clock::now();

  // Create OS window ONCE
  osWindow = windowing.createWindow(OsWindow::CreateInfo{
    .resolution = resolution,
    .resizeable = true,
  });

  // Init Etna + vkWindow + swapchain
  initEtna();

  // cmd manager once
  commandManager = ctx().createPerFrameCmdMgr();

  // Create resources
  createOrReloadTexture();
  createOffscreenProcImage();
  createDescriptors();
  createPipelines();
}

App::~App()
{
  if (etna::is_initilized())
  {
    ETNA_CHECK_VK_RESULT(ctx().getDevice().waitIdle());
    etna::shutdown();
  }
}

float App::getTimeSeconds() const
{
  const auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::duration<float>>(now - startTime).count();
}

void App::initEtna()
{
  // Required instance extensions from GLFW layer
  auto instExts = windowing.getRequiredVulkanInstanceExtensions();

  // MUST enable swapchain extension (otherwise swapchain ops can segfault)
  static const char* devExtsArr[] = {"VK_KHR_swapchain"};
  std::span<char const* const> devExts{devExtsArr, 1};

  // Enable dynamic rendering + sync2
  vk::PhysicalDeviceSynchronization2Features sync2Feat{};
  sync2Feat.synchronization2 = VK_TRUE;

  vk::PhysicalDeviceDynamicRenderingFeatures dynFeat{};
  dynFeat.dynamicRendering = VK_TRUE;
  dynFeat.pNext = &sync2Feat;

  vk::PhysicalDeviceFeatures2 feats{};
  feats.pNext = &dynFeat;

  etna::InitParams params{};
  params.applicationName = "local_shadertoy2";
  params.applicationVersion = vk::makeApiVersion(0, 1, 0, 0);
  params.instanceExtensions = instExts;
  params.deviceExtensions = devExts;
  params.features = feats;
  params.numFramesInFlight = 2;
  params.generateBarriersAutomatically = true;

  etna::initialize(params);

  // Create surface AFTER instance exists
  auto surface = osWindow->createVkSurface(ctx().getInstance());

  vkWindow = ctx().createWindow(etna::Window::CreateInfo{
    .surface = std::move(surface),
  });

  // Create swapchain once
  auto osRes = osWindow->getResolution();
  if (osRes.x == 0 || osRes.y == 0) osRes = resolution;

  swapExtent = vkWindow->recreateSwapchain(etna::Window::DesiredProperties{
    .resolution = vk::Extent2D{osRes.x, osRes.y},
    .vsync = useVsync,
    .autoGamma = true,
    .numFramesInFlight = params.numFramesInFlight,
  });

  resolution = {swapExtent.width, swapExtent.height};
  procFormat = vkWindow->getCurrentFormat();
}

void App::recreateSwapchainAndOffscreen()
{
  auto osRes = osWindow->getResolution();
  if (osRes.x == 0 || osRes.y == 0)
    return;

  swapExtent = vkWindow->recreateSwapchain(etna::Window::DesiredProperties{
    .resolution = vk::Extent2D{osRes.x, osRes.y},
    .vsync = useVsync,
    .autoGamma = true,
    .numFramesInFlight = static_cast<uint32_t>(commandManager->getCmdBufferCount()),
  });

  resolution = {swapExtent.width, swapExtent.height};
  procFormat = vkWindow->getCurrentFormat();

  // Recreate offscreen RT + descriptors + pipelines (safe and simple)
  createOffscreenProcImage();
  createDescriptors();
  createPipelines();
}

void App::createOffscreenProcImage()
{
  auto device = ctx().getDevice();

  procImage.reset();
  procMemory.reset();
  procView.reset();

  vk::ImageCreateInfo ici{};
  ici.imageType = vk::ImageType::e2D;
  ici.format = procFormat;
  ici.extent = vk::Extent3D{resolution.x, resolution.y, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = vk::SampleCountFlagBits::e1;
  ici.tiling = vk::ImageTiling::eOptimal;
  ici.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
  ici.initialLayout = vk::ImageLayout::eUndefined;

  {
    auto rv = device.createImageUnique(ici);
    ETNA_CHECK_VK_RESULT(rv.result);
    procImage = std::move(rv.value);
  }

  auto memReq = device.getImageMemoryRequirements(procImage.get());
  vk::MemoryAllocateInfo mai{};
  mai.allocationSize = memReq.size;
  mai.memoryTypeIndex =
    findMemoryType(memReq.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

  {
    auto rv = device.allocateMemoryUnique(mai);
    ETNA_CHECK_VK_RESULT(rv.result);
    procMemory = std::move(rv.value);
  }

  ETNA_CHECK_VK_RESULT(device.bindImageMemory(procImage.get(), procMemory.get(), 0));

  vk::ImageViewCreateInfo ivci{};
  ivci.image = procImage.get();
  ivci.viewType = vk::ImageViewType::e2D;
  ivci.format = procFormat;
  ivci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  ivci.subresourceRange.levelCount = 1;
  ivci.subresourceRange.layerCount = 1;

  {
    auto rv = device.createImageViewUnique(ivci);
    ETNA_CHECK_VK_RESULT(rv.result);
    procView = std::move(rv.value);
  }

  if (!procSampler)
  {
    vk::SamplerCreateInfo sci{};
    sci.magFilter = vk::Filter::eLinear;
    sci.minFilter = vk::Filter::eLinear;
    sci.mipmapMode = vk::SamplerMipmapMode::eLinear;
    sci.addressModeU = vk::SamplerAddressMode::eRepeat;
    sci.addressModeV = vk::SamplerAddressMode::eRepeat;
    sci.addressModeW = vk::SamplerAddressMode::eRepeat;
    sci.maxLod = 0.0f;

    auto rv = device.createSamplerUnique(sci);
    ETNA_CHECK_VK_RESULT(rv.result);
    procSampler = std::move(rv.value);
  }
}

void App::createOrReloadTexture()
{
  auto device = ctx().getDevice();

  const std::string texPath =
    std::string(LOCAL_SHADERTOY2_ASSETS_ROOT) + "textures/test_tex_1.png";

  int w = 0, h = 0, comp = 0;
  stbi_uc* pixels = stbi_load(texPath.c_str(), &w, &h, &comp, 4);
  if (!pixels)
    throw std::runtime_error(std::string("stbi_load failed: ") + texPath);

  texW = uint32_t(w);
  texH = uint32_t(h);
  const vk::DeviceSize imageSize = vk::DeviceSize(texW) * vk::DeviceSize(texH) * 4;

  // staging buffer
  vk::UniqueBuffer stagingBuf;
  vk::UniqueDeviceMemory stagingMem;

  vk::BufferCreateInfo bci{};
  bci.size = imageSize;
  bci.usage = vk::BufferUsageFlagBits::eTransferSrc;

  {
    auto rv = device.createBufferUnique(bci);
    ETNA_CHECK_VK_RESULT(rv.result);
    stagingBuf = std::move(rv.value);
  }

  auto bReq = device.getBufferMemoryRequirements(stagingBuf.get());
  vk::MemoryAllocateInfo mai{};
  mai.allocationSize = bReq.size;
  mai.memoryTypeIndex = findMemoryType(
    bReq.memoryTypeBits,
    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

  {
    auto rv = device.allocateMemoryUnique(mai);
    ETNA_CHECK_VK_RESULT(rv.result);
    stagingMem = std::move(rv.value);
  }

  ETNA_CHECK_VK_RESULT(device.bindBufferMemory(stagingBuf.get(), stagingMem.get(), 0));

  {
    auto mapRv = device.mapMemory(stagingMem.get(), 0, imageSize);
    ETNA_CHECK_VK_RESULT(mapRv.result);
    std::memcpy(mapRv.value, pixels, size_t(imageSize));
    device.unmapMemory(stagingMem.get());
  }

  stbi_image_free(pixels);

  // GPU image
  vk::ImageCreateInfo ici{};
  ici.imageType = vk::ImageType::e2D;
  ici.format = vk::Format::eR8G8B8A8Unorm;
  ici.extent = vk::Extent3D{texW, texH, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = vk::SampleCountFlagBits::e1;
  ici.tiling = vk::ImageTiling::eOptimal;
  ici.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
  ici.initialLayout = vk::ImageLayout::eUndefined;

  {
    auto rv = device.createImageUnique(ici);
    ETNA_CHECK_VK_RESULT(rv.result);
    texImage = std::move(rv.value);
  }

  auto iReq = device.getImageMemoryRequirements(texImage.get());
  vk::MemoryAllocateInfo imgAlloc{};
  imgAlloc.allocationSize = iReq.size;
  imgAlloc.memoryTypeIndex =
    findMemoryType(iReq.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

  {
    auto rv = device.allocateMemoryUnique(imgAlloc);
    ETNA_CHECK_VK_RESULT(rv.result);
    texMemory = std::move(rv.value);
  }
  ETNA_CHECK_VK_RESULT(device.bindImageMemory(texImage.get(), texMemory.get(), 0));

  // one-shot cmd to copy
  vk::CommandPoolCreateInfo cpci{};
  cpci.queueFamilyIndex = ctx().getQueueFamilyIdx();
  cpci.flags = vk::CommandPoolCreateFlagBits::eTransient;

  vk::UniqueCommandPool pool;
  {
    auto rv = device.createCommandPoolUnique(cpci);
    ETNA_CHECK_VK_RESULT(rv.result);
    pool = std::move(rv.value);
  }

  vk::CommandBufferAllocateInfo cbai{};
  cbai.commandPool = pool.get();
  cbai.level = vk::CommandBufferLevel::ePrimary;
  cbai.commandBufferCount = 1;

  vk::CommandBuffer cmd;
  {
    auto rv = device.allocateCommandBuffers(cbai);
    ETNA_CHECK_VK_RESULT(rv.result);
    cmd = rv.value[0];
  }

  ETNA_CHECK_VK_RESULT(cmd.begin(vk::CommandBufferBeginInfo{}));

  // transition to transfer dst
  {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
    b.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    b.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    b.oldLayout = vk::ImageLayout::eUndefined;
    b.newLayout = vk::ImageLayout::eTransferDstOptimal;
    b.image = texImage.get();
    b.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    vk::DependencyInfo dep{};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    cmd.pipelineBarrier2(dep);
  }

  vk::BufferImageCopy bic{};
  bic.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  bic.imageSubresource.layerCount = 1;
  bic.imageExtent = vk::Extent3D{texW, texH, 1};

  cmd.copyBufferToImage(
    stagingBuf.get(), texImage.get(), vk::ImageLayout::eTransferDstOptimal, 1, &bic);

  // transition to shader read
  {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    b.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    b.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    b.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    b.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    b.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    b.image = texImage.get();
    b.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    vk::DependencyInfo dep{};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    cmd.pipelineBarrier2(dep);
  }

  ETNA_CHECK_VK_RESULT(cmd.end());

  vk::SubmitInfo si{};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;

  ETNA_CHECK_VK_RESULT(ctx().getQueue().submit(1, &si, {}));
  ETNA_CHECK_VK_RESULT(ctx().getQueue().waitIdle());

  // view + sampler
  vk::ImageViewCreateInfo ivci{};
  ivci.image = texImage.get();
  ivci.viewType = vk::ImageViewType::e2D;
  ivci.format = vk::Format::eR8G8B8A8Unorm;
  ivci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  ivci.subresourceRange.levelCount = 1;
  ivci.subresourceRange.layerCount = 1;

  {
    auto rv = device.createImageViewUnique(ivci);
    ETNA_CHECK_VK_RESULT(rv.result);
    texView = std::move(rv.value);
  }

  vk::SamplerCreateInfo sci{};
  sci.magFilter = vk::Filter::eLinear;
  sci.minFilter = vk::Filter::eLinear;
  sci.mipmapMode = vk::SamplerMipmapMode::eLinear;
  sci.addressModeU = vk::SamplerAddressMode::eRepeat;
  sci.addressModeV = vk::SamplerAddressMode::eRepeat;
  sci.addressModeW = vk::SamplerAddressMode::eRepeat;
  sci.maxLod = 0.0f;

  {
    auto rv = device.createSamplerUnique(sci);
    ETNA_CHECK_VK_RESULT(rv.result);
    texSampler = std::move(rv.value);
  }

  spdlog::info("Loaded texture: {} ({}x{})", texPath, texW, texH);
}

void App::createDescriptors()
{
  auto device = ctx().getDevice();

  // set=0 binding0: proc (combined sampler), binding1: loaded texture
  vk::DescriptorSetLayoutBinding b0{};
  b0.binding = 0;
  b0.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  b0.descriptorCount = 1;
  b0.stageFlags = vk::ShaderStageFlagBits::eFragment;

  vk::DescriptorSetLayoutBinding b1 = b0;
  b1.binding = 1;

  std::array<vk::DescriptorSetLayoutBinding, 2> bindings{b0, b1};

  {
    auto rv = device.createDescriptorSetLayoutUnique(
      vk::DescriptorSetLayoutCreateInfo{
        .bindingCount = uint32_t(bindings.size()),
        .pBindings = bindings.data()
      });
    ETNA_CHECK_VK_RESULT(rv.result);
    dsetLayout = std::move(rv.value);
  }

  vk::DescriptorPoolSize ps{};
  ps.type = vk::DescriptorType::eCombinedImageSampler;
  ps.descriptorCount = 2;

  {
    auto rv = device.createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo{
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &ps,
    });
    ETNA_CHECK_VK_RESULT(rv.result);
    dpool = std::move(rv.value);
  }

  {
    auto allocRv = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo{
      .descriptorPool = dpool.get(),
      .descriptorSetCount = 1,
      .pSetLayouts = &dsetLayout.get(),
    });
    ETNA_CHECK_VK_RESULT(allocRv.result);
    dset = allocRv.value[0];
  }

  if (!procView || !texView)
    return;

  vk::DescriptorImageInfo procInfo{procSampler.get(), procView.get(), vk::ImageLayout::eShaderReadOnlyOptimal};
  vk::DescriptorImageInfo texInfo{texSampler.get(), texView.get(), vk::ImageLayout::eShaderReadOnlyOptimal};

  vk::WriteDescriptorSet w0{};
  w0.dstSet = dset;
  w0.dstBinding = 0;
  w0.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  w0.descriptorCount = 1;
  w0.pImageInfo = &procInfo;

  vk::WriteDescriptorSet w1 = w0;
  w1.dstBinding = 1;
  w1.pImageInfo = &texInfo;

  std::array<vk::WriteDescriptorSet, 2> writes{w0, w1};
  device.updateDescriptorSets(uint32_t(writes.size()), writes.data(), 0, nullptr);
}

void App::createPipelines()
{
  auto device = ctx().getDevice();

  const std::string root = std::string(LOCAL_SHADERTOY2_SHADERS_ROOT);

  auto vertSpv = readSpv((root + "fullscreen.vert.spv").c_str());
  auto procSpv = readSpv((root + "proc.frag.spv").c_str());
  auto mainSpv = readSpv((root + "toy.frag.spv").c_str());

  {
    auto rv = device.createShaderModuleUnique(vk::ShaderModuleCreateInfo{
      .codeSize = vertSpv.size() * sizeof(uint32_t),
      .pCode = vertSpv.data(),
    });
    ETNA_CHECK_VK_RESULT(rv.result);
    vertModule = std::move(rv.value);
  }
  {
    auto rv = device.createShaderModuleUnique(vk::ShaderModuleCreateInfo{
      .codeSize = procSpv.size() * sizeof(uint32_t),
      .pCode = procSpv.data(),
    });
    ETNA_CHECK_VK_RESULT(rv.result);
    procFragModule = std::move(rv.value);
  }
  {
    auto rv = device.createShaderModuleUnique(vk::ShaderModuleCreateInfo{
      .codeSize = mainSpv.size() * sizeof(uint32_t),
      .pCode = mainSpv.data(),
    });
    ETNA_CHECK_VK_RESULT(rv.result);
    mainFragModule = std::move(rv.value);
  }

  vk::PushConstantRange pcr{};
  pcr.stageFlags = vk::ShaderStageFlagBits::eFragment;
  pcr.offset = 0;
  pcr.size = sizeof(Push);

  // Pass A layout (no descriptors)
  {
    auto rv = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
      .setLayoutCount = 0,
      .pSetLayouts = nullptr,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
    });
    ETNA_CHECK_VK_RESULT(rv.result);
    procPipelineLayout = std::move(rv.value);
  }

  // Pass B layout (descriptor set + push constants)
  {
    auto rv = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo{
      .setLayoutCount = 1,
      .pSetLayouts = &dsetLayout.get(),
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
    });
    ETNA_CHECK_VK_RESULT(rv.result);
    mainPipelineLayout = std::move(rv.value);
  }

  auto makePipe = [&](vk::ShaderModule frag, vk::PipelineLayout layout, vk::Format fmt) -> vk::UniquePipeline {
    vk::PipelineShaderStageCreateInfo stages[2] = {
      vk::PipelineShaderStageCreateInfo{
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = vertModule.get(),
        .pName = "main",
      },
      vk::PipelineShaderStageCreateInfo{
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = frag,
        .pName = "main",
      },
    };

    vk::PipelineVertexInputStateCreateInfo vi{};
    vk::PipelineInputAssemblyStateCreateInfo ia{.topology = vk::PrimitiveTopology::eTriangleList};

    vk::DynamicState dynStates[] = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dyn{.dynamicStateCount = 2, .pDynamicStates = dynStates};

    vk::PipelineViewportStateCreateInfo vp{.viewportCount = 1, .scissorCount = 1};

    vk::PipelineRasterizationStateCreateInfo rs{};
    rs.polygonMode = vk::PolygonMode::eFill;
    rs.cullMode = vk::CullModeFlagBits::eNone;
    rs.frontFace = vk::FrontFace::eCounterClockwise;
    rs.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples = vk::SampleCountFlagBits::e1};

    vk::PipelineColorBlendAttachmentState cbAttach{};
    cbAttach.colorWriteMask =
      vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo cb{.attachmentCount = 1, .pAttachments = &cbAttach};

    vk::PipelineRenderingCreateInfo renderingInfo{.colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};

    vk::GraphicsPipelineCreateInfo gpci{};
    gpci.pNext = &renderingInfo;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = layout;
    gpci.renderPass = VK_NULL_HANDLE;

    auto rv = device.createGraphicsPipelineUnique(VK_NULL_HANDLE, gpci);
    ETNA_CHECK_VK_RESULT(rv.result);
    return std::move(rv.value);
  };

  procPipeline = makePipe(procFragModule.get(), procPipelineLayout.get(), procFormat);
  mainPipeline = makePipe(mainFragModule.get(), mainPipelineLayout.get(), vkWindow->getCurrentFormat());
}

void App::drawFrame()
{
  etna::begin_frame();

  auto next = vkWindow->acquireNext();
  if (!next)
  {
    recreateSwapchainAndOffscreen();
    etna::end_frame();
    return;
  }

  auto [backbuffer, backbufferView, backbufferAvailableSem, backbufferReadyForPresentSem] = *next;

  vk::CommandBuffer cmd = commandManager->acquireNext();
  ETNA_CHECK_VK_RESULT(cmd.begin(vk::CommandBufferBeginInfo{}));

  // ---- PASS A: render procedural into procImage ----
  etna::set_state(
    cmd, procImage.get(),
    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    vk::AccessFlagBits2::eColorAttachmentWrite,
    vk::ImageLayout::eColorAttachmentOptimal,
    vk::ImageAspectFlagBits::eColor);
  etna::flush_barriers(cmd);

  vk::ClearValue clearA{};
  clearA.color = vk::ClearColorValue(std::array<float,4>{0,0,0,1});

  vk::RenderingAttachmentInfo attA{
    .imageView = procView.get(),
    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eStore,
    .clearValue = clearA,
  };

  vk::RenderingInfo riA{
    .renderArea = vk::Rect2D(vk::Offset2D{0,0}, vk::Extent2D{resolution.x, resolution.y}),
    .layerCount = 1,
    .colorAttachmentCount = 1,
    .pColorAttachments = &attA,
  };

  cmd.beginRendering(riA);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, procPipeline.get());

  vk::Viewport vp{0,0, float(resolution.x), float(resolution.y), 0.0f, 1.0f};
  vk::Rect2D sc{vk::Offset2D{0,0}, vk::Extent2D{resolution.x, resolution.y}};
  cmd.setViewport(0, 1, &vp);
  cmd.setScissor(0, 1, &sc);

  Push pc{};
  pc.iResolution = glm::vec2(float(resolution.x), float(resolution.y));
  pc.iTime = getTimeSeconds();
  pc._pad0 = 0.0f;
  pc.iMouse = glm::vec4(0,0,0,0);

  cmd.pushConstants(procPipelineLayout.get(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(Push), &pc);
  cmd.draw(3, 1, 0, 0);
  cmd.endRendering();

  // Make proc readable for pass B
  etna::set_state(
    cmd, procImage.get(),
    vk::PipelineStageFlagBits2::eFragmentShader,
    vk::AccessFlagBits2::eShaderRead,
    vk::ImageLayout::eShaderReadOnlyOptimal,
    vk::ImageAspectFlagBits::eColor);
  etna::flush_barriers(cmd);

  // ---- PASS B: render final into swapchain ----
  etna::set_state(
    cmd, backbuffer,
    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    vk::AccessFlagBits2::eColorAttachmentWrite,
    vk::ImageLayout::eColorAttachmentOptimal,
    vk::ImageAspectFlagBits::eColor);
  etna::flush_barriers(cmd);

  vk::ClearValue clearB{};
  clearB.color = vk::ClearColorValue(std::array<float,4>{0,0,0,1});

  vk::RenderingAttachmentInfo attB{
    .imageView = backbufferView,
    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .loadOp = vk::AttachmentLoadOp::eClear,
    .storeOp = vk::AttachmentStoreOp::eStore,
    .clearValue = clearB,
  };

  vk::RenderingInfo riB{
    .renderArea = vk::Rect2D(vk::Offset2D{0,0}, vk::Extent2D{resolution.x, resolution.y}),
    .layerCount = 1,
    .colorAttachmentCount = 1,
    .pColorAttachments = &attB,
  };

  cmd.beginRendering(riB);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, mainPipeline.get());
  cmd.setViewport(0, 1, &vp);
  cmd.setScissor(0, 1, &sc);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, mainPipelineLayout.get(), 0, 1, &dset, 0, nullptr);

  cmd.pushConstants(mainPipelineLayout.get(), vk::ShaderStageFlagBits::eFragment, 0, sizeof(Push), &pc);
  cmd.draw(3, 1, 0, 0);
  cmd.endRendering();

  etna::set_state(
    cmd, backbuffer,
    vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    {},
    vk::ImageLayout::ePresentSrcKHR,
    vk::ImageAspectFlagBits::eColor);
  etna::flush_barriers(cmd);

  ETNA_CHECK_VK_RESULT(cmd.end());

  auto renderingDone = commandManager->submit(
    cmd, backbufferAvailableSem, std::move(backbufferReadyForPresentSem));

  vkWindow->present(std::move(renderingDone), backbufferView);

  etna::end_frame();
}

void App::run()
{
  while (!osWindow->isBeingClosed())
  {
    windowing.poll();
    drawFrame();
  }
}
