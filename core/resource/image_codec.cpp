// image_codec 的实现:stb 单头库（实现宏只能在一个编译单元定义，本文件即该单元）。
#include "resource/image_codec.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#include <algorithm>

namespace rd {

ImageData decodeImageRGBA8(const void* data, uint64_t size, uint32_t maxDim) {
  ImageData img;
  int w = 0, h = 0, channels = 0;
  uint8_t* decoded = stbi_load_from_memory(static_cast<const stbi_uc*>(data),
                                           static_cast<int>(size), &w, &h, &channels, 4);
  if (!decoded) return img;
  img.width = static_cast<uint32_t>(w);
  img.height = static_cast<uint32_t>(h);
  img.pixels.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
  stbi_image_free(decoded);
  // 尺寸上限:等比降采样(画质分级纹理旋钮)
  if (maxDim > 0 && (img.width > maxDim || img.height > maxDim)) {
    const float s = float(maxDim) / float(std::max(img.width, img.height));
    const uint32_t nw = std::max(1u, uint32_t(img.width * s));
    const uint32_t nh = std::max(1u, uint32_t(img.height * s));
    std::vector<uint8_t> dst(size_t(nw) * nh * 4);
    stbir_resize_uint8_srgb(img.pixels.data(), img.width, img.height, 0, dst.data(), nw, nh,
                            0, STBIR_RGBA);
    img.width = nw;
    img.height = nh;
    img.pixels = std::move(dst);
  }
  return img;
}

ImageData loadImageRGBA8(const char* path) {
  ImageData img;
  int w = 0, h = 0, channels = 0;
  uint8_t* data = stbi_load(path, &w, &h, &channels, 4);
  if (!data) return img;
  img.width = static_cast<uint32_t>(w);
  img.height = static_cast<uint32_t>(h);
  img.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);
  return img;
}

bool saveImagePNG(const char* path, uint32_t w, uint32_t h, const uint8_t* rgba) {
  return stbi_write_png(path, static_cast<int>(w), static_cast<int>(h), 4, rgba,
                        static_cast<int>(w * 4)) != 0;
}

} // namespace rd
