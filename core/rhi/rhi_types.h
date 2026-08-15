/**
 * @file rhi_types.h
 * @brief RHI（渲染硬件接口）公共类型：资源句柄、枚举、描述体（Desc）。
 *
 * 三后端（Vulkan/Metal/GLES）共用同一套类型；所有 GPU 资源只经 rhi::Device
 * 创建/销毁，句柄为不透明 32 位值（0 无效），不拥有资源。
 *
 * 绑定约定（三后端统一，见文件底部 formatSize 上方的注释块）：
 *   uniform slot N ↔ Metal buffer(N) ↔ Vulkan set0 binding N ↔ GLES binding N；
 *   vertex binding N ↔ Metal buffer(N+1)；texture slot N 有固定偏移。
 */
#pragma once
#include "foundation/handle.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rd {

/// 渲染后端种类。
enum class Backend { Vulkan, Metal, GLES };

// ---- 资源句柄 ----
// 每个资源类型一个空 Tag 结构体，使句柄在编译期互不兼容（见 foundation/handle.h）。
struct BufferTag;
struct ShaderModuleTag;
struct PipelineTag;
struct TargetTag;
using BufferHandle = Handle<BufferTag>;              ///< GPU 缓冲（顶点/索引/uniform）
using ShaderModuleHandle = Handle<ShaderModuleTag>;  ///< 已编译着色器模块
using PipelineHandle = Handle<PipelineTag>;          ///< 渲染管线状态对象
using TargetHandle = Handle<TargetTag>;              ///< 渲染目标（离屏 FBO 或 swapchain 帧）
struct SwapChainTag;
using SwapChainHandle = Handle<SwapChainTag>;        ///< 上屏交换链
struct TextureTag;
struct SamplerTag;
using TextureHandle = Handle<TextureTag>;            ///< 纹理（2D/Cube）
using SamplerHandle = Handle<SamplerTag>;            ///< 采样器状态

// ---- 枚举 ----

/// 像素/顶点属性格式。
enum class Format {
  RGBA8_UNORM,         ///< 4×8bit 归一化 RGBA
  BGRA8_UNORM,         ///< 4×8bit 归一化 BGRA（Metal swapchain 颜色格式，layer 限制）
  R32G32_FLOAT,        ///< 2×float32（如 2D UV）
  R32G32B32_FLOAT,     ///< 3×float32（如 3D 位置）
  R32G32B32A32_FLOAT,  ///< 4×float32
  D32_FLOAT,           ///< 32bit 深度
};

/// 缓冲用途位标志（可按位或组合，如 Vertex|Index 不常见，Uniform 常单用）。
enum class BufferUsage : uint32_t {
  Vertex = 1u << 0,   ///< 顶点缓冲
  Index = 1u << 1,    ///< 索引缓冲
  Uniform = 1u << 2,  ///< uniform 缓冲
};

/// BufferUsage 位或运算，便于 usage = BufferUsage::Vertex | BufferUsage::Index。
constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
  return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
