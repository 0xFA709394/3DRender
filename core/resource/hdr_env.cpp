// HDR 环境加载:stb_image float 解码(stb 实现宏在 image_codec.cpp,此处仅调 API)。
#include "resource/hdr_env.h"
#include "foundation/log.h"
#include <stb_image.h>

namespace rd {

bool loadHdrEnv(const char* path, HdrEnv& out) {
  if (!path || !path[0]) return false;
  if (!stbi_is_hdr(path)) {
    RD_LOGW("resource.hdr", "非 HDR 文件: %s", path);
    return false;
  }
  int w = 0, h = 0, n = 0;
  float* data = stbi_loadf(path, &w, &h, &n, 4);  // 强制 RGBA
  if (!data) {
    RD_LOGW("resource.hdr", "解码失败: %s", path);
    return false;
  }
  out.width = uint32_t(w);
  out.height = uint32_t(h);
  out.pixels.assign(data, data + size_t(w) * h * 4);
  stbi_image_free(data);
  RD_LOGI("resource.hdr", "HDR 环境加载 %ux%u: %s", out.width, out.height, path);
  return true;
}

} // namespace rd
