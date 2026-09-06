// gltf_loader 的实现:cgltf 解析 → 固定交错布局顶点(stride 48,含切线)
// + 自适应索引 + 全材质纹理解码 + 包围球计算。
#include "resource/gltf_loader.h"
#include "resource/mesh_utils.h"
#include "foundation/log.h"
#include "foundation/math.h"

// cgltf 是单头库:实现宏只能在一个编译单元定义(本文件即该单元)
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#include <cmath>
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
  if (!found->data || !found->data->buffer_view) return;  // draco/无数据 → 解码层填充前跳过
  for (cgltf_size v = 0; v < vertexCount; ++v) {
    float tmp[4] = {0, 0, 0, 0};
    cgltf_accessor_read_float(found->data, v, tmp, compCount);
    float* out = dst + size_t(v) * dstStride;
    for (uint32_t c = 0; c < compCount; ++c) out[c] = tmp[c];
  }
}

/// 读文件全部字节(外链 URI 用);失败返回空 vector 并记警告。
std::vector<uint8_t> readFileBytes(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    RD_LOGW("resource.gltf", "外链资源打开失败: %s", path.c_str());
    return out;
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0) {
    out.resize(size_t(n));
    if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
  }
  fclose(f);
  return out;
}

/// 图像字节 → ImageData:KTX2 走 ktx2_codec(带转码目标),否则 stb 解码(带 maxDim)。
ImageData decodeImageBytes(const uint8_t* bytes, uint64_t size,
                           const TextureLoadPref& pref) {
  ImageData img;
  if (!bytes || size == 0) return img;
  if (isKtx2(bytes, size)) {
    Ktx2Image k = decodeKtx2(bytes, size, pref.ktx2Target);
    img.width = k.width;
    img.height = k.height;
    img.pixels = std::move(k.data);
    img.format = k.format;
    img.mipLevels = k.mipLevels;
    return img;
  }
  img = decodeImageRGBA8(bytes, size, pref.maxDim);
  return img;
}

/// 解码纹理视图:内嵌 buffer_view 与外链 URI(相对 gltf 文件目录)两条路径;
/// KHR_texture_basisu 优先取 basisu_image。
ImageData decodeImage(const cgltf_texture* tex, const char* gltfDir,
                      const TextureLoadPref& pref) {
  ImageData img;
  if (!tex) return img;
  const cgltf_image* image = tex->basisu_image ? tex->basisu_image : tex->image;
  if (!image) return img;
  if (image->buffer_view) {
    const cgltf_buffer_view* bv = image->buffer_view;
    const auto* bytes = static_cast<const uint8_t*>(bv->buffer->data);
    return decodeImageBytes(bytes + bv->offset, uint64_t(bv->size), pref);
  }
  if (image->uri) {
    const std::string full = std::string(gltfDir) + "/" + image->uri;
    const auto bytes = readFileBytes(full);
    if (!bytes.empty()) return decodeImageBytes(bytes.data(), bytes.size(), pref);
  }
  return img;
}

