// ============================================================================
// Vulkan 后端实现（macOS 经 MoltenVK，Android 原生）
//
// 架构要点：
// - MoltenVK 集成：macOS 直接链接 libMoltenVK.dylib（ICD），不经过 Vulkan Loader，
//   因此不请求 VK_KHR_portability_enumeration（loader 层扩展）；device 级
//   VK_KHR_portability_subset 在可用时启用（MoltenVK 要求）。
// - 单命令缓冲 + 单队列串行模型（P0 简化）：全设备一个 VkCommandBuffer，
//   acquireCommandBuffer reset+begin，submit end+queueSubmit，waitIdle=vkQueueWaitIdle。
// - 绑定约定（见 rhi_types.h）：单一 descriptor set（set 0），binding 0..3=uniform
//   buffer（texture slot N 用 binding N+4 的 combined-image-sampler）；bind 时直接
//   vkUpdateDescriptorSets 写全局唯一 set。
// - 坐标系：用负高度视口做 y 翻转，对齐 Metal/GLES 的 y-up NDC；翻转后正面绕序
//   变 CW（frontFace=CLOCKWISE）。
// - readback：每个离屏目标带一张 LINEAR tiling 的 staging 图像；endRenderPass 时
//   把颜色附件拷贝进 staging，readbackTarget 直接 map 读取（注意 rowPitch 对齐）。
// - render pass 与 swapchain 表面格式绑定：swapchain 表面格式 ≠ RGBA8 时重建
//   render pass（须在创建 pipeline 之前，engine 流程保证 set_surface 先执行）。
// ============================================================================
#include "vulkan_device.h"
#include "foundation/log.h"
#include <vulkan/vulkan.h>
#if defined(__ANDROID__)
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#endif
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

/// Vulkan 调用检查：失败记日志（含表达式、错误码、行号）并 return false。
/// 只能用于返回 bool 的函数。
#define VK_CHECK(x)                                                    \
  do {                                                                 \
    VkResult res_ = (x);                                               \
    if (res_ != VK_SUCCESS) {                                          \
      RD_LOGE("rhi.vk", "%s 失败 (%d) @%d", #x, int(res_), __LINE__);  \
      return false;                                                    \
    }                                                                  \
  } while (0)

namespace rd {
namespace {

/// rhi Format → VkFormat 映射（本后端支持全部 Format 枚举值）。
VkFormat toVkFormat(Format f) {
  switch (f) {
    case Format::RGBA8_UNORM:        return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::BGRA8_UNORM:        return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::R32G32_FLOAT:       return VK_FORMAT_R32G32_SFLOAT;
    case Format::R32G32B32_FLOAT:    return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::D32_FLOAT:          return VK_FORMAT_D32_SFLOAT;
  }
  return VK_FORMAT_UNDEFINED;
}

// ---- 资源记录（句柄表的 value）----

/// 缓冲：VkBuffer + 独占内存（P0 简化：每缓冲一次 vkAllocateMemory）。
struct BufferRec { VkBuffer buffer; VkDeviceMemory memory; };
struct ShaderRec { VkShaderModule module; ShaderStage stage; std::string entry; };
/// 管线：VkPipeline + 布局（bind 描述符要用）+ 拓扑/剔除缓存（当前未在录制期使用）。
struct PipelineRec { VkPipeline pipeline; VkPipelineLayout layout; VkPrimitiveTopology topology; VkCullModeFlags cull; };
/// 渲染目标：离屏目标含颜色图像 + framebuffer + readback 用 staging 图像；
/// swapchain 图像复用该结构（isSwapchain=true，资源由 swapchain 管理）。
struct TargetRec {
  VkImage color; VkDeviceMemory colorMem; VkImageView view; VkFramebuffer fb;
  VkImage staging; VkDeviceMemory stagingMem;   ///< readback 暂存（LINEAR tiling，host 可读）
  uint32_t width, height;
  VkDeviceSize stagingRowPitch;                 ///< staging 行距（可能大于 width*4）
  bool isSwapchain = false; // swapchain 图像：资源由 swapchain 管理
};

/// 交换链：surface + swapchain 对象 + 每帧图像注册的 TargetHandle 列表。
struct SwapChainRec {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  std::vector<TargetHandle> imageTargets;  ///< 每张 swapchain 图像对应一个 TargetHandle
  uint32_t width = 0, height = 0;
  uint32_t currentIndex = 0;               ///< 最近 acquire 到的图像下标
#if defined(__ANDROID__)
  ANativeWindow* window = nullptr;         ///< 持有引用（create 时 acquire，destroy 时 release）
#endif
};

/// uniform slot 数上限（绑定约定 slot 0..3）。
constexpr uint32_t kMaxUniformSlots = 4;

/// 纹理：device-local 图像 + 视图；isCube 决定 viewType 与上传的面数。
struct TextureRec {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  bool isCube = false;
};
struct SamplerRec { VkSampler sampler = VK_NULL_HANDLE; };

class VulkanDevice;

/**
 * Vulkan 命令缓冲实现：薄封装 VkCommandBuffer 的录制调用。
 * 缓存当前 render pass 目标（endRenderPass 要做 readback 拷贝）与管线布局
 * （绑定描述符集要用）。
 */
class VulkanCommandBuffer final : public CommandBuffer {
public:
  explicit VulkanCommandBuffer(VulkanDevice* device) : device_(device) {}
  void beginRenderPass(TargetHandle target, const ClearColor& clear) override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) override;
  void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;
  void bindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) override;
  void draw(uint32_t vertexCount, uint32_t firstVertex) override;
  void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) override;
  void endRenderPass() override;

  VulkanDevice* device_;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;             ///< 指向设备唯一 VkCommandBuffer
  TargetHandle currentTarget_;                       ///< 当前 pass 目标（endRenderPass 拷贝用）
  VkPipelineLayout currentLayout_ = VK_NULL_HANDLE;  ///< 当前管线布局（绑描述符用）
  VkPrimitiveTopology topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
};

