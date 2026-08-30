// ============================================================================
// GLES(OpenGL ES 3) 后端实现 —— 仅 Android（整个实现包在 #if defined(__ANDROID__) 内）
//
// 架构要点：
// - EGL 生命周期：Device 持有一份 display/context/config，外加 1x1 pbuffer surface
//   用于离屏渲染期间保活 context（GL 命令必须有 current surface 才能执行）。
// - surface 切换：渲染 swapchain 帧时 current 切到窗口 surface；离屏渲染/资源管理
//   时切回 pbuffer。beginRenderPass 与所有资源函数入口处都会确保 current 正确。
// - 绑定约定（见 rhi_types.h）：uniform slot N ↔ GL_UNIFORM_BUFFER binding N；
//   uniform block 名 "UBO" 硬编码映射到 slot 0（P0 约定，P1 由反射 JSON 驱动）。
// - 无 VAO 缓存：每次 draw 前由 applyVertexState 重建顶点属性指针（P0 最简实现）。
// - 坐标系差异：GL 原点左下，readback 时逐行翻转为顶向下，与其他后端输出一致。
// - 纹理/采样器:ES3.0 sampler object;bindTexture 按 slot 绑纹理单元 + glBindSampler,
//   sampler uniform 命名约定 texN(见 rhi_types.h 绑定约定)。
// ============================================================================
#include "gles_device.h"

#if defined(__ANDROID__)
#include "foundation/log.h"
#include "rhi/retire_queue.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/native_window.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace rd {
namespace {

// ---- 资源记录（句柄表的 value）----

/// 缓冲：记录 GL 对象 + 创建时推导的绑定 target（updateBuffer/destroy 时要用）+ 大小。
struct BufferRec {
  GLuint buffer = 0;
  GLenum target = GL_ARRAY_BUFFER;
  uint64_t size = 0;
  bool hostVisible = false;  ///< hostWrite/hostRead(行为一致性守卫用)
};
struct ShaderRec { GLuint shader = 0; ShaderStage stage; };
/// 管线：GL program + 顶点布局缓存（draw 时重建属性指针用）+ 拓扑/剔除/混合/深度状态。
struct PipelineRec {
  GLuint program = 0;
  std::vector<VertexBinding> bindings;
  std::vector<VertexAttribute> attribs;
  GLenum topology = GL_TRIANGLES;
  CullMode cull = CullMode::None;
  BlendDesc blend;
  bool depthTest = false;
  bool depthWrite = false;
  DepthCompareOp depthCompare = DepthCompareOp::Less;
};

/// 管线缓存 key:影响 program 链接与状态的全部参数。
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

/// rhi BlendFactor → GL 枚举映射。
GLenum toGLBlendFactor(BlendFactor f) {
  switch (f) {
    case BlendFactor::Zero: return GL_ZERO;
    case BlendFactor::One: return GL_ONE;
    case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha: return GL_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
  }
  return GL_ONE;
}
/// 渲染目标：离屏 FBO 或 swapchain 默认帧缓冲（fbo=0）。
struct TargetRec {
  GLuint fbo = 0;       // 0 = 默认帧缓冲（swapchain）
  GLuint colorTex = 0;  // 离屏自建颜色纹理(texture-backed 时为 0,附件归纹理所有)
  uint32_t width = 0, height = 0;
  bool isSwapchain = false;
  bool textureBacked = false;  ///< 挂载已有纹理的 face/mip 子资源
  GLuint depthRbo = 0;         ///< 深度 renderbuffer(hasDepth 时有效)
  bool hasDepth = false;
  EGLSurface surface = EGL_NO_SURFACE; // swapchain target 专用：beginRenderPass 时切回窗口 surface
  TextureHandle colorHandle;   ///< 自建路径注册的可采样颜色句柄
  TextureHandle srcTexture;    ///< textureBacked 的源纹理(destroy 不释放)
  uint32_t samples = 1;        ///< MSAA 采样数(>1 时渲染到 msaaColorRbo,blit 到 colorTex)
  GLuint msaaColorRbo = 0;     ///< MSAA 颜色 renderbuffer(samples>1 时有效)
  GLuint resolveFbo = 0;       ///< resolve 目标 FBO(colorTex 挂在这里)
  bool depthOnly = false;      ///< depth-only 目标(阴影贴图;FBO 只挂深度附件)
};
struct SwapChainRec {
  ANativeWindow* window = nullptr;      ///< 持有引用（create 时 acquire，destroy 时 release）
  EGLSurface surface = EGL_NO_SURFACE;
  uint32_t width = 0, height = 0;
  TargetHandle currentTarget;           ///< swapchain 帧目标（fbo=0，跨帧复用句柄值）
};
/// 纹理:GL 对象 + 绑定 target(2D/CUBE_MAP)+ 尺寸/mip 元数据。
struct TextureRec {
  GLuint tex = 0;
  GLenum target = GL_TEXTURE_2D;
  uint32_t width = 0, height = 0, mipLevels = 1;
  Format format = Format::RGBA8_UNORM;
};
struct SamplerRec { GLuint sampler = 0; };

// ---- rhi 枚举 → GL 枚举的映射 ----

GLenum toGLTopology(PrimitiveTopology t) {
  switch (t) {
    case PrimitiveTopology::TriangleList:  return GL_TRIANGLES;
    case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:      return GL_LINES;
  }
  return GL_TRIANGLES;
}

/// 顶点属性元素类型：仅 RGBA8_UNORM 用归一化字节，其余按 float。
GLenum toGLAttribType(Format f) { return f == Format::RGBA8_UNORM ? GL_UNSIGNED_BYTE : GL_FLOAT; }

/// 纹理格式 → (internalFormat, upload format, type) 三元组。
/// 压缩格式 upload/type 返回 0(走 glCompressedTexImage2D 路径)。
void toGLTexFormat(Format f, GLint& internal, GLenum& upload, GLenum& type) {
  switch (f) {
    case Format::RGBA8_UNORM:
      internal = GL_RGBA8; upload = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
    case Format::R32G32_FLOAT:
      internal = GL_RG32F; upload = GL_RG; type = GL_FLOAT; break;
    case Format::R32G32B32A32_FLOAT:
      internal = GL_RGBA32F; upload = GL_RGBA; type = GL_FLOAT; break;
    case Format::D32_FLOAT:
      internal = GL_DEPTH_COMPONENT32F; upload = GL_DEPTH_COMPONENT; type = GL_FLOAT;
      break;
    case Format::R16G16B16A16_FLOAT:
      internal = GL_RGBA16F; upload = GL_RGBA; type = GL_HALF_FLOAT; break;
    case Format::ASTC_4x4_UNORM:
      internal = GL_COMPRESSED_RGBA_ASTC_4x4_KHR; upload = 0; type = 0; break;
    case Format::ETC2_RGBA8_UNORM:
      internal = GL_COMPRESSED_RGBA8_ETC2_EAC; upload = 0; type = 0; break;
    default:  // 其余格式(顶点用居多)按 RGBA8 兜底
      internal = GL_RGBA8; upload = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
  }
}
/// 顶点属性分量数。
GLint toGLAttribSize(Format f) {
  switch (f) {
    case Format::R32G32_FLOAT:        return 2;
    case Format::R32G32B32_FLOAT:     return 3;
    case Format::R32G32B32A32_FLOAT:  return 4;
    case Format::RGBA8_UNORM:         return 4;
    default:                          return 3;
  }
}

class GLESDevice;

/**
 * GLES 命令缓冲实现：延迟回放模型。
 * GL 没有真正的"命令缓冲"：录制期把每个调用存为闭包(状态按值快照),
 * submit 时统一回放——与 Vulkan/Metal 的"录制/提交"两阶段语义对齐,
 * 错误检查收敛到 submit 边界,也为后续 pass 排序/去重打底。
 */
class GLESCommandBuffer final : public CommandBuffer {
public:
  explicit GLESCommandBuffer(GLESDevice* device) : device_(device) {}
  void beginRenderPass(TargetHandle target, const ClearColor& clear) override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) override;
  void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;
  /// 纹理绑定:slot N ↔ 纹理单元 N;sampler uniform 命名约定 texN(见 rhi_types.h)。
  void bindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) override;
  void draw(uint32_t vertexCount, uint32_t firstVertex) override;
  void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) override;
  void drawInstanced(uint32_t vertexCount, uint32_t firstVertex, uint32_t instanceCount,
                     uint32_t firstInstance) override;
  void drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset,
                            uint32_t instanceCount, uint32_t firstInstance) override;
  /// pass 结束:MSAA 目标录制一条 resolve blit(回放期执行);
  /// 非 MSAA 无动作(回放模型下清屏/绑定都已在 beginRenderPass 闭包内)。
  void endRenderPass() override {
    if (current_.samples <= 1) return;
    const GLuint src = current_.fbo, dst = current_.resolveFbo;
    const uint32_t w = current_.width, h = current_.height;
    cmds_.push_back([src, dst, w, h] {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
      glBlitFramebuffer(0, 0, GLint(w), GLint(h), 0, 0, GLint(w), GLint(h),
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    });
  }

  /// 回放全部已录命令并清空(submit 调用)。
  void replay() {
    for (auto& c : cmds_) c();
    cmds_.clear();
  }
  /// 丢弃未提交命令(acquire 时防残留)。
  void reset() { cmds_.clear(); }

private:
  /// draw 回放前按快照重建顶点属性指针(无 VAO 缓存)。
  static void applyVertexState(GLESDevice* device, const PipelineRec& pipe,
                               const std::array<BufferHandle, 8>& vbs,
                               const std::array<uint64_t, 8>& offs);

  GLESDevice* device_;
  TargetRec current_{};                 ///< 当前 render pass 的目标(录制期快照)
  PipelineRec pipeline_{};              ///< 当前绑定管线的缓存
  std::array<BufferHandle, 8> vertexBuffers_{};   ///< 顶点缓冲绑定状态
  std::array<uint64_t, 8> vertexOffsets_{};
  BufferHandle indexBuffer_;            ///< 索引缓冲绑定状态
  uint64_t indexOffset_ = 0;
  IndexType indexType_ = IndexType::UInt16;
  std::vector<std::function<void()>> cmds_;       ///< 已录制命令(submit 回放)
};

