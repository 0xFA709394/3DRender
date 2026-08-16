#include "common/ktx2_gen.h"
#include "foundation/log.h"
#include <ktx.h>
#include <cstdio>
#include <vector>

namespace rd::test {

std::vector<uint8_t> makeTestKtx2(uint32_t size) {
  std::vector<uint8_t> out;
  ktxTextureCreateInfo ci{};
  ci.vkFormat = 37;  // VK_FORMAT_R8G8B8A8_UNORM(避免引 vulkan 头)
  ci.baseWidth = size;
  ci.baseHeight = size;
  ci.baseDepth = 1;
  ci.numDimensions = 2;
  ci.numLevels = 2;  // size 与 size/2 两级(第 2 级数据也要填)
  ci.numLayers = 1;
  ci.numFaces = 1;
  ci.isArray = KTX_FALSE;
  ci.generateMipmaps = KTX_FALSE;
  ktxTexture2* tex = nullptr;
  if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) != KTX_SUCCESS)
    return out;
  // 确定性图案:level0 = 红绿棋盘,level1 = 纯蓝
  for (uint32_t level = 0; level < 2; ++level) {
    const uint32_t w = size >> level;
    std::vector<uint8_t> px(size_t(w) * w * 4);
    for (uint32_t y = 0; y < w; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
        if (level == 0) {
          p[0] = ((x ^ y) & 1) ? 220 : 30;  // 红通道棋盘
          p[1] = ((x ^ y) & 1) ? 30 : 220;  // 绿通道反相
        } else {
          p[2] = 255;
        }
        p[3] = 255;
      }
    if (ktxTexture_SetImageFromMemory(ktxTexture(tex), level, 0, 0, px.data(),
                                      px.size()) != KTX_SUCCESS) {
      ktxTexture_Destroy(ktxTexture(tex));
      return out;
    }
  }
  // ETC1S 压缩(确定性编码;若库配置不含编码器,此处失败 → 退化为未压缩 ktx2)
  if (ktxTexture2_CompressBasis(tex, 0) != KTX_SUCCESS) {
    RD_LOGW("test.ktx2", "CompressBasis 不可用,退化为未压缩 ktx2");
  }
  ktx_uint8_t* bytes = nullptr;
  ktx_size_t len = 0;
  if (ktxTexture_WriteToMemory(ktxTexture(tex), &bytes, &len) == KTX_SUCCESS)
    out.assign(bytes, bytes + len);
  ktxTexture_Destroy(ktxTexture(tex));
  return out;
}

bool writeTestKtx2(const char* path, uint32_t size) {
  const auto bytes = makeTestKtx2(size);
  if (bytes.empty()) return false;
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  fclose(f);
  return ok;
}

} // namespace rd::test