class VulkanDevice final : public Device {
public:
  ~VulkanDevice() override;
  bool init(const DeviceDesc& desc);
  Backend backend() const override { return Backend::Vulkan; }

  BufferHandle createBuffer(const BufferDesc& desc) override;
  void updateBuffer(BufferHandle buffer, const void* data, uint64_t size, uint64_t offset) override;
  void destroyBuffer(BufferHandle buffer) override;
  ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) override;
  void destroyShaderModule(ShaderModuleHandle module) override;
  PipelineHandle createPipeline(const PipelineDesc& desc) override;
  void destroyPipeline(PipelineHandle pipeline) override;
  TargetHandle createOffscreenTarget(const OffscreenTargetDesc& desc) override;
  void destroyTarget(TargetHandle target) override;
  TextureHandle createTexture(const TextureDesc& desc) override;
  void destroyTexture(TextureHandle texture) override;
  SamplerHandle createSampler(const SamplerDesc& desc) override;
  void destroySampler(SamplerHandle sampler) override;
  bool readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) override;
  CommandBuffer* acquireCommandBuffer() override;
  void submit(CommandBuffer* cmd) override;
  void waitIdle() override;
  SwapChainHandle createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) override;
  void resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) override;
  TargetHandle acquireSwapChainTarget(SwapChainHandle swapChain) override;
  void present(SwapChainHandle swapChain) override;
  void destroySwapChain(SwapChainHandle swapChain) override;
  /// swapchain 颜色格式 = 当前 render pass 格式（创建 swapchain 时可能已重建为表面格式）。
  Format swapChainColorFormat(SwapChainHandle swapChain) const override {
    return renderPassFormat_ == VK_FORMAT_B8G8R8A8_UNORM ? Format::BGRA8_UNORM
                                                        : Format::RGBA8_UNORM;
  }

  // ---- CommandBuffer 访问（句柄表查询与描述符写入辅助）----
  VkBuffer buffer(BufferHandle h) const {
    auto it = buffers_.find(h);
    return it == buffers_.end() ? VK_NULL_HANDLE : it->second.buffer;
  }
  bool pipeline(PipelineHandle h, PipelineRec& out) const {
    auto it = pipelines_.find(h);
    if (it == pipelines_.end()) return false;
    out = it->second;
    return true;
  }
  bool target(TargetHandle h, TargetRec& out) const {
    auto it = targets_.find(h);
    if (it == targets_.end()) return false;
    out = it->second;
    return true;
  }
  const TextureRec* texture(TextureHandle h) const {
    auto it = textures_.find(h);
    return it == textures_.end() ? nullptr : &it->second;
  }
  VkSampler sampler(SamplerHandle h) const {
    auto it = samplers_.find(h);
    return it == samplers_.end() ? VK_NULL_HANDLE : it->second.sampler;
  }
  VkRenderPass renderPass() const { return renderPass_; }
  VkDescriptorSet descriptorSet() const { return descSet_; }
  VkDevice device() const { return device_; }
  /// 写全局唯一 descriptor set 的 uniform binding（绑定约定：slot N ↔ set0 binding N）。
  void writeUniformDescriptor(uint32_t slot, VkBuffer buffer, uint64_t offset, uint64_t size) {
    VkDescriptorBufferInfo info{buffer, offset, size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descSet_;
    write.dstBinding = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

private:
  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
  bool createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling,
                   VkImageUsageFlags usage, VkMemoryPropertyFlags memProps, VkImage& image,
                   VkDeviceMemory& memory);
  bool createRenderPass(VkFormat format);
  bool createSwapchainObject(SwapChainRec& rec, VkSwapchainKHR oldSwapchain);
  bool buildSwapChainTargets(SwapChainRec& rec);
  void destroySwapChainImages(SwapChainRec& rec);

  std::unordered_map<SwapChainHandle, SwapChainRec> swapChains_;
  VkFence acquireFence_ = VK_NULL_HANDLE;      ///< acquire 图像用的 fence（P0 串行模型）
  VkFormat renderPassFormat_ = VK_FORMAT_R8G8B8A8_UNORM;  ///< render pass 颜色格式（可能随 swapchain 重建）

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;       ///< 设备唯一命令缓冲（单线程模型）
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;  ///< set0：binding 0..3 uniform + 4..11 sampler
  VkDescriptorPool descPool_ = VK_NULL_HANDLE;
  VkDescriptorSet descSet_ = VK_NULL_HANDLE;          ///< 全局唯一 descriptor set
  VulkanCommandBuffer cmdBuf_{this};
  uint32_t nextId_ = 1;                        ///< 句柄分配器（1 起，0 留作无效）
  std::unordered_map<BufferHandle, BufferRec> buffers_;
  std::unordered_map<ShaderModuleHandle, ShaderRec> shaders_;
  std::unordered_map<PipelineHandle, PipelineRec> pipelines_;
  std::unordered_map<TargetHandle, TargetRec> targets_;
  std::unordered_map<TextureHandle, TextureRec> textures_;
  std::unordered_map<SamplerHandle, SamplerRec> samplers_;
};

// ---------------- Device 初始化 ----------------