class GLESDevice final : public Device {
public:
  ~GLESDevice() override { shutdownEGL(); }
  bool init(const DeviceDesc&);
  Backend backend() const override { return Backend::GLES; }
  const DeviceCaps& caps() const override { return caps_; }
  /// ES3 对颜色附件保证 1..GL_MAX_SAMPLES 内的计数;按 caps 截断。
  uint32_t snapSampleCount(uint32_t requested) const override {
    return std::min(requested, caps_.get(Capability::msaa));
  }

  BufferHandle createBuffer(const BufferDesc& desc) override;
  void updateBuffer(BufferHandle buffer, const void* data, uint64_t size, uint64_t offset) override;
  void destroyBuffer(BufferHandle buffer) override;
  ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) override;
  void destroyShaderModule(ShaderModuleHandle module) override;
  PipelineHandle createPipeline(const PipelineDesc& desc) override;
  void destroyPipeline(PipelineHandle pipeline) override;
  TargetHandle createOffscreenTarget(const OffscreenTargetDesc& desc) override;
  void destroyTarget(TargetHandle target) override;
  void targetSize(TargetHandle target, uint32_t& outW, uint32_t& outH) const override;
  TextureHandle targetColorTexture(TargetHandle target) override;
  TextureHandle createTexture(const TextureDesc& desc) override;
  void destroyTexture(TextureHandle texture) override;
  void updateTexture(TextureHandle tex, uint32_t mipLevel, uint32_t face, const void* data,
                     uint64_t size) override;
  bool generateMipmaps(TextureHandle tex) override;
  SamplerHandle createSampler(const SamplerDesc& desc) override;
  void destroySampler(SamplerHandle sampler) override;
  bool readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) override;
  /// 取命令缓冲:丢弃可能未提交的残留命令,返回设备内唯一实例。
  CommandBuffer* acquireCommandBuffer() override {
    cmdBuf_.reset();
    return &cmdBuf_;
  }
  /// 提交:统一回放已录命令;错误检查收敛到此边界(debug 构建)。
  void submit(CommandBuffer*) override {
    cmdBuf_.replay();
#ifndef NDEBUG
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) RD_LOGE("rhi.gles", "GL 错误 0x%x @submit", err);
#endif
  }
  void waitIdle() override;
  /// 帧括号。GL 删除语义(glDelete* 标记后不再被引用时释放)使资源销毁天然安全,
  /// 退休队列在 GLES 上仅作形式统一:endFrame 立即确认本帧完成。
  void beginFrame() override { ++frameIndex_; }
  void endFrame() override { retire_.onFrameComplete(frameIndex_); }
  SwapChainHandle createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) override;
  void resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) override;
  TargetHandle acquireSwapChainTarget(SwapChainHandle swapChain) override;
  void present(SwapChainHandle swapChain) override;
  void destroySwapChain(SwapChainHandle swapChain) override;
  /// EGL config 固定请求 RGBA8，故 swapchain 颜色格式恒为 RGBA8_UNORM。
  Format swapChainColorFormat(SwapChainHandle) const override { return Format::RGBA8_UNORM; }

  // ---- CommandBuffer 访问 / EGL current 管理 ----
  /// 将指定 surface 置为当前（draw/read 共用同一 surface）。
  bool makeCurrent(EGLSurface surface);
  /// 确保 current 为离屏 pbuffer（资源管理与离屏渲染前调用）。
  void ensureOffscreenCurrent();
  GLuint buffer(BufferHandle h) const {
    auto it = buffers_.find(h);
    return it == buffers_.end() ? 0 : it->second.buffer;
  }
  const BufferRec* bufferRec(BufferHandle h) const {
    auto it = buffers_.find(h);
    return it == buffers_.end() ? nullptr : &it->second;
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
  const TextureRec* textureRec(TextureHandle h) const {
    auto it = textures_.find(h);
    return it == textures_.end() ? nullptr : &it->second;
  }
  GLuint samplerGl(SamplerHandle h) const {
    auto it = samplers_.find(h);
    return it == samplers_.end() ? 0 : it->second.sampler;
  }

private:
  void shutdownEGL();

  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLConfig config_ = nullptr;
  EGLSurface pbuffer_ = EGL_NO_SURFACE; // 1x1，离屏渲染时保活 context
  uint32_t nextId_ = 1;                 ///< 句柄分配器（1 起，0 留作无效）
  std::unordered_map<BufferHandle, BufferRec> buffers_;
  std::unordered_map<ShaderModuleHandle, ShaderRec> shaders_;
  std::unordered_map<PipelineHandle, PipelineRec> pipelines_;
  /// 管线(program)缓存:缓存持有 GL program,句柄表只持引用
  std::unordered_map<PipelineKey, GLuint, PipelineKeyHash> programCache_;
  std::unordered_map<TargetHandle, TargetRec> targets_;
  std::unordered_map<SwapChainHandle, SwapChainRec> swapChains_;
  std::unordered_map<TextureHandle, TextureRec> textures_;
  std::unordered_map<SamplerHandle, SamplerRec> samplers_;
  GLESCommandBuffer cmdBuf_{this};      ///< 设备内唯一命令缓冲（单线程模型）
  DeviceCaps caps_;                     ///< 能力表(init 内上报)
  RetireQueue retire_;                  ///< 资源退休队列(GL 删除语义下为形式统一)
  uint64_t frameIndex_ = 0;             ///< 当前帧序号
};

