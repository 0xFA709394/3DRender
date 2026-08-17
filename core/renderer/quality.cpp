#include "renderer/quality.h"

namespace rd {

QualityPreset qualityPreset(QualityTier t) {
  switch (t) {
    case QualityTier::High: return {1.0f, 4, 256, 6, 4096, 2048, 1, 0};
    case QualityTier::Mid:  return {0.75f, 2, 128, 5, 2048, 1024, 1, 0};
    case QualityTier::Low:  return {0.5f, 1, 64, 4, 1024, 0, 0, 1};
  }
  return {1.0f, 1, 64, 5, 4096, 0, 0, 0};
}

QualityTier qualityFromCaps(uint32_t msaa, uint32_t maxTextureSize) {
  if (msaa >= 4 && maxTextureSize >= 8192) return QualityTier::High;
  if (msaa >= 2) return QualityTier::Mid;
  return QualityTier::Low;
}

} // namespace rd