/// 初始化：实例 → 物理设备/图形队列 → 设备 → 命令池/命令缓冲 → 离屏 render pass
/// → 描述符布局/池/set → acquire fence。任一步失败返回 false（VK_CHECK 记日志）。
bool VulkanDevice::init(const DeviceDesc& desc) {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "rdrenderer";
  app.apiVersion = VK_API_VERSION_1_1;

  std::vector<const char*> extensions;
  VkInstanceCreateFlags flags = 0;
  // 注意：macOS 上我们直接链接 libMoltenVK.dylib（ICD），不经过 Vulkan Loader，
  // 因此不需要也不能请求 VK_KHR_portability_enumeration（那是 loader 层扩展）。
  // 若日后改为经 Loader 加载 MoltenVK，需恢复该扩展与 ENUMERATE_PORTABILITY_BIT flag。
#if defined(__ANDROID__)
  // Android 需要 surface 扩展用于上屏渲染
  extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif

  // 校验层：仅在请求且环境可用时启用（逐层枚举查找 KHRONOS_validation）
  std::vector<const char*> layers;
  if (desc.enableValidation) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    for (const auto& l : available) {
      if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        break;
      }
    }
  }

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.flags = flags;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(extensions.size());
  ici.ppEnabledExtensionNames = extensions.data();
  ici.enabledLayerCount = uint32_t(layers.size());
  ici.ppEnabledLayerNames = layers.data();
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_));

  // 选物理设备：取第一个带图形队列族的设备（P0 不做评分/选择策略）
  uint32_t physCount = 0;
  vkEnumeratePhysicalDevices(instance_, &physCount, nullptr);
  if (physCount == 0) {
    RD_LOGE("rhi.vk", "无物理设备");
    return false;
  }
  std::vector<VkPhysicalDevice> physList(physCount);
  vkEnumeratePhysicalDevices(instance_, &physCount, physList.data());

  for (auto phys : physList) {
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; ++i) {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        phys_ = phys;
        queueFamily_ = i;
        break;
      }
    }
    if (phys_) break;
  }
  if (!phys_) {
    RD_LOGE("rhi.vk", "无图形队列");
    return false;
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  std::vector<const char*> deviceExtensions;
#if defined(__APPLE__)
  // MoltenVK 要求启用 portability_subset（若设备提供）
  uint32_t extCount = 0;
  vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> exts(extCount);
  vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, exts.data());
  for (const auto& e : exts) {
    if (strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
      deviceExtensions.push_back("VK_KHR_portability_subset");
      break;
    }
  }
#endif
#if defined(__ANDROID__)
  deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#endif

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = uint32_t(deviceExtensions.size());
  dci.ppEnabledExtensionNames = deviceExtensions.data();
  VK_CHECK(vkCreateDevice(phys_, &dci, nullptr, &device_));
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

  // RESET_COMMAND_BUFFER_BIT：允许单命令缓冲反复 reset 重录
  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = queueFamily_;
  VK_CHECK(vkCreateCommandPool(device_, &cpci, nullptr, &cmdPool_));

  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cmdPool_;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, &cmd_));

  // 离屏 render pass：单颜色附件，结束后转为 TRANSFER_SRC 供 readback 拷贝
  if (!createRenderPass(VK_FORMAT_R8G8B8A8_UNORM)) return false;

  // 描述符布局（绑定约定）：
  // binding 0..3：uniform buffer；binding 4..11：combined image sampler（texture slot 0..7）
  VkDescriptorSetLayoutBinding bindings[12]{};
  for (uint32_t i = 0; i < kMaxUniformSlots; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  for (uint32_t i = 0; i < 8; ++i) {
    bindings[4 + i].binding = 4 + i;
    bindings[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4 + i].descriptorCount = 1;
    bindings[4 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = 12;
  dslci.pBindings = bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &setLayout_));

  // 描述符池：只需容纳 1 个 set（全局唯一，bind 时原地覆写）
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxUniformSlots},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
  };
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 2;
  dpci.pPoolSizes = poolSizes;
  VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_));

  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = descPool_;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &setLayout_;
  VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &descSet_));

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(vkCreateFence(device_, &fci, nullptr, &acquireFence_));
  return true;
}

/// render pass 参数化创建（swapchain 表面格式可能与离屏 RGBA8 不同）：
/// 单颜色附件 Clear→Store；finalLayout=TRANSFER_SRC 供 endRenderPass 的 readback 拷贝；
/// 两条 subpass 依赖保证写完成后再做 transfer 读。
bool VulkanDevice::createRenderPass(VkFormat format) {
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;

  // 依赖：外部→subpass（颜色输出可写）；subpass→外部（颜色写完 → transfer 可读）
  VkSubpassDependency deps[2]{};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &color;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  rpci.dependencyCount = 2;
  rpci.pDependencies = deps;
  VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_));
  renderPassFormat_ = format;
  return true;
}

/// 析构：等 GPU 空闲后按依赖逆序销毁（资源句柄表中的对象由调用方先行销毁；
/// P0 简化：渲染循环退出前调用方应 destroy 全部资源）。
VulkanDevice::~VulkanDevice() {
  if (!device_) return;
  vkDeviceWaitIdle(device_);
  vkDestroyDescriptorPool(device_, descPool_, nullptr);
  vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
  vkDestroyRenderPass(device_, renderPass_, nullptr);
  vkDestroyCommandPool(device_, cmdPool_, nullptr);
  if (acquireFence_) vkDestroyFence(device_, acquireFence_, nullptr);
  vkDestroyDevice(device_, nullptr);
  vkDestroyInstance(instance_, nullptr);
}

/// 在物理设备内存类型中查找同时满足 typeBits 与属性要求的类型下标；找不到返回 UINT32_MAX。
uint32_t VulkanDevice::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  }
  return UINT32_MAX;
}

/// 建图辅助：创建 2D 图像 + 分配绑定独占内存（mipLevels/arrayLayers 固定为 1，
/// 供渲染目标/staging 用；纹理走 createTexture 的独立路径）。
bool VulkanDevice::createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling,
                               VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
                               VkImage& image, VkDeviceMemory& memory) {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = tiling;
  ici.usage = usage;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(device_, &ici, nullptr, &image));

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, memProps);
  if (mai.memoryTypeIndex == UINT32_MAX) return false;
  VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &memory));
  VK_CHECK(vkBindImageMemory(device_, image, memory, 0));
  return true;
}

// ---------------- 资源 ----------------

/// 创建缓冲：usage 映射 Vulkan usage 位。
/// P0 简化：内存统一 HOST_VISIBLE|COHERENT（可直写直读，性能非最优；
/// P1 再引入 device-local + staging 路径）。
BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
  VkBufferUsageFlags usage = 0;
  if (hasFlag(desc.usage, BufferUsage::Vertex)) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (hasFlag(desc.usage, BufferUsage::Index)) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (hasFlag(desc.usage, BufferUsage::Uniform)) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = desc.size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer;
  if (vkCreateBuffer(device_, &bci, nullptr, &buffer) != VK_SUCCESS) return {};

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, buffer, &req);
  // P0 简化：全部 HOST_VISIBLE|COHERENT（P1 再引入 device-local + staging）
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex =
      findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory memory;
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &memory) != VK_SUCCESS ||
      vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
    return {};
  }
  BufferHandle h(nextId_++);
  buffers_.emplace(h, BufferRec{buffer, memory});
  if (desc.data) updateBuffer(h, desc.data, desc.size, 0);
  return h;
}