// ---------------- CommandBuffer 实现 ----------------

void GLESCommandBuffer::beginRenderPass(TargetHandle target, const ClearColor& clear) {
  TargetRec t;
  if (!device_->target(target, t)) return;
  current_ = t;  // 录制期快照(供状态查询)
  // GL 调用全部延迟到回放;surface 切换也在回放期(渲染线程)执行
  cmds_.emplace_back([this, t, clear] {
    if (t.isSwapchain) {
      device_->makeCurrent(t.surface);
    } else {
      device_->ensureOffscreenCurrent();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, GLsizei(t.width), GLsizei(t.height));
    if (t.depthOnly) {  // 纯深度目标:无颜色缓冲,只清深度
      const GLenum noDraw = GL_NONE;  // ES3 只有 glDrawBuffers(数组形式)
      glDrawBuffers(1, &noDraw);
      glReadBuffer(GL_NONE);
      glDepthMask(GL_TRUE);
      glClearDepthf(clear.depth);
      glClear(GL_DEPTH_BUFFER_BIT);
      return;
    }
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    if (t.hasDepth) {
      glDepthMask(GL_TRUE);  // 清深度前确保可写
      glClearDepthf(clear.depth);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    } else {
      glClear(GL_COLOR_BUFFER_BIT);
    }
  });
}

void GLESCommandBuffer::bindPipeline(PipelineHandle pipeline) {
  PipelineRec rec;
  if (!device_->pipeline(pipeline, rec)) return;
  pipeline_ = rec;  // 录制期缓存(draw 快照用)
  cmds_.emplace_back([rec] {
    glUseProgram(rec.program);
    // GL 剔除/混合是全局状态而非管线对象:绑管线时(回放期)落地
    if (rec.cull == CullMode::None) {
      glDisable(GL_CULL_FACE);
    } else {
      glEnable(GL_CULL_FACE);
      glCullFace(rec.cull == CullMode::Back ? GL_BACK : GL_FRONT);
      glFrontFace(GL_CCW);
    }
    if (rec.blend.enable) {
      glEnable(GL_BLEND);
      glBlendFuncSeparate(toGLBlendFactor(rec.blend.srcColor),
                          toGLBlendFactor(rec.blend.dstColor),
                          toGLBlendFactor(rec.blend.srcAlpha),
                          toGLBlendFactor(rec.blend.dstAlpha));
    } else {
      glDisable(GL_BLEND);
    }
    // 深度状态:管线驱动(须配 depth 目标;否则深度写入无附着点)
    if (rec.depthTest) {
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(rec.depthCompare == DepthCompareOp::Greater ? GL_GREATER : GL_LESS);
    } else {
      glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(rec.depthWrite ? GL_TRUE : GL_FALSE);
  });
}

/// 仅记录绑定状态（GL 顶点属性在 draw 回放的 applyVertexState 统一设置）。
void GLESCommandBuffer::bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) {
  vertexBuffers_[binding] = buffer;
  vertexOffsets_[binding] = offset;
}

/// GL 的索引缓冲在 draw 时绑定：这里仅记录。
void GLESCommandBuffer::bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) {
  indexBuffer_ = buffer;
  indexOffset_ = offset;
  indexType_ = type;
}

/// 绑定约定：uniform slot N ↔ GL_UNIFORM_BUFFER binding N;录制期解析 GL id,回放期落地。
void GLESCommandBuffer::bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                          uint64_t size) {
  const BufferRec* rec = device_->bufferRec(buffer);
  if (!rec) return;
  GLuint glBuf = rec->buffer;
  cmds_.emplace_back([slot, glBuf, offset, size] {
    glBindBufferRange(GL_UNIFORM_BUFFER, slot, glBuf, GLintptr(offset), GLsizeiptr(size));
  });
}

/// 纹理绑定:slot N ↔ 纹理单元 N + glBindSampler;sampler uniform texN 写入单元号。
void GLESCommandBuffer::bindTexture(uint32_t slot, TextureHandle texture,
                                    SamplerHandle sampler) {
  const TextureRec* tex = device_->textureRec(texture);
  GLuint sam = device_->samplerGl(sampler);
  if (!tex || sam == 0) return;
  TextureRec t = *tex;              // 快照(target/tex id)
  GLuint program = pipeline_.program;  // 当前管线(bindTexture 须在 bindPipeline 后)
  cmds_.emplace_back([slot, t, sam, program] {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(t.target, t.tex);
    glBindSampler(slot, sam);
    char name[8];
    snprintf(name, sizeof(name), "tex%u", slot);
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1i(loc, GLint(slot));
  });
}

void GLESCommandBuffer::draw(uint32_t vertexCount, uint32_t firstVertex) {
  PipelineRec pipe = pipeline_;                 // 快照
  auto vbs = vertexBuffers_;
  auto offs = vertexOffsets_;
  cmds_.emplace_back([this, pipe, vbs, offs, vertexCount, firstVertex] {
    applyVertexState(device_, pipe, vbs, offs);
    glDrawArrays(pipe.topology, GLint(firstVertex), GLsizei(vertexCount));
  });
}

