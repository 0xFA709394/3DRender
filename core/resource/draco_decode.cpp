#include "resource/draco_decode.h"
#include "draco/compression/decode.h"
#include "draco/core/decoder_buffer.h"
#include "draco/mesh/mesh.h"
#include "foundation/log.h"
#include <cstring>

namespace rd {

namespace {

/// 扩展 attribute 的 JSON 值(draco unique_id)被 cgltf_fixup_pointers 解释为
/// 1-based accessor 下标并改写为 &accessors[uid-1]——由指针差还原 uid。
/// (cgltf 固有约定:uid ≤ accessors_count,否则整体 parse 失败——生成器/工具
/// 的 uid 分配遵守;外部文件 uid 通常为小顺序值,实际兼容。)
uint32_t uidOf(const cgltf_attribute& a, const cgltf_data* data) {
  return uint32_t(a.data - data->accessors);  // PTRINDEX(idx)=idx+1;fixup 减 1 还原
}

void storeComponent(void* dst, cgltf_component_type ct, float v) {
  switch (ct) {
    case cgltf_component_type_r_8u: *static_cast<uint8_t*>(dst) = uint8_t(v); break;
    case cgltf_component_type_r_16u: *static_cast<uint16_t*>(dst) = uint16_t(v); break;
    case cgltf_component_type_r_32u: *static_cast<uint32_t*>(dst) = uint32_t(v); break;
    default: *static_cast<float*>(dst) = v; break;  // 5126(8/16 有符号不出现在语义里)
  }
}

uint32_t componentSize(cgltf_component_type ct) {
  switch (ct) {
    case cgltf_component_type_r_8:
    case cgltf_component_type_r_8u: return 1;
    case cgltf_component_type_r_16:
    case cgltf_component_type_r_16u: return 2;
    default: return 4;
  }
}

uint32_t typeComponents(cgltf_type t) {
  switch (t) {
    case cgltf_type_scalar: return 1;
    case cgltf_type_vec2: return 2;
    case cgltf_type_vec3: return 3;
    case cgltf_type_vec4: return 4;
    default: return 0;  // mat 系不出现在 draco 语义
  }
}

/// 解码一个 primitive → block;成功时同步改写 prim 指针;失败 false(保持原样)。
bool decodePrimitive(const cgltf_data* data, cgltf_primitive& prim, DracoPrimBlock& out) {
  const cgltf_draco_mesh_compression& ext = prim.draco_mesh_compression;
  const cgltf_buffer_view* bv = ext.buffer_view;
  const cgltf_buffer* buf = bv ? bv->buffer : nullptr;
  if (!bv || !buf || !buf->data) return false;
  const uint8_t* stream = static_cast<const uint8_t*>(buf->data) + bv->offset;

  draco::DecoderBuffer dbuf;
  dbuf.Init(reinterpret_cast<const char*>(stream), bv->size);
  draco::Decoder decoder;
  draco::StatusOr<std::unique_ptr<draco::Mesh>> res =
      decoder.DecodeMeshFromBuffer(&dbuf);
  if (!res.ok()) {
    RD_LOGW("resource.draco", "流解码失败: %s", res.status().error_msg());
    return false;
  }
  const draco::Mesh& mesh = *res.value();

  const cgltf_accessor* idxJson = prim.indices;
  if (!idxJson || mesh.num_faces() == 0 || idxJson->count != mesh.num_faces() * 3)
    return false;
  const uint32_t iSize = componentSize(idxJson->component_type);

  // 布局:blob = [索引][attr1 4对齐][attr2]...;jsonSrc[0]=索引 JSON
  std::vector<size_t> offsets{0};
  std::vector<const cgltf_accessor*> jsons{idxJson};
  std::vector<const draco::PointAttribute*> attrs{nullptr};
  size_t blobSize = size_t(idxJson->count) * iSize;
  for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
    const cgltf_attribute& pa = prim.attributes[a];
    if (!pa.data) continue;
    const draco::PointAttribute* da = nullptr;
    for (cgltf_size e = 0; e < ext.attributes_count; ++e)
      if (ext.attributes[e].name && pa.name &&
          std::strcmp(ext.attributes[e].name, pa.name) == 0) {
        da = mesh.GetAttributeByUniqueId(uidOf(ext.attributes[e], data));
        break;
      }
    if (!da) {
      RD_LOGW("resource.draco", "属性 %s 无对应 draco 流,primitive 拒绝",
              pa.name ? pa.name : "?");
      return false;
    }
    const uint32_t comps = typeComponents(pa.data->type);
    if (comps == 0) return false;
    blobSize = (blobSize + 3) & ~size_t(3);
    offsets.push_back(blobSize);
    jsons.push_back(pa.data);
    attrs.push_back(da);
    blobSize += size_t(pa.data->count) * comps * componentSize(pa.data->component_type);
  }
  if (blobSize == 0) return false;