/// 更新缓冲子区间：map → memcpy → unmap（COHERENT 无需 flush）。
void VulkanDevice::updateBuffer(BufferHandle buffer, const void* data, uint64_t size,
                                uint64_t offset) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  void* mapped = nullptr;
  vkMapMemory(device_, it->second.memory, offset, size, 0, &mapped);
  memcpy(mapped, data, size);
  vkUnmapMemory(device_, it->second.memory);
}

void VulkanDevice::destroyBuffer(BufferHandle buffer) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  vkDestroyBuffer(device_, it->second.buffer, nullptr);
  vkFreeMemory(device_, it->second.memory, nullptr);
  buffers_.erase(it);
}

/// 加载 SPIR-V 模块（入口名约定 "main"）。
ShaderModuleHandle VulkanDevice::createShaderModule(const ShaderModuleDesc& desc) {
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = desc.code.size();
  smci.pCode = reinterpret_cast<const uint32_t*>(desc.code.data());
  VkShaderModule module;
  if (vkCreateShaderModule(device_, &smci, nullptr, &module) != VK_SUCCESS) return {};
  ShaderModuleHandle h(nextId_++);
  shaders_.emplace(h, ShaderRec{module, desc.stage, desc.entryPoint});
  return h;
}

void VulkanDevice::destroyShaderModule(ShaderModuleHandle module) {
  auto it = shaders_.find(module);
  if (it == shaders_.end()) return;
  vkDestroyShaderModule(device_, it->second.module, nullptr);
  shaders_.erase(it);
}

PipelineHandle VulkanDevice::createPipeline(const PipelineDesc& desc) {
  // 深度附件尚未实现，先拒绝而非静默错误（与 Metal/GLES 一致）。
  if (desc.depthTest) {
    RD_LOGE("rhi.vk", "P0-1 离屏目标不支持 depthTest（P1 引入深度附件）");
    return {};
  }
  auto vsIt = shaders_.find(desc.vertexShader);
  auto fsIt = shaders_.find(desc.fragmentShader);
  if (vsIt == shaders_.end() || fsIt == shaders_.end()) {
    RD_LOGE("rhi.vk", "createPipeline: shader 句柄无效");
    return {};
  }

  // 着色器阶段（入口名取 ShaderModuleDesc::entryPoint，SPIR-V 约定 "main"）
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vsIt->second.module;
  stages[0].pName = vsIt->second.entry.c_str();
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fsIt->second.module;
  stages[1].pName = fsIt->second.entry.c_str();

  // 顶点输入布局（binding/attribute 直接一一映射）
  std::vector<VkVertexInputBindingDescription> bindings;
  for (const auto& b : desc.vertexBindings) {
    bindings.push_back({b.binding, b.stride, VK_VERTEX_INPUT_RATE_VERTEX});
  }
  std::vector<VkVertexInputAttributeDescription> attribs;
  for (const auto& a : desc.attributes) {
    attribs.push_back({a.location, a.binding, toVkFormat(a.format), a.offset});
  }
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = uint32_t(bindings.size());
  vi.pVertexBindingDescriptions = bindings.data();
  vi.vertexAttributeDescriptionCount = uint32_t(attribs.size());
  vi.pVertexAttributeDescriptions = attribs.data();

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  if (desc.topology == PrimitiveTopology::TriangleStrip) topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  if (desc.topology == PrimitiveTopology::LineList) topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  ia.topology = topology;

  // viewport/scissor 为动态状态（beginRenderPass 时按目标尺寸设置）
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  VkCullModeFlags cull = VK_CULL_MODE_NONE;
  if (desc.cullMode == CullMode::Back) cull = VK_CULL_MODE_BACK_BIT;
  if (desc.cullMode == CullMode::Front) cull = VK_CULL_MODE_FRONT_BIT;
  rs.cullMode = cull;
  // 视口用负高度做 y 翻转（对齐 Metal/GLES 的 y-up NDC），翻转后绕序变 CW
  rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // 深度固定关闭（depthTest 已在入口拒绝）
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_FALSE;
  ds.depthWriteEnable = VK_FALSE;

  // 无混合，RGBA 全写
  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blendAttachment;

  VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynamicStates;

  // 管线布局 = 全局唯一 set0 布局（uniform 0..3 + sampler 4..11）
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &setLayout_;
  VkPipelineLayout layout;
  VkResult layoutResult = vkCreatePipelineLayout(device_, &plci, nullptr, &layout);
  if (layoutResult != VK_SUCCESS) {
    RD_LOGE("rhi.vk", "vkCreatePipelineLayout 失败 (%d)", int(layoutResult));
    return {};
  }

  VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pDepthStencilState = &ds;
  gpci.pColorBlendState = &cb;
  gpci.pDynamicState = &dyn;
  gpci.layout = layout;
  gpci.renderPass = renderPass_;
  VkPipeline pipeline;
  VkResult pipelineResult =
      vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline);
  if (pipelineResult != VK_SUCCESS) {
    RD_LOGE("rhi.vk", "vkCreateGraphicsPipelines 失败 (%d)", int(pipelineResult));
    vkDestroyPipelineLayout(device_, layout, nullptr);
    return {};
  }
  PipelineHandle h(nextId_++);
  pipelines_.emplace(h, PipelineRec{pipeline, layout, topology, cull});
  return h;
}

void VulkanDevice::destroyPipeline(PipelineHandle pipeline) {
  auto it = pipelines_.find(pipeline);
  if (it == pipelines_.end()) return;
  vkDestroyPipeline(device_, it->second.pipeline, nullptr);
  vkDestroyPipelineLayout(device_, it->second.layout, nullptr);
  pipelines_.erase(it);
}