/// 读取材质(glTF metallic-roughness + KHR_texture_transform + unlit + basisu)。
MaterialData readMaterial(const cgltf_primitive& prim, const char* gltfDir,
                          const TextureLoadPref& pref) {
  MaterialData m;
  const cgltf_material* mat = prim.material;
  if (!mat) return m;
  m.unlit = mat->unlit;
  m.alphaBlend = mat->alpha_mode == cgltf_alpha_mode_blend;
  if (mat->alpha_mode == cgltf_alpha_mode_mask)
    m.alphaCutoff = mat->alpha_cutoff > 0.0f ? mat->alpha_cutoff : 0.5f;  // glTF 默认 0.5
  if (mat->has_pbr_metallic_roughness) {
    const auto& pbr = mat->pbr_metallic_roughness;
    if (pbr.base_color_texture.texture) {
      m.baseColor = decodeImage(pbr.base_color_texture.texture, gltfDir, pref);
      if (pbr.base_color_texture.has_transform) {  // KHR_texture_transform
        m.uvOffset[0] = pbr.base_color_texture.transform.offset[0];
        m.uvOffset[1] = pbr.base_color_texture.transform.offset[1];
        m.uvScale[0] = pbr.base_color_texture.transform.scale[0];
        m.uvScale[1] = pbr.base_color_texture.transform.scale[1];
      }
    }
    memcpy(m.baseColorFactor, pbr.base_color_factor, sizeof(m.baseColorFactor));
    m.metallicFactor = pbr.metallic_factor;
    m.roughnessFactor = pbr.roughness_factor;
    if (pbr.metallic_roughness_texture.texture)
      m.metallicRoughness = decodeImage(pbr.metallic_roughness_texture.texture, gltfDir, pref);
  }
  if (mat->normal_texture.texture) {
    m.normal = decodeImage(mat->normal_texture.texture, gltfDir, pref);
    m.normalScale = mat->normal_texture.scale;
  }
  if (mat->emissive_texture.texture)
    m.emissive = decodeImage(mat->emissive_texture.texture, gltfDir, pref);
  memcpy(m.emissiveFactor, mat->emissive_factor, sizeof(m.emissiveFactor));
  // KHR_materials_emissive_strength:emissive 强度 >1(HDR bloom 提亮)
  if (mat->has_emissive_strength) {
    const float es = float(mat->emissive_strength.emissive_strength);
    for (int c = 0; c < 3; ++c) m.emissiveFactor[c] *= es;
  }
  if (mat->occlusion_texture.texture) {
    m.occlusion = decodeImage(mat->occlusion_texture.texture, gltfDir, pref);
    m.occlusionStrength = mat->occlusion_texture.scale;
  }
  // KHR_materials_clearcoat:清漆层(独立 GGX 瓣 + 独立法线/粗糙度)
  if (mat->has_clearcoat) {
    const auto& cc = mat->clearcoat;
    m.clearcoatFactor = float(cc.clearcoat_factor);
    m.clearcoatRoughnessFactor = float(cc.clearcoat_roughness_factor);
    if (cc.clearcoat_texture.texture)
      m.clearcoat = decodeImage(cc.clearcoat_texture.texture, gltfDir, pref);
    if (cc.clearcoat_roughness_texture.texture)
      m.clearcoatRough = decodeImage(cc.clearcoat_roughness_texture.texture, gltfDir, pref);
    if (cc.clearcoat_normal_texture.texture) {
      m.clearcoatNormal = decodeImage(cc.clearcoat_normal_texture.texture, gltfDir, pref);
      m.clearcoatNormalScale = float(cc.clearcoat_normal_texture.scale);
    }
  }
  // KHR_materials_sheen:织物绒面(Charlie 分布)
  if (mat->has_sheen) {
    const auto& sh = mat->sheen;
    for (int c = 0; c < 3; ++c) m.sheenColorFactor[c] = float(sh.sheen_color_factor[c]);
    m.sheenRoughnessFactor = float(sh.sheen_roughness_factor);
    if (sh.sheen_color_texture.texture)
      m.sheenColor = decodeImage(sh.sheen_color_texture.texture, gltfDir, pref);
    if (sh.sheen_roughness_texture.texture)
      m.sheenRough = decodeImage(sh.sheen_roughness_texture.texture, gltfDir, pref);
  }
  // KHR_materials_specular:介质高光强度/颜色
  if (mat->has_specular) {
    const auto& sp = mat->specular;
    m.specularFactor = float(sp.specular_factor);
    for (int c = 0; c < 3; ++c) m.specularColorFactor[c] = float(sp.specular_color_factor[c]);
    if (sp.specular_color_texture.texture)
      m.specularColorTex = decodeImage(sp.specular_color_texture.texture, gltfDir, pref);
    if (sp.specular_texture.texture)
      m.specularTex = decodeImage(sp.specular_texture.texture, gltfDir, pref);
  }
  // KHR_materials_ior:折射率 → 介质 f0
  if (mat->has_ior) m.ior = float(mat->ior.ior);
  // KHR_materials_transmission:透射(场景色折射采样,渲染层 pass 拆分)
  if (mat->has_transmission) {
    const auto& tr = mat->transmission;
    m.transmissionFactor = float(tr.transmission_factor);
    if (tr.transmission_texture.texture)
      m.transmissionTex = decodeImage(tr.transmission_texture.texture, gltfDir, pref);
  }
  // KHR_materials_volume:厚度 + Beer-Lambert 吸收
  if (mat->has_volume) {
    const auto& vo = mat->volume;
    m.thicknessFactor = float(vo.thickness_factor);
    if (vo.thickness_texture.texture)
      m.thicknessTex = decodeImage(vo.thickness_texture.texture, gltfDir, pref);
    for (int c = 0; c < 3; ++c) m.attenuationColor[c] = float(vo.attenuation_color[c]);
    m.attenuationDistance =
        vo.attenuation_distance > 0.0f ? float(vo.attenuation_distance) : 0.0f;
  }
  return m;
}

} // namespace

