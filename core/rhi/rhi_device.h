/**
 * @file rhi_device.h
 * @brief RHI 核心接口：CommandBuffer（命令录制）与 Device（GPU 设备抽象）。
 *
 * 线程约束：同一 Device（及其派生的 CommandBuffer/SwapChain）的所有调用必须在
 * 同一线程（Android=RenderView 渲染线程；iOS=主线程 MTKView 惯例；测试内单线程）。
 *
 * 错误处理：内核不使用异常；创建失败返回无效句柄并记日志。
 */
#pragma once
#include "rhi/rhi_capability.h"
#include "rhi/rhi_types.h"
#include <memory>

namespace rd {

/**
 * @brief 一帧渲染命令的录制对象。
 *
 * 每帧从 Device::acquireCommandBuffer 获取，录制一帧的渲染命令后由
 * Device::submit 提交。生命周期由 Device 持有，调用方不得 delete。
 *
 * 典型用法：
 * @code
 *   auto* cmd = device.acquireCommandBuffer();
 *   cmd->beginRenderPass(target, clear);
 *   cmd->bindPipeline(pipeline);
 *   cmd->bindVertexBuffer(0, vbo, 0);
 *   cmd->draw(3, 0);
 *   cmd->endRenderPass();
 *   device.submit(cmd);
 * @endcode
 */
class CommandBuffer {
public:
  virtual ~CommandBuffer() = default;

  /**
   * @brief 开始一个 render pass（绑定渲染目标并清屏）。
   * @param target 离屏目标或 acquireSwapChainTarget 得到的 swapchain 帧目标。
   * @param clear  清屏颜色。
   */
  virtual void beginRenderPass(TargetHandle target, const ClearColor& clear) = 0;
  /// 绑定渲染管线（之后的 draw 使用该管线的 shader 与状态）。
  virtual void bindPipeline(PipelineHandle pipeline) = 0;
  /**
   * @brief 绑定顶点缓冲到 binding 槽位。
   * @param binding 槽位号，与 PipelineDesc::vertexBindings 对应；
   *        Metal 侧映射为 buffer(N+1)（见 rhi_types.h 绑定约定）。
   */
  virtual void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) = 0;
  /// 绑定索引缓冲；type 指明索引元素位宽（UInt16/UInt32）。
  virtual void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) = 0;
  /**
   * @brief 绑定 uniform 缓冲到 slot。
   * @param slot 0..3；三后端映射见 rhi_types.h 绑定约定。
   * @param offset/size 绑定缓冲的子区间（字节）。
   */
  virtual void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                 uint64_t size) = 0;
  /**
   * @brief 绑定纹理+采样器到 slot。
   * @param slot 0..7；Metal 映射为 texture/sampler(N+4)，Vulkan 为
   *        set0 binding(N+4) combined-image-sampler，GLES 为纹理单元 N。
   */
  virtual void bindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) = 0;
  /// 非索引绘制：从 firstVertex 起画 vertexCount 个顶点。
  virtual void draw(uint32_t vertexCount, uint32_t firstVertex) = 0;
  /// 索引绘制：indexCount 个索引，起始索引 firstIndex，顶点偏移 vertexOffset。
  virtual void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) = 0;
  /// 实例化非索引绘制。firstInstance 在 GLES(ES3.0) 不受支持,非 0 时记警告按 0 处理。
  virtual void drawInstanced(uint32_t vertexCount, uint32_t firstVertex,
                             uint32_t instanceCount, uint32_t firstInstance) = 0;
  /// 实例化索引绘制;firstInstance 同上 GLES 限制。
  virtual void drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex,
                                    int32_t vertexOffset, uint32_t instanceCount,
                                    uint32_t firstInstance) = 0;
  /// 结束当前 render pass（与 beginRenderPass 配对）。
  virtual void endRenderPass() = 0;
};

/**
 * @brief GPU 设备抽象。所有 GPU 资源只经本接口创建/销毁。
 *
 * 所有方法只在渲染线程调用（P0-1 测试内单线程使用）。
 * createXxx 失败返回无效句柄（Handle::valid()==false）并记日志；
 * destroyXxx 传入无效句柄是安全的（后端直接忽略）。
 */
class Device {
public:
  virtual ~Device() = default;
  /// 返回本设备的后端种类。
  virtual Backend backend() const = 0;
  /// 返回本设备能力表(init 时上报,之后只读;缺省 0 = 不支持)。
  virtual const DeviceCaps& caps() const = 0;
  /// 将请求的 MSAA 采样数对齐到设备实际支持值(向下取最近支持值,最小 1)。
  /// 背景:iOS 模拟器(MTLSimDriver)只支持 4x,拒绝 2x;真机支持 {2,4}。
  virtual uint32_t snapSampleCount(uint32_t requested) const = 0;

  /// @name 缓冲
  /// @{
  /// 创建缓冲；desc.data 非空则创建时上传。失败返回无效句柄。
  virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
  /// 更新缓冲子区间（offset+size 不得超过创建时的 size）。
  virtual void updateBuffer(BufferHandle buffer, const void* data, uint64_t size,
                            uint64_t offset) = 0;
  /// 销毁缓冲；无效句柄安全忽略。
  virtual void destroyBuffer(BufferHandle buffer) = 0;
  /// @}

