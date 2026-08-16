// ============================================================================
// Metal 后端实现（Objective-C++）
//
// 架构要点：
// - 句柄表：每类资源一张 unordered_map<Handle, Rec>，句柄值由 nextId_ 自增分配；
//   destroy 即 erase，ARC 负责 ObjC 对象释放。
// - 绑定约定（见 rhi_types.h）：uniform slot N ↔ Metal buffer(N)（顶点/片段同时绑）;
//   texture slot N ↔ texture/sampler(N+4)；vertex binding N ↔ buffer(N+4)
//   （uniform 0..3 占 buffer 0..3,顶点从 4 起,避免槽位碰撞）。
// - 着色器：加载离线编译的 metallib（dispatch_data 包装），入口名约定 "main0"
//   （spirv-cross 生成 MSL 的默认入口名）。
// - swapchain：CAMetalLayer 颜色格式限 BGRA8 系（iOS 尤其严格），
//   因此 swapChainColorFormat 恒返回 BGRA8_UNORM；创建 pipeline 时 colorFormat 须匹配。
// - 命令模型：全设备共用一个 MetalCommandBuffer 成员，acquireCommandBuffer 时
//   从队列取新的 MTLCommandBuffer；submit commit 并记录 lastCmd_，waitIdle 等待之
//   （单线程渲染模型下的最简实现）。
// ============================================================================
#include "metal_device.h"
#include "foundation/log.h"
#include "rhi/retire_queue.h"
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rd {
namespace {

// ---- rhi 枚举 → Metal 枚举的映射 ----

/// 像素格式映射；不支持的格式返回 MTLPixelFormatInvalid（由调用方校验）。
MTLPixelFormat toMTLPixelFormat(Format f) {
  switch (f) {
    case Format::RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case Format::BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case Format::R32G32_FLOAT: return MTLPixelFormatRG32Float;
    case Format::D32_FLOAT:   return MTLPixelFormatDepth32Float;
    case Format::ASTC_4x4_UNORM: return MTLPixelFormatASTC_4x4_LDR;
    case Format::ETC2_RGBA8_UNORM: return MTLPixelFormatInvalid;  // Metal 不支持 ETC2
    default:                  return MTLPixelFormatInvalid;
  }
}

/// 顶点属性格式映射；不支持的格式返回 MTLVertexFormatInvalid。
MTLVertexFormat toMTLVertexFormat(Format f) {
  switch (f) {
    case Format::R32G32_FLOAT:        return MTLVertexFormatFloat2;
    case Format::R32G32B32_FLOAT:     return MTLVertexFormatFloat3;
    case Format::R32G32B32A32_FLOAT:  return MTLVertexFormatFloat4;
    case Format::RGBA8_UNORM:         return MTLVertexFormatUChar4Normalized;
    default:                          return MTLVertexFormatInvalid;
  }
}

/// 图元拓扑映射（draw 参数而非管线状态）。
MTLPrimitiveType toMTLTopology(PrimitiveTopology t) {
  switch (t) {
    case PrimitiveTopology::TriangleList:  return MTLPrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    case PrimitiveTopology::LineList:      return MTLPrimitiveTypeLine;
  }
  return MTLPrimitiveTypeTriangle;
}

/// 面剔除映射。
MTLCullMode toMTLCull(CullMode m) {
  switch (m) {
    case CullMode::Back:  return MTLCullModeBack;
    case CullMode::Front: return MTLCullModeFront;
    case CullMode::None:  return MTLCullModeNone;
  }
  return MTLCullModeNone;
}

/// 混合因子映射。
MTLBlendFactor toMTLBlendFactor(BlendFactor f) {
  switch (f) {
    case BlendFactor::Zero: return MTLBlendFactorZero;
    case BlendFactor::One: return MTLBlendFactorOne;
    case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
    case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
    case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
  }
  return MTLBlendFactorOne;
}

/// 管线缓存 key:影响 MTLRenderPipelineState 创建的全部参数。
struct PipelineKey {
  uint32_t vs = 0, fs = 0;
  uint32_t topology = 0, cull = 0;
  bool depthTest = false, depthWrite = false;
  uint32_t depthCompare = 0;
  bool blendEnable = false;
  uint32_t srcColor = 0, dstColor = 0, srcAlpha = 0, dstAlpha = 0;
  uint32_t colorFormat = 0;
  uint32_t sampleCount = 1;
  std::vector<VertexBinding> bindings;
  std::vector<VertexAttribute> attribs;
  bool operator==(const PipelineKey& o) const {
    return vs == o.vs && fs == o.fs && topology == o.topology && cull == o.cull &&
           depthTest == o.depthTest && depthWrite == o.depthWrite &&
           depthCompare == o.depthCompare &&
           blendEnable == o.blendEnable && srcColor == o.srcColor && dstColor == o.dstColor &&
           srcAlpha == o.srcAlpha && dstAlpha == o.dstAlpha && colorFormat == o.colorFormat &&
           sampleCount == o.sampleCount && bindings == o.bindings && attribs == o.attribs;
  }
};
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const {
    size_t h = std::hash<uint64_t>()((uint64_t(k.vs) << 32) | k.fs);
    auto mix = [&h](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
    mix(k.topology); mix(k.cull); mix(k.colorFormat); mix(k.sampleCount);
    mix(k.depthTest); mix(k.depthWrite); mix(k.depthCompare); mix(k.blendEnable);
    mix(k.srcColor); mix(k.dstColor); mix(k.srcAlpha); mix(k.dstAlpha);
    for (const auto& b : k.bindings) {
      mix((size_t(b.binding) << 8) | b.stride);
      mix(uint32_t(b.stepRate));
    }
    for (const auto& a : k.attribs)
      mix((size_t(a.location) << 24) ^ (size_t(a.offset) << 8) ^ uint32_t(a.format) ^ a.binding);
    return h;
  }
};

/// 由 PipelineDesc 构造缓存 key。
PipelineKey makePipelineKey(const PipelineDesc& desc) {
  PipelineKey key;
  key.vs = desc.vertexShader.value();
  key.fs = desc.fragmentShader.value();
  key.topology = uint32_t(desc.topology);
  key.cull = uint32_t(desc.cullMode);
  key.depthTest = desc.depthTest;
  key.depthWrite = desc.depthWrite;
  key.depthCompare = uint32_t(desc.depthCompare);
  key.blendEnable = desc.blend.enable;
  key.srcColor = uint32_t(desc.blend.srcColor);
  key.dstColor = uint32_t(desc.blend.dstColor);
  key.srcAlpha = uint32_t(desc.blend.srcAlpha);
  key.dstAlpha = uint32_t(desc.blend.dstAlpha);
  key.colorFormat = uint32_t(desc.colorFormat);
  key.sampleCount = desc.sampleCount;
  key.bindings = desc.vertexBindings;
  key.attribs = desc.attributes;
  return key;
}

// ---- 资源记录（句柄表的 value）：持有 ObjC 对象 + 渲染所需的状态缓存 ----

struct BufferRec { id<MTLBuffer> buffer; bool hostVisible = false; };
struct ShaderRec { id<MTLLibrary> library; std::string entry; };  ///< entry=入口名（约定 main0）
struct PipelineRec {
  id<MTLRenderPipelineState> state;
  MTLPrimitiveType topology;
  MTLCullMode cull;
  id<MTLDepthStencilState> depthState = nil;  ///< 深度状态(depthTest/Write 时创建)
};
struct TargetRec {
  id<MTLTexture> color;
  uint32_t width;
  uint32_t height;
  bool textureBacked = false;  ///< 挂载已有纹理的 face/mip(资源归纹理所有)
  uint32_t face = 0, mip = 0;
  id<MTLTexture> depth = nil;  ///< 深度附件(Depth32Float,hasDepth 时有效)
  bool hasDepth = false;
  TextureHandle colorHandle;   ///< 自建路径注册的可采样颜色句柄
  TextureHandle srcTexture;    ///< textureBacked 的源纹理(destroy 不释放)
};
struct SwapChainRec {
  CAMetalLayer* layer = nil;
  uint32_t width = 0, height = 0;
  id<CAMetalDrawable> drawable = nil;   ///< 当前帧 drawable（acquire 与 present 之间有效）
  TargetHandle currentTarget;           ///< 当前帧 drawable 纹理注册的 TargetHandle（跨帧复用句柄值）
};
struct TextureRec {
  id<MTLTexture> texture;
  uint32_t width = 0, height = 0, mipLevels = 1;
  Format format = Format::RGBA8_UNORM;
  bool isCube = false;
};
struct SamplerRec { id<MTLSamplerState> sampler; };

class MetalDevice;

/**
 * Metal 命令缓冲实现。
 * 缓存当前绑定的管线状态（拓扑/剔除在 draw 时要用）与索引缓冲绑定状态
 * （Metal 的索引缓冲是 draw 时的参数而非独立绑定命令，故先记录、drawIndexed 时使用）。
 */
class MetalCommandBuffer final : public CommandBuffer {
public:
  explicit MetalCommandBuffer(MetalDevice* device) : device_(device) {}
  void beginRenderPass(TargetHandle target, const ClearColor& clear) override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) override;
  void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;
  void bindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) override;
  void draw(uint32_t vertexCount, uint32_t firstVertex) override;
  void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) override;
  void drawInstanced(uint32_t vertexCount, uint32_t firstVertex, uint32_t instanceCount,
                     uint32_t firstInstance) override;
  void drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset,
                            uint32_t instanceCount, uint32_t firstInstance) override;
  void endRenderPass() override;

  MetalDevice* device_;                    ///< 回指设备（查句柄表）
  id<MTLCommandBuffer> cmd_ = nil;         ///< 本帧 MTLCommandBuffer
  id<MTLRenderCommandEncoder> encoder_ = nil;  ///< 当前 render pass 的编码器
  PipelineRec pipeline_{};                 ///< 当前绑定管线的缓存（draw 需要 topology）
  BufferHandle indexBuffer_;               ///< 当前绑定的索引缓冲（drawIndexed 时使用）
  uint64_t indexOffset_ = 0;
  IndexType indexType_ = IndexType::UInt16;
};

