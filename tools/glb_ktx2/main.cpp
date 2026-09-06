// glb_ktx2:glb 内嵌纹理 ASTC 化(KTX2 容器)——demo 包体积瘦身。
// 机制:glb 解析 → 逐图像 stb 解码 →(可选 maxDim 降采样)→ libktx CompressAstc
// → KTX2 blob 追加到 BIN 尾 + JSON 改 mimeType/bufferView(append-only,BIN 前部不动)。
#include <ktx.h>
#include <cgltf.h>
#include "draco/attributes/geometry_attribute.h"
#include "draco/compression/encode.h"
#include "draco/core/encoder_buffer.h"
#include "draco/mesh/mesh.h"
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

#pragma pack(push, 1)
struct GlbHeader {
  uint32_t magic, version, length;
};
struct ChunkHeader {
  uint32_t length, type;
};
#pragma pack(pop)

int gMaxDim = 1024;   // 0=不降采样
int gQuality = 2;       // astcenc qualityLevel 0=fastest..4=exhaustive
bool gDraco = false;    // --draco:未压缩几何 → Draco 重打包
int gQp = 14, gQn = 10, gQt = 12;  // 位置/法线/UV 量化 bits

/// RGBA8 像素 → ASTC 4x4 KTX2 blob;失败返回空。
std::vector<uint8_t> encodeAstcKtx2(const uint8_t* rgba, uint32_t w, uint32_t h) {
  ktxTextureCreateInfo ci{};
  ci.vkFormat = 37;  // VK_FORMAT_R8G8B8A8_UNORM(避免引 vulkan 头)
  ci.baseWidth = w;
  ci.baseHeight = h;
  ci.baseDepth = 1;
  ci.numDimensions = 2;
  ci.numLevels = 1;
  ci.numLayers = 1;
  ci.numFaces = 1;
  ci.isArray = KTX_FALSE;
  ci.generateMipmaps = KTX_FALSE;
  ktxTexture2* tex = nullptr;
  if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) != KTX_SUCCESS)
    return {};
  ktx_size_t offset = 0;
  bool ok = ktxTexture_SetImageFromMemory(ktxTexture(tex), 0, 0, 0, rgba,
                                          size_t(w) * h * 4) == KTX_SUCCESS;
  (void)offset;
  std::vector<uint8_t> out;
  if (ok) {
    ktxAstcParams ap{};
    ap.structSize = sizeof(ap);
    ap.blockDimension = KTX_PACK_ASTC_BLOCK_DIMENSION_4x4;
    ap.qualityLevel = gQuality;  // 0=fastest .. 4=exhaustive
    ap.mode = KTX_PACK_ASTC_ENCODER_MODE_LDR;
    if (ktxTexture2_CompressAstcEx(tex, &ap) != KTX_SUCCESS) {
      fprintf(stderr, "  ASTC 编码失败(%ux%u)\n", w, h);
    } else {
      ktx_uint8_t* data = nullptr;
      ktx_size_t size = 0;
      if (ktxTexture_WriteToMemory(ktxTexture(tex), &data, &size) == KTX_SUCCESS) {
        out.assign(data, data + size);
        free(data);  // WriteToMemory 分配(malloc)
      }
    }
  }
  ktxTexture_Destroy(ktxTexture(tex));
  return out;
}

