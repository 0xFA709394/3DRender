#include "common/draco_gen.h"
#include "draco/attributes/geometry_attribute.h"
#include "draco/compression/encode.h"
#include "draco/core/encoder_buffer.h"
#include "draco/mesh/mesh.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace rd::test {
namespace {
constexpr uint32_t kSeg = 32, kRings = 16;  // 与 primitives::makeSphere(0.5,32,16) 同参数化
}

void dracoSphereSource(std::vector<float>& pos, std::vector<float>& nrm,
                       std::vector<float>& uv, std::vector<uint16_t>& idx) {
  const float r = 0.5f;
  pos.clear();
  nrm.clear();
  uv.clear();
  idx.clear();
  for (uint32_t y = 0; y <= kRings; ++y)
    for (uint32_t x = 0; x <= kSeg; ++x) {
      const float u = float(x) / float(kSeg), v = float(y) / float(kRings);
      const float theta = u * 2.0f * 3.14159265f, phi = v * 3.14159265f;
      const float dx = std::sin(phi) * std::cos(theta);
      const float dy = std::cos(phi);
      const float dz = std::sin(phi) * std::sin(theta);
      pos.push_back(r * dx);
      pos.push_back(r * dy);
      pos.push_back(r * dz);
      nrm.push_back(dx);
      nrm.push_back(dy);
      nrm.push_back(dz);
      uv.push_back(u);
      uv.push_back(v);
    }
  const uint32_t w = kSeg + 1;
  for (uint32_t y = 0; y < kRings; ++y)
    for (uint32_t x = 0; x < kSeg; ++x) {
      const uint16_t a = uint16_t(y * w + x), b = uint16_t(y * w + x + 1);
      const uint16_t c = uint16_t((y + 1) * w + x), d = uint16_t((y + 1) * w + x + 1);
      idx.push_back(a);
      idx.push_back(c);
      idx.push_back(b);
      idx.push_back(b);
      idx.push_back(c);
      idx.push_back(d);
    }
}

std::string writeDracoSphere(const std::string& dirStr) {
  namespace fs = std::filesystem;
  const fs::path dir(dirStr);
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::vector<float> pos, nrm, uv;
  std::vector<uint16_t> idx;
  dracoSphereSource(pos, nrm, uv, idx);
  const uint32_t vc = uint32_t(pos.size() / 3);
  const uint32_t tc = uint32_t(idx.size() / 3);
  std::vector<uint8_t> joints(vc * 4, 0);   // 全根骨 0
  std::vector<float> weights(vc * 4, 0.0f); // (1,0,0,0)
  for (uint32_t v = 0; v < vc; ++v) weights[v * 4] = 1.0f;

  draco::Mesh mesh;
  mesh.set_num_points(vc);  // identity mapping 不自动推 point 数(缺失=编码器越界)
  mesh.SetNumFaces(tc);
  for (uint32_t f = 0; f < tc; ++f) {
    mesh.SetFace(draco::FaceIndex(f),
                 draco::Mesh::Face({draco::PointIndex(idx[f * 3]),
                              draco::PointIndex(idx[f * 3 + 1]),
                              draco::PointIndex(idx[f * 3 + 2])}));
  }
  auto addAttr = [&](draco::GeometryAttribute::Type t, uint32_t comps,
                     draco::DataType dt, const void* data, size_t bytesPerVal,
                     uint32_t uniqueId) {
    draco::GeometryAttribute ga;
    ga.Init(t, nullptr, int(comps), dt, false, uint32_t(bytesPerVal), 0);
    const int ai = mesh.AddAttribute(ga, true, vc);
    mesh.attribute(ai)->set_unique_id(uniqueId);
    for (uint32_t v = 0; v < vc; ++v)
      mesh.attribute(ai)->SetAttributeValue(
          draco::AttributeValueIndex(v),
          reinterpret_cast<const uint8_t*>(data) + size_t(v) * bytesPerVal);
  };
  addAttr(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32, pos.data(), 12, 1);
  addAttr(draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32, nrm.data(), 12, 2);
  addAttr(draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32, uv.data(), 8, 3);
  addAttr(draco::GeometryAttribute::GENERIC, 4, draco::DT_UINT8, joints.data(), 4, 4);
  addAttr(draco::GeometryAttribute::GENERIC, 4, draco::DT_FLOAT32, weights.data(), 16, 5);

  draco::Encoder encoder;
  encoder.SetSpeedOptions(10, 10);  // 最快档=确定性
  encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 14);
  draco::EncoderBuffer ebuf;
  if (!encoder.EncodeMeshToBuffer(mesh, &ebuf).ok()) return {};
  const std::vector<uint8_t> bin(reinterpret_cast<const uint8_t*>(ebuf.data()),
                                 reinterpret_cast<const uint8_t*>(ebuf.data()) +
                                     ebuf.size());

  char json[2048];
  const int jsonLen = std::snprintf(
      json, sizeof(json),
      R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
      R"("nodes":[{"mesh":0,"skin":0},{"children":[]}],)"
      R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,)"
      R"("TEXCOORD_0":2,"JOINTS_0":3,"WEIGHTS_0":4},"indices":5,"material":0,)"
      R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,)"
      R"("attributes":{"POSITION":1,"NORMAL":2,"TEXCOORD_0":3,"JOINTS_0":4,)"
      R"("WEIGHTS_0":5}}}}]}],)"
      R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0.8,0.6,0.5,1]}}],)"
      R"("skins":[{"joints":[1]}],)"
      R"("extensionsUsed":["KHR_draco_mesh_compression"],)"
      R"("extensionsRequired":["KHR_draco_mesh_compression"],)"
      R"("buffers":[{"byteLength":%u}],)"
      R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":%u}],)"
      R"("accessors":[{"componentType":5126,"count":%u,"type":"VEC3"},)"
      R"({"componentType":5126,"count":%u,"type":"VEC3"},)"
      R"({"componentType":5126,"count":%u,"type":"VEC2"},)"
      R"({"componentType":5121,"count":%u,"type":"VEC4"},)"
      R"({"componentType":5126,"count":%u,"type":"VEC4"},)"
      R"({"componentType":5123,"count":%u,"type":"SCALAR"}]})",
      uint32_t(bin.size()), uint32_t(bin.size()), vc, vc, vc, vc, vc, tc * 3);
  const int jsonPad = (4 - (jsonLen % 4)) % 4;
  const int binPad = (4 - (int(bin.size()) % 4)) % 4;
  const uint32_t total =
      12 + uint32_t(8 + jsonLen + jsonPad) + uint32_t(8 + bin.size() + binPad);
  const fs::path out = dir / "draco_sphere.glb";
  FILE* f = fopen(out.string().c_str(), "wb");
  if (!f) return {};
  auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  w32(0x46546C67);
  w32(2);
  w32(total);
  w32(uint32_t(jsonLen + jsonPad));
  w32(0x4E4F534A);
  fwrite(json, 1, size_t(jsonLen), f);
  for (int i = 0; i < jsonPad; ++i) fputc(' ', f);
  w32(uint32_t(bin.size() + binPad));
  w32(0x004E4942);
  fwrite(bin.data(), 1, bin.size(), f);
  for (size_t i = 0; i < size_t(binPad); ++i) fputc(0, f);
  fclose(f);
  return out.string();
}

} // namespace rd::test