class MetalDevice final : public Device {
public:
  /// 析构前等 GPU 空闲，保证资源释放时无在飞命令引用。
  ~MetalDevice() override { waitIdle(); }

  /// 初始化：取系统默认 Metal 设备并创建命令队列。失败记日志返回 false。
  bool init(const DeviceDesc&) {
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
      RD_LOGE("rhi.metal", "MTLCreateSystemDefaultDevice 失败");
      return false;
    }
    queue_ = [device_ newCommandQueue];
    if (queue_ == nil) return false;

    // ---- 能力上报(Apple GPU 家族判定)----
    caps_.set(Capability::max_texture_size,
              [device_ supportsFamily:MTLGPUFamilyApple3] ? 16384u : 8192u);
    caps_.set(Capability::max_texture_slots, 8);
    caps_.set(Capability::max_uniform_buffer_slots, 4);
    caps_.set(Capability::instancing, 1);
    caps_.set(Capability::msaa, 4);   // Apple 全家族支持 4x MSAA
    caps_.set(Capability::depth_texture, 1);
    caps_.set(Capability::cube_render_target, 1);
    caps_.set(Capability::generate_mipmap, 1);
    caps_.set(Capability::anisotropy, 16);  // Apple GPU 实际支持 16
    const bool isAppleGpu = [device_ supportsFamily:MTLGPUFamilyApple1] ||
                            [device_ supportsFamily:MTLGPUFamilyMac2];
    caps_.set(Capability::texture_compression_astc, isAppleGpu ? 1 : 0);
    caps_.set(Capability::texture_compression_etc2, 0);  // Metal 无 ETC2
    return true;
  }

  Backend backend() const override { return Backend::Metal; }
  const DeviceCaps& caps() const override { return caps_; }

  /// 创建缓冲。hostWrite/hostRead → Shared(CPU/GPU 共享,可直写直读);
  /// 否则 Private(GPU 独占,渲染最快),初始数据经临时 Shared 缓冲 blit 上传。
  BufferHandle createBuffer(const BufferDesc& desc) override {
    const bool hostVisible = desc.hostWrite || desc.hostRead;
    id<MTLBuffer> b =
        [device_ newBufferWithLength:desc.size
                             options:hostVisible ? MTLResourceStorageModeShared
                                                 : MTLResourceStorageModePrivate];
    if (!b) return {};
    if (desc.data) {
      if (hostVisible) {
        memcpy(b.contents, desc.data, desc.size);
      } else {
        // Private 存储:经临时 Shared 缓冲 blit 上传(串行等完成,P0 风格)
        id<MTLBuffer> staging = [device_ newBufferWithBytes:desc.data
                                                     length:desc.size
                                                    options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:staging sourceOffset:0 toBuffer:b destinationOffset:0 size:desc.size];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
      }
    }
    BufferHandle h(nextId_++);
    buffers_.emplace(h, BufferRec{b, hostVisible});
    return h;
  }

  /// 更新缓冲子区间；无效句柄安全忽略。仅 hostVisible 缓冲可更新。
  void updateBuffer(BufferHandle buffer, const void* data, uint64_t size, uint64_t offset) override {
    auto it = buffers_.find(buffer);
    if (it == buffers_.end()) return;
    if (!it->second.hostVisible) {
      RD_LOGE("rhi.metal", "updateBuffer 作用于 Private 缓冲(须 hostWrite=true 创建)");
      return;
    }
    memcpy(static_cast<uint8_t*>(it->second.buffer.contents) + offset, data, size);
  }

  /// 销毁缓冲:句柄立即失效;底层对象由退休闭包持有,帧完成后释放(ARC)。
  void destroyBuffer(BufferHandle buffer) override {
    auto it = buffers_.find(buffer);
    if (it == buffers_.end()) return;
    id<MTLBuffer> b = it->second.buffer;
    buffers_.erase(it);
    retire_.retire(frameIndex_, [b] { (void)b; });
  }

  /**
   * 加载 metallib 为 MTLLibrary。
   * newLibraryWithData: 要求 dispatch_data_t 包装；用 DISPATCH_DATA_DESTRUCTOR_FREE
   * 让 dispatch 在释放时 free 我们 malloc 的副本，避免泄漏。
   */
  ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) override {
    void* copy = malloc(desc.code.size());
    memcpy(copy, desc.code.data(), desc.code.size());
    dispatch_data_t d = dispatch_data_create(copy, desc.code.size(),
        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_FREE);
    NSError* err = nil;
    id<MTLLibrary> lib = [device_ newLibraryWithData:d error:&err];
    if (!lib) {
      RD_LOGE("rhi.metal", "metallib 加载失败: %s",
              err ? err.localizedDescription.UTF8String : "unknown");
      return {};
    }
    ShaderModuleHandle h(nextId_++);
    shaders_.emplace(h, ShaderRec{lib, desc.entryPoint});
    return h;
  }

  void destroyShaderModule(ShaderModuleHandle module) override {
    auto it = shaders_.find(module);
    if (it == shaders_.end()) return;
    ShaderRec rec = it->second;
    shaders_.erase(it);
    retire_.retire(frameIndex_, [rec] { (void)rec; });
  }

  PipelineHandle createPipeline(const PipelineDesc& desc) override {
    // MSAA 尚未实现，先拒绝而非静默错误（与 Vulkan/GLES 一致）。
    // 注意:depthTest/depthWrite 管线须配 depth=true 的渲染目标(反之亦然)。
    if (desc.sampleCount != 1) {
      RD_LOGE("rhi.metal", "MSAA 为 P2 预留,当前拒绝 sampleCount != 1");
      return {};
    }
    auto vsIt = shaders_.find(desc.vertexShader);
    auto fsIt = shaders_.find(desc.fragmentShader);
    if (vsIt == shaders_.end() || fsIt == shaders_.end()) return {};

    // 缓存命中:直接包装新句柄返回(底层 MTLRenderPipelineState 由缓存持有;
    // depthState 轻量,逐句柄创建)
    PipelineKey key = makePipelineKey(desc);
    if (auto it = pipelineCache_.find(key); it != pipelineCache_.end()) {
      PipelineHandle h(nextId_++);
      pipelines_.emplace(h, PipelineRec{it->second, toMTLTopology(desc.topology),
                                        toMTLCull(desc.cullMode),
                                        makeDepthState(desc)});
      return h;
    }

    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    // 入口名约定：metallib 内为 "main0"（spirv-cross 生成 MSL 的默认名）。
    pd.vertexFunction = [vsIt->second.library
        newFunctionWithName:@(vsIt->second.entry.c_str())];
    pd.fragmentFunction = [fsIt->second.library
        newFunctionWithName:@(fsIt->second.entry.c_str())];
    // 颜色格式须与渲染目标一致：渲染到 swapchain 时调用方应传 swapChainColorFormat()。
    pd.colorAttachments[0].pixelFormat = toMTLPixelFormat(desc.colorFormat);
    if (!pd.vertexFunction || !pd.fragmentFunction) return {};
    // 深度:PSO 需声明附件格式(与 depth 目标匹配);状态经独立 DepthStencilState 下发
    if (desc.depthTest || desc.depthWrite)
      pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    // 混合状态(默认关闭)
    pd.colorAttachments[0].blendingEnabled = desc.blend.enable;
    pd.colorAttachments[0].sourceRGBBlendFactor = toMTLBlendFactor(desc.blend.srcColor);
    pd.colorAttachments[0].destinationRGBBlendFactor = toMTLBlendFactor(desc.blend.dstColor);
    pd.colorAttachments[0].sourceAlphaBlendFactor = toMTLBlendFactor(desc.blend.srcAlpha);
    pd.colorAttachments[0].destinationAlphaBlendFactor = toMTLBlendFactor(desc.blend.dstAlpha);

    // 顶点布局：RHI binding N ↔ Metal buffer(N+4)(uniform slot 0..3 占 buffer 0..3)。
    MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
    for (const auto& a : desc.attributes) {
      vd.attributes[a.location].format = toMTLVertexFormat(a.format);
      vd.attributes[a.location].offset = a.offset;
      vd.attributes[a.location].bufferIndex = a.binding + 4;  // 约定:0..3 留给 uniform
    }
    for (const auto& b : desc.vertexBindings) {
      vd.layouts[b.binding + 4].stride = b.stride;
      vd.layouts[b.binding + 4].stepFunction =
          b.stepRate == VertexStepRate::Instance ? MTLVertexStepFunctionPerInstance
                                                 : MTLVertexStepFunctionPerVertex;
    }
    pd.vertexDescriptor = vd;

    NSError* err = nil;
    id<MTLRenderPipelineState> state =
        [device_ newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!state) {
      RD_LOGE("rhi.metal", "pipeline 创建失败: %s",
              err ? err.localizedDescription.UTF8String : "unknown");
      return {};
    }
    // 缓存拓扑/剔除：Metal 的图元类型是 draw 参数而非管线状态。
    pipelineCache_.emplace(key, state);
    PipelineHandle h(nextId_++);
    pipelines_.emplace(h, PipelineRec{state, toMTLTopology(desc.topology),
                                      toMTLCull(desc.cullMode), makeDepthState(desc)});
    return h;
  }

  void destroyPipeline(PipelineHandle pipeline) override {
    // 只释放句柄引用;底层对象由 pipelineCache_ 持有(设备析构时统一释放)
    pipelines_.erase(pipeline);
  }

  /// 由 desc 创建深度状态(depthTest/Write 关闭时返回 nil)。
  id<MTLDepthStencilState> makeDepthState(const PipelineDesc& desc) {
    if (!desc.depthTest && !desc.depthWrite) return nil;
    MTLDepthStencilDescriptor* dsd = [[MTLDepthStencilDescriptor alloc] init];
    dsd.depthCompareFunction = desc.depthCompare == DepthCompareOp::Greater
                                   ? MTLCompareFunctionGreater
                                   : MTLCompareFunctionLess;
    dsd.depthWriteEnabled = desc.depthWrite;
    return [device_ newDepthStencilStateWithDescriptor:dsd];
  }

  /// 创建离屏目标。colorFromTexture 非空时挂载该纹理的 face/mip 子资源为颜色附件;
  /// 否则自建单张颜色纹理(Shared 存储,便于 readback 直接读取)。
  TargetHandle createOffscreenTarget(const OffscreenTargetDesc& desc) override {
    if (desc.colorFromTexture.valid()) {
      auto it = textures_.find(desc.colorFromTexture);
      if (it == textures_.end()) return {};
      if (it->second.isCube && !caps_.supports(Capability::cube_render_target)) return {};
      if (desc.face >= (it->second.isCube ? 6u : 1u) || desc.mipLevel >= it->second.mipLevels)
        return {};
      TargetHandle h(nextId_++);
      TargetRec rec;
      rec.color = it->second.texture;  // 共享底层纹理(ARC 强引用)
      rec.width = desc.width;
      rec.height = desc.height;
      rec.textureBacked = true;
      rec.face = desc.face;
      rec.mip = desc.mipLevel;
      rec.srcTexture = desc.colorFromTexture;
      targets_.emplace(h, rec);
      return h;
    }
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:toMTLPixelFormat(desc.colorFormat)
                                                           width:desc.width
                                                          height:desc.height
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared; // macOS 统一内存，便于 readback
    id<MTLTexture> tex = [device_ newTextureWithDescriptor:td];
    if (!tex) return {};
    TargetHandle h(nextId_++);
    TargetRec rec;
    rec.color = tex;
    rec.width = desc.width;
    rec.height = desc.height;
    // 注册可采样颜色句柄(纹理归 TargetRec 所有,句柄仅引用)
    {
      TextureRec trec;
      trec.texture = tex;
      trec.width = desc.width;
      trec.height = desc.height;
      trec.mipLevels = 1;
      trec.format = desc.colorFormat;
      trec.isCube = false;
      rec.colorHandle = TextureHandle(nextId_++);
      textures_.emplace(rec.colorHandle, trec);
    }
    // 深度附件(Depth32Float,Private;Metal 深度状态在 encoder 侧按管线设置)
    if (desc.depth) {
      MTLTextureDescriptor* dd =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                             width:desc.width
                                                            height:desc.height
                                                         mipmapped:NO];
      dd.usage = MTLTextureUsageRenderTarget;
      dd.storageMode = MTLStorageModePrivate;
      rec.depth = [device_ newTextureWithDescriptor:dd];
      if (!rec.depth) return {};
      rec.hasDepth = true;
    }
    targets_.emplace(h, rec);
    return h;
  }

  void destroyTarget(TargetHandle target) override {
    auto it = targets_.find(target);
    if (it == targets_.end()) return;
    TargetRec rec = it->second;
    targets_.erase(it);
    if (rec.colorHandle.valid()) textures_.erase(rec.colorHandle);  // 只摘句柄
    retire_.retire(frameIndex_, [rec] { (void)rec; });
  }

  void targetSize(TargetHandle target, uint32_t& outW, uint32_t& outH) const override {
    auto it = targets_.find(target);
    outW = it != targets_.end() ? it->second.width : 0;
    outH = it != targets_.end() ? it->second.height : 0;
  }

  TextureHandle targetColorTexture(TargetHandle target) override {
    auto it = targets_.find(target);
    if (it == targets_.end()) return {};
    const TargetRec& t = it->second;
    return t.textureBacked ? t.srcTexture : t.colorHandle;
  }

  TextureHandle createTexture(const TextureDesc& desc) override {
    // ---- 前置校验（与 GLES/Vulkan 保持一致的失败语义）----
    if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0) {
      RD_LOGE("rhi.metal", "createTexture: width/height/mipLevels 不能为 0");
      return {};
    }
    if (desc.type == TextureType::Cube && desc.width != desc.height) {
      RD_LOGE("rhi.metal", "createTexture: cube 纹理必须方形（%ux%u）", desc.width, desc.height);
      return {};
    }
    // mip 级数上限：floor(log2(maxDim))+1
    const uint32_t maxDim = desc.width > desc.height ? desc.width : desc.height;
    const uint32_t maxMipLevels = uint32_t(std::floor(std::log2(double(maxDim)))) + 1;
    if (desc.mipLevels > maxMipLevels) {
      RD_LOGE("rhi.metal", "createTexture: mipLevels %u 超出上限 %u（%ux%u）", desc.mipLevels,
              maxMipLevels, desc.width, desc.height);
      return {};
    }
    if ((desc.format == Format::ASTC_4x4_UNORM &&
         !caps_.supports(Capability::texture_compression_astc)) ||
        (desc.format == Format::ETC2_RGBA8_UNORM &&
         !caps_.supports(Capability::texture_compression_etc2))) {
      RD_LOGE("rhi.metal", "createTexture: 压缩格式 %d 不受本后端支持", int(desc.format));
      return {};
    }
    MTLTextureDescriptor* td;
    if (desc.type == TextureType::Cube) {
      td = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:toMTLPixelFormat(desc.format)
                                                                 size:desc.width
                                                            mipmapped:desc.mipLevels > 1];
    } else {
      td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:toMTLPixelFormat(desc.format)
                                                              width:desc.width
                                                             height:desc.height
                                                          mipmapped:desc.mipLevels > 1];
    }
    td.mipmapLevelCount = desc.mipLevels;
    td.usage = MTLTextureUsageShaderRead;
    if (hasFlag(desc.usage, TextureUsage::RenderTargetAttachment))
      td.usage |= MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [device_ newTextureWithDescriptor:td];
    if (!tex) return {};

    // 初始数据上传：布局为 slice（cube 6 面）× mip 逐层紧凑排列，
    // 每层尺寸逐级减半；越界检查失败时返回无效句柄（已创建的 tex 由 ARC 回收）。
    if (desc.data) {
      const FormatBlockInfo bi = formatBlockInfo(desc.format);
      const uint8_t* src = static_cast<const uint8_t*>(desc.data);
      uint64_t offset = 0;
      const uint32_t slices = desc.type == TextureType::Cube ? 6 : 1;
      for (uint32_t slice = 0; slice < slices; ++slice) {
        uint32_t w = desc.width;
        uint32_t h = desc.height;
        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
          const uint64_t levelBytes = formatMipBytes(desc.format, w, h);
          if (offset + levelBytes > desc.dataSize) {
            RD_LOGE("rhi.metal", "createTexture: 数据越界（slice %u mip %u）", slice, mip);
            return {};
          }
          // 压缩格式 bytesPerRow = 每行 block 数 × block 字节数
          const uint64_t rowBytes =
              uint64_t((w + bi.blockW - 1) / bi.blockW) * bi.bytesPerBlock;
          [tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
                 mipmapLevel:mip
                        slice:slice
                    withBytes:src + offset
                 bytesPerRow:rowBytes
               bytesPerImage:desc.type == TextureType::Cube ? levelBytes : 0];
          offset += levelBytes;
          w = w > 1 ? w / 2 : 1;
          h = h > 1 ? h / 2 : 1;
        }
      }
    }
    TextureHandle h(nextId_++);
    textures_.emplace(h, TextureRec{tex, desc.width, desc.height, desc.mipLevels, desc.format,
                                    desc.type == TextureType::Cube});
    return h;
  }

  void destroyTexture(TextureHandle texture) override {
    auto it = textures_.find(texture);
    if (it == textures_.end()) return;
    id<MTLTexture> t = it->second.texture;
    textures_.erase(it);
    retire_.retire(frameIndex_, [t] { (void)t; });
  }

  /// 更新纹理子资源(slice=face,level=mip;数据为整层紧凑像素)。
  void updateTexture(TextureHandle tex, uint32_t mipLevel, uint32_t face, const void* data,
                     uint64_t size) override {
    auto it = textures_.find(tex);
    if (it == textures_.end() || !data) return;
    const TextureRec& tr = it->second;
    if (mipLevel >= tr.mipLevels || face >= (tr.isCube ? 6u : 1u)) return;
    uint32_t w = tr.width >> mipLevel, h = tr.height >> mipLevel;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    const uint64_t need = formatMipBytes(tr.format, w, h);
    if (size < need) {
      RD_LOGE("rhi.metal", "updateTexture: 数据不足(需 %llu)", (unsigned long long)need);
      return;
    }
    const FormatBlockInfo bi = formatBlockInfo(tr.format);
    const uint64_t rowBytes = uint64_t((w + bi.blockW - 1) / bi.blockW) * bi.bytesPerBlock;
    [tr.texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                  mipmapLevel:mipLevel
                        slice:face
                    withBytes:data
                  bytesPerRow:rowBytes
                bytesPerImage:tr.isCube ? need : 0];
  }

  /// 生成全部 mip 链(blit encoder;Shared/Private 均可)。
  bool generateMipmaps(TextureHandle tex) override {
    auto it = textures_.find(tex);
    if (it == textures_.end() || it->second.mipLevels < 2) return false;
    id<MTLCommandBuffer> cb = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit generateMipmapsForTexture:it->second.texture];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return true;
  }

  /// 创建采样器：rhi 过滤/寻址枚举一一映射到 MTLSamplerDescriptor。
  SamplerHandle createSampler(const SamplerDesc& desc) override {
    auto toMinMag = [](Filter f) {
      return f == Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    };
    auto toWrap = [](WrapMode m) {
      return m == WrapMode::Repeat ? MTLSamplerAddressModeRepeat
                                   : MTLSamplerAddressModeClampToEdge;
    };
    MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = toMinMag(desc.minFilter);
    sd.magFilter = toMinMag(desc.magFilter);
    sd.mipFilter = desc.mipFilter == Filter::Linear ? MTLSamplerMipFilterLinear
                                                    : MTLSamplerMipFilterNearest;
    sd.sAddressMode = toWrap(desc.wrapU);
    sd.tAddressMode = toWrap(desc.wrapV);
    sd.rAddressMode = toWrap(desc.wrapW);
    // 各向异性:>1 且设备支持时启用,等级取请求与上限的较小值
    if (desc.maxAnisotropy > 1 && caps_.supports(Capability::anisotropy)) {
      sd.maxAnisotropy = NSUInteger(std::min(desc.maxAnisotropy,
                                             caps_.get(Capability::anisotropy)));
    }
    id<MTLSamplerState> sampler = [device_ newSamplerStateWithDescriptor:sd];
    if (!sampler) return {};
    SamplerHandle h(nextId_++);
    samplers_.emplace(h, SamplerRec{sampler});
    return h;
  }

  void destroySampler(SamplerHandle sampler) override {
    auto it = samplers_.find(sampler);
    if (it == samplers_.end()) return;
    id<MTLSamplerState> s = it->second.sampler;
    samplers_.erase(it);
    retire_.retire(frameIndex_, [s] { (void)s; });
  }

  /**
   * 读出目标像素。仅支持 Shared 存储的离屏目标;texture-backed 目标按 face/mip 读取。
   * swapchain drawable 纹理是 Private 存储（framebufferOnly=YES），不支持直接读取。
   */
  bool readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) override {
    auto it = targets_.find(target);
    if (it == targets_.end()) return false;
    if (it->second.color.storageMode == MTLStorageModePrivate) {
      RD_LOGW("rhi.metal", "swapchain target 不支持 readback");
      return false;
    }
    const TargetRec& t = it->second;
    if (outSize < uint64_t(t.width) * t.height * 4) return false;
    waitIdle();  // 确保渲染完成再读
    [t.color getBytes:outRGBA8
          bytesPerRow:t.width * 4
       bytesPerImage:uint64_t(t.width) * t.height * 4
           fromRegion:MTLRegionMake2D(0, 0, t.width, t.height)
          mipmapLevel:t.textureBacked ? t.mip : 0
                slice:t.textureBacked ? t.face : 0];
    return true;
  }

  /// 取本帧命令缓冲：复用设备内唯一的 MetalCommandBuffer，仅更换底层 MTLCommandBuffer。
  CommandBuffer* acquireCommandBuffer() override {
    cmdBuf_.cmd_ = [queue_ commandBuffer];
    return &cmdBuf_;
  }

  /// 提交：注册完成回调(只写帧序号,释放在 endFrame 渲染线程执行)并 commit。
  void submit(CommandBuffer*) override {
    uint64_t f = frameIndex_;
    [cmdBuf_.cmd_ addCompletedHandler:^(id<MTLCommandBuffer>) {
      completedFrame_.store(f);
    }];
    [cmdBuf_.cmd_ commit];
    lastCmd_ = cmdBuf_.cmd_;
  }

  /// 帧括号:渲染循环每帧调用;endFrame 按已完成序号推进退休队列。
  void beginFrame() override { ++frameIndex_; }
  void endFrame() override { retire_.onFrameComplete(completedFrame_.load()); }

  /// 等待最近一次提交的命令完成；未提交过任何命令时是 no-op;附加清空退休队列。
  void waitIdle() override {
    if (lastCmd_) {
      [lastCmd_ waitUntilCompleted];
      lastCmd_ = nil;
    }
    retire_.flushAll();
  }

  /**
   * 创建交换链：nativeWindow 必须是 CAMetalLayer*（iOS 由 RenderView 提供）。
   * __bridge 不转移所有权——layer 的生命周期由平台层管理。
   */
  SwapChainHandle createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) override {
    if (!nativeWindow || width == 0 || height == 0) return {};
    CAMetalLayer* layer = (__bridge CAMetalLayer*)nativeWindow;
    if (![layer isKindOfClass:[CAMetalLayer class]]) return {};
    layer.device = device_;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm; // layer 仅支持 BGRA8 系（iOS 尤其严格）
    layer.drawableSize = CGSizeMake(width, height);
    layer.framebufferOnly = YES;
    SwapChainHandle h(nextId_++);
    SwapChainRec rec;
    rec.layer = layer;
    rec.width = width;
    rec.height = height;
    swapChains_.emplace(h, rec);
    return h;
  }

  /// 尺寸变化：更新记录并同步 layer.drawableSize（下一帧生效）。
  void resizeSwapChain(SwapChainHandle sc, uint32_t width, uint32_t height) override {
    auto it = swapChains_.find(sc);
    if (it == swapChains_.end() || width == 0 || height == 0) return;
    it->second.width = width;
    it->second.height = height;
    it->second.layer.drawableSize = CGSizeMake(width, height);
  }

  /**
   * 获取当前帧渲染目标：nextDrawable 取 drawable（池耗尽时返回 nil → 返回无效句柄，
   * 调用方应跳过本帧）。drawable 纹理每帧不同，但复用同一个 TargetHandle 值注册，
   * 使调用方拿到的句柄稳定、内容即时更新。
   */
  TargetHandle acquireSwapChainTarget(SwapChainHandle sc) override {
    auto it = swapChains_.find(sc);
    if (it == swapChains_.end()) return {};
    SwapChainRec& rec = it->second;
    rec.drawable = [rec.layer nextDrawable];
    if (!rec.drawable) return {};
    // 复用/注册当前帧 target（drawable 纹理每帧不同，内容即时更新）
    if (!rec.currentTarget.valid()) {
      rec.currentTarget = TargetHandle(nextId_++);
    }
    targets_[rec.currentTarget] = TargetRec{rec.drawable.texture, rec.width, rec.height};
    return rec.currentTarget;
  }

  /// 上屏：先等 GPU 完成再 present（单缓冲命令模型下的简单同步）。
  void present(SwapChainHandle sc) override {
    auto it = swapChains_.find(sc);
    if (it == swapChains_.end() || !it->second.drawable) return;
    waitIdle();
    [it->second.drawable present];
    it->second.drawable = nil;
  }

  void destroySwapChain(SwapChainHandle sc) override {
    auto it = swapChains_.find(sc);
    if (it == swapChains_.end()) return;
    if (it->second.currentTarget.valid()) targets_.erase(it->second.currentTarget);
    swapChains_.erase(it);
  }

  /// Metal layer 颜色格式恒为 BGRA8（创建 pipeline 时 colorFormat 须用此值）。
  Format swapChainColorFormat(SwapChainHandle) const override { return Format::BGRA8_UNORM; }

  // ---- CommandBuffer 访问的内部状态（句柄表查询辅助）----
  id<MTLBuffer> buffer(BufferHandle h) const {
    auto it = buffers_.find(h);
    return it == buffers_.end() ? nil : it->second.buffer;
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
  id<MTLTexture> texture(TextureHandle h) const {
    auto it = textures_.find(h);
    return it == textures_.end() ? nil : it->second.texture;
  }
  id<MTLSamplerState> sampler(SamplerHandle h) const {
    auto it = samplers_.find(h);
    return it == samplers_.end() ? nil : it->second.sampler;
  }

private:
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLCommandBuffer> lastCmd_ = nil;   ///< 最近提交的命令（waitIdle 等待对象）
  MetalCommandBuffer cmdBuf_{this};      ///< 设备内唯一命令缓冲（单线程模型）
  DeviceCaps caps_;                      ///< 能力表(init 内上报)
  RetireQueue retire_;                   ///< 资源退休队列(帧完成驱动释放)
  uint64_t frameIndex_ = 0;              ///< 当前帧序号(beginFrame 推进)
  std::atomic<uint64_t> completedFrame_{0};  ///< completedHandler 异步写的完成序号
  uint32_t nextId_ = 1;                  ///< 句柄分配器（1 起，0 留作无效）
  std::unordered_map<BufferHandle, BufferRec> buffers_;
  std::unordered_map<ShaderModuleHandle, ShaderRec> shaders_;
  std::unordered_map<PipelineHandle, PipelineRec> pipelines_;
  /// 管线缓存:缓存持有底层 MTLRenderPipelineState(ARC),句柄表只持引用
  std::unordered_map<PipelineKey, id<MTLRenderPipelineState>, PipelineKeyHash> pipelineCache_;
  std::unordered_map<TargetHandle, TargetRec> targets_;
  std::unordered_map<SwapChainHandle, SwapChainRec> swapChains_;
  std::unordered_map<TextureHandle, TextureRec> textures_;
  std::unordered_map<SamplerHandle, SamplerRec> samplers_;
};