/**
 * 创建离屏目标：device-local 颜色图像（COLOR_ATTACHMENT|TRANSFER_SRC）+ view +
 * framebuffer + LINEAR tiling 的 staging 图像（host 可读，readback 用）；
 * 并查询 staging 的 rowPitch（可能大于 width*4，读取时须按行距步进）。
 */
TargetHandle VulkanDevice::createOffscreenTarget(const OffscreenTargetDesc& desc) {
  TargetRec rec{};
  rec.width = desc.width;
  rec.height = desc.height;
  if (!createImage(desc.width, desc.height, toVkFormat(desc.colorFormat),
                   VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, rec.color, rec.colorMem)) {
    return {};
  }

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = rec.color;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = toVkFormat(desc.colorFormat);
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &vci, nullptr, &rec.view) != VK_SUCCESS) return {};

  VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = renderPass_;
  fbci.attachmentCount = 1;
  fbci.pAttachments = &rec.view;
  fbci.width = desc.width;
  fbci.height = desc.height;
  fbci.layers = 1;
  if (vkCreateFramebuffer(device_, &fbci, nullptr, &rec.fb) != VK_SUCCESS) return {};

  // readback staging：LINEAR tiling + host 可见，接收颜色附件的拷贝
  if (!createImage(desc.width, desc.height, toVkFormat(desc.colorFormat), VK_IMAGE_TILING_LINEAR,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   rec.staging, rec.stagingMem)) {
    return {};
  }
  VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
  VkSubresourceLayout layout;
  vkGetImageSubresourceLayout(device_, rec.staging, &sub, &layout);
  rec.stagingRowPitch = layout.rowPitch;

  TargetHandle h(nextId_++);
  targets_.emplace(h, rec);
  return h;
}

void VulkanDevice::destroyTarget(TargetHandle target) {
  auto it = targets_.find(target);
  if (it == targets_.end()) return;
  if (it->second.isSwapchain) { // 资源随 swapchain 销毁
    targets_.erase(it);
    return;
  }
  const TargetRec& t = it->second;
  vkDestroyFramebuffer(device_, t.fb, nullptr);
  vkDestroyImageView(device_, t.view, nullptr);
  vkDestroyImage(device_, t.color, nullptr);
  vkFreeMemory(device_, t.colorMem, nullptr);
  vkDestroyImage(device_, t.staging, nullptr);
  vkFreeMemory(device_, t.stagingMem, nullptr);
  targets_.erase(it);
}

/**
 * 创建纹理：device-local 图像（cube 需 CUBE_COMPATIBLE 标志与 6 个 arrayLayer）；
 * desc.data 非空时经 staging buffer 上传（UNDEFINED→TRANSFER_DST→拷贝→
 * SHADER_READ_ONLY 的 layout 转换链）。上传/数据不足失败时清理已建资源并返回无效句柄。
 *
 * @note 数据布局：面 × mip 紧凑排列（面序 +X,-X,+Y,-Y,+Z,-Z），每层尺寸逐级减半；
 *       调用方须保证 dataSize 足够（offset 累计超过 dataSize 判失败）。
 * @note 前置校验 cube 方形与 mip 上限（与 Metal 后端一致的失败语义）；
 *       MoltenVK 下超额 mip 不会被 vkCreateImage 干净拒绝，而是触发 Metal 断言
 *       直接 abort 进程，因此必须在进入 Vulkan 调用前拦截。
 */
TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
  if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0) return {};
  if (desc.type == TextureType::Cube && desc.width != desc.height) {
    RD_LOGE("rhi.vk", "createTexture: cube 纹理必须方形（%ux%u）", desc.width, desc.height);
    return {};
  }
  const uint32_t maxDim = desc.width > desc.height ? desc.width : desc.height;
  const uint32_t maxMipLevels = uint32_t(std::floor(std::log2(double(maxDim)))) + 1;
  if (desc.mipLevels > maxMipLevels) {
    RD_LOGE("rhi.vk", "createTexture: mipLevels %u 超出上限 %u（%ux%u）", desc.mipLevels,
            maxMipLevels, desc.width, desc.height);
    return {};
  }
  const uint32_t faces = desc.type == TextureType::Cube ? 6 : 1;

  TextureRec rec;
  rec.isCube = desc.type == TextureType::Cube;

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = toVkFormat(desc.format);
  ici.extent = {desc.width, desc.height, 1};
  ici.mipLevels = desc.mipLevels;
  ici.arrayLayers = faces;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (rec.isCube) ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ici, nullptr, &rec.image) != VK_SUCCESS) return {};

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, rec.image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &rec.memory) != VK_SUCCESS ||
      vkBindImageMemory(device_, rec.image, rec.memory, 0) != VK_SUCCESS) {
    if (rec.memory) vkFreeMemory(device_, rec.memory, nullptr);
    vkDestroyImage(device_, rec.image, nullptr);
    return {};
  }

  if (desc.data && desc.dataSize > 0) {
    // staging buffer（host visible）
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = desc.dataSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    bool stagingOk =
        vkCreateBuffer(device_, &bci, nullptr, &staging) == VK_SUCCESS;
    if (stagingOk) {
      VkMemoryRequirements sreq;
      vkGetBufferMemoryRequirements(device_, staging, &sreq);
      VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      smai.allocationSize = sreq.size;
      smai.memoryTypeIndex =
          findMemoryType(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      stagingOk = smai.memoryTypeIndex != UINT32_MAX &&
                  vkAllocateMemory(device_, &smai, nullptr, &stagingMem) == VK_SUCCESS &&
                  vkBindBufferMemory(device_, staging, stagingMem, 0) == VK_SUCCESS;
    }
    if (stagingOk) {
      void* mapped = nullptr;
      vkMapMemory(device_, stagingMem, 0, desc.dataSize, 0, &mapped);
      memcpy(mapped, desc.data, desc.dataSize);
      vkUnmapMemory(device_, stagingMem);
    }

    // 记录拷贝命令（复用单 cmd + waitIdle，P0 风格串行化）
    if (stagingOk) {
      vkResetCommandBuffer(cmd_, 0);
      VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      vkBeginCommandBuffer(cmd_, &bi);

      // layout 转换 1：UNDEFINED → TRANSFER_DST_OPTIMAL（准备接收拷贝）
      VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      toDst.srcAccessMask = 0;
      toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      toDst.image = rec.image;
      toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipLevels, 0, faces};
      vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

      // 数据布局：面 × mip 紧凑排列
      std::vector<VkBufferImageCopy> copies;
      uint64_t offset = 0;
      for (uint32_t face = 0; face < faces; ++face) {
        uint32_t w = desc.width, h = desc.height;
        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
          VkBufferImageCopy c{};
          c.bufferOffset = offset;
          c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1};
          c.imageExtent = {w, h, 1};
          copies.push_back(c);
          offset += uint64_t(w) * h * formatSize(desc.format);
          w = w > 1 ? w / 2 : 1;
          h = h > 1 ? h / 2 : 1;
        }
      }
      if (offset > desc.dataSize) { // 数据不足
        vkEndCommandBuffer(cmd_);
        stagingOk = false;
      } else {
        vkCmdCopyBufferToImage(cmd_, staging, rec.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                               copies.data());

        // layout 转换 2：TRANSFER_DST → SHADER_READ_ONLY（供片段着色器采样）
        VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.image = rec.image;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipLevels, 0, faces};
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toRead);
        vkEndCommandBuffer(cmd_);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue_);
      }
    }
    // staging 是一次性资源：无论成败都释放
    if (staging) vkDestroyBuffer(device_, staging, nullptr);
    if (stagingMem) vkFreeMemory(device_, stagingMem, nullptr);
    if (!stagingOk) {
      vkFreeMemory(device_, rec.memory, nullptr);
      vkDestroyImage(device_, rec.image, nullptr);
      return {};
    }
  }

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = rec.image;
  vci.viewType = rec.isCube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
  vci.format = toVkFormat(desc.format);
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mipLevels, 0, faces};
  if (vkCreateImageView(device_, &vci, nullptr, &rec.view) != VK_SUCCESS) {
    vkFreeMemory(device_, rec.memory, nullptr);
    vkDestroyImage(device_, rec.image, nullptr);
    return {};
  }

  TextureHandle h(nextId_++);
  textures_.emplace(h, rec);
  return h;
}