  out.blob.reset(new uint8_t[blobSize]);
  std::memset(out.blob.get(), 0, blobSize);
  out.buffer = cgltf_buffer{};
  out.buffer.data = out.blob.get();
  out.buffer.size = cgltf_size(blobSize);
  out.views.resize(jsons.size());
  out.accessors.resize(jsons.size());
  out.jsonSrc = jsons;
  for (size_t k = 0; k < jsons.size(); ++k) {
    const cgltf_accessor* src = jsons[k];
    out.views[k] = cgltf_buffer_view{};
    out.views[k].buffer = &out.buffer;
    out.views[k].offset = cgltf_size(offsets[k]);
    out.views[k].size = cgltf_size(size_t(src->count) * typeComponents(src->type) *
                                   componentSize(src->component_type));
    out.views[k].stride = 0;  // 紧凑
    out.accessors[k] = *src;  // 复制 JSON 声明(count/type/component_type/...)
    out.accessors[k].buffer_view = &out.views[k];
    out.accessors[k].offset = 0;
    out.accessors[k].is_sparse = 0;
    std::memset(&out.accessors[k].sparse, 0, sizeof(cgltf_accessor_sparse));
  }

  uint8_t* base = out.blob.get();
  // 索引(faces)
  for (cgltf_size i = 0; i < idxJson->count; ++i) {
    const draco::Mesh::Face& f = mesh.face(draco::FaceIndex(i / 3));
    const uint32_t v = f[size_t(i % 3)].value();
    storeComponent(base + offsets[0] + size_t(i) * iSize, idxJson->component_type,
                   float(v));
  }
  // 属性(mapped_index 逐 glTF 顶点)
  float tmp[4];
  for (size_t k = 1; k < jsons.size(); ++k) {
    const cgltf_accessor* src = jsons[k];
    const draco::PointAttribute* da = attrs[k];
    const uint32_t comps = typeComponents(src->type);
    const uint32_t cSize = componentSize(src->component_type);
    for (cgltf_size v = 0; v < src->count; ++v) {
      const draco::AttributeValueIndex avi = da->mapped_index(draco::PointIndex(v));
      if (!da->ConvertValue(avi, int(comps), tmp))
        for (uint32_t c = 0; c < comps; ++c) tmp[c] = 0.0f;
      for (uint32_t c = 0; c < comps; ++c)
        storeComponent(base + offsets[k] + (size_t(v) * comps + c) * cSize,
                       src->component_type, tmp[c]);
    }
  }

  // 改写 prim 指针(索引 + 语义匹配的属性)
  prim.indices = &out.accessors[0];
  for (cgltf_size a = 0; a < prim.attributes_count; ++a)
    for (size_t k = 1; k < jsons.size(); ++k)
      if (prim.attributes[a].data == jsons[k]) {
        prim.attributes[a].data = &out.accessors[k];
        break;
      }
  return true;
}

} // namespace

size_t applyDracoDecoding(cgltf_data* data,
                          std::vector<std::unique_ptr<DracoPrimBlock>>& blocks) {
  size_t decoded = 0;
  for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
    cgltf_mesh& mesh = data->meshes[mi];
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
      cgltf_primitive& prim = mesh.primitives[pi];
      if (!prim.has_draco_mesh_compression) continue;
      auto block = std::make_unique<DracoPrimBlock>();
      if (decodePrimitive(data, prim, *block)) {
        blocks.push_back(std::move(block));
        ++decoded;
      } else {
        RD_LOGW("resource.draco", "mesh %s primitive %u 解码失败,跳过",
                mesh.name ? mesh.name : "", uint32_t(pi));
      }
    }
  }
  return decoded;
}

} // namespace rd