// ---- MetalCommandBuffer 各命令的实现 ----

/// 开始 render pass：以 Clear 加载动作绑定颜色附件，并设置全幅 viewport;
/// texture-backed 目标按 face/mip 指定 slice/level。
void MetalCommandBuffer::beginRenderPass(TargetHandle target, const ClearColor& clear) {
  TargetRec t;
  if (!device_->target(target, t)) return;
  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = t.color;
  if (t.textureBacked) {
    rp.colorAttachments[0].slice = t.face;  // cube 面(2D 传 0)
    rp.colorAttachments[0].level = t.mip;   // mip 级
  }
  if (t.hasDepth) {
    rp.depthAttachment.texture = t.depth;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = clear.depth;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
  }
  rp.colorAttachments[0].loadAction = MTLLoadActionClear;
  rp.colorAttachments[0].clearColor = MTLClearColorMake(clear.r, clear.g, clear.b, clear.a);
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  encoder_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
  MTLViewport vp{0, 0, double(t.width), double(t.height), 0, 1};
  [encoder_ setViewport:vp];
}

void MetalCommandBuffer::bindPipeline(PipelineHandle pipeline) {
  if (!device_->pipeline(pipeline, pipeline_)) return;
  [encoder_ setRenderPipelineState:pipeline_.state];
  [encoder_ setCullMode:pipeline_.cull];
  if (pipeline_.depthState) [encoder_ setDepthStencilState:pipeline_.depthState];
}

