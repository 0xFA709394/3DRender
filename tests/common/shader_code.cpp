// shader_code.h 的实现：按后端选扩展名读文件；读失败返回空 vector（调用方判空）。
#include "common/shader_code.h"
#include <fstream>

namespace rd::test {
namespace {
// 二进制方式整个读入；文件不存在/不可读返回空 vector
std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto size = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}
} // namespace

ShaderCode loadCubeShaderCode(Backend backend, const std::string& shaderDir) {
  ShaderCode out;
  // 后端 → 产物扩展名/入口名（spirv-cross 约定 Metal 入口为 main0）
  const char* ext = ".spv";
  if (backend == Backend::Metal) ext = ".metallib";
  if (backend == Backend::GLES) ext = ".gles";
  out.vs = readFile(shaderDir + "/cube.vert" + ext);
  out.fs = readFile(shaderDir + "/cube.frag" + ext);
  out.entry = (backend == Backend::Metal) ? "main0" : "main";
  return out;
}

ShaderBlob loadShaderCode(Backend backend, const std::string& shaderDir,
                          const std::string& name) {
  ShaderBlob out;
  const char* ext = ".spv";
  if (backend == Backend::Metal) ext = ".metallib";
  if (backend == Backend::GLES) ext = ".gles";
  out.code = readFile(shaderDir + "/" + name + ext);
  out.entry = (backend == Backend::Metal) ? "main0" : "main";
  return out;
}

} // namespace rd::test