/// 索引绘制。vertexOffset（baseVertex）当前未支持——参数被忽略
/// （ES3 无 glDrawElementsBaseVertex，需要时需用 ES3.2 或扩展）。
void GLESCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t) {
  PipelineRec pipe = pipeline_;
  auto vbs = vertexBuffers_;
  auto offs = vertexOffsets_;
  GLuint ib = device_->buffer(indexBuffer_);    // 录制期解析 GL id
  uint64_t ioff = indexOffset_;
  IndexType itype = indexType_;
  cmds_.emplace_back([this, pipe, vbs, offs, ib, ioff, itype, indexCount, firstIndex] {
    applyVertexState(device_, pipe, vbs, offs);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
    const bool u16 = itype == IndexType::UInt16;
    glDrawElements(pipe.topology, GLsizei(indexCount),
                   u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                   reinterpret_cast<const void*>(uintptr_t(ioff + firstIndex * (u16 ? 2 : 4))));
  });
}

void GLESCommandBuffer::drawInstanced(uint32_t vertexCount, uint32_t firstVertex,
                                      uint32_t instanceCount, uint32_t firstInstance) {
  if (firstInstance != 0)
    RD_LOGW("rhi.gles", "ES3.0 不支持 firstInstance,按 0 处理");
  PipelineRec pipe = pipeline_;
  auto vbs = vertexBuffers_;
  auto offs = vertexOffsets_;
  cmds_.emplace_back([this, pipe, vbs, offs, vertexCount, firstVertex, instanceCount] {
    applyVertexState(device_, pipe, vbs, offs);
    glDrawArraysInstanced(pipe.topology, GLint(firstVertex), GLsizei(vertexCount),
                          GLsizei(instanceCount));
  });
}

void GLESCommandBuffer::drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex,
                                             int32_t vertexOffset, uint32_t instanceCount,
                                             uint32_t firstInstance) {
  if (vertexOffset != 0 || firstInstance != 0)
    RD_LOGW("rhi.gles", "ES3.0 不支持 baseVertex/baseInstance,按 0 处理");
  PipelineRec pipe = pipeline_;
  auto vbs = vertexBuffers_;
  auto offs = vertexOffsets_;
  GLuint ib = device_->buffer(indexBuffer_);
  uint64_t ioff = indexOffset_;
  IndexType itype = indexType_;
  cmds_.emplace_back([this, pipe, vbs, offs, ib, ioff, itype, indexCount, firstIndex,
                      instanceCount] {
    applyVertexState(device_, pipe, vbs, offs);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
    const bool u16 = itype == IndexType::UInt16;
    glDrawElementsInstanced(pipe.topology, GLsizei(indexCount),
                            u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                            reinterpret_cast<const void*>(
                                uintptr_t(ioff + firstIndex * (u16 ? 2 : 4))),
                            GLsizei(instanceCount));
  });
}

/// 回放期按快照重建全部顶点属性指针;divisor 按 stepRate 显式覆盖(状态有粘性)。
void GLESCommandBuffer::applyVertexState(GLESDevice* device, const PipelineRec& pipe,
                                         const std::array<BufferHandle, 8>& vbs,
                                         const std::array<uint64_t, 8>& offs) {
  for (const auto& a : pipe.attribs) {
    glEnableVertexAttribArray(a.location);
    glBindBuffer(GL_ARRAY_BUFFER, device->buffer(vbs[a.binding]));
    // 在 bindings 中线性查找该属性所在槽位的 stride 与 stepRate
    uint32_t stride = 0;
    VertexStepRate rate = VertexStepRate::Vertex;
    for (const auto& b : pipe.bindings) {
      if (b.binding == a.binding) { stride = b.stride; rate = b.stepRate; }
    }
    glVertexAttribPointer(a.location, toGLAttribSize(a.format), toGLAttribType(a.format),
                          a.format == Format::RGBA8_UNORM ? GL_TRUE : GL_FALSE, GLsizei(stride),
                          reinterpret_cast<const void*>(uintptr_t(offs[a.binding] + a.offset)));
    glVertexAttribDivisor(a.location, rate == VertexStepRate::Instance ? 1 : 0);
  }
}

// ---------------- EGL 生命周期 ----------------

/// 初始化 EGL：取默认 display → 选 RGBA8/ES3 config → 建 ES3 context → 建 1x1 pbuffer
/// 并置为 current。任何一步失败记日志返回 false。
bool GLESDevice::init(const DeviceDesc&) {
  display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
    RD_LOGE("rhi.gles", "eglInitialize 失败");
    return false;
  }
  // config：ES3 可渲染 + 窗口/pbuffer 双用途 + RGBA8
  const EGLint configAttrs[] = {EGL_RENDERABLE_TYPE,
                                EGL_OPENGL_ES3_BIT,
                                EGL_SURFACE_TYPE,
                                EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                EGL_RED_SIZE,
                                8,
                                EGL_GREEN_SIZE,
                                8,
                                EGL_BLUE_SIZE,
                                8,
                                EGL_ALPHA_SIZE,
                                8,
                                EGL_NONE};
  EGLint numConfigs = 0;
  if (!eglChooseConfig(display_, configAttrs, &config_, 1, &numConfigs) || numConfigs < 1) {
    RD_LOGE("rhi.gles", "eglChooseConfig 失败");
    return false;
  }
  const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttrs);
  if (context_ == EGL_NO_CONTEXT) {
    RD_LOGE("rhi.gles", "eglCreateContext 失败");
    return false;
  }
  // 1x1 pbuffer：离屏渲染/资源管理期间保活 context（GL 命令必须有 current surface）
  const EGLint pbufAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  pbuffer_ = eglCreatePbufferSurface(display_, config_, pbufAttrs);
  if (pbuffer_ == EGL_NO_SURFACE) {
    RD_LOGE("rhi.gles", "pbuffer 创建失败");
    return false;
  }
  if (!makeCurrent(pbuffer_)) return false;

  // ---- 能力上报(ES3 核心能力 + 扩展位)----
  GLint maxTex = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
  caps_.set(Capability::max_texture_size, static_cast<uint32_t>(maxTex));
  caps_.set(Capability::max_texture_slots, 16);  // slot0..15(恰压 ES3 保证的 16 单元线)
  caps_.set(Capability::max_uniform_buffer_slots, 4);
  caps_.set(Capability::instancing, 1);  // ES3 核心
  GLint maxSamples = 0;
  glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
  caps_.set(Capability::msaa,
            static_cast<uint32_t>(maxSamples >= 4 ? 4 : maxSamples >= 2 ? 2 : 1));
  caps_.set(Capability::depth_texture, 1);  // ES3 核心
  caps_.set(Capability::cube_render_target, 1);
  caps_.set(Capability::generate_mipmap, 1);
  const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (exts && strstr(exts, "GL_EXT_texture_filter_anisotropic")) {
    GLfloat maxAniso = 0;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
    caps_.set(Capability::anisotropy, static_cast<uint32_t>(maxAniso));
  }
  caps_.set(Capability::texture_compression_etc2, 1);  // ES3 核心
  caps_.set(Capability::texture_compression_astc,
            exts && strstr(exts, "GL_KHR_texture_compression_astc_ldr") ? 1 : 0);
  caps_.set(Capability::hdr_render_target,
            exts && strstr(exts, "GL_EXT_color_buffer_half_float") ? 1 : 0);
  return true;
}