/// 简单双线性降采样(RGBA8)。
std::vector<uint8_t> downscale(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh,
                               uint32_t dw, uint32_t dh) {
  std::vector<uint8_t> dst(size_t(dw) * dh * 4);
  for (uint32_t y = 0; y < dh; ++y)
    for (uint32_t x = 0; x < dw; ++x) {
      // 双线性:映射到源坐标
      const float fx = (float(x) + 0.5f) * float(sw) / float(dw) - 0.5f;
      const float fy = (float(y) + 0.5f) * float(sh) / float(dh) - 0.5f;
      const int x0 = std::max(0, int(fx)), y0 = std::max(0, int(fy));
      const int x1 = std::min(int(sw) - 1, x0 + 1), y1 = std::min(int(sh) - 1, y0 + 1);
      const float tx = std::min(1.0f, std::max(0.0f, fx - x0));
      const float ty = std::min(1.0f, std::max(0.0f, fy - y0));
      for (int c = 0; c < 4; ++c) {
        const float v = (float(src[(size_t(y0) * sw + x0) * 4 + c]) * (1 - tx) * (1 - ty) +
                         float(src[(size_t(y0) * sw + x1) * 4 + c]) * tx * (1 - ty) +
                         float(src[(size_t(y1) * sw + x0) * 4 + c]) * (1 - tx) * ty +
                         float(src[(size_t(y1) * sw + x1) * 4 + c]) * tx * ty);
        dst[(size_t(y) * dw + x) * 4 + c] = uint8_t(v + 0.5f);
      }
    }
  return dst;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr,
            "用法: %s <in.glb> <out.glb> [--max-dim N] [--quality 0-4] "
            "[--draco [--qp N] [--qn N] [--qt N]]\n", argv[0]);
    return 1;
  }
  const char* inPath = argv[1];
  const char* outPath = argv[2];
  for (int i = 3; i < argc; ++i) {
    if (!strcmp(argv[i], "--draco")) gDraco = true;
    else if (i + 1 < argc && !strcmp(argv[i], "--max-dim")) gMaxDim = atoi(argv[++i]);
    else if (i + 1 < argc && !strcmp(argv[i], "--quality"))
      gQuality = std::min(4, std::max(0, atoi(argv[++i])));
    else if (i + 1 < argc && !strcmp(argv[i], "--qp")) gQp = atoi(argv[++i]);
    else if (i + 1 < argc && !strcmp(argv[i], "--qn")) gQn = atoi(argv[++i]);
    else if (i + 1 < argc && !strcmp(argv[i], "--qt")) gQt = atoi(argv[++i]);
  }
  std::vector<uint8_t> glb;
  {
    FILE* f = fopen(inPath, "rb");
    if (!f) { fprintf(stderr, "打不开 %s\n", inPath); return 1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    glb.resize(size_t(n));
    if (fread(glb.data(), 1, size_t(n), f) != size_t(n)) { fclose(f); return 1; }
    fclose(f);
  }
  GlbHeader hdr{};
  memcpy(&hdr, glb.data(), sizeof(hdr));
  if (hdr.magic != 0x46546C67u) { fprintf(stderr, "非 glb\n"); return 1; }
  ChunkHeader jsonCh{}, binCh{};
  memcpy(&jsonCh, glb.data() + 12, sizeof(jsonCh));
  if (jsonCh.type != 0x4E4F534Au) { fprintf(stderr, "首 chunk 非 JSON\n"); return 1; }
  const size_t jsonOff = 20;
  const size_t binHdrOff = jsonOff + jsonCh.length;
  const bool hasBin = binHdrOff + 8 <= glb.size();
  if (!hasBin) { fprintf(stderr, "无 BIN chunk\n"); return 1; }
  memcpy(&binCh, glb.data() + binHdrOff, sizeof(binCh));
  if (binCh.type != 0x004E4942u) { fprintf(stderr, "第二 chunk 非 BIN\n"); return 1; }
  const size_t binOff = binHdrOff + 8;
  const size_t binLen = binCh.length;

  json j = json::parse(glb.data() + jsonOff, glb.data() + jsonOff + jsonCh.length, nullptr,
                       false);
  if (j.is_discarded()) { fprintf(stderr, "JSON 解析失败\n"); return 1; }

  // 图像 bufferView 索引集合
  std::vector<int> imgBv;
  if (j.contains("images") && j["images"].is_array())
    for (auto& img : j["images"])
      if (img.contains("bufferView")) imgBv.push_back(img["bufferView"].get<int>());

  // 全量重排 BIN:非图像 bufferView 原样拷贝;图像的替换为 KTX2 blob
  std::vector<uint8_t> newBin;
  size_t inOrigBytes = 0, outNewBytes = 0;
  auto appendAligned = [&](const uint8_t* p, size_t n) {
    while (newBin.size() % 4) newBin.push_back(0);
    const size_t off = newBin.size();
    newBin.insert(newBin.end(), p, p + n);
    return off;
  };

  if (!j.contains("bufferViews") || !j["bufferViews"].is_array()) {
    fprintf(stderr, "无 bufferViews\n");
    return 1;
  }
  auto& bvs = j["bufferViews"];
  std::vector<int> imgBvSet(imgBv.begin(), imgBv.end());
  std::sort(imgBvSet.begin(), imgBvSet.end());

  // ---- 可选 Draco 几何重打包(与纹理正交;先编码收集,重排循环再丢旧几何 bv) ----
  std::vector<int> droppedBv;
  std::vector<std::pair<std::vector<uint8_t>, json>> dracoAdds;  // (blob, extJson)
  std::vector<std::pair<size_t, size_t>> dracoPrimAt;  // (mesh,prim) 对应 adds 下标
  if (gDraco) {
    cgltf_options opts{};
    cgltf_data* cd = nullptr;
    if (cgltf_parse_file(&opts, inPath, &cd) == cgltf_result_success &&
        cgltf_load_buffers(&opts, cd, inPath) == cgltf_result_success && cd) {
      size_t inGeo = 0, outGeo = 0, skipped = 0;
      for (cgltf_size mi = 0; mi < cd->meshes_count; ++mi)
        for (cgltf_size pi = 0; pi < cd->meshes[mi].primitives_count; ++pi) {
          const cgltf_primitive& prim = cd->meshes[mi].primitives[pi];
          if (prim.has_draco_mesh_compression) { skipped++; continue; }
          json& jp = j["meshes"][size_t(mi)]["primitives"][size_t(pi)];
          if (jp.contains("extensions") &&
              jp["extensions"].contains("KHR_draco_mesh_compression")) {
            skipped++;
            continue;
          }
          const cgltf_accessor* pos = nullptr;
          for (cgltf_size a = 0; a < prim.attributes_count; ++a)
            if (prim.attributes[a].type == cgltf_attribute_type_position)
              pos = prim.attributes[a].data;
          if (!pos || !prim.indices || prim.targets_count > 0) { skipped++; continue; }
          draco::Mesh mesh;
          const uint32_t vc = uint32_t(pos->count), tc = uint32_t(prim.indices->count / 3);
          mesh.set_num_points(vc);
          mesh.SetNumFaces(tc);
          for (uint32_t f = 0; f < tc; ++f) {
            uint32_t iv[3];
            for (int k = 0; k < 3; ++k)
              iv[k] = uint32_t(cgltf_accessor_read_index(prim.indices, f * 3 + k));
            mesh.SetFace(draco::FaceIndex(f),
                         draco::Mesh::Face({draco::PointIndex(iv[0]),
                                            draco::PointIndex(iv[1]),
                                            draco::PointIndex(iv[2])}));
          }
          json extAttrs = json::object();
          bool ok = true;
          for (cgltf_size a = 0; a < prim.attributes_count && ok; ++a) {
            const cgltf_attribute& pa = prim.attributes[a];
            const cgltf_accessor* acc = pa.data;
            if (!acc || !acc->buffer_view) continue;
            if (pa.type != cgltf_attribute_type_position &&
                pa.type != cgltf_attribute_type_normal &&
                pa.type != cgltf_attribute_type_tangent &&
                pa.type != cgltf_attribute_type_texcoord &&
                pa.type != cgltf_attribute_type_joints &&
                pa.type != cgltf_attribute_type_weights) {
              ok = false;  // 其余语义不支持 → 该 primitive 放弃(JSON 未动)
              break;
            }
            const uint32_t comps = acc->type == cgltf_type_vec2   ? 2
                                   : acc->type == cgltf_type_vec3 ? 3
                                   : acc->type == cgltf_type_vec4 ? 4
                                                                  : 0;
            if (comps == 0) { ok = false; break; }
            std::vector<float> vals(size_t(vc) * comps);
            for (uint32_t v = 0; v < vc; ++v)
              cgltf_accessor_read_float(acc, v, &vals[size_t(v) * comps], comps);
            const draco::GeometryAttribute::Type gt =
                pa.type == cgltf_attribute_type_position
                    ? draco::GeometryAttribute::POSITION
                : pa.type == cgltf_attribute_type_normal
                    ? draco::GeometryAttribute::NORMAL
                : pa.type == cgltf_attribute_type_texcoord
                    ? draco::GeometryAttribute::TEX_COORD
                    : draco::GeometryAttribute::GENERIC;  // tangent/joints/weights(draco 无 TANGENT 语义)
            draco::GeometryAttribute ga;
            ga.Init(gt, nullptr, int(comps), draco::DT_FLOAT32, false, comps * 4, 0);
            const int ai = mesh.AddAttribute(ga, true, vc);
            const uint32_t uid = uint32_t((reinterpret_cast<const uint8_t*>(acc) -
                                           reinterpret_cast<const uint8_t*>(cd->accessors)) /
                                          sizeof(cgltf_accessor));
            mesh.attribute(ai)->set_unique_id(uid);
            for (uint32_t v = 0; v < vc; ++v)
              mesh.attribute(ai)->SetAttributeValue(draco::AttributeValueIndex(v),
                                                    &vals[size_t(v) * comps]);
            extAttrs[pa.name] = uid;
          }
          if (!ok) { skipped++; continue; }  // 中途拒绝:JSON 未动,原样保留
          // 先编码(读原始数据,JSON 未动),后按收益决定是否接管
          draco::Encoder enc;
          enc.SetAttributeQuantization(draco::GeometryAttribute::POSITION, gQp);
          enc.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, gQn);
          enc.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, gQt);
          draco::EncoderBuffer eb;
          if (!enc.EncodeMeshToBuffer(mesh, &eb).ok()) { skipped++; continue; }
          // 原几何字节数 = 语义/索引 accessor 引用的唯一 bv 总长
          size_t origGeo = 0;
          std::vector<int> oldBvs;
          {
            std::vector<int> accs;
            for (auto& kv : extAttrs.items()) accs.push_back(kv.value().get<int>());
            accs.push_back(int((reinterpret_cast<uintptr_t>(prim.indices) -
                               reinterpret_cast<uintptr_t>(cd->accessors)) /
                              sizeof(cgltf_accessor)));
            for (int ai : accs)
              if (j["accessors"][size_t(ai)].contains("bufferView"))
                oldBvs.push_back(j["accessors"][size_t(ai)]["bufferView"].get<int>());
            std::sort(oldBvs.begin(), oldBvs.end());
            oldBvs.erase(std::unique(oldBvs.begin(), oldBvs.end()), oldBvs.end());
            for (int bvi : oldBvs)
              origGeo += bvs[size_t(bvi)]["byteLength"].get<size_t>();
          }
          if (eb.size() >= origGeo) {
            skipped++;
            printf("  draco: primitive(m%zu p%zu) 无收益(%zuB ≥ 原 %zuB),保留原样\n",
                   size_t(mi), size_t(pi), eb.size(), origGeo);
            continue;
          }
          // 验证通过且有收益:剥离语义/索引 accessor 的 bufferView + 标记旧 bv 丢弃
          const auto stripAcc = [&](uint32_t accIdx) {
            if (j["accessors"][size_t(accIdx)].contains("bufferView")) {
              droppedBv.push_back(j["accessors"][size_t(accIdx)]["bufferView"].get<int>());
              j["accessors"][size_t(accIdx)].erase("bufferView");
              j["accessors"][size_t(accIdx)].erase("byteOffset");
            }
          };
          for (auto& kv : extAttrs.items()) stripAcc(uint32_t(kv.value().get<int>()));
          stripAcc(uint32_t((reinterpret_cast<uintptr_t>(prim.indices) -
                             reinterpret_cast<uintptr_t>(cd->accessors)) /
                            sizeof(cgltf_accessor)));
          dracoAdds.emplace_back(
              std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(eb.data()),
                                   reinterpret_cast<const uint8_t*>(eb.data()) + eb.size()),
              json{{"attributes", extAttrs}});
          dracoPrimAt.emplace_back(size_t(mi), size_t(pi));
          inGeo += size_t(pos->count) * 12 + prim.indices->count * 2;
          outGeo += eb.size();
        }
      cgltf_free(cd);
      if (!dracoAdds.empty()) {
        printf("  draco: %zu primitive,几何 ≈%.1fKB → %.1fKB\n", dracoAdds.size(),
               double(inGeo) / 1024.0, double(outGeo) / 1024.0);
        json eu = j.value("extensionsUsed", json::array());
        if (std::find(eu.begin(), eu.end(), "KHR_draco_mesh_compression") == eu.end())
          eu.push_back("KHR_draco_mesh_compression");
        j["extensionsUsed"] = eu;
      } else {
        printf("  draco: 无可压缩 primitive(跳过 %zu)\n", skipped);
      }
    } else {
      fprintf(stderr, "  draco: cgltf 解析失败,跳过几何压缩\n");
    }
  }

  for (int bvi = 0; bvi < int(bvs.size()); ++bvi) {
    auto& bv = bvs[bvi];
    if (std::find(droppedBv.begin(), droppedBv.end(), bvi) != droppedBv.end()) {
      // 旧几何数据被 draco 取代:不拷贝,置零(条目保留保下标稳定)
      bv["byteOffset"] = 0;
      bv["byteLength"] = 0;
      continue;
    }
    const size_t off = bv.value("byteOffset", 0);
    const size_t len = bv["byteLength"].get<size_t>();
    const uint8_t* src = glb.data() + binOff + off;
    const bool isImg = std::binary_search(imgBvSet.begin(), imgBvSet.end(), bvi);
    if (!isImg) {  // 非图像:原样拷贝
      const size_t no = appendAligned(src, len);
      bv["byteOffset"] = no;
      continue;
    }
    // 图像:已是 KTX2 则原样拷贝;否则解码→降采样→ASTC
    const bool alreadyKtx2 = len > 4 && src[0] == 0xAB && !memcmp(src + 1, "KTX", 3);
    if (alreadyKtx2) {
      const size_t no = appendAligned(src, len);
      bv["byteOffset"] = no;
      continue;
    }
    int w = 0, h = 0, n = 0;
    stbi_uc* dec = stbi_load_from_memory(src, int(len), &w, &h, &n, 4);
    if (!dec) {  // 解码失败兜底:原样保留
      const size_t no = appendAligned(src, len);
      bv["byteOffset"] = no;
      continue;
    }
    std::vector<uint8_t> rgba(dec, dec + size_t(w) * h * 4);
    stbi_image_free(dec);
    if (gMaxDim > 0 && (w > gMaxDim || h > gMaxDim)) {
      const float s = float(gMaxDim) / float(std::max(w, h));
      const uint32_t dw = std::max(1u, uint32_t(float(w) * s));
      const uint32_t dh = std::max(1u, uint32_t(float(h) * s));
      rgba = downscale(rgba, uint32_t(w), uint32_t(h), dw, dh);
      w = int(dw);
      h = int(dh);
    }
    auto ktx2 = encodeAstcKtx2(rgba.data(), uint32_t(w), uint32_t(h));
    // 无收益(已压缩的小图等)保留原样
    if (!ktx2.empty() && ktx2.size() >= len) {
      const size_t no = appendAligned(src, len);
      bv["byteOffset"] = no;
      printf("  图像 bv%d: 保留原样(%zuB ≤ ASTC %zuB)\n", bvi, len, ktx2.size());
      continue;
    }
    const size_t no = appendAligned(ktx2.empty() ? src : ktx2.data(),
                                    ktx2.empty() ? len : ktx2.size());
    if (!ktx2.empty()) {
      bv["byteOffset"] = no;
      bv["byteLength"] = ktx2.size();
      // mimeType 改 ktx2
      for (auto& img : j["images"])
        if (img.value("bufferView", -1) == bvi) img["mimeType"] = "image/ktx2";
      inOrigBytes += len;
      outNewBytes += ktx2.size();
      printf("  图像 bv%d: %zuB → %zuB (ASTC 4x4 %dx%d)\n", bvi, len, ktx2.size(), w, h);
    }
  }
  // draco blob 追加 + primitive 扩展落 JSON(bufferView 下标此时已稳定)
  for (size_t k = 0; k < dracoAdds.size(); ++k) {
    const size_t no = appendAligned(dracoAdds[k].first.data(), dracoAdds[k].first.size());
    const int newView = int(bvs.size());
    bvs.push_back(json{{"buffer", 0}, {"byteOffset", no},
                       {"byteLength", dracoAdds[k].first.size()}});
    auto& ext = dracoAdds[k].second;
    ext["bufferView"] = newView;
    j["meshes"][dracoPrimAt[k].first]["primitives"][dracoPrimAt[k].second]
     ["extensions"]["KHR_draco_mesh_compression"] = ext;
  }

  if (outNewBytes == 0 && dracoAdds.empty()) {
    fprintf(stderr, "无可转换图像与几何\n");
    return 1;
  }
  j["buffers"][0]["byteLength"] = newBin.size();
  const std::string jsonOut = j.dump();

  // 重组 glb:头 + JSON chunk(4 对齐) + BIN chunk(长度与数据均 4 对齐;glb 规范)
  while (newBin.size() % 4) newBin.push_back(0);
  std::vector<uint8_t> out;
  std::string jp = jsonOut;
  while (jp.size() % 4) jp += ' ';
  const uint32_t jsonLen = uint32_t(jp.size());
  const uint32_t total = 12 + 8 + jsonLen + 8 + uint32_t(newBin.size());
  hdr.length = total;
  out.reserve(total);
  out.insert(out.end(), reinterpret_cast<uint8_t*>(&hdr),
             reinterpret_cast<uint8_t*>(&hdr) + 12);
  ChunkHeader jc{jsonLen, 0x4E4F534Au};
  out.insert(out.end(), reinterpret_cast<uint8_t*>(&jc),
             reinterpret_cast<uint8_t*>(&jc) + 8);
  out.insert(out.end(), jp.begin(), jp.end());
  ChunkHeader bc{uint32_t(newBin.size()), 0x004E4942u};
  out.insert(out.end(), reinterpret_cast<uint8_t*>(&bc),
             reinterpret_cast<uint8_t*>(&bc) + 8);
  out.insert(out.end(), newBin.begin(), newBin.end());
  FILE* f = fopen(outPath, "wb");
  if (!f) { fprintf(stderr, "写不出 %s\n", outPath); return 1; }
  fwrite(out.data(), 1, out.size(), f);
  fclose(f);
  printf("完成: %s → %s(%.1fMB → %.1fMB,纹理 %zuKB → %zuKB)\n", inPath, outPath,
         glb.size() / 1048576.0, out.size() / 1048576.0, inOrigBytes / 1024,
         outNewBytes / 1024);
  return 0;
}