/// 查询 value 是否包含 flag 位。
constexpr bool hasFlag(BufferUsage value, BufferUsage flag) {
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

/// 索引缓冲元素类型。
enum class IndexType { UInt16, UInt32 };
/// 纹理维度/种类。
enum class TextureType { Texture2D, Cube };

/// 纹理用途位标志。
enum class TextureUsage : uint32_t {
  Sampled = 1u << 0,                 ///< 可被 shader 采样(默认)
  RenderTargetAttachment = 1u << 1,  ///< 可作为渲染目标附件(cube face/mip 渲染)
};
/// TextureUsage 位或运算。
constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
  return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
/// 查询 value 是否包含 flag 位。
constexpr bool hasFlag(TextureUsage value, TextureUsage flag) {
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
};/// 采样过滤方式。
enum class Filter { Nearest, Linear };
/// 寻址（wrap）模式。
enum class WrapMode { Clamp, Repeat };

/// 着色器阶段（P0/P1 仅顶点+片段两阶段）。
enum class ShaderStage { Vertex, Fragment };
/// 图元拓扑。
enum class PrimitiveTopology { TriangleList, TriangleStrip, LineList };

/// 深度比较方式(Reverse-Z 预留:Greater + clearDepth 0.0 + 投影 near/far 对调)。
enum class DepthCompareOp { Less, Greater };
/// 面剔除模式。
enum class CullMode { None, Front, Back };

/// 混合因子(最小完备集;PBR 透明与常见合成足够)。
enum class BlendFactor { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha };

/// 混合状态(默认关闭,经典 src-alpha 混合预设)。
struct BlendDesc {
  bool enable = false;
  BlendFactor srcColor = BlendFactor::SrcAlpha;
  BlendFactor dstColor = BlendFactor::OneMinusSrcAlpha;
  BlendFactor srcAlpha = BlendFactor::One;
  BlendFactor dstAlpha = BlendFactor::OneMinusDstAlpha;
};

// ---- 描述体（Desc）：创建资源时传入的参数包，均有默认值，按需覆盖 ----

/// 清屏值（RGBA + 深度;默认不透明黑 + 远平面 1.0）。
struct ClearColor {
  float r = 0, g = 0, b = 0, a = 1;
  float depth = 1.0f;  ///< 深度清屏值;Reverse-Z 场景传 0.0
};

/// 缓冲创建参数。
struct BufferDesc {
  uint64_t size = 0;                          ///< 字节数
  BufferUsage usage = BufferUsage::Vertex;    ///< 用途位标志
  /// CPU 频繁写(动态 uniform/顶点)。false → device-local(渲染最快),
  /// 初始数据经内部 staging 上传,且之后 updateBuffer 会被拒绝(记日志)。
  bool hostWrite = false;
  bool hostRead = false;                      ///< CPU 回读(staging/截图用途)
  const void* data = nullptr;                 ///< 非空则创建时随带上传(大小须等于 size)
};

/// 着色器模块创建参数。
struct ShaderModuleDesc {
  ShaderStage stage = ShaderStage::Vertex;
  std::vector<uint8_t> code;              ///< 按后端分别是 SPIR-V / metallib / GLSL ES 文本
  /// 入口函数名。约定：Metal(metallib) 为 "main0"（spirv-cross 约定）；
  /// SPIR-V/GLSL ES 为 "main"。默认 "main0" 与 Metal 对齐。
  std::string entryPoint = "main0";
};

/// 顶点缓冲步进频率:每顶点 / 每实例。
enum class VertexStepRate : uint32_t { Vertex, Instance };

/// 顶点缓冲绑定描述：一条 binding 槽位的步长与步进频率。
struct VertexBinding {
  uint32_t binding = 0;  ///< binding 槽位号（与 VertexAttribute::binding 对应）
  uint32_t stride = 0;   ///< 相邻顶点(或实例)间字节步长
  VertexStepRate stepRate = VertexStepRate::Vertex;  ///< Instance = 每实例步进
  bool operator==(const VertexBinding& o) const {
    return binding == o.binding && stride == o.stride && stepRate == o.stepRate;
  }
};

/// 顶点属性描述：shader location 与缓冲中偏移的映射。
struct VertexAttribute {
  uint32_t location = 0;                      ///< shader 中的 location
  Format format = Format::R32G32B32_FLOAT;    ///< 属性格式
  uint32_t offset = 0;                        ///< 属性在单个顶点内的字节偏移
  uint32_t binding = 0;                       ///< 来源 VertexBinding 槽位号
  bool operator==(const VertexAttribute& o) const {
    return location == o.location && format == o.format && offset == o.offset &&
           binding == o.binding;
  }
};

/// 渲染管线创建参数。
struct PipelineDesc {
  ShaderModuleHandle vertexShader;      ///< 顶点着色器（必须有效）
  ShaderModuleHandle fragmentShader;    ///< 片段着色器（必须有效）
  std::vector<VertexBinding> vertexBindings;  ///< 顶点缓冲槽位布局
  std::vector<VertexAttribute> attributes;    ///< 顶点属性布局
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;  ///< 图元拓扑
  CullMode cullMode = CullMode::None;   ///< 面剔除
  bool depthTest = false;               ///< 深度测试(须配 depth=true 的渲染目标)
  bool depthWrite = false;              ///< 深度写入
  DepthCompareOp depthCompare = DepthCompareOp::Less;  ///< 深度比较(Reverse-Z 用 Greater)
  BlendDesc blend;                      ///< 颜色混合(默认关闭)
  /// MSAA 采样数(预留;>1 需 caps().msaa 支持,当前后端拒绝非 1 值)。
  uint32_t sampleCount = 1;
  /// 颜色附件格式。渲染到 swapchain 时必须与 Device::swapChainColorFormat
  /// 返回的格式一致（Metal layer 限 BGRA8 系），否则后端可能创建失败。
  Format colorFormat = Format::RGBA8_UNORM;
};

/// 纹理创建参数。
struct TextureDesc {
  TextureType type = TextureType::Texture2D;  ///< 2D 或 Cube
  /// 用途位标志;作为渲染目标附件(cube face/mip 渲染)须带 RenderTargetAttachment。
  TextureUsage usage = TextureUsage::Sampled;
  uint32_t width = 0;                         ///< 像素宽（Cube 须等于 height）
  uint32_t height = 0;                        ///< 像素高
  Format format = Format::RGBA8_UNORM;        ///< 像素格式
  /// mip 级数。上限为 floor(log2(max(width,height)))+1，超限创建失败（返回无效句柄）。
  uint32_t mipLevels = 1;
  /// 初始数据，非空则创建时上传。
  /// 数据布局：2D = 逐 mip 紧凑排列；Cube = 6 面 × 逐 mip（面序 +X,-X,+Y,-Y,+Z,-Z）。
  const void* data = nullptr;
  uint64_t dataSize = 0;                      ///< data 的字节数（须与布局严格匹配）
};

/// 采样器创建参数（全部为固定功能状态，默认线性过滤+重复寻址）。
struct SamplerDesc {
  Filter minFilter = Filter::Linear;    ///< 缩小过滤
  Filter magFilter = Filter::Linear;    ///< 放大过滤
  Filter mipFilter = Filter::Linear;    ///< mip 间过滤
  WrapMode wrapU = WrapMode::Repeat;    ///< U 向寻址
  WrapMode wrapV = WrapMode::Repeat;    ///< V 向寻址
  WrapMode wrapW = WrapMode::Repeat;    ///< W 向寻址（cube 用）
  /// 各向异性等级;>1 且 caps().anisotropy 支持时启用(取两者较小值)。
  uint32_t maxAnisotropy = 1;
};

/// 离屏渲染目标创建参数。
struct OffscreenTargetDesc {
  uint32_t width = 0;                         ///< 像素宽
  uint32_t height = 0;                        ///< 像素高
  Format colorFormat = Format::RGBA8_UNORM;   ///< 自建颜色附件格式
  bool depth = false;                         ///< 附带深度附件(P1 引入实现)
  /// 非空:不自建颜色附件,改为挂载该纹理的指定子资源(face/mip)。
  /// 纹理创建时须带 TextureUsage::RenderTargetAttachment;cube 纹理
  /// 还需 caps() 支持 Capability::cube_render_target。
  TextureHandle colorFromTexture;
  uint32_t face = 0;                          ///< cube 面 0..5(+X,-X,+Y,-Y,+Z,-Z);2D 传 0
  uint32_t mipLevel = 0;                      ///< 挂载的 mip 级
};

/// 设备创建参数。
struct DeviceDesc {
  Backend backend = Backend::Vulkan;    ///< 选择后端；不可用（未编译/无设备）时工厂返回 nullptr
  bool enableValidation = false;        ///< 是否启用后端校验层（仅 Vulkan 有效，调试用）
};

// 绑定约定（三后端统一）：
//   uniform slot N(0..3) ↔ Metal buffer(N) ↔ Vulkan set0 binding N ↔ GLES binding N
//   texture slot N(0..7) ↔ Metal texture/sampler(N+4) ↔ Vulkan set0 binding(N+4) combined-image-sampler
//                        ↔ GLES 纹理单元 N（sampler uniform 名:texN）
//   vertex binding N ↔ Metal buffer(N+1)

/// 返回 Format 每像素/每元素的字节数（如 RGBA8_UNORM=4，R32G32B32_FLOAT=12）。
uint32_t formatSize(Format f);

inline uint32_t formatSize(Format f) {
  switch (f) {
    case Format::RGBA8_UNORM:
    case Format::BGRA8_UNORM:
    case Format::D32_FLOAT:
      return 4;
    case Format::R32G32_FLOAT:
      return 8;
    case Format::R32G32B32_FLOAT:
      return 12;
    case Format::R32G32B32A32_FLOAT:
      return 16;
  }
  return 0;
}

} // namespace rd
