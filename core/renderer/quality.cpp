#include "renderer/quality.h"

namespace rd {

QualityPreset qualityPreset(QualityTier t) {
  switch (t) {
    case QualityTier::High: return {1.0f, 4, 256, 6, 4096};
    case QualityTier::Mid:  return {0.75f, 2, 128, 5, 2048};
    case QualityTier::Low:  return {0.5f, 1, 64, 4, 1024};
  }
  return {1.0f, 1, 64, 5, 4096};
}

QualityTier qualityFromCaps(uint32_t msaa, uint32_t maxTextureSize) {
  if (msaa >= 4 && maxTextureSize >= 8192) return QualityTier::High;
  if (msaa >= 2) return QualityTier::Mid;
  return QualityTier::Low;
}

} // namespace rd