/// 绑定约定：vertex binding N ↔ Metal buffer(N+4)。
void MetalCommandBuffer::bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) {
  [encoder_ setVertexBuffer:device_->buffer(buffer) offset:offset atIndex:binding + 4];
}

/// Metal 的索引缓冲是 draw 参数而非独立状态：这里仅记录，drawIndexed 时取用。
void MetalCommandBuffer::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) {
  indexBuffer_ = buffer;
  indexOffset_ = offset;
  indexType_ = type;
}

/// 绑定约定：texture slot N ↔ fragment texture/sampler(N+4)。
void MetalCommandBuffer::bindTexture(uint32_t slot, TextureHandle texture,
                                     SamplerHandle sampler) {
  [encoder_ setFragmentTexture:device_->texture(texture) atIndex:slot + 4];
  [encoder_ setFragmentSamplerState:device_->sampler(sampler) atIndex:slot + 4];
}

/// 绑定约定：uniform slot N ↔ buffer(N)，顶点/片段阶段同时绑定。
/// size 参数在 Metal 侧不需要（整段 buffer 可见），故忽略。
void MetalCommandBuffer::bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                           uint64_t) {
  id<MTLBuffer> b = device_->buffer(buffer);
  [encoder_ setVertexBuffer:b offset:offset atIndex:slot];
  [encoder_ setFragmentBuffer:b offset:offset atIndex:slot];
}

