/**
 * @file quality.h
 * @brief 画质分级:高/中/低三档预设 + caps 启发式默认档。
 * 旋钮:渲染分辨率缩放 / MSAA 档位 / IBL prefilter 尺寸 / 纹理解码尺寸上限。
 */
#pragma once
#include <cstdint>

namespace rd {

enum class QualityTier : uint32_t { High = 0, Mid = 1, Low = 2 };

struct QualityPreset {
  float renderScale;          ///< 场景目标分辨率缩放(0.5/0.75/1.0)
  uint32_t msaa;              ///< 场景目标 MSAA 采样数(超 caps 时 clamp)
  uint32_t iblPrefilterSize;  ///< prefilter cube 边长
  uint32_t iblPrefilterMips;  ///< prefilter mip 级数(roughness 粒度)
  uint32_t maxTextureDim;     ///< 纹理解码尺寸上限(等比降采样)
  uint32_t shadowMapSize;     ///< 阴影贴图边长(0=关阴影)
  uint32_t postEnabled;       ///< HDR 后处理链(Bloom+ACES);caps 缺失自动回落
  uint32_t fxaaEnabled;       ///< FXAA(与 MSAA 互斥,仅 msaa==1 时生效)
  uint32_t extMaterials;      ///< KHR 扩展材质四件套(clearcoat/sheen/specular/ior)
  uint32_t transmission;      ///< KHR transmission/volume(两段 pass 透射/体积吸收)
};

/// 三档预设表。
QualityPreset qualityPreset(QualityTier t);
/// caps 启发式默认档:msaa≥4 且 maxTextureSize≥8192 → High;msaa≥2 → Mid;否则 Low。
QualityTier qualityFromCaps(uint32_t msaa, uint32_t maxTextureSize);

} // namespace rd