bool GLESDevice::makeCurrent(EGLSurface surface) {
  return eglMakeCurrent(display_, surface, surface, context_) == EGL_TRUE;
}

/// 若当前 surface 不是离屏 pbuffer 则切回（资源函数入口的统一前置动作）。
void GLESDevice::ensureOffscreenCurrent() {
  EGLSurface current = eglGetCurrentSurface(EGL_DRAW);
  if (current != pbuffer_) makeCurrent(pbuffer_);
}

/// 反初始化：先删除缓存的 program(须有 current context),再解绑销毁 EGL 资源。
/// 注意：窗口 surface 由 destroySwapChain 各自销毁，这里不处理。
void GLESDevice::shutdownEGL() {
  if (display_ != EGL_NO_DISPLAY) {
    makeCurrent(pbuffer_);  // 删除 GL 对象需要 current context
    for (auto& kv : programCache_) glDeleteProgram(kv.second);
    programCache_.clear();
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (pbuffer_ != EGL_NO_SURFACE) eglDestroySurface(display_, pbuffer_);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    eglTerminate(display_);
  }
}

// ---------------- 资源 ----------------

/// 创建缓冲：target 由 usage 推导（Index→ELEMENT_ARRAY，Uniform→UNIFORM，否则 ARRAY），
/// 之后 updateBuffer/destroyBuffer 都按记录的 target 绑定。
/// hostWrite → GL_DYNAMIC_DRAW 提示,否则 GL_STATIC_DRAW;hostRead 无直接支持。
BufferHandle GLESDevice::createBuffer(const BufferDesc& desc) {
  ensureOffscreenCurrent();
  GLuint buf = 0;
  glGenBuffers(1, &buf);
  GLenum target = GL_ARRAY_BUFFER;
  if (hasFlag(desc.usage, BufferUsage::Index)) target = GL_ELEMENT_ARRAY_BUFFER;
  if (hasFlag(desc.usage, BufferUsage::Uniform)) target = GL_UNIFORM_BUFFER;
  glBindBuffer(target, buf);
  GLenum hint = desc.hostWrite ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
  if (desc.hostRead)
    RD_LOGW("rhi.gles", "hostRead 缓冲在 GLES 无直接支持,按 hostWrite 处理");
  glBufferData(target, GLsizeiptr(desc.size), desc.data, hint);
  BufferHandle h(nextId_++);
  buffers_.emplace(h, BufferRec{buf, target, desc.size, desc.hostWrite || desc.hostRead});
  return h;
}

void GLESDevice::updateBuffer(BufferHandle buffer, const void* data, uint64_t size,
                              uint64_t offset) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  if (!it->second.hostVisible) {
    // GL 本身允许,但为与 Vulkan/Metal 行为一致(防写出不 portable 的代码)而拒绝
    RD_LOGE("rhi.gles", "updateBuffer 作用于非 hostWrite 缓冲(须 hostWrite=true 创建)");
    return;
  }
  ensureOffscreenCurrent();
  glBindBuffer(it->second.target, it->second.buffer);
  glBufferSubData(it->second.target, GLintptr(offset), GLsizeiptr(size), data);
}

void GLESDevice::destroyBuffer(BufferHandle buffer) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  ensureOffscreenCurrent();
  glDeleteBuffers(1, &it->second.buffer);
  buffers_.erase(it);
}

/// 编译 GLSL ES 源码（入口名约定 "main"）；失败记 info log 返回无效句柄。
ShaderModuleHandle GLESDevice::createShaderModule(const ShaderModuleDesc& desc) {
  ensureOffscreenCurrent();
  GLenum type = desc.stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
  GLuint shader = glCreateShader(type);
  const GLchar* src = reinterpret_cast<const GLchar*>(desc.code.data());
  const GLint len = GLint(desc.code.size());
  glShaderSource(shader, 1, &src, &len);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char info[1024];
    glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
    RD_LOGE("rhi.gles", "shader 编译失败: %s", info);
    glDeleteShader(shader);
    return {};
  }
  ShaderModuleHandle h(nextId_++);
  shaders_.emplace(h, ShaderRec{shader, desc.stage});
  return h;
}

void GLESDevice::destroyShaderModule(ShaderModuleHandle module) {
  auto it = shaders_.find(module);
  if (it == shaders_.end()) return;
  ensureOffscreenCurrent();
  glDeleteShader(it->second.shader);
  shaders_.erase(it);
}

PipelineHandle GLESDevice::createPipeline(const PipelineDesc& desc) {
  // MSAA 尚未实现，先拒绝而非静默错误（与 Metal/Vulkan 一致）。
  // 注意:depthTest/depthWrite 管线须配 depth=true 的渲染目标(反之亦然)。
  const uint32_t maxMsaa = caps_.get(Capability::msaa);
  if (desc.sampleCount == 0 || desc.sampleCount > maxMsaa) {
    RD_LOGE("rhi.gles", "createPipeline: sampleCount %u 超出 caps %u", desc.sampleCount, maxMsaa);
    return {};
  }
  auto vsIt = shaders_.find(desc.vertexShader);
  auto fsIt = shaders_.find(desc.fragmentShader);
  if (vsIt == shaders_.end() || fsIt == shaders_.end()) return {};

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
  if (auto it = programCache_.find(key); it != programCache_.end()) {
    PipelineRec cached;
    cached.program = it->second;
    cached.bindings = desc.vertexBindings;
    cached.attribs = desc.attributes;
    cached.topology = toGLTopology(desc.topology);
    cached.cull = desc.cullMode;
    cached.blend = desc.blend;
    cached.depthTest = desc.depthTest;
    cached.depthWrite = desc.depthWrite;
    cached.depthCompare = desc.depthCompare;
    PipelineHandle h(nextId_++);
    pipelines_.emplace(h, cached);
    return h;
  }

  ensureOffscreenCurrent();
  GLuint program = glCreateProgram();
  glAttachShader(program, vsIt->second.shader);
  glAttachShader(program, fsIt->second.shader);
  glLinkProgram(program);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char info[1024];
    glGetProgramInfoLog(program, sizeof(info), nullptr, info);
    RD_LOGE("rhi.gles", "program 链接失败: %s", info);
    glDeleteProgram(program);
    return {};
  }
  // uniform block 名 → slot 约定(绑定约定;cube 遗留 "UBO"→0;
  // PBR 系 FrameUBO→0,ItemUBO→1;不存在的块名返回 INVALID_INDEX 自动跳过)
  static const struct {
    const char* name;
    uint32_t slot;
  } kBlockTable[] = {
      {"UBO", 0}, {"FrameUBO", 0}, {"ItemUBO", 1}, {"BlitUBO", 0}, {"ShadowUBO", 0},
      {"LightUBO", 2}, {"JointUBO", 3},
  };
  for (const auto& b : kBlockTable) {
    GLuint blockIndex = glGetUniformBlockIndex(program, b.name);
    if (blockIndex != GL_INVALID_INDEX) glUniformBlockBinding(program, blockIndex, b.slot);
  }
  // 语义命名 sampler → slot 一次性写入(链接期;pbr_forward 系用描述性命名,
  // bindTexture 回放期的 tex%u 查表对它们无效——此前 GLES 上全部落单元 0,本表修复)。
  // texN 命名的简单 shader(blit/composite 等)仍由 bindTexture 回放期覆盖。
  static const struct {
    const char* name;
    uint32_t slot;
  } kSamplerTable[] = {
      {"texBaseColor", 0},     {"texMR", 1},           {"texNormal", 2},
      {"texEmissive", 3},      {"texOcclusion", 4},    {"texPrefilter", 5},
      {"texBrdfLut", 6},       {"texShadow", 7},       {"texShadowSpot", 8},
      {"texEquirect", 0},      {"texEnv", 0},          {"texClearcoat", 9},
      {"texClearcoatRough", 10}, {"texClearcoatNormal", 11}, {"texSheenColor", 12},
      {"texSheenRough", 13},   {"texSpecularColor", 14}, {"texSpecular", 15},
  };
  glUseProgram(program);
  for (const auto& s : kSamplerTable) {
    GLint loc = glGetUniformLocation(program, s.name);
    if (loc >= 0) glUniform1i(loc, GLint(s.slot));
  }
  programCache_.emplace(key, program);
  PipelineRec rec;
  rec.program = program;
  rec.bindings = desc.vertexBindings;
  rec.attribs = desc.attributes;
  rec.topology = toGLTopology(desc.topology);
  rec.cull = desc.cullMode;
  rec.blend = desc.blend;
  rec.depthTest = desc.depthTest;
  rec.depthWrite = desc.depthWrite;
  rec.depthCompare = desc.depthCompare;
  PipelineHandle h(nextId_++);
  pipelines_.emplace(h, rec);
  return h;
}