void VulkanDevice::destroyTexture(TextureHandle texture) {
  auto it = textures_.find(texture);
  if (it == textures_.end()) return;
  vkDestroyImageView(device_, it->second.view, nullptr);
  vkDestroyImage(device_, it->second.image, nullptr);
  vkFreeMemory(device_, it->second.memory, nullptr);
  textures_.erase(it);
}

/// 创建采样器：rhi 过滤/寻址枚举一一映射到 VkSamplerCreateInfo。
SamplerHandle VulkanDevice::createSampler(const SamplerDesc& desc) {
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  auto filter = [](Filter f) { return f == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; };
  auto wrap = [](WrapMode m) {
    return m == WrapMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                 : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  };
  sci.minFilter = filter(desc.minFilter);
  sci.magFilter = filter(desc.magFilter);
  sci.mipmapMode = desc.mipFilter == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                    : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = wrap(desc.wrapU);
  sci.addressModeV = wrap(desc.wrapV);
  sci.addressModeW = wrap(desc.wrapW);
  // maxLod 必须显式设置：零初始化=0.0 会把采样 LOD 钳到 mip0（Metal 的
  // lodMaxClamp 默认 FLT_MAX、GLES 的 GL_TEXTURE_MAX_LOD 默认 1000，均不钳）。
  // VK_LOD_CLAMP_NONE(=1000.0f) 表示不钳上界，与其余后端行为对齐。
  sci.maxLod = VK_LOD_CLAMP_NONE;
  VkSampler sampler;
  if (vkCreateSampler(device_, &sci, nullptr, &sampler) != VK_SUCCESS) return {};
  SamplerHandle h(nextId_++);
  samplers_.emplace(h, SamplerRec{sampler});
  return h;
}

void VulkanDevice::destroySampler(SamplerHandle sampler) {
  auto it = samplers_.find(sampler);
  if (it == samplers_.end()) return;
  vkDestroySampler(device_, it->second.sampler, nullptr);
  samplers_.erase(it);
}

/**
 * 读出目标像素：map staging 图像（endRenderPass 已把颜色拷贝进来），
 * 按 stagingRowPitch 逐行拷贝为紧凑 RGBA8。swapchain 目标不支持（无 staging）。
 */
bool VulkanDevice::readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) {
  auto it = targets_.find(target);
  if (it == targets_.end()) return false;
  const TargetRec& t = it->second;
  if (t.isSwapchain) {
    RD_LOGW("rhi.vk", "swapchain target 不支持 readback");
    return false;
  }
  const uint64_t rowBytes = uint64_t(t.width) * 4;
  if (outSize < rowBytes * t.height) return false;
  waitIdle();
  void* mapped = nullptr;
  if (vkMapMemory(device_, t.stagingMem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  uint8_t* dst = static_cast<uint8_t*>(outRGBA8);
  for (uint32_t y = 0; y < t.height; ++y) {
    memcpy(dst + rowBytes * y, src + t.stagingRowPitch * y, rowBytes);
  }
  vkUnmapMemory(device_, t.stagingMem);
  return true;
}

/// 取本帧命令缓冲：reset 并以 ONE_TIME_SUBMIT 开始录制（单缓冲串行模型）。
CommandBuffer* VulkanDevice::acquireCommandBuffer() {
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &bi);
  cmdBuf_.cmd_ = cmd_;
  return &cmdBuf_;
}

/// 提交：结束录制并 queueSubmit（无 fence——调用方用 waitIdle 同步）。
void VulkanDevice::submit(CommandBuffer*) {
  vkEndCommandBuffer(cmd_);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
}

void VulkanDevice::waitIdle() { vkQueueWaitIdle(queue_); }

// ---------------- CommandBuffer 录制 ----------------

/// 开始 render pass：Clear 加载动作 + 全幅 scissor + 负高度视口（y 翻转）。
void VulkanCommandBuffer::beginRenderPass(TargetHandle target, const ClearColor& clear) {
  TargetRec t;
  if (!device_->target(target, t)) return;
  currentTarget_ = target;

  VkClearValue clearValue{};
  clearValue.color = {{clear.r, clear.g, clear.b, clear.a}};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = device_->renderPass();
  rp.framebuffer = t.fb;
  rp.renderArea = {{0, 0}, {t.width, t.height}};
  rp.clearValueCount = 1;
  rp.pClearValues = &clearValue;
  vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);

  // 负高度视口：y 翻转对齐 Metal/GLES
  VkViewport viewport{0, float(t.height), float(t.width), -float(t.height), 0, 1};
  vkCmdSetViewport(cmd_, 0, 1, &viewport);
  VkRect2D scissor{{0, 0}, {t.width, t.height}};
  vkCmdSetScissor(cmd_, 0, 1, &scissor);
}