void MetalCommandBuffer::draw(uint32_t vertexCount, uint32_t firstVertex) {
  [encoder_ drawPrimitives:pipeline_.topology vertexStart:firstVertex vertexCount:vertexCount];
}

/// 索引绘制。vertexOffset（baseVertex）当前未支持——参数被忽略
/// （MTLRenderCommandEncoder 另有带 baseVertex 的重载，需要时再引入）。
void MetalCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t) {
  const bool u16 = indexType_ == IndexType::UInt16;
  [encoder_ drawIndexedPrimitives:pipeline_.topology
                       indexCount:indexCount
                        indexType:u16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                      indexBuffer:device_->buffer(indexBuffer_)
                indexBufferOffset:indexOffset_ + firstIndex * (u16 ? 2 : 4)];
}

void MetalCommandBuffer::drawInstanced(uint32_t vertexCount, uint32_t firstVertex,
                                       uint32_t instanceCount, uint32_t firstInstance) {
  [encoder_ drawPrimitives:pipeline_.topology
               vertexStart:firstVertex
               vertexCount:vertexCount
             instanceCount:instanceCount
              baseInstance:firstInstance];
}

void MetalCommandBuffer::drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex,
                                              int32_t vertexOffset, uint32_t instanceCount,
                                              uint32_t firstInstance) {
  const bool u16 = indexType_ == IndexType::UInt16;
  [encoder_ drawIndexedPrimitives:pipeline_.topology
                       indexCount:indexCount
                        indexType:u16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                      indexBuffer:device_->buffer(indexBuffer_)
                indexBufferOffset:indexOffset_ + firstIndex * (u16 ? 2 : 4)
                    instanceCount:instanceCount
                       baseVertex:vertexOffset
                     baseInstance:firstInstance];
}

void MetalCommandBuffer::endRenderPass() { [encoder_ endEncoding]; }

} // namespace

/// 工厂入口（供 rhi_factory.cpp 调用）：构造并初始化，失败返回 nullptr。
std::unique_ptr<Device> createMetalDevice(const DeviceDesc& desc) {
  auto device = std::make_unique<MetalDevice>();
  if (!device->init(desc)) return nullptr;
  return device;
}

} // namespace rd
