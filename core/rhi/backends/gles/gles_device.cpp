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
// - 纹理/采样器：createTexture/createSampler 尚未实现（返回无效句柄）；
//   CommandBuffer::bindTexture 为空操作。
// ============================================================================
#include "gles_device.h"

#if defined(__ANDROID__)
#include "foundation/log.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rd {
namespace {

// ---- 资源记录（句柄表的 value）----

/// 缓冲：记录 GL 对象 + 创建时推导的绑定 target（updateBuffer/destroy 时要用）+ 大小。
struct BufferRec { GLuint buffer = 0; GLenum target = GL_ARRAY_BUFFER; uint64_t size = 0; };
struct ShaderRec { GLuint shader = 0; ShaderStage stage; };
/// 管线：GL program + 顶点布局缓存（draw 时重建属性指针用）+ 拓扑/剔除状态。
struct PipelineRec {
  GLuint program = 0;
  std::vector<VertexBinding> bindings;
  std::vector<VertexAttribute> attribs;
  GLenum topology = GL_TRIANGLES;
  CullMode cull = CullMode::None;
};
/// 渲染目标：离屏 FBO 或 swapchain 默认帧缓冲（fbo=0）。
struct TargetRec {
  GLuint fbo = 0;       // 0 = 默认帧缓冲（swapchain）
  GLuint colorTex = 0;  // 离屏纹理
  uint32_t width = 0, height = 0;
  bool isSwapchain = false;
  EGLSurface surface = EGL_NO_SURFACE; // swapchain target 专用：beginRenderPass 时切回窗口 surface
};
struct SwapChainRec {
  ANativeWindow* window = nullptr;      ///< 持有引用（create 时 acquire，destroy 时 release）
  EGLSurface surface = EGL_NO_SURFACE;
  uint32_t width = 0, height = 0;
  TargetHandle currentTarget;           ///< swapchain 帧目标（fbo=0，跨帧复用句柄值）
};

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
 * GLES 命令缓冲实现。
 * GL 是立即执行模型，没有真正的"命令缓冲"：这里把绑定调用记录为局部状态，
 * draw 时统一落地（顶点属性指针在 draw 前由 applyVertexState 重建）。
 */
class GLESCommandBuffer final : public CommandBuffer {
public:
  explicit GLESCommandBuffer(GLESDevice* device) : device_(device) {}
  void beginRenderPass(TargetHandle target, const ClearColor& clear) override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) override;
  void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;
  /// 纹理绑定：GLES 后端纹理功能尚未实现，暂为空操作（其他后端正常）。
  void bindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) override {}
  void draw(uint32_t vertexCount, uint32_t firstVertex) override;
  void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) override;
  /// GL 立即执行模型无需结束 pass 的动作，空实现。
  void endRenderPass() override {}

private:
  void applyVertexState();  ///< draw 前按当前管线布局重建顶点属性指针

  GLESDevice* device_;
  TargetRec current_{};                 ///< 当前 render pass 的目标
  PipelineRec pipeline_{};              ///< 当前绑定管线的缓存
  BufferHandle vertexBuffers_[8];       ///< 顶点缓冲绑定状态（binding 槽位上限 8）
  uint64_t vertexOffsets_[8] = {};
  BufferHandle indexBuffer_;            ///< 索引缓冲绑定状态（drawIndexed 时使用）
  uint64_t indexOffset_ = 0;
  IndexType indexType_ = IndexType::UInt16;
};

class GLESDevice final : public Device {
public:
  ~GLESDevice() override { shutdownEGL(); }
  bool init(const DeviceDesc&);
  Backend backend() const override { return Backend::GLES; }

  BufferHandle createBuffer(const BufferDesc& desc) override;
  void updateBuffer(BufferHandle buffer, const void* data, uint64_t size, uint64_t offset) override;
  void destroyBuffer(BufferHandle buffer) override;
  ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) override;
  void destroyShaderModule(ShaderModuleHandle module) override;
  PipelineHandle createPipeline(const PipelineDesc& desc) override;
  void destroyPipeline(PipelineHandle pipeline) override;
  TargetHandle createOffscreenTarget(const OffscreenTargetDesc& desc) override;
  void destroyTarget(TargetHandle target) override;
  // 纹理/采样器：P1 后续任务实现（当前 Metal/Vulkan 后端已支持，GLES 暂返回无效句柄）
  TextureHandle createTexture(const TextureDesc&) override { return {}; }
  void destroyTexture(TextureHandle) override {}
  SamplerHandle createSampler(const SamplerDesc&) override { return {}; }
  void destroySampler(SamplerHandle) override {}
  bool readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) override;
  /// GL 立即执行模型无需获取/提交命令：直接返回设备内唯一命令缓冲。
  CommandBuffer* acquireCommandBuffer() override { return &cmdBuf_; }
  void submit(CommandBuffer* cmd) override;
  void waitIdle() override;
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
  std::unordered_map<TargetHandle, TargetRec> targets_;
  std::unordered_map<SwapChainHandle, SwapChainRec> swapChains_;
  GLESCommandBuffer cmdBuf_{this};      ///< 设备内唯一命令缓冲（单线程模型）
};