/// 绑定管线并随之绑定全局唯一 descriptor set（set 0）。
void VulkanCommandBuffer::bindPipeline(PipelineHandle pipeline) {
  PipelineRec rec;
  if (!device_->pipeline(pipeline, rec)) return;
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rec.pipeline);
  currentLayout_ = rec.layout;
  topology_ = rec.topology;
  VkDescriptorSet set = device_->descriptorSet();
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rec.layout, 0, 1,
                          &set, 0, nullptr);
}

void VulkanCommandBuffer::bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) {
  VkBuffer b = device_->buffer(buffer);
  vkCmdBindVertexBuffers(cmd_, binding, 1, &b, &offset);
}

void VulkanCommandBuffer::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) {
  vkCmdBindIndexBuffer(cmd_, device_->buffer(buffer), offset,
                       type == IndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

/// 绑定约定：texture slot N ↔ set0 binding(N+4) combined-image-sampler；
/// 直接覆写全局唯一 descriptor set（须在 bindPipeline 之后、draw 之前调用才生效）。
void VulkanCommandBuffer::bindTexture(uint32_t slot, TextureHandle texture,
                                      SamplerHandle sampler) {
  const TextureRec* rec = device_->texture(texture);
  VkSampler s = device_->sampler(sampler);
  if (!rec || s == VK_NULL_HANDLE) return;
  VkDescriptorImageInfo info{s, rec->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = device_->descriptorSet();
  write.dstBinding = slot + 4;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &info;
  vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

/// 绑定约定：uniform slot N ↔ set0 binding N（同样覆写全局 descriptor set）。
void VulkanCommandBuffer::bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                            uint64_t size) {
  device_->writeUniformDescriptor(slot, device_->buffer(buffer), offset, size);
}

void VulkanCommandBuffer::draw(uint32_t vertexCount, uint32_t firstVertex) {
  vkCmdDraw(cmd_, vertexCount, 1, firstVertex, 0);
}

void VulkanCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t firstIndex,
                                      int32_t vertexOffset) {
  vkCmdDrawIndexed(cmd_, indexCount, 1, firstIndex, vertexOffset, 0);
}

/**
 * 结束 render pass 并做 readback 拷贝：
 * staging: UNDEFINED → TRANSFER_DST，颜色附件（此时 finalLayout=TRANSFER_SRC）
 * → staging 整幅拷贝，最后 staging → GENERAL 供 host 读。
 */
void VulkanCommandBuffer::endRenderPass() {
  vkCmdEndRenderPass(cmd_);

  TargetRec t;
  if (!device_->target(currentTarget_, t)) return;
  // staging: UNDEFINED -> TRANSFER_DST，拷贝后 -> GENERAL（供 host 读）
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.image = t.staging;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);

  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.extent = {t.width, t.height, 1};
  vkCmdCopyImage(cmd_, t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.staging,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
}

// ---------------- SwapChain（Android）----------------

/**
 * 创建 swapchain 对象：按表面能力取 extent/imageCount，格式用 renderPassFormat_
 * （与 render pass 一致），presentMode=FIFO（垂直同步，必支持）。
 * oldSwapchain 非空表示重建（resize）：先建新再销毁旧。
 */
bool VulkanDevice::createSwapchainObject(SwapChainRec& rec, VkSwapchainKHR oldSwapchain) {
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, rec.surface, &caps));

  // currentExtent 为 UINT32_MAX 表示表面尺寸可变，用请求尺寸
  VkExtent2D extent = caps.currentExtent.width != UINT32_MAX
                          ? caps.currentExtent
                          : VkExtent2D{rec.width, rec.height};
  uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

  VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  swci.surface = rec.surface;
  swci.minImageCount = imageCount;
  swci.imageFormat = renderPassFormat_;
  swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  swci.imageExtent = extent;
  swci.imageArrayLayers = 1;
  swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swci.preTransform = caps.currentTransform;
  swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swci.clipped = VK_TRUE;
  swci.oldSwapchain = oldSwapchain;
  VkSwapchainKHR swapchain;
  VK_CHECK(vkCreateSwapchainKHR(device_, &swci, nullptr, &swapchain));
  if (oldSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
  rec.swapchain = swapchain;
  rec.width = extent.width;
  rec.height = extent.height;
  return true;
}

/// 为每张 swapchain 图像建 view + framebuffer，并各注册一个 TargetHandle。
bool VulkanDevice::buildSwapChainTargets(SwapChainRec& rec) {
  uint32_t count = 0;
  vkGetSwapchainImagesKHR(device_, rec.swapchain, &count, nullptr);
  std::vector<VkImage> images(count);
  vkGetSwapchainImagesKHR(device_, rec.swapchain, &count, images.data());
  for (auto image : images) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = renderPassFormat_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &view));
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = renderPass_;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = rec.width;
    fbci.height = rec.height;
    fbci.layers = 1;
    VkFramebuffer fb;
    VK_CHECK(vkCreateFramebuffer(device_, &fbci, nullptr, &fb));
    TargetRec t{};
    t.color = image;
    t.view = view;
    t.fb = fb;
    t.width = rec.width;
    t.height = rec.height;
    t.isSwapchain = true;
    TargetHandle th(nextId_++);
    targets_.emplace(th, t);
    rec.imageTargets.push_back(th);
  }
  return true;
}

