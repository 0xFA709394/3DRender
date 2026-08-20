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

SsimResult compareSSIM(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h,
                       double errTol) {
  auto luma = [](const uint8_t* p) {
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
  };
  const double c1 = (0.01 * 255) * (0.01 * 255);
  const double c2 = (0.03 * 255) * (0.03 * 255);
  double sum = 0;
  uint32_t blocks = 0;
  for (uint32_t by = 0; by < h; by += 8)
    for (uint32_t bx = 0; bx < w; bx += 8) {
      const uint32_t bw = std::min(8u, w - bx), bh = std::min(8u, h - by);
      const uint32_t n = bw * bh;
      double mux = 0, muy = 0;
      for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x) {
          mux += luma(a + ((by + y) * w + bx + x) * 4);
          muy += luma(b + ((by + y) * w + bx + x) * 4);
        }
      mux /= n;
      muy /= n;
      double vx = 0, vy = 0, cxy = 0;
      for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x) {
          const double lx = luma(a + ((by + y) * w + bx + x) * 4) - mux;
          const double ly = luma(b + ((by + y) * w + bx + x) * 4) - muy;
          vx += lx * lx;
          vy += ly * ly;
          cxy += lx * ly;
        }
      vx /= n;
      vy /= n;
      cxy /= n;
      sum += ((2 * mux * muy + c1) * (2 * cxy + c2)) /
             ((mux * mux + muy * muy + c1) * (vx + vy + c2));
      ++blocks;
    }
  const double err = 1.0 - sum / double(blocks);
  return {err <= errTol, err};
}

} // namespace rd::test
