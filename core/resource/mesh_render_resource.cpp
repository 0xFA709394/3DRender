// MeshRenderResource 的实现:ModelAsset → device-local GPU 资源上传与释放。
// 占位纹理约定:baseColor/MR/occlusion 缺省绑白(采样 1,与 factor 相乘恒等);
// emissive 缺省绑黑(0 贡献);normal 缺省绑平面法线(0.5,0.5,1)。
#include "resource/mesh_render_resource.h"
#include "foundation/log.h"

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
  td.data = img.pixels.data();
  td.dataSize = uint64_t(img.pixels.size());
  return dev.createTexture(td);
}

} // namespace

std::shared_ptr<MeshRenderResource> MeshRenderResource::upload(Device& dev,
                                                               const ModelAsset& model) {
  if (!model.valid()) return nullptr;
  auto res = std::shared_ptr<MeshRenderResource>(new MeshRenderResource());
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
    g.baseColorTex = uploadOr(dev, m.material.baseColor, res->fallbackWhite_);
    g.mrTex = uploadOr(dev, m.material.metallicRoughness, res->fallbackWhite_);
    g.normalTex = uploadOr(dev, m.material.normal, res->fallbackNormal_);
    g.emissiveTex = uploadOr(dev, m.material.emissive, res->fallbackBlack_);
    g.occlusionTex = uploadOr(dev, m.material.occlusion, res->fallbackWhite_);
    // CPU 像素已上传,清空以省内存(材质 factor 等元数据保留)
    m.material.baseColor.pixels.clear();
    m.material.metallicRoughness.pixels.clear();
    m.material.normal.pixels.clear();
    m.material.emissive.pixels.clear();
    m.material.occlusion.pixels.clear();
    g.material = std::move(m.material);
    if (!g.vbo.valid() || !g.ibo.valid() || !g.baseColorTex.valid() || !g.mrTex.valid() ||
        !g.normalTex.valid() || !g.emissiveTex.valid() || !g.occlusionTex.valid()) {
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
                            g.occlusionTex}) {
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