/// 销毁 swapchain 图像的 view/framebuffer 并移除对应 TargetHandle
/// （图像本身随 swapchain 对象销毁，不单独 vkDestroyImage）。
void VulkanDevice::destroySwapChainImages(SwapChainRec& rec) {
  for (auto th : rec.imageTargets) {
    auto it = targets_.find(th);
    if (it == targets_.end()) continue;
    vkDestroyFramebuffer(device_, it->second.fb, nullptr);
    vkDestroyImageView(device_, it->second.view, nullptr);
    targets_.erase(it);
  }
  rec.imageTargets.clear();
}

/**
 * 创建交换链（仅 Android；其他平台返回无效句柄）：
 * ANativeWindow → VkSurfaceKHR → 校验 present 支持 → 表面格式与 render pass 对齐
 * （不一致时重建 render pass）→ swapchain 对象 + 每帧图像 target。
 */
SwapChainHandle VulkanDevice::createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) {
#if defined(__ANDROID__)
  if (!nativeWindow || width == 0 || height == 0) return {};
  ANativeWindow* window = static_cast<ANativeWindow*>(nativeWindow);
  ANativeWindow_acquire(window);

  VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  sci.window = window;
  VkSurfaceKHR surface;
  if (vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface) != VK_SUCCESS) {
    RD_LOGE("rhi.vk", "Android surface 创建失败");
    ANativeWindow_release(window);
    return {};
  }
  VkBool32 presentSupport = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queueFamily_, surface, &presentSupport);
  if (!presentSupport) {
    RD_LOGE("rhi.vk", "队列不支持 present");
    vkDestroySurfaceKHR(instance_, surface, nullptr);
    ANativeWindow_release(window);
    return {};
  }

  // 表面格式须与 render pass 一致；不一致时重建 render pass（须在创建 pipeline 之前，
  // engine 流程保证：set_surface 先于场景初始化）
  uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface, &formatCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface, &formatCount, formats.data());
  // 优先 RGBA8，否则取表面首选格式
  VkFormat surfaceFormat = formats.empty() ? VK_FORMAT_R8G8B8A8_UNORM : formats[0].format;
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      surfaceFormat = f.format;
      break;
    }
  }
  if (surfaceFormat != renderPassFormat_) {
    RD_LOGI("rhi.vk", "swapchain 格式 %d ≠ render pass 格式 %d，重建 render pass",
            int(surfaceFormat), int(renderPassFormat_));
    vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (!createRenderPass(surfaceFormat)) {
      vkDestroySurfaceKHR(instance_, surface, nullptr);
      ANativeWindow_release(window);
      return {};
    }
  }

  SwapChainRec rec;
  rec.surface = surface;
  rec.width = width;
  rec.height = height;
  rec.window = window;
  if (!createSwapchainObject(rec, VK_NULL_HANDLE) || !buildSwapChainTargets(rec)) {
    vkDestroySurfaceKHR(instance_, surface, nullptr);
    ANativeWindow_release(window);
    return {};
  }
  SwapChainHandle h(nextId_++);
  swapChains_.emplace(h, rec);
  return h;
#else
  (void)nativeWindow; (void)width; (void)height;
  return {};
#endif
}

/// 重建交换链：等 GPU 空闲 → 销毁图像资源 → 以旧 swapchain 重建 → 重建 targets。
void VulkanDevice::resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end() || width == 0 || height == 0) return;
  vkDeviceWaitIdle(device_);
  SwapChainRec& rec = it->second;
  destroySwapChainImages(rec);
  rec.width = width;
  rec.height = height;
  createSwapchainObject(rec, rec.swapchain); // oldSwapchain 传入并销毁
  buildSwapChainTargets(rec);
}

/**
 * 获取当前帧图像：acquireNextImage + fence 等待（P0 串行模型）。
 * OUT_OF_DATE（表面尺寸变化中等 resize 重建）或其他错误返回无效句柄，调用方跳帧；
 * SUBOPTIMAL 仍可用，继续渲染。
 */
TargetHandle VulkanDevice::acquireSwapChainTarget(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return {};
  SwapChainRec& rec = it->second;
  VkResult r = vkAcquireNextImageKHR(device_, rec.swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                     acquireFence_, &rec.currentIndex);
  vkWaitForFences(device_, 1, &acquireFence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &acquireFence_);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) return {}; // 等 resize 后重建
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return {};
  return rec.imageTargets[rec.currentIndex];
}

/// 上屏：先等 GPU 空闲（P0 简化：帧串行；P1 引入 acquire/present 信号量）。
void VulkanDevice::present(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  SwapChainRec& rec = it->second;
  waitIdle(); // P0 简化：帧串行；P1 引入 acquire/present 信号量
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.swapchainCount = 1;
  pi.pSwapchains = &rec.swapchain;
  pi.pImageIndices = &rec.currentIndex;
  vkQueuePresentKHR(queue_, &pi);
}

void VulkanDevice::destroySwapChain(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  vkDeviceWaitIdle(device_);
  SwapChainRec& rec = it->second;
  destroySwapChainImages(rec);
  if (rec.swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, rec.swapchain, nullptr);
  if (rec.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, rec.surface, nullptr);
#if defined(__ANDROID__)
  if (rec.window) ANativeWindow_release(rec.window);
#endif
  swapChains_.erase(it);
}

} // namespace

/// 工厂入口（供 rhi_factory.cpp 调用）：构造并初始化，失败返回 nullptr。
std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc) {
  auto device = std::make_unique<VulkanDevice>();
  if (!device->init(desc)) return nullptr;
  return device;
}

} // namespace rd
