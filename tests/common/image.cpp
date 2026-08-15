// image.h 的实现：PNG 读写委托 core/resource/image_codec；容差比较本地实现。
#include "common/image.h"
#include "resource/image_codec.h"
#include <cstdlib>

namespace rd::test {

bool savePNG(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
  return rd::saveImagePNG(path.c_str(), width, height, rgba);
}

Image loadPNG(const std::string& path) {
  Image img;
  auto d = rd::loadImageRGBA8(path.c_str());
  img.width = d.width;
  img.height = d.height;
  img.pixels = std::move(d.pixels);
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
