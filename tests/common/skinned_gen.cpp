#include "common/skinned_gen.h"
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace rd::test {

std::string writeSkinnedQuad(const std::string& dirStr) {
  namespace fs = std::filesystem;
  const fs::path dir(dirStr);
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string binPath = (dir / "quad_skin.bin").string();
  const std::string gltfPath = (dir / "quad_skin.gltf").string();

  // 顶点:(-0.5,0) (0.5,0) (-0.5,2) (0.5,2),法线 +Z
  const float pos[12] = {-0.5f, 0, 0, 0.5f, 0, 0, -0.5f, 2, 0, 0.5f, 2, 0};
  const float nrm[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
  const uint8_t joints[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
  const float weights[16] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  const uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
  // IBM:joint0=单位;joint1=translate(0,-1,0)(bind 全局平移 y=1 的逆)
  const float ibm[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                         1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
  // clip 采样:times=[0,1],quats=[identity, rotZ90(0,0,0.7071,0.7071)]
  const float times[2] = {0, 1};
  const float rots[8] = {0, 0, 0, 1, 0, 0, 0.70710678f, 0.70710678f};

  FILE* f = fopen(binPath.c_str(), "wb");
  if (!f) return {};
  fwrite(pos, 4, 12, f);     // bv0 @0
  fwrite(nrm, 4, 12, f);     // bv1 @48
  fwrite(uv, 4, 8, f);       // bv2 @96
  fwrite(joints, 1, 16, f);  // bv3 @128
  fwrite(weights, 4, 16, f); // bv4 @144
  fwrite(idx, 2, 6, f);      // bv5 @208
  fwrite(ibm, 4, 32, f);     // bv6 @220
  fwrite(times, 4, 2, f);    // bv7 @348
  fwrite(rots, 4, 8, f);     // bv8 @356 (总长 388)
  fclose(f);

  const char* json = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0, 2]}], "scene": 0,
    "nodes": [
      {"children": [1]},
      {"translation": [0, 1, 0]},
      {"mesh": 0, "skin": 0}
    ],
    "meshes": [{"primitives": [{"attributes": {
        "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2,
        "JOINTS_0": 3, "WEIGHTS_0": 4},
        "indices": 5, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.7, 0.7, 0.8, 1],
      "metallicFactor": 0.0, "roughnessFactor": 0.9}}],
    "skins": [{"joints": [0, 1], "inverseBindMatrices": 6, "skeleton": 0}],
    "animations": [{"name": "bend", "channels": [
        {"target": {"node": 1, "path": "rotation"}, "sampler": 0}],
      "samplers": [{"input": 7, "output": 8, "interpolation": "LINEAR"}]}],
    "buffers": [{"uri": "quad_skin.bin", "byteLength": 388}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 48},
      {"buffer": 0, "byteOffset": 48, "byteLength": 48},
      {"buffer": 0, "byteOffset": 96, "byteLength": 32},
      {"buffer": 0, "byteOffset": 128, "byteLength": 16},
      {"buffer": 0, "byteOffset": 144, "byteLength": 64},
      {"buffer": 0, "byteOffset": 208, "byteLength": 12},
      {"buffer": 0, "byteOffset": 220, "byteLength": 128},
      {"buffer": 0, "byteOffset": 348, "byteLength": 8},
      {"buffer": 0, "byteOffset": 356, "byteLength": 32}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
      {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
      {"bufferView": 3, "componentType": 5121, "count": 4, "type": "VEC4"},
      {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC4"},
      {"bufferView": 5, "componentType": 5123, "count": 6, "type": "SCALAR"},
      {"bufferView": 6, "componentType": 5126, "count": 2, "type": "MAT4"},
      {"bufferView": 7, "componentType": 5126, "count": 2, "type": "SCALAR"},
      {"bufferView": 8, "componentType": 5126, "count": 2, "type": "VEC4"}]
  })";
  f = fopen(gltfPath.c_str(), "wb");
  if (!f) return {};
  fwrite(json, 1, strlen(json), f);
  fclose(f);
  return gltfPath;
}

} // namespace rd::test
