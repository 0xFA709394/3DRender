// gltf_loader 的实现:cgltf 解析 → 固定交错布局顶点(stride 48,含切线)
// + 自适应索引 + 全材质纹理解码 + 包围球计算。
#include "resource/gltf_loader.h"
#include "resource/mesh_utils.h"
#include "foundation/log.h"

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
  if (mat->occlusion_texture.texture) {
    m.occlusion = decodeImage(mat->occlusion_texture.texture, gltfDir, pref);
    m.occlusionStrength = mat->occlusion_texture.scale;
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
      if (prim.type != cgltf_primitive_type_triangles || !prim.indices) continue;

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

      // 切线(uv 缺失/退化时降级,法线贴图近似)
      if (!computeTangents(out.vertices.data(), uint32_t(vertexCount), out.indices.data(),
                           out.indexCount, out.indexType, strideFloats)) {
        RD_LOGW("resource.gltf", "mesh %s 切线计算失败(uv 缺失/退化),法线贴图降级",
                out.name.c_str());
      }

      out.material = readMaterial(prim, gltfDir.c_str(), pref);
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
                                                                        : -1;
      if (chd.path < 0) continue;  // weights(morph)跳过
      const cgltf_accessor* in = ch.sampler->input;
      const cgltf_accessor* out = ch.sampler->output;
      chd.times.resize(in->count);
      for (cgltf_size k = 0; k < in->count; ++k)
        cgltf_accessor_read_float(in, k, &chd.times[k], 1);
      const uint32_t comps = chd.path == 1 ? 4 : 3;
      chd.values.resize(out->count * comps);
      for (cgltf_size k = 0; k < out->count; ++k)
        cgltf_accessor_read_float(out, k, &chd.values[k * comps], comps);
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
