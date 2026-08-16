// ktx2_codec 的实现:libktx 内存解码 + BasisU 转码,输出逐 mip 紧凑数据。
#include "resource/ktx2_codec.h"
#include "foundation/log.h"
#include <ktx.h>
#include <algorithm>
#include <cstring>

namespace rd {

bool isKtx2(const void* data, uint64_t size) {
  static const uint8_t kMagic[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                     0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  return size >= 12 && memcmp(data, kMagic, 12) == 0;
}

Ktx2Target pickTranscodeTarget(bool astc, bool etc2) {
  if (astc) return Ktx2Target::Astc;
  if (etc2) return Ktx2Target::Etc2;
  return Ktx2Target::Rgba32;
}

namespace {
/// 非 supercompressed KTX2 的 vkFormat → rhi Format;不支持返回 false。
bool mapVkFormat(uint32_t vkFormat, Format& out) {
  switch (vkFormat) {
    case 37: out = Format::RGBA8_UNORM; return true;        // VK_FORMAT_R8G8B8A8_UNORM
    case 43: out = Format::RGBA8_UNORM; return true;        // VK_FORMAT_R8G8B8A8_SRGB(按 UNORM 读)
    case 157: out = Format::ASTC_4x4_UNORM; return true;    // VK_FORMAT_ASTC_4x4_UNORM_BLOCK
    case 147: out = Format::ETC2_RGBA8_UNORM; return true;  // VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK
    default: return false;
  }
}
} // namespace

Ktx2Image decodeKtx2(const void* data, uint64_t size, Ktx2Target target) {
  Ktx2Image out;
  if (!isKtx2(data, size)) {
    RD_LOGE("resource.ktx2", "非 KTX2 魔数");
    return out;
  }
  ktxTexture2* tex = nullptr;
  if (ktxTexture2_CreateFromMemory(static_cast<const ktx_uint8_t*>(data), size,
                                   KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                   &tex) != KTX_SUCCESS) {
    RD_LOGE("resource.ktx2", "KTX2 解析失败");
    return out;
  }
  if (ktxTexture2_NeedsTranscoding(tex)) {
    ktx_transcode_fmt_e fmt;
    switch (target) {
      case Ktx2Target::Astc:
        fmt = KTX_TTF_ASTC_4x4_RGBA;
        out.format = Format::ASTC_4x4_UNORM;
        break;
      case Ktx2Target::Etc2:
        fmt = KTX_TTF_ETC2_RGBA;
        out.format = Format::ETC2_RGBA8_UNORM;
        break;
      default:
        fmt = KTX_TTF_RGBA32;
        out.format = Format::RGBA8_UNORM;
        break;
    }
    if (ktxTexture2_TranscodeBasis(tex, fmt, 0) != KTX_SUCCESS) {
      RD_LOGE("resource.ktx2", "BasisU 转码失败(target=%d)", int(target));
      ktxTexture_Destroy(ktxTexture(tex));
      return Ktx2Image{};
    }
  } else if (!mapVkFormat(tex->vkFormat, out.format)) {
    RD_LOGE("resource.ktx2", "不支持的 vkFormat %u", tex->vkFormat);
    ktxTexture_Destroy(ktxTexture(tex));
    return Ktx2Image{};
  }
  out.width = tex->baseWidth;
  out.height = tex->baseHeight;
  out.mipLevels = std::max(1u, tex->numLevels);
  // 逐 mip 紧凑收集
  uint64_t total = 0;
  for (uint32_t m = 0; m < out.mipLevels; ++m) {
    total += formatMipBytes(out.format, std::max(1u, out.width >> m),
                            std::max(1u, out.height >> m));
  }
  out.data.resize(total);
  uint64_t off = 0;
  const uint8_t* base = ktxTexture_GetData(ktxTexture(tex));
  for (uint32_t m = 0; m < out.mipLevels; ++m) {
    const uint32_t mw = std::max(1u, out.width >> m);
    const uint32_t mh = std::max(1u, out.height >> m);
    const uint64_t bytes = formatMipBytes(out.format, mw, mh);
    size_t imgOff = 0;
    ktxTexture_GetImageOffset(ktxTexture(tex), m, 0, 0, &imgOff);
    memcpy(out.data.data() + off, base + imgOff, bytes);
    off += bytes;
  }
  ktxTexture_Destroy(ktxTexture(tex));
  return out;
}

} // namespace rd