void GLESDevice::destroyPipeline(PipelineHandle pipeline) {
  // 只释放句柄引用;底层 program 由 programCache_ 持有(设备析构时统一删除)
  pipelines_.erase(pipeline);
}

/// 创建离屏目标。colorFromTexture 非空时挂载该纹理的 face/mip 为颜色附件;
/// 否则自建 RGBA8 颜色纹理挂到 FBO 的 COLOR_ATTACHMENT0（无深度附件）。
TargetHandle GLESDevice::createOffscreenTarget(const OffscreenTargetDesc& desc) {
  ensureOffscreenCurrent();
  if (desc.sampleCount > 1) {
    const uint32_t maxMsaa = caps_.get(Capability::msaa);
    if (desc.sampleCount > maxMsaa) {
      RD_LOGE("rhi.gles", "MSAA 目标 sampleCount %u 超出 caps %u", desc.sampleCount, maxMsaa);
      return {};
    }
    if (desc.colorFromTexture.valid()) {
      RD_LOGE("rhi.gles", "texture-backed 目标不支持 MSAA");
      return {};
    }
  }
  if (desc.depthFromTexture.valid()) {
    if (desc.sampleCount != 1) {
      RD_LOGE("rhi.gles", "depth-only 目标不支持 MSAA");
      return {};
    }
    auto dit = textures_.find(desc.depthFromTexture);
    if (dit == textures_.end() || dit->second.format != Format::D32_FLOAT) return {};
    TargetRec rec;
    rec.width = desc.width;
    rec.height = desc.height;
    rec.depthOnly = true;
    rec.hasDepth = true;
    rec.srcTexture = desc.depthFromTexture;
    glGenFramebuffers(1, &rec.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           dit->second.tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
      RD_LOGE("rhi.gles", "depth-only FBO 不完整");
      glDeleteFramebuffers(1, &rec.fbo);
      return {};
    }
    TargetHandle h(nextId_++);
    targets_.emplace(h, rec);
    return h;
  }
  if (desc.colorFromTexture.valid()) {
    auto it = textures_.find(desc.colorFromTexture);
    if (it == textures_.end()) return {};
    if (it->second.target == GL_TEXTURE_CUBE_MAP &&
        !caps_.supports(Capability::cube_render_target))
      return {};
    TargetRec rec;
    rec.width = desc.width;
    rec.height = desc.height;
    rec.textureBacked = true;
    rec.srcTexture = desc.colorFromTexture;
    glGenFramebuffers(1, &rec.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
    GLenum attachmentTarget = it->second.target == GL_TEXTURE_CUBE_MAP
                                  ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + desc.face
                                  : GL_TEXTURE_2D;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, attachmentTarget,
                           it->second.tex, GLint(desc.mipLevel));
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      RD_LOGE("rhi.gles", "texture-backed FBO 不完整");
      glDeleteFramebuffers(1, &rec.fbo);
      return {};
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    TargetHandle h(nextId_++);
    targets_.emplace(h, rec);
    return h;
  }
  TargetRec rec;
  rec.width = desc.width;
  rec.height = desc.height;
  rec.samples = desc.sampleCount;
  glGenTextures(1, &rec.colorTex);
  glBindTexture(GL_TEXTURE_2D, rec.colorTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(desc.width), GLsizei(desc.height), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  // FBO 附件纹理无需过滤参数之外的 mip 设置；Nearest 避免采样意外
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &rec.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
  if (desc.sampleCount > 1) {
    // MSAA:渲染到多重采样 renderbuffer;colorTex 挂 resolveFbo 作 resolve 目标
    glGenRenderbuffers(1, &rec.msaaColorRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rec.msaaColorRbo);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, GLsizei(desc.sampleCount), GL_RGBA8,
                                     GLsizei(desc.width), GLsizei(desc.height));
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              rec.msaaColorRbo);
    glGenFramebuffers(1, &rec.resolveFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rec.resolveFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rec.colorTex,
                          0);
    glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
  } else {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rec.colorTex,
                           0);
  }
  // 深度 renderbuffer(ES3 通用;可采样深度纹理归 P2 阴影;MSAA 同步采样数)
  if (desc.depth) {
    glGenRenderbuffers(1, &rec.depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rec.depthRbo);
    if (desc.sampleCount > 1) {
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, GLsizei(desc.sampleCount),
                                       GL_DEPTH_COMPONENT24, GLsizei(desc.width),
                                       GLsizei(desc.height));
    } else {
      glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, GLsizei(desc.width),
                            GLsizei(desc.height));
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              rec.depthRbo);
    rec.hasDepth = true;
  }
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    RD_LOGE("rhi.gles", "FBO 不完整");
    return {};
  }
  if (rec.resolveFbo) {  // resolve FBO 完整性一并检查
    glBindFramebuffer(GL_FRAMEBUFFER, rec.resolveFbo);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
      RD_LOGE("rhi.gles", "resolve FBO 不完整");
      return {};
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  // 注册可采样颜色句柄(纹理归 TargetRec 所有,句柄仅引用)
  {
    TextureRec trec;
    trec.tex = rec.colorTex;
    trec.target = GL_TEXTURE_2D;
    trec.width = desc.width;
    trec.height = desc.height;
    trec.mipLevels = 1;
    trec.format = desc.colorFormat;
    rec.colorHandle = TextureHandle(nextId_++);
    textures_.emplace(rec.colorHandle, trec);
  }
  TargetHandle h(nextId_++);
  targets_.emplace(h, rec);
  return h;
}