// ---------------- CommandBuffer 实现 ----------------

void GLESCommandBuffer::beginRenderPass(TargetHandle target, const ClearColor& clear) {
  TargetRec t;
  if (!device_->target(target, t)) return;
  current_ = t;
  // 每帧开始都确保 current 正确：updateBuffer 等操作可能已把 current 切到 pbuffer
  if (t.isSwapchain) {
    device_->makeCurrent(t.surface);
  } else {
    device_->ensureOffscreenCurrent();
  }
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  glViewport(0, 0, GLsizei(t.width), GLsizei(t.height));
  glClearColor(clear.r, clear.g, clear.b, clear.a);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_DEPTH_TEST);  // 深度测试未实现（P1 引入），固定关闭
}

void GLESCommandBuffer::bindPipeline(PipelineHandle pipeline) {
  PipelineRec rec;
  if (!device_->pipeline(pipeline, rec)) return;
  pipeline_ = rec;
  glUseProgram(rec.program);
  // GL 剔除是全局状态而非管线对象：绑管线时立即落地。
  if (rec.cull == CullMode::None) {
    glDisable(GL_CULL_FACE);
  } else {
    glEnable(GL_CULL_FACE);
    glCullFace(rec.cull == CullMode::Back ? GL_BACK : GL_FRONT);
    glFrontFace(GL_CCW);
  }
}

/// 仅记录绑定状态（GL 顶点属性在 draw 前的 applyVertexState 统一设置）。
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

/// 绑定约定：uniform slot N ↔ GL_UNIFORM_BUFFER binding N。
void GLESCommandBuffer::bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                          uint64_t size) {
  const BufferRec* rec = device_->bufferRec(buffer);
  if (!rec) return;
  glBindBufferRange(GL_UNIFORM_BUFFER, slot, rec->buffer, GLintptr(offset), GLsizeiptr(size));
}

void GLESCommandBuffer::draw(uint32_t vertexCount, uint32_t firstVertex) {
  applyVertexState();
  glDrawArrays(pipeline_.topology, GLint(firstVertex), GLsizei(vertexCount));
}

/// 索引绘制。vertexOffset（baseVertex）当前未支持——参数被忽略
/// （ES3 无 glDrawElementsBaseVertex，需要时需用 ES3.2 或扩展）。
void GLESCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t) {
  applyVertexState();
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, device_->buffer(indexBuffer_));
  const bool u16 = indexType_ == IndexType::UInt16;
  glDrawElements(pipeline_.topology, GLsizei(indexCount),
                 u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                 reinterpret_cast<const void*>(uintptr_t(indexOffset_ + firstIndex * (u16 ? 2 : 4))));
}

