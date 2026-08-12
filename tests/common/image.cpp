// image.h 的实现：stb 单头库的 PNG 读写 + 逐像素容差比较。
#include "common/image.h"

// stb 的 implementation 宏只能在一个编译单元定义（本文件即该单元）
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdlib>

namespace rd::test {

bool savePNG(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
  return stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                        rgba, static_cast<int>(width * 4)) != 0;
}

Image loadPNG(const std::string& path) {
  Image img;
  int w = 0, h = 0, channels = 0;
  // req_comp=4：无论源图通道数都强制转 RGBA8
  uint8_t* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
  if (!data) return img;
  img.width = static_cast<uint32_t>(w);
  img.height = static_cast<uint32_t>(h);
  img.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);
  return img;
}

CompareResult compareRGBA8(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h,
                           int channelTol, double ratioTol) {
  CompareResult r;
  uint64_t diffPixels = 0;
  const uint64_t total = static_cast<uint64_t>(w) * h;
  for (uint64_t i = 0; i < total; ++i) {
    // 像素级差值 = 四通道差的最大值
    int maxDiff = 0;
    for (int c = 0; c < 4; ++c) {
      int d = std::abs(static_cast<int>(a[i * 4 + c]) - static_cast<int>(b[i * 4 + c]));
      maxDiff = std::max(maxDiff, d);
    }
    r.maxChannelDiff = std::max(r.maxChannelDiff, maxDiff);
    if (maxDiff > channelTol) ++diffPixels;
  }
  r.diffRatio = total ? static_cast<double>(diffPixels) / static_cast<double>(total) : 0.0;
  r.pass = r.diffRatio <= ratioTol;
  return r;
}

} // namespace rd::test
