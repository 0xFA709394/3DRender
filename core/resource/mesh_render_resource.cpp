// MeshRenderResource 的实现:ModelAsset → device-local GPU 资源上传与释放。
// 占位纹理约定:baseColor/MR/occlusion 缺省绑白(采样 1,与 factor 相乘恒等);
// emissive 缺省绑黑(0 贡献);normal 缺省绑平面法线(0.5,0.5,1)。
#include "resource/mesh_render_resource.h"
#include "foundation/log.h"
#include <cstring>

namespace rd {
namespace {

TextureHandle makePixel(Device& dev, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t px[4] = {r, g, b, 255};
  rd::TextureDesc td;
  td.width = 1;
  td.height = 1;
  td.data = px;
  td.dataSize = 4;
  return dev.createTexture(td);
}

TextureHandle uploadOr(Device& dev, const ImageData& img, TextureHandle fallback) {
  if (img.width == 0) return fallback;
  rd::TextureDesc td;
  td.width = img.width;
  td.height = img.height;
  td.format = img.format;        // 压缩格式直通(RHI caps 门控)
  td.mipLevels = img.mipLevels;
  td.data = img.pixels.data();
  td.dataSize = uint64_t(img.pixels.size());
  auto tex = dev.createTexture(td);
  if (!tex.valid() && img.format != Format::RGBA8_UNORM) {
    // 压缩格式目标由 caps 推导,正常不会失败;失败时回退占位避免整个模型加载失败
    RD_LOGE("resource", "压缩纹理创建失败(format=%d),回退占位", int(img.format));
    return fallback;
  }
  return tex;
}

/// float → half(IEEE 754 位技巧;morph 增量 RGBA16F 打包用)
uint16_t toHalf(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  const int32_t exp = int32_t((x >> 23) & 0xffu) - 127 + 15;
  const uint32_t mant = (x >> 13) & 0x3ffu;
  if (exp <= 0) return uint16_t(sign);            // 下溢 → 0(保号)
  if (exp >= 31) return uint16_t(sign | 0x7c00u); // 上溢 → inf
  return uint16_t(sign | (uint32_t(exp) << 10) | mant);
}

} // namespace

std::shared_ptr<MeshRenderResource> MeshRenderResource::upload(Device& dev,
                                                               const ModelAsset& model) {
  if (!model.valid()) return nullptr;
  auto res = std::shared_ptr<MeshRenderResource>(new MeshRenderResource());
  memcpy(res->boundingCenter_, model.boundingCenter, sizeof(res->boundingCenter_));
  res->boundingRadius_ = model.boundingRadius;
  res->fallbackWhite_ = makePixel(dev, 255, 255, 255);
  res->fallbackBlack_ = makePixel(dev, 0, 0, 0);
  res->fallbackNormal_ = makePixel(dev, 128, 128, 255);
  res->sampler_ = dev.createSampler({});
  if (!res->fallbackWhite_.valid() || !res->fallbackBlack_.valid() ||
      !res->fallbackNormal_.valid() || !res->sampler_.valid()) {
    res->destroy(dev);
    return nullptr;
  }

  for (auto m : model.meshes) {  // 按值拷贝:上传后清空 CPU 侧像素
    MeshGpuData g;
    g.vbo = dev.createBuffer({uint64_t(m.vertices.size() * 4), BufferUsage::Vertex, false,
                              false, m.vertices.data()});
    g.ibo = dev.createBuffer({uint64_t(m.indices.size()), BufferUsage::Index, false, false,
                              m.indices.data()});
    g.indexType = m.indexType;
    g.indexCount = m.indexCount;
    g.skinned = m.skinned;
    g.baseColorTex = uploadOr(dev, m.material.baseColor, res->fallbackWhite_);
    g.mrTex = uploadOr(dev, m.material.metallicRoughness, res->fallbackWhite_);
    g.normalTex = uploadOr(dev, m.material.normal, res->fallbackNormal_);
    g.emissiveTex = uploadOr(dev, m.material.emissive, res->fallbackBlack_);
    g.occlusionTex = uploadOr(dev, m.material.occlusion, res->fallbackWhite_);
    g.clearcoatTex = uploadOr(dev, m.material.clearcoat, res->fallbackWhite_);
    g.clearcoatRoughTex = uploadOr(dev, m.material.clearcoatRough, res->fallbackWhite_);
    g.clearcoatNormalTex = uploadOr(dev, m.material.clearcoatNormal, res->fallbackNormal_);
    g.sheenColorTex = uploadOr(dev, m.material.sheenColor, res->fallbackWhite_);
    g.sheenRoughTex = uploadOr(dev, m.material.sheenRough, res->fallbackWhite_);
    g.specularColorTex = uploadOr(dev, m.material.specularColorTex, res->fallbackWhite_);
    g.specularTex = uploadOr(dev, m.material.specularTex, res->fallbackWhite_);
    g.transmissionTex = uploadOr(dev, m.material.transmissionTex, res->fallbackWhite_);
    g.thicknessTex = uploadOr(dev, m.material.thicknessTex, res->fallbackWhite_);
    // morph 增量纹理:RGBA16F,宽=顶点数,高=目标数×2(行 t*2=POS、t*2+1=NORMAL)
    if (m.morph && !m.morphWeights.empty() && m.morphPosDeltas.size() >=
                                                    m.morphWeights.size() * 3) {
      const uint32_t targets = uint32_t(m.morphWeights.size());
      const uint32_t verts =
          uint32_t(m.morphPosDeltas.size() / (size_t(targets) * 3));
      if (verts > 0 && m.morphPosDeltas.size() == size_t(targets) * verts * 3) {
        std::vector<uint16_t> px(size_t(verts) * targets * 2 * 4);
        for (uint32_t t = 0; t < targets; ++t)
          for (uint32_t v = 0; v < verts; ++v) {
            const size_t posBase = (size_t(verts) * (t * 2 + 0) + v) * 4;
            const size_t nrmBase = (size_t(verts) * (t * 2 + 1) + v) * 4;
            for (int c = 0; c < 3; ++c) {
              px[posBase + c] = toHalf(m.morphPosDeltas[(size_t(t) * verts + v) * 3 + c]);
              px[nrmBase + c] =
                  toHalf(m.morphNormalDeltas.size() == m.morphPosDeltas.size()
                             ? m.morphNormalDeltas[(size_t(t) * verts + v) * 3 + c]
                             : 0.0f);
            }
            px[posBase + 3] = 0;
            px[nrmBase + 3] = 0;
          }
        TextureDesc mtd;
        mtd.width = verts;
        mtd.height = targets * 2;
        mtd.format = Format::R16G16B16A16_FLOAT;
        mtd.usage = TextureUsage::Sampled;
        mtd.data = px.data();
        mtd.dataSize = px.size() * 2;
        g.morphTex = dev.createTexture(mtd);
        g.morph = g.morphTex.valid();
        if (!g.morph)
          RD_LOGW("resource", "morph 纹理创建失败,该 mesh 按无 morph 渲染");
      }
      g.morphWeights = m.morphWeights;
    }
    // CPU 像素已上传,清空以省内存(材质 factor 等元数据保留)
    m.material.baseColor.pixels.clear();
    m.material.metallicRoughness.pixels.clear();
    m.material.normal.pixels.clear();
    m.material.emissive.pixels.clear();
    m.material.occlusion.pixels.clear();
    m.material.clearcoat.pixels.clear();
    m.material.clearcoatRough.pixels.clear();
    m.material.clearcoatNormal.pixels.clear();
    m.material.sheenColor.pixels.clear();
    m.material.sheenRough.pixels.clear();
    m.material.specularColorTex.pixels.clear();
    m.material.specularTex.pixels.clear();
    m.material.transmissionTex.pixels.clear();
    m.material.thicknessTex.pixels.clear();
    g.material = std::move(m.material);
    if (!g.vbo.valid() || !g.ibo.valid() || !g.baseColorTex.valid() || !g.mrTex.valid() ||
        !g.normalTex.valid() || !g.emissiveTex.valid() || !g.occlusionTex.valid() ||
        !g.clearcoatTex.valid() || !g.clearcoatRoughTex.valid() ||
        !g.clearcoatNormalTex.valid() || !g.sheenColorTex.valid() ||
        !g.sheenRoughTex.valid() || !g.specularColorTex.valid() || !g.specularTex.valid() ||
        !g.transmissionTex.valid() || !g.thicknessTex.valid()) {
      res->destroy(dev);
      return nullptr;
    }
    res->meshes_.push_back(g);
  }
  return res;
}

void MeshRenderResource::destroy(Device& dev) {
  for (auto& g : meshes_) {
    if (g.vbo.valid()) dev.destroyBuffer(g.vbo);
    if (g.ibo.valid()) dev.destroyBuffer(g.ibo);
    for (TextureHandle t : {g.baseColorTex, g.mrTex, g.normalTex, g.emissiveTex,
                            g.occlusionTex, g.clearcoatTex, g.clearcoatRoughTex,
                            g.clearcoatNormalTex, g.sheenColorTex, g.sheenRoughTex,
                            g.specularColorTex, g.specularTex, g.transmissionTex,
                            g.thicknessTex, g.morphTex}) {
      // 只销毁非占位纹理(占位纹理由本对象统一销毁)
      if (t.valid() && t != fallbackWhite_ && t != fallbackBlack_ && t != fallbackNormal_)
        dev.destroyTexture(t);
    }
  }
  meshes_.clear();
  if (fallbackWhite_.valid()) dev.destroyTexture(fallbackWhite_);
  if (fallbackBlack_.valid()) dev.destroyTexture(fallbackBlack_);
  if (fallbackNormal_.valid()) dev.destroyTexture(fallbackNormal_);
  if (sampler_.valid()) dev.destroySampler(sampler_);
  fallbackWhite_ = {};
  fallbackBlack_ = {};
  fallbackNormal_ = {};
  sampler_ = {};
}

} // namespace rd
