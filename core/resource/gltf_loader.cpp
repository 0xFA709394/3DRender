// gltf_loader 的实现:cgltf 解析 → 固定交错布局顶点 + 自适应索引 + 内嵌纹理解码。
#include "resource/gltf_loader.h"
#include "foundation/log.h"

// cgltf 是单头库:实现宏只能在一个编译单元定义(本文件即该单元)
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <cstring>

namespace rd {
namespace {

/// 从 attributes 中按类型读 float 分量到 dst(每顶点 compCount 个,步长 dstStride);
/// 属性缺失保持调用方预置的 0。
void readFloatAttr(const cgltf_attribute* attrs, cgltf_size attrCount,
                   cgltf_attribute_type type, cgltf_size vertexCount, uint32_t compCount,
                   float* dst, uint32_t dstStride) {
  const cgltf_attribute* found = nullptr;
  for (cgltf_size i = 0; i < attrCount; ++i) {
    if (attrs[i].type == type) {
      found = &attrs[i];
      break;
    }
  }
  if (!found) return;
  for (cgltf_size v = 0; v < vertexCount; ++v) {
    float tmp[4] = {0, 0, 0, 0};
    cgltf_accessor_read_float(found->data, v, tmp, compCount);
    float* out = dst + size_t(v) * dstStride;
    for (uint32_t c = 0; c < compCount; ++c) out[c] = tmp[c];
  }
}

/// 解码 primitive 的 baseColor 纹理(仅内嵌 buffer view 路径;URI 外链记警告跳过)。
ImageData decodeBaseColor(const cgltf_primitive& prim) {
  ImageData img;
  const cgltf_material* mat = prim.material;
  if (!mat || !mat->has_pbr_metallic_roughness) return img;
  const cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
  if (!tex || !tex->image) return img;
  const cgltf_image* image = tex->image;
  if (image->buffer_view) {
    const cgltf_buffer_view* bv = image->buffer_view;
    const auto* bytes = static_cast<const uint8_t*>(bv->buffer->data);
    img = decodeImageRGBA8(bytes + bv->offset, uint64_t(bv->size));
  } else if (image->uri) {
    RD_LOGW("resource.gltf", "外链纹理 URI 暂不支持(2c KTX2 一起处理): %s", image->uri);
  }
  return img;
}

} // namespace

ModelAsset loadGltf(const char* path) {
  ModelAsset model;
  cgltf_options options{};
  cgltf_data* data = nullptr;
  if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
    RD_LOGE("resource.gltf", "解析失败: %s", path);
    return model;
  }
  if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
    RD_LOGE("resource.gltf", "缓冲加载失败: %s", path);
    cgltf_free(data);
    return model;
  }

  for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
    const cgltf_mesh& mesh = data->meshes[mi];
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
      const cgltf_primitive& prim = mesh.primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles || !prim.indices) continue;

      MeshData out;
      out.name = mesh.name ? mesh.name : "mesh";
      const cgltf_accessor* pos = nullptr;
      for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
        if (prim.attributes[ai].type == cgltf_attribute_type_position) {
          pos = prim.attributes[ai].data;
          break;
        }
      }
      if (!pos) continue;
      const cgltf_size vertexCount = pos->count;
      out.vertices.resize(size_t(vertexCount) * 8, 0.0f);  // 8 float/顶点,缺省 0
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_position,
                    vertexCount, 3, out.vertices.data(), 8);
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_normal,
                    vertexCount, 3, out.vertices.data() + 3, 8);
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_texcoord,
                    vertexCount, 2, out.vertices.data() + 6, 8);

      // 索引:源 u32 或顶点数超 u16 范围 → UInt32
      const cgltf_size indexCount = prim.indices->count;
      const bool needU32 = prim.indices->component_type == cgltf_component_type_r_32u ||
                           vertexCount > 65535;
      out.indexType = needU32 ? IndexType::UInt32 : IndexType::UInt16;
      out.indexCount = uint32_t(indexCount);
      if (needU32) {
        out.indices.resize(size_t(indexCount) * 4);
        auto* dst = reinterpret_cast<uint32_t*>(out.indices.data());
        for (cgltf_size i = 0; i < indexCount; ++i)
          dst[i] = uint32_t(cgltf_accessor_read_index(prim.indices, i));
      } else {
        out.indices.resize(size_t(indexCount) * 2);
        auto* dst = reinterpret_cast<uint16_t*>(out.indices.data());
        for (cgltf_size i = 0; i < indexCount; ++i)
          dst[i] = uint16_t(cgltf_accessor_read_index(prim.indices, i));
      }

      out.baseColor = decodeBaseColor(prim);
      model.meshes.push_back(std::move(out));
    }
  }
  cgltf_free(data);
  if (!model.valid()) RD_LOGE("resource.gltf", "无有效 mesh: %s", path);
  return model;
}

} // namespace rd