/// 销毁离屏目标；swapchain 帧目标（fbo=0）无 GL 资源，直接忽略;
/// texture-backed 只删 FBO(纹理归调用方)。
void GLESDevice::destroyTarget(TargetHandle target) {
  auto it = targets_.find(target);
  if (it == targets_.end() || it->second.isSwapchain) return;
  ensureOffscreenCurrent();
  if (it->second.colorHandle.valid()) textures_.erase(it->second.colorHandle);  // 只摘句柄
  glDeleteFramebuffers(1, &it->second.fbo);
  if (it->second.resolveFbo) glDeleteFramebuffers(1, &it->second.resolveFbo);
  if (it->second.msaaColorRbo) glDeleteRenderbuffers(1, &it->second.msaaColorRbo);
  if (it->second.colorTex) glDeleteTextures(1, &it->second.colorTex);
  if (it->second.depthRbo) glDeleteRenderbuffers(1, &it->second.depthRbo);
  targets_.erase(it);
}

void GLESDevice::targetSize(TargetHandle target, uint32_t& outW, uint32_t& outH) const {
  auto it = targets_.find(target);
  outW = it != targets_.end() ? it->second.width : 0;
  outH = it != targets_.end() ? it->second.height : 0;
}

TextureHandle GLESDevice::targetColorTexture(TargetHandle target) {
  auto it = targets_.find(target);
  if (it == targets_.end() || it->second.isSwapchain) return {};
  const TargetRec& t = it->second;
  if (t.depthOnly) return t.srcTexture;
  return t.textureBacked ? t.srcTexture : t.colorHandle;
}

/// 创建纹理:2D/Cube,逐 face/mip 上传(数据布局与其他后端一致:面 × mip 紧凑排列,
/// 面序 +X,-X,+Y,-Y,+Z,-Z)。无数据则分配空存储。前置校验与其他后端同语义。
TextureHandle GLESDevice::createTexture(const TextureDesc& desc) {
  if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0) return {};
  if (desc.type == TextureType::Cube && desc.width != desc.height) {
    RD_LOGE("rhi.gles", "createTexture: cube 纹理必须方形(%ux%u)", desc.width, desc.height);
    return {};
  }
  const uint32_t maxDim = desc.width > desc.height ? desc.width : desc.height;
  const uint32_t maxMip = uint32_t(std::floor(std::log2(double(maxDim)))) + 1;
  if (desc.mipLevels > maxMip) {
    RD_LOGE("rhi.gles", "createTexture: mipLevels %u 超出上限 %u", desc.mipLevels, maxMip);
    return {};
  }
  ensureOffscreenCurrent();
  TextureRec rec;
  rec.target = desc.type == TextureType::Cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
  rec.width = desc.width;
  rec.height = desc.height;
  rec.mipLevels = desc.mipLevels;
  rec.format = desc.format;
  glGenTextures(1, &rec.tex);
  glBindTexture(rec.target, rec.tex);
  if ((desc.format == Format::ASTC_4x4_UNORM &&
       !caps_.supports(Capability::texture_compression_astc)) ||
      (desc.format == Format::ETC2_RGBA8_UNORM &&
       !caps_.supports(Capability::texture_compression_etc2))) {
    RD_LOGE("rhi.gles", "createTexture: 压缩格式 %d 不受本后端支持", int(desc.format));
    glDeleteTextures(1, &rec.tex);
    return {};
  }
  GLint internal;
  GLenum uploadFmt, uploadType;
  toGLTexFormat(desc.format, internal, uploadFmt, uploadType);
  const bool compressed = uploadFmt == 0;  // 压缩格式:glCompressedTexImage2D
  const uint8_t* src = static_cast<const uint8_t*>(desc.data);
  uint64_t offset = 0;
  const uint32_t faces = desc.type == TextureType::Cube ? 6 : 1;
  for (uint32_t face = 0; face < faces; ++face) {
    uint32_t w = desc.width, hgt = desc.height;
    for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
      const uint64_t bytes = formatMipBytes(desc.format, w, hgt);
      if (src && offset + bytes > desc.dataSize) {
        RD_LOGE("rhi.gles", "createTexture: 数据越界(face %u mip %u)", face, mip);
        glDeleteTextures(1, &rec.tex);
        return {};
      }
      GLenum faceTarget = rec.target == GL_TEXTURE_CUBE_MAP
                              ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
      if (compressed) {
        glCompressedTexImage2D(faceTarget, GLint(mip), GLenum(internal), GLsizei(w),
                               GLsizei(hgt), 0, GLsizei(bytes),
                               src ? src + offset : nullptr);
      } else {
        glTexImage2D(faceTarget, GLint(mip), internal, GLsizei(w),
                     GLsizei(hgt), 0, uploadFmt, uploadType,
                     src ? src + offset : nullptr);
      }
      offset += bytes;
      w = w > 1 ? w / 2 : 1;
      hgt = hgt > 1 ? hgt / 2 : 1;
    }
  }
  glBindTexture(rec.target, 0);
  TextureHandle h(nextId_++);
  textures_.emplace(h, rec);
  return h;
}

void GLESDevice::destroyTexture(TextureHandle texture) {
  auto it = textures_.find(texture);
  if (it == textures_.end()) return;
  ensureOffscreenCurrent();
  glDeleteTextures(1, &it->second.tex);
  textures_.erase(it);
}

/// 创建采样器(ES3.0 sampler object):rhi 过滤/寻址枚举映射到 glSamplerParameteri。
SamplerHandle GLESDevice::createSampler(const SamplerDesc& desc) {
  ensureOffscreenCurrent();
  GLuint s = 0;
  glGenSamplers(1, &s);
  // minFilter 综合 min+mip(ES 枚举是联合形式):线性 min + 线性 mip → 三线性
  GLint minFilter = desc.minFilter == Filter::Linear
                        ? (desc.mipFilter == Filter::Linear ? GL_LINEAR_MIPMAP_LINEAR
                                                            : GL_LINEAR_MIPMAP_NEAREST)
                        : (desc.mipFilter == Filter::Linear ? GL_NEAREST_MIPMAP_LINEAR
                                                            : GL_NEAREST_MIPMAP_NEAREST);
  glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minFilter);
  glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER,
                      desc.magFilter == Filter::Linear ? GL_LINEAR : GL_NEAREST);
  auto toWrap = [](WrapMode m) {
    return m == WrapMode::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
  };
  glSamplerParameteri(s, GL_TEXTURE_WRAP_S, toWrap(desc.wrapU));
  glSamplerParameteri(s, GL_TEXTURE_WRAP_T, toWrap(desc.wrapV));
  glSamplerParameteri(s, GL_TEXTURE_WRAP_R, toWrap(desc.wrapW));
  if (desc.maxAnisotropy > 1 && caps_.supports(Capability::anisotropy)) {
    GLfloat a = GLfloat(std::min(desc.maxAnisotropy, caps_.get(Capability::anisotropy)));
    glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, a);
  }
  if (desc.compareEnable) {  // 深度比较(阴影采样)
    glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC, GL_LESS);
  }
  SamplerHandle h(nextId_++);
  samplers_.emplace(h, SamplerRec{s});
  return h;
}