/// 每次 draw 前按当前管线布局重建全部顶点属性指针（P0 无 VAO 缓存的最简实现）。
void GLESCommandBuffer::applyVertexState() {
  for (const auto& a : pipeline_.attribs) {
    glEnableVertexAttribArray(a.location);
    glBindBuffer(GL_ARRAY_BUFFER, device_->buffer(vertexBuffers_[a.binding]));
    // 在 bindings 中线性查找该属性所在槽位的 stride
    uint32_t stride = 0;
    for (const auto& b : pipeline_.bindings) {
      if (b.binding == a.binding) stride = b.stride;
    }
    glVertexAttribPointer(a.location, toGLAttribSize(a.format), toGLAttribType(a.format),
                          a.format == Format::RGBA8_UNORM ? GL_TRUE : GL_FALSE, GLsizei(stride),
                          reinterpret_cast<const void*>(uintptr_t(vertexOffsets_[a.binding] + a.offset)));
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
  return makeCurrent(pbuffer_);
}

bool GLESDevice::makeCurrent(EGLSurface surface) {
  return eglMakeCurrent(display_, surface, surface, context_) == EGL_TRUE;
}

/// 若当前 surface 不是离屏 pbuffer 则切回（资源函数入口的统一前置动作）。
void GLESDevice::ensureOffscreenCurrent() {
  EGLSurface current = eglGetCurrentSurface(EGL_DRAW);
  if (current != pbuffer_) makeCurrent(pbuffer_);
}

/// 反初始化：解绑 context，销毁 pbuffer/context，终止 display。
/// 注意：窗口 surface 由 destroySwapChain 各自销毁，这里不处理。
void GLESDevice::shutdownEGL() {
  if (display_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (pbuffer_ != EGL_NO_SURFACE) eglDestroySurface(display_, pbuffer_);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    eglTerminate(display_);
  }
}

// ---------------- 资源 ----------------

/// 创建缓冲：target 由 usage 推导（Index→ELEMENT_ARRAY，Uniform→UNIFORM，否则 ARRAY），
/// 之后 updateBuffer/destroyBuffer 都按记录的 target 绑定。
BufferHandle GLESDevice::createBuffer(const BufferDesc& desc) {
  ensureOffscreenCurrent();
  GLuint buf = 0;
  glGenBuffers(1, &buf);
  GLenum target = GL_ARRAY_BUFFER;
  if (hasFlag(desc.usage, BufferUsage::Index)) target = GL_ELEMENT_ARRAY_BUFFER;
  if (hasFlag(desc.usage, BufferUsage::Uniform)) target = GL_UNIFORM_BUFFER;
  glBindBuffer(target, buf);
  glBufferData(target, GLsizeiptr(desc.size), desc.data, GL_STATIC_DRAW);
  BufferHandle h(nextId_++);
  buffers_.emplace(h, BufferRec{buf, target, desc.size});
  return h;
}

void GLESDevice::updateBuffer(BufferHandle buffer, const void* data, uint64_t size,
                              uint64_t offset) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
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
  // 深度附件尚未实现，先拒绝而非静默错误（与 Metal 后端一致）。
  if (desc.depthTest) {
    RD_LOGE("rhi.gles", "P0-2 不支持 depthTest（P1 引入深度附件）");
    return {};
  }
  auto vsIt = shaders_.find(desc.vertexShader);
  auto fsIt = shaders_.find(desc.fragmentShader);
  if (vsIt == shaders_.end() || fsIt == shaders_.end()) return {};
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
  // uniform block "UBO" ↔ slot 0（P0 硬编码；P1 由反射 JSON 驱动）
  GLuint blockIndex = glGetUniformBlockIndex(program, "UBO");
  if (blockIndex != GL_INVALID_INDEX) {
    glUniformBlockBinding(program, blockIndex, 0);
  }
  PipelineRec rec;
  rec.program = program;
  rec.bindings = desc.vertexBindings;
  rec.attribs = desc.attributes;
  rec.topology = toGLTopology(desc.topology);
  rec.cull = desc.cullMode;
  PipelineHandle h(nextId_++);
  pipelines_.emplace(h, rec);
  return h;
}

void GLESDevice::destroyPipeline(PipelineHandle pipeline) {
  auto it = pipelines_.find(pipeline);
  if (it == pipelines_.end()) return;
  ensureOffscreenCurrent();
  glDeleteProgram(it->second.program);
  pipelines_.erase(it);
}

/// 创建离屏目标：RGBA8 颜色纹理挂到 FBO 的 COLOR_ATTACHMENT0（无深度附件）。
TargetHandle GLESDevice::createOffscreenTarget(const OffscreenTargetDesc& desc) {
  ensureOffscreenCurrent();
  TargetRec rec;
  rec.width = desc.width;
  rec.height = desc.height;
  glGenTextures(1, &rec.colorTex);
  glBindTexture(GL_TEXTURE_2D, rec.colorTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(desc.width), GLsizei(desc.height), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  // FBO 附件纹理无需过滤参数之外的 mip 设置；Nearest 避免采样意外
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &rec.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rec.colorTex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    RD_LOGE("rhi.gles", "FBO 不完整");
    return {};
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  TargetHandle h(nextId_++);
  targets_.emplace(h, rec);
  return h;
}

/// 销毁离屏目标；swapchain 帧目标（fbo=0）无 GL 资源，直接忽略。
void GLESDevice::destroyTarget(TargetHandle target) {
  auto it = targets_.find(target);
  if (it == targets_.end() || it->second.isSwapchain) return;
  ensureOffscreenCurrent();
  glDeleteFramebuffers(1, &it->second.fbo);
  glDeleteTextures(1, &it->second.colorTex);
  targets_.erase(it);
}

/**
 * 读出目标像素。仅支持离屏目标；swapchain 默认帧缓冲不可读返回 false。
 * GL 原点左下 → 逐行翻转为顶向下，与 Metal/Vulkan 的 readback 输出一致。
 */
bool GLESDevice::readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) {
  auto it = targets_.find(target);
  if (it == targets_.end() || it->second.isSwapchain) return false;
  const TargetRec& t = it->second;
  const uint64_t rowBytes = uint64_t(t.width) * 4;
  if (outSize < rowBytes * t.height) return false;
  ensureOffscreenCurrent();
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
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

/// GL 立即执行模型：命令在录制时已落地，submit 无需动作。
void GLESDevice::submit(CommandBuffer*) {}
/// 阻塞直到 GL 管线排空。
void GLESDevice::waitIdle() { glFinish(); }

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