  /// @name 着色器模块
  /// @{
  /// 编译/加载着色器模块（code 按后端为 SPIR-V/metallib/GLSL ES）。失败返回无效句柄。
  virtual ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) = 0;
  virtual void destroyShaderModule(ShaderModuleHandle module) = 0;
  /// @}

  /// @name 管线
  /// @{
  /**
   * @brief 创建渲染管线。
   * @note 渲染到 swapchain 时 desc.colorFormat 必须与 swapChainColorFormat()
   *       一致（Metal layer 限 BGRA8 系）。失败返回无效句柄。
   */
  virtual PipelineHandle createPipeline(const PipelineDesc& desc) = 0;
  virtual void destroyPipeline(PipelineHandle pipeline) = 0;
  /// @}

  /// @name 渲染目标
  /// @{
  /// 创建离屏渲染目标（颜色附件，可选 D32 深度附件）。失败返回无效句柄。
  virtual TargetHandle createOffscreenTarget(const OffscreenTargetDesc& desc) = 0;
  /// 销毁渲染目标（离屏或 swapchain 帧目标均经此释放后端侧封装）。
  virtual void destroyTarget(TargetHandle target) = 0;
  /// 查询目标尺寸（离屏/texture-backed/swapchain 目标均有效；无效句柄输出 0）。
  virtual void targetSize(TargetHandle target, uint32_t& outW, uint32_t& outH) const = 0;
  /// 目标的可采样颜色纹理：MSAA 目标返回 resolve 纹理；texture-backed 返回源纹理；
  /// swapchain 目标/无效句柄返回无效 TextureHandle。
  /// 句柄生命周期随目标（destroyTarget 后失效，勿 destroyTexture）。
  virtual TextureHandle targetColorTexture(TargetHandle target) = 0;
  /// @}

  /// @name 纹理与采样器
  /// @{
  /**
   * @brief 创建纹理；desc.data 非空则创建时上传。
   * @note 约束：Cube 必须 width==height；mipLevels 不得超过
   *       floor(log2(max(w,h)))+1。违反约束返回无效句柄并记日志。
   */
  virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
  virtual void destroyTexture(TextureHandle texture) = 0;
  /// 更新纹理子资源(2D 时 face 传 0);数据为整层紧凑像素(格式须与创建时一致)。
  virtual void updateTexture(TextureHandle tex, uint32_t mipLevel, uint32_t face,
                             const void* data, uint64_t size) = 0;
  /// 运行时生成全部 mip 链;能力门控,不支持/纹理无效/mip<2 返回 false。
  virtual bool generateMipmaps(TextureHandle tex) = 0;
  /// 创建采样器（固定功能过滤/寻址状态）。
  virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
  virtual void destroySampler(SamplerHandle sampler) = 0;
  /// @}

  /**
   * @brief 读出目标像素为紧凑排列的 RGBA8。
   * @param outRGBA8 输出缓冲；@param outSize 需 >= width*height*4。
   * @return 成功 true；失败（目标无效/缓冲过小/后端不支持）false。
   */
  virtual bool readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) = 0;

  // ---- SwapChain（上屏渲染）----
  /**
   * @brief 创建交换链。
   * @param nativeWindow Android=ANativeWindow*，iOS=CAMetalLayer*。
   *        nullptr/非法返回无效句柄。
   */
  virtual SwapChainHandle createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) = 0;
  /// 通知交换链尺寸变化（如旋转/分屏后）。
  virtual void resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) = 0;
  /**
   * @brief 获取当前帧可渲染目标（Metal: nextDrawable；Vulkan: acquireNextImage；
   *        GLES: 默认帧缓冲）。
   * @return 失败（如表面丢失中）返回无效 TargetHandle，调用方应跳过本帧；
   *         返回的目标由 acquireSwapChainTarget 内部管理，随 present/下次 acquire 失效。
   */
  virtual TargetHandle acquireSwapChainTarget(SwapChainHandle swapChain) = 0;
  /// 提交并上屏当前帧。
  virtual void present(SwapChainHandle swapChain) = 0;
  virtual void destroySwapChain(SwapChainHandle swapChain) = 0;
  /**
   * @brief swapchain 的颜色格式（创建 pipeline 时须与之匹配；
   *        Metal layer 仅支持 BGRA8 系）。
   */
  virtual Format swapChainColorFormat(SwapChainHandle swapChain) const = 0;

  /// @name 帧括号
  /// @{
  /// 渲染循环每帧开始调用一次:帧序号推进。离屏一次性渲染可不调用
  /// (退休队列由 waitIdle 兜底清空)。
  virtual void beginFrame() = 0;
  /// 每帧结束调用一次(present 之后):按帧完成机制推进资源退休。
  virtual void endFrame() = 0;
  /// @}

  /// @name 命令与同步
  /// @{
  /// 获取本帧命令缓冲；返回值由 Device 持有，勿 delete。
  virtual CommandBuffer* acquireCommandBuffer() = 0;
  /// 提交已录制的命令缓冲执行。
  virtual void submit(CommandBuffer* cmd) = 0;
  /// 阻塞直到 GPU 空闲（测试 readback/截图前使用；渲染循环中一般不需要）；
  /// 附加语义:清空资源退休队列。
  virtual void waitIdle() = 0;
  /// @}
};

/**
 * @brief 设备工厂：按 desc.backend 创建对应后端的设备。
 * @return 后端不可用时返回 nullptr（编译期未启用或运行期无设备），并记警告日志。
 */
std::unique_ptr<Device> createDevice(const DeviceDesc& desc);

} // namespace rd
