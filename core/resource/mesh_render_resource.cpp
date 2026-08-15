// MeshRenderResource 的实现:ModelAsset → device-local GPU 资源上传与释放。
#include "resource/mesh_render_resource.h"
#include "foundation/log.h"

namespace rd {

std::shared_ptr<MeshRenderResource> MeshRenderResource::upload(Device& dev,
                                                               const ModelAsset& model) {
  if (!model.valid()) return nullptr;
  auto res = std::shared_ptr<MeshRenderResource>(new MeshRenderResource());
  // 1x1 中性灰占位:无纹理 mesh 的统一绑定对象(优雅降级)
  const uint8_t gray[4] = {128, 128, 128, 255};
  rd::TextureDesc fd;
  fd.width = 1;
  fd.height = 1;
  fd.data = gray;
  fd.dataSize = 4;
  res->fallbackTex_ = dev.createTexture(fd);
  res->sampler_ = dev.createSampler({});
  if (!res->fallbackTex_.valid() || !res->sampler_.valid()) {
    res->destroy(dev);
    return nullptr;
  }

  for (const auto& m : model.meshes) {
    MeshGpuData g;
    g.vbo = dev.createBuffer({uint64_t(m.vertices.size() * 4), BufferUsage::Vertex, false,
                              false, m.vertices.data()});
    g.ibo = dev.createBuffer({uint64_t(m.indices.size()), BufferUsage::Index, false, false,
                              m.indices.data()});
    g.indexType = m.indexType;
    g.indexCount = m.indexCount;
    if (m.baseColor.width > 0) {
      rd::TextureDesc td;
      td.width = m.baseColor.width;
      td.height = m.baseColor.height;
      td.data = m.baseColor.pixels.data();
      td.dataSize = uint64_t(m.baseColor.pixels.size());
      g.baseColorTex = dev.createTexture(td);
    } else {
      g.baseColorTex = res->fallbackTex_;
      RD_LOGW("resource.gltf", "mesh %s 无 baseColor 纹理,用 1x1 灰占位", m.name.c_str());
    }
    if (!g.vbo.valid() || !g.ibo.valid() || !g.baseColorTex.valid()) {
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
    if (g.baseColorTex.valid() && g.baseColorTex != fallbackTex_)
      dev.destroyTexture(g.baseColorTex);
  }
  meshes_.clear();
  if (fallbackTex_.valid()) dev.destroyTexture(fallbackTex_);
  if (sampler_.valid()) dev.destroySampler(sampler_);
  fallbackTex_ = {};
  sampler_ = {};
}

} // namespace rd