ModelAsset loadGltf(const char* path) { return loadGltf(path, TextureLoadPref{}); }

ModelAsset loadGltf(const char* path, const TextureLoadPref& pref) {
  ModelAsset model;
  // gltf 文件目录(外链 URI 相对它解析)
  const std::string pathStr = path ? path : "";
  const size_t slash = pathStr.find_last_of('/');
  const std::string gltfDir = slash == std::string::npos ? "." : pathStr.substr(0, slash);
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

  float bmin[3] = {1e30f, 1e30f, 1e30f};
  float bmax[3] = {-1e30f, -1e30f, -1e30f};

  // ---- 节点层级导入(mesh 挂载用;蒙皮模型必需)----
  model.nodes.reserve(data->nodes_count);
  for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
    const cgltf_node* node = &data->nodes[ni];
    AnimNodeData an;
    an.parent = node->parent ? int32_t(cgltf_node_index(data, node->parent)) : -1;
    if (node->has_translation)
      for (int c = 0; c < 3; ++c) an.translation[c] = float(node->translation[c]);
    if (node->has_rotation)
      for (int c = 0; c < 4; ++c) an.rotation[c] = float(node->rotation[c]);
    if (node->has_scale)
      for (int c = 0; c < 3; ++c) an.scale[c] = float(node->scale[c]);
    if (node->has_matrix) {
      // matrix 形式:v1 取平移(TRS 形式为绝对主流;静态模型节点变换本就忽略)
      cgltf_float wm[16];
      cgltf_node_transform_local(node, wm);
      an.translation[0] = float(wm[12]);
      an.translation[1] = float(wm[13]);
      an.translation[2] = float(wm[14]);
      RD_LOGD("resource.gltf", "matrix 形式节点,仅取平移(TRS 退化)");
    }
    model.nodes.push_back(an);
  }

  // ---- mesh:节点驱动遍历(游离 mesh 不导入,与 glTF 语义一致)----
  for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
    const cgltf_node* node = &data->nodes[ni];
    if (!node->mesh) continue;
    const cgltf_mesh& mesh = *node->mesh;
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
      const cgltf_primitive& prim = mesh.primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles &&
          prim.type != cgltf_primitive_type_triangle_strip)
        continue;

      MeshData out;
      out.name = mesh.name ? mesh.name : "mesh";
      out.nodeIndex = int32_t(ni);
      const cgltf_accessor* pos = nullptr;
      const cgltf_accessor* jointsAcc = nullptr;
      const cgltf_accessor* weightsAcc = nullptr;
      for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
        if (prim.attributes[ai].type == cgltf_attribute_type_position)
          pos = prim.attributes[ai].data;
        if (prim.attributes[ai].type == cgltf_attribute_type_joints)
          jointsAcc = prim.attributes[ai].data;
        if (prim.attributes[ai].type == cgltf_attribute_type_weights)
          weightsAcc = prim.attributes[ai].data;
      }
      if (!pos) continue;
      // draco 未解码(解码失败/未接线):数据 accessor 无 bufferView → 跳过该 primitive
      if (prim.has_draco_mesh_compression && (!pos->buffer_view ||
          (prim.indices && !prim.indices->buffer_view))) {
        RD_LOGW("resource.gltf", "mesh %s draco primitive 未解码,跳过",
                mesh.name ? mesh.name : "");
        continue;
      }
      out.skinned = jointsAcc != nullptr;
      const uint32_t strideFloats = out.skinned ? 20 : 12;
      const cgltf_size vertexCount = pos->count;
      out.vertices.resize(size_t(vertexCount) * strideFloats, 0.0f);
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_position,
                    vertexCount, 3, out.vertices.data(), strideFloats);
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_normal,
                    vertexCount, 3, out.vertices.data() + 3, strideFloats);
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_texcoord,
                    vertexCount, 2, out.vertices.data() + 10, strideFloats);


      // 蒙皮属性:joints(u8/u16 → float)/weights(归一化自动转 float)
      if (out.skinned) {
        for (cgltf_size v = 0; v < vertexCount; ++v) {
          float* dst = out.vertices.data() + size_t(v) * strideFloats;
          if (jointsAcc) {
            cgltf_uint j4[4] = {};
            cgltf_accessor_read_uint(jointsAcc, v, j4, 4);
            for (int c = 0; c < 4; ++c) dst[12 + c] = float(j4[c]);
          }
          if (weightsAcc) {
            float w4[4] = {};
            cgltf_accessor_read_float(weightsAcc, v, w4, 4);
            for (int c = 0; c < 4; ++c) dst[16 + c] = w4[c];
          } else {
            dst[16] = 1.0f;  // 缺省:全权重根骨
          }
        }
      }

      // 包围球累积(POSITION min/max)
      for (cgltf_size v = 0; v < vertexCount; ++v) {
        const float* p = out.vertices.data() + size_t(v) * strideFloats;
        for (int c = 0; c < 3; ++c) {
          if (p[c] < bmin[c]) bmin[c] = p[c];
          if (p[c] > bmax[c]) bmax[c] = p[c];
        }
      }

      // 索引序列(u32 中间形态):有索引按源,无索引顺序生成;
      // triangle_strip 分解为三角形列表(交替绕序)
      const bool isStrip = prim.type == cgltf_primitive_type_triangle_strip;
      std::vector<uint32_t> seq;
      if (prim.indices) {
        seq.resize(prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; ++i)
          seq[i] = uint32_t(cgltf_accessor_read_index(prim.indices, i));
      } else {
        seq.resize(vertexCount);
        for (uint32_t v = 0; v < vertexCount; ++v) seq[v] = v;
      }
      std::vector<uint32_t> tris;
      if (isStrip) {
        tris.reserve(seq.size() * 3);
        for (size_t k = 0; k + 2 < seq.size(); ++k) {
          if (k % 2 == 0) {
            tris.push_back(seq[k]);
            tris.push_back(seq[k + 1]);
            tris.push_back(seq[k + 2]);
          } else {
            tris.push_back(seq[k + 1]);
            tris.push_back(seq[k]);
            tris.push_back(seq[k + 2]);
          }
        }
      } else {
        tris = std::move(seq);
      }
      const cgltf_size indexCount = tris.size();
      const bool needU32 = prim.indices &&
                               prim.indices->component_type == cgltf_component_type_r_32u ||
                           vertexCount > 65535;
      out.indexType = needU32 ? IndexType::UInt32 : IndexType::UInt16;
      out.indexCount = uint32_t(indexCount);
      if (needU32) {
        out.indices.resize(size_t(indexCount) * 4);
        auto* dst = reinterpret_cast<uint32_t*>(out.indices.data());
        for (cgltf_size i = 0; i < indexCount; ++i) dst[i] = tris[i];
      } else {
        out.indices.resize(size_t(indexCount) * 2);
        auto* dst = reinterpret_cast<uint16_t*>(out.indices.data());
        for (cgltf_size i = 0; i < indexCount; ++i) dst[i] = uint16_t(tris[i]);
      }

      // 法线缺失 → 逐面 flat 法线(glTF 允许;Fox 等老模型无 NORMAL);
      // 须在索引生成之后(依赖 out.indices)
      {
        bool hasNormal = false;
        for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
          if (prim.attributes[ai].type == cgltf_attribute_type_normal) hasNormal = true;
        if (!hasNormal) {
          const uint32_t triCount = out.indexCount / 3;
          for (uint32_t t = 0; t < triCount; ++t) {
            const size_t base = size_t(t) * 3;
            uint32_t iv[3];
            for (int k = 0; k < 3; ++k) {
              const size_t byteOff = (base + k) * (out.indexType == IndexType::UInt32 ? 4 : 2);
              iv[k] = out.indexType == IndexType::UInt32
                          ? *reinterpret_cast<const uint32_t*>(out.indices.data() + byteOff)
                          : *reinterpret_cast<const uint16_t*>(out.indices.data() + byteOff);
            }
            float* pa = out.vertices.data() + size_t(iv[0]) * strideFloats;
            float* pb = out.vertices.data() + size_t(iv[1]) * strideFloats;
            float* pc = out.vertices.data() + size_t(iv[2]) * strideFloats;
            const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
            const float e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                          e1[0] * e2[1] - e1[1] * e2[0]};
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-8f) {
              n[0] /= len;
              n[1] /= len;
              n[2] /= len;
            }
            for (int k = 0; k < 3; ++k) {
              float* pv = out.vertices.data() + size_t(iv[k]) * strideFloats;
              pv[3] = n[0];
              pv[4] = n[1];
              pv[5] = n[2];
            }
          }
        }
      }
      // 切线(uv 缺失/退化时降级,法线贴图近似)
      if (!computeTangents(out.vertices.data(), uint32_t(vertexCount), out.indices.data(),
                           out.indexCount, out.indexType, strideFloats)) {
        RD_LOGW("resource.gltf", "mesh %s 切线计算失败(uv 缺失/退化),法线贴图降级",
                out.name.c_str());
      }

      out.material = readMaterial(prim, gltfDir.c_str(), pref);
      // ---- morph targets(P4-C):POSITION/NORMAL 增量,TANGENT 忽略 ----
      // 8 目标上限(glTF 一致性最低要求线);读 accessor 经 cgltf(sparse 自动展开)
      if (prim.targets_count > 0 && vertexCount > 0) {
        constexpr uint32_t kMaxMorphTargets = 8;
        const uint32_t used =
            std::min(uint32_t(prim.targets_count), kMaxMorphTargets);
        if (prim.targets_count > kMaxMorphTargets)
          RD_LOGW("resource.gltf", "mesh %s morph targets %u 超上限,截断到 %u",
                  out.name.c_str(), uint32_t(prim.targets_count), kMaxMorphTargets);
        out.morphPosDeltas.assign(size_t(used) * vertexCount * 3, 0.0f);
        out.morphNormalDeltas.assign(size_t(used) * vertexCount * 3, 0.0f);
        bool anyValid = false;
        for (uint32_t t = 0; t < used; ++t) {
          const cgltf_attribute *pos = nullptr, *nrm = nullptr;
          for (cgltf_size a = 0; a < prim.targets[t].attributes_count; ++a) {
            const auto& at = prim.targets[t].attributes[a];
            if (at.type == cgltf_attribute_type_position) pos = &at;
            else if (at.type == cgltf_attribute_type_normal) nrm = &at;
            else if (at.type == cgltf_attribute_type_tangent)
              RD_LOGW("resource.gltf", "mesh %s morph TANGENT 增量忽略(v1 限制)",
                      out.name.c_str());
          }
          if (!pos) continue;  // glTF 语义:morph 目标须含 POSITION
          float* pp = &out.morphPosDeltas[size_t(t) * vertexCount * 3];
          float* nn = &out.morphNormalDeltas[size_t(t) * vertexCount * 3];
          for (cgltf_size v = 0; v < vertexCount; ++v) {
            cgltf_float d[3] = {0, 0, 0};
            if (cgltf_accessor_read_float(pos->data, v, d, 3))
              for (int c = 0; c < 3; ++c) pp[v * 3 + c] = float(d[c]);
            if (nrm) {
              cgltf_float dn[3] = {0, 0, 0};
              if (cgltf_accessor_read_float(nrm->data, v, dn, 3))
                for (int c = 0; c < 3; ++c) nn[v * 3 + c] = float(dn[c]);
            }
          }
          anyValid = true;
        }
        if (anyValid) {
          out.morph = true;
          if (mesh.weights_count > 0) {
            for (uint32_t t = 0; t < used && t < mesh.weights_count; ++t)
              out.morphWeights.push_back(float(mesh.weights[t]));
          } else {
            out.morphWeights.assign(used, 0.0f);
          }
          for (uint32_t t = 0; t < used && t < mesh.target_names_count; ++t)
            out.morphTargetNames.push_back(mesh.target_names[t]
                                               ? mesh.target_names[t]
                                               : "");
        } else {
          out.morphPosDeltas.clear();
          out.morphNormalDeltas.clear();
        }
      }
      if (model.nodes[ni].mesh < 0)
        model.nodes[ni].mesh = int32_t(model.meshes.size());
      model.meshes.push_back(std::move(out));
    }
  }

  // ---- skins ----
  for (cgltf_size si = 0; si < data->skins_count; ++si) {
    const cgltf_skin& skin = data->skins[si];
    SkinData sd;
    sd.joints.reserve(skin.joints_count);
    for (cgltf_size j = 0; j < skin.joints_count; ++j)
      sd.joints.push_back(int32_t(cgltf_node_index(data, skin.joints[j])));
    sd.inverseBindMatrices.resize(skin.joints_count * 16);
    if (skin.inverse_bind_matrices) {
      for (cgltf_size j = 0; j < skin.joints_count; ++j)
        cgltf_accessor_read_float(skin.inverse_bind_matrices, j,
                                  &sd.inverseBindMatrices[j * 16], 16);
    } else {  // 缺省单位
      for (cgltf_size j = 0; j < skin.joints_count; ++j)
        for (int k = 0; k < 16; ++k)
          sd.inverseBindMatrices[j * 16 + k] = (k % 5 == 0) ? 1.0f : 0.0f;
    }
    sd.skeletonRoot = skin.skeleton ? int32_t(cgltf_node_index(data, skin.skeleton)) : -1;
    model.skins.push_back(std::move(sd));
  }

  // ---- animations ----
  for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
    const cgltf_animation& anim = data->animations[ai];
    AnimClipData clip;
    clip.name = anim.name ? anim.name : "clip";
    for (cgltf_size ci = 0; ci < anim.channels_count; ++ci) {
      const cgltf_animation_channel& ch = anim.channels[ci];
      if (!ch.target_node || !ch.sampler) continue;
      AnimChannelData chd;
      chd.node = int32_t(cgltf_node_index(data, ch.target_node));
      chd.path = ch.target_path == cgltf_animation_path_type_translation ? 0
                 : ch.target_path == cgltf_animation_path_type_rotation ? 1
                 : ch.target_path == cgltf_animation_path_type_scale    ? 2
                 : ch.target_path == cgltf_animation_path_type_weights  ? 3
                                                                        : -1;
      if (chd.path < 0) continue;
      const cgltf_accessor* in = ch.sampler->input;
      const cgltf_accessor* out = ch.sampler->output;
      chd.times.resize(in->count);
      for (cgltf_size k = 0; k < in->count; ++k)
        cgltf_accessor_read_float(in, k, &chd.times[k], 1);
      uint32_t comps;
      if (chd.path == 3) {
        // weights:输出 SCALAR 扁平 keys×targets;目标数取目标 mesh(截断同静态解析)
        uint32_t targets = 0;
        if (ch.target_node->mesh && ch.target_node->mesh->primitives_count > 0)
          targets = std::min(uint32_t(ch.target_node->mesh->primitives[0].targets_count),
                             8u);
        if (targets == 0) continue;  // 无 morph 目标:通道无意义
        comps = targets;
      } else {
        comps = chd.path == 1 ? 4 : 3;
      }
      if (chd.path == 3) {
        // weights:输出 accessor 为逐标量扁平(keys×targets),逐元素读
        chd.values.resize(out->count);
        for (cgltf_size k = 0; k < out->count; ++k)
          cgltf_accessor_read_float(out, k, &chd.values[k], 1);
      } else {
        chd.values.resize(out->count * comps);
        for (cgltf_size k = 0; k < out->count; ++k)
          cgltf_accessor_read_float(out, k, &chd.values[k * comps], comps);
      }
      if (in->count > 0) clip.duration = std::max(clip.duration, chd.times[in->count - 1]);
      clip.channels.push_back(std::move(chd));
    }
    model.animations.push_back(std::move(clip));
  }

  // KHR_lights_punctual:遍历节点取世界变换(方向=旋转×(0,0,-1) 取反=+Z 列)
  for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
    const cgltf_node* node = &data->nodes[ni];
    if (!node->light) continue;
    if (model.lights.size() >= 4) {
      RD_LOGW("resource.gltf", "灯光超过 4 盏,截断");
      break;
    }
    const cgltf_light* l = node->light;
    cgltf_float m[16];
    cgltf_node_transform_world(node, m);  // 列主序世界矩阵
    LightData out;
    out.direction[0] = float(m[8]);   // +Z 列 = glTF 灯向(0,0,-1) 的反向 = 指向光源
    out.direction[1] = float(m[9]);
    out.direction[2] = float(m[10]);
    // 归一化(节点可能带缩放;零向量回退默认)
    {
      const float len = std::sqrt(out.direction[0] * out.direction[0] +
                                  out.direction[1] * out.direction[1] +
                                  out.direction[2] * out.direction[2]);
      if (len > 1e-6f) {
        out.direction[0] /= len;
        out.direction[1] /= len;
        out.direction[2] /= len;
      } else {
        out.direction[0] = 0;
        out.direction[1] = 1;
        out.direction[2] = 0;
      }
    }
    out.position[0] = float(m[12]);
    out.position[1] = float(m[13]);
    out.position[2] = float(m[14]);
    out.color[0] = l->color[0] * l->intensity;
    out.color[1] = l->color[1] * l->intensity;
    out.color[2] = l->color[2] * l->intensity;
    out.range = l->range;
    switch (l->type) {
      case cgltf_light_type_directional: out.type = LightType::Directional; break;
      case cgltf_light_type_point: out.type = LightType::Point; break;
      default: out.type = LightType::Spot; break;
    }
    out.innerCone = l->spot_inner_cone_angle;
    out.outerCone = l->spot_outer_cone_angle;
    model.lights.push_back(out);
  }
  // ---- 节点世界变换烘焙(非蒙皮 mesh;glTF 语义:蒙皮 mesh 忽略其节点变换)----
  // 修复:此前节点 TRS/matrix 完全未生效(DamagedHelmet 应立起、BoomBox 应转身、
  // Lantern 多部件应各就其位)。烘焙进顶点,运行时零成本。
  {
    // 每节点世界矩阵(层级递归;cgltf 直接给 world)
    std::vector<math::Mat4> nodeWorld(model.nodes.size(), math::Mat4(1.0f));
    bool anyTransform = false;
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
      cgltf_float wm[16];
      cgltf_node_transform_world(&data->nodes[ni], wm);
      math::Mat4 m;
      memcpy(&m, wm, sizeof(m));
      nodeWorld[ni] = m;
      // 非恒等判定
      const math::Mat4 I(1.0f);
      for (int c = 0; c < 16 && !anyTransform; ++c)
        if (std::fabs(reinterpret_cast<const float*>(&m)[c] -
                      reinterpret_cast<const float*>(&I)[c]) > 1e-6f)
          anyTransform = true;
    }
    if (anyTransform) {
      // 重置包围累积(烘焙后重算;蒙皮 mesh 未烘焙但也须计入)
      bmin[0] = bmin[1] = bmin[2] = 1e30f;
      bmax[0] = bmax[1] = bmax[2] = -1e30f;
      for (auto& mesh : model.meshes) {
        const bool bake = !mesh.skinned && mesh.nodeIndex >= 0 &&
                          size_t(mesh.nodeIndex) < nodeWorld.size();
        const glm::mat4* wp = bake ? &nodeWorld[size_t(mesh.nodeIndex)] : nullptr;
        glm::mat3 nm(1.0f);
        if (wp) nm = glm::transpose(glm::inverse(glm::mat3(*wp)));
        const uint32_t strideF = mesh.skinned ? 20 : 12;
        const size_t nv = mesh.vertices.size() / strideF;
        for (size_t vi = 0; vi < nv; ++vi) {
          float* p = &mesh.vertices[vi * strideF];
          if (wp) {
            const math::Vec4 tp = (*wp) * math::Vec4(p[0], p[1], p[2], 1.0f);
            p[0] = tp.x; p[1] = tp.y; p[2] = tp.z;
            const math::Vec3 tn = glm::normalize(nm * math::Vec3(p[3], p[4], p[5]));
            p[3] = tn.x; p[4] = tn.y; p[5] = tn.z;
            const math::Vec3 tt = glm::normalize(nm * math::Vec3(p[6], p[7], p[8]));
            p[6] = tt.x; p[7] = tt.y; p[8] = tt.z;  // w(手性)保持
          }
          // 包围重算(全部 mesh,烘焙与否都计入)
          for (int c = 0; c < 3; ++c) {
            if (p[c] < bmin[c]) bmin[c] = p[c];
            if (p[c] > bmax[c]) bmax[c] = p[c];
          }
        }
        // morph 增量同步烘焙(线性部分:pos 增量=mat3(world),normal 增量=法线矩阵)
        if (mesh.morph && wp) {
          const glm::mat3 lp(*wp);
          for (size_t i = 0; i + 2 < mesh.morphPosDeltas.size(); i += 3) {
            math::Vec3 d(mesh.morphPosDeltas[i], mesh.morphPosDeltas[i + 1],
                         mesh.morphPosDeltas[i + 2]);
            d = lp * d;
            mesh.morphPosDeltas[i] = d.x;
            mesh.morphPosDeltas[i + 1] = d.y;
            mesh.morphPosDeltas[i + 2] = d.z;
          }
          for (size_t i = 0; i + 2 < mesh.morphNormalDeltas.size(); i += 3) {
            math::Vec3 d(mesh.morphNormalDeltas[i], mesh.morphNormalDeltas[i + 1],
                         mesh.morphNormalDeltas[i + 2]);
            d = nm * d;
            mesh.morphNormalDeltas[i] = d.x;
            mesh.morphNormalDeltas[i + 1] = d.y;
            mesh.morphNormalDeltas[i + 2] = d.z;
          }
        }
      }
    }
  }
  cgltf_free(data);
  if (!model.valid()) {
    RD_LOGE("resource.gltf", "无有效 mesh: %s", path);
    return model;
  }
  // 包围球:center = min/max 中点,radius = 到角点最大距离
  for (int c = 0; c < 3; ++c) model.boundingCenter[c] = (bmin[c] + bmax[c]) * 0.5f;
  float r = 0;
  for (int c = 0; c < 3; ++c) {
    float d = (bmax[c] - bmin[c]) * 0.5f;
    r += d * d;
  }
  model.boundingRadius = std::sqrt(r);
  return model;
}

} // namespace rd