void GLESDevice::destroySampler(SamplerHandle sampler) {
  auto it = samplers_.find(sampler);
  if (it == samplers_.end()) return;
  ensureOffscreenCurrent();
  glDeleteSamplers(1, &it->second.sampler);
  samplers_.erase(it);
}

/// 更新纹理子资源(face/mip 定位;数据为整层紧凑像素,格式与创建时一致)。
void GLESDevice::updateTexture(TextureHandle tex, uint32_t mipLevel, uint32_t face,
                               const void* data, uint64_t size) {
  auto it = textures_.find(tex);
  if (it == textures_.end() || !data) return;
  const TextureRec& tr = it->second;
  if (mipLevel >= tr.mipLevels || face >= (tr.target == GL_TEXTURE_CUBE_MAP ? 6u : 1u)) return;
  uint32_t w = tr.width >> mipLevel, hgt = tr.height >> mipLevel;
  if (w == 0) w = 1;
  if (hgt == 0) hgt = 1;
  const uint64_t need = formatMipBytes(tr.format, w, hgt);
  if (size < need) {
    RD_LOGE("rhi.gles", "updateTexture: 数据不足(需 %llu)", (unsigned long long)need);
    return;
  }
  ensureOffscreenCurrent();
  glBindTexture(tr.target, tr.tex);
  GLenum faceTarget =
      tr.target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
  GLint internal;
  GLenum uploadFmt, uploadType;
  toGLTexFormat(tr.format, internal, uploadFmt, uploadType);
  if (uploadFmt == 0) {  // 压缩格式
    glCompressedTexSubImage2D(faceTarget, GLint(mipLevel), 0, 0, GLsizei(w), GLsizei(hgt),
                              GLenum(internal), GLsizei(need), data);
  } else {
    glTexSubImage2D(faceTarget, GLint(mipLevel), 0, 0, GLsizei(w), GLsizei(hgt), uploadFmt,
                    uploadType, data);
  }
  glBindTexture(tr.target, 0);
}

/// 生成全部 mip 链(ES3 核心能力)。
bool GLESDevice::generateMipmaps(TextureHandle tex) {
  auto it = textures_.find(tex);
  if (it == textures_.end() || it->second.mipLevels < 2) return false;
  ensureOffscreenCurrent();
  glBindTexture(it->second.target, it->second.tex);
  glGenerateMipmap(it->second.target);
  glBindTexture(it->second.target, 0);
  return true;
}

/**
 * 读出目标像素。仅支持离屏目标；swapchain 默认帧缓冲不可读返回 false。
 * GL 原点左下 → 逐行翻转为顶向下，与 Metal/Vulkan 的 readback 输出一致。
 */
bool GLESDevice::readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) {
  auto it = targets_.find(target);
  if (it == targets_.end() || it->second.isSwapchain || it->second.depthOnly) return false;
  const TargetRec& t = it->second;
  const uint64_t rowBytes = uint64_t(t.width) * 4;
  if (outSize < rowBytes * t.height) return false;
  ensureOffscreenCurrent();
  glBindFramebuffer(GL_FRAMEBUFFER, t.samples > 1 ? t.resolveFbo : t.fbo);
  std::vector<uint8_t> raw(rowBytes * t.height);
  glReadPixels(0, 0, GLsizei(t.width), GLsizei(t.height), GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  // GL 原点左下 → 翻转为顶向下
  uint8_t* out = static_cast<uint8_t*>(outRGBA8);
  for (uint32_t y = 0; y < t.height; ++y) {
    memcpy(out + rowBytes * y, raw.data() + rowBytes * (t.height - 1 - y), rowBytes);
  }
  return true;
}

/// 阻塞直到 GL 管线排空;附加清空退休队列。
void GLESDevice::waitIdle() {
  glFinish();
  retire_.flushAll();
}

// ---------------- SwapChain ----------------

/**
 * 创建交换链：nativeWindow 为 ANativeWindow*（由平台层/RenderView 传入）。
 * 对 window 额外 acquire 一次引用，destroy 时 release——保证 surface 生命周期
 * 独立于调用方的窗口引用。
 */
SwapChainHandle GLESDevice::createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) {
  if (!nativeWindow || width == 0 || height == 0) return {};
  ANativeWindow* window = static_cast<ANativeWindow*>(nativeWindow);
  ANativeWindow_acquire(window);
  EGLSurface surface = eglCreateWindowSurface(display_, config_, window, nullptr);
  if (surface == EGL_NO_SURFACE) {
    RD_LOGE("rhi.gles", "eglCreateWindowSurface 失败");
    ANativeWindow_release(window);
    return {};
  }
  SwapChainRec rec;
  rec.window = window;
  rec.surface = surface;
  rec.width = width;
  rec.height = height;
  SwapChainHandle h(nextId_++);
  swapChains_.emplace(h, rec);
  return h;
}

/// 仅更新记录的尺寸：EGL 窗口 surface 随窗口自动调整，无需重建。
void GLESDevice::resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  it->second.width = width;
  it->second.height = height; // EGL surface 随窗口自动调整
}

/**
 * 获取当前帧渲染目标：切换 current 到窗口 surface（失败返回无效句柄，调用方跳帧），
 * 并返回代表默认帧缓冲（fbo=0）的复用 TargetHandle。
 */
TargetHandle GLESDevice::acquireSwapChainTarget(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return {};
  SwapChainRec& rec = it->second;
  if (!makeCurrent(rec.surface)) return {};
  if (!rec.currentTarget.valid()) {
    TargetRec t;
    t.isSwapchain = true;
    t.fbo = 0;
    rec.currentTarget = TargetHandle(nextId_++);
    targets_.emplace(rec.currentTarget, t);
  }
  TargetRec& t = targets_[rec.currentTarget];
  t.width = rec.width;
  t.height = rec.height;
  t.surface = rec.surface;
  return rec.currentTarget;
}

/// 上屏：eglSwapBuffers 交换前后台缓冲。
void GLESDevice::present(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  eglSwapBuffers(display_, it->second.surface);
}

void GLESDevice::destroySwapChain(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  if (it->second.currentTarget.valid()) targets_.erase(it->second.currentTarget);
  if (it->second.surface != EGL_NO_SURFACE) eglDestroySurface(display_, it->second.surface);
  if (it->second.window) ANativeWindow_release(it->second.window);
  swapChains_.erase(it);
}

} // namespace

/// 工厂入口（供 rhi_factory.cpp 调用）：构造并初始化，失败返回 nullptr。
std::unique_ptr<Device> createGLESDevice(const DeviceDesc& desc) {
  auto device = std::make_unique<GLESDevice>();
  if (!device->init(desc)) return nullptr;
  return device;
}

} // namespace rd
#endif // __ANDROID__
