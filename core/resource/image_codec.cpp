// image_codec 的实现:stb 单头库（实现宏只能在一个编译单元定义，本文件即该单元）。
#include "resource/image_codec.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace rd {

ImageData decodeImageRGBA8(const void* data, uint64_t size) {
  ImageData img;
  int w = 0, h = 0, channels = 0;
  uint8_t* decoded = stbi_load_from_memory(static_cast<const stbi_uc*>(data),
                                           static_cast<int>(size), &w, &h, &channels, 4);
  if (!decoded) return img;
  img.width = static_cast<uint32_t>(w);
  img.height = static_cast<uint32_t>(h);
  img.pixels.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
  stbi_image_free(decoded);
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
