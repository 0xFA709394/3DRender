// CubeScene 的实现：几何数据 + 资源创建 + 每帧渲染（MVP 更新 → 命令录制 → 提交）。
#include "scene/cube_scene.h"
#include "foundation/math.h"
#include <glm/glm.hpp>

namespace rd::demo {
namespace {

// 顶点数据：位置(x,y,z) + 颜色(r,g,b) 交错排列，stride=24 字节。
// 单位立方体（±1），8 个顶点各配一种颜色便于目视验证朝向。
const float kVertices[] = {
    // pos(x,y,z)          color(r,g,b)
    -1, -1, -1,           1.0f, 0.0f, 0.0f, // 0
     1, -1, -1,           0.0f, 1.0f, 0.0f, // 1
     1,  1, -1,           0.0f, 0.0f, 1.0f, // 2
    -1,  1, -1,           1.0f, 1.0f, 0.0f, // 3
    -1, -1,  1,           1.0f, 0.0f, 1.0f, // 4
     1, -1,  1,           0.0f, 1.0f, 1.0f, // 5
     1,  1,  1,           1.0f, 1.0f, 1.0f, // 6
    -1,  1,  1,           0.2f, 0.2f, 0.2f, // 7
};

// 索引：12 三角形 × 3。全部外侧 CCW（front-face = CCW），配合背面剔除，
// 凸体闭合网格在无深度缓冲下也能得到正确遮挡。
const uint16_t kIndices[] = {
    4, 5, 6,  6, 7, 4,  // front (+z)
    1, 0, 3,  1, 3, 2,  // back  (-z)
    1, 2, 6,  1, 6, 5,  // right (+x)
    0, 4, 7,  0, 7, 3,  // left  (-x)
    3, 7, 6,  3, 6, 2,  // top   (+y)
    0, 1, 5,  0, 5, 4,  // bottom(-y)
};

} // namespace

bool CubeScene::init(Device& device, const uint8_t* vsCode, size_t vsSize, const uint8_t* fsCode,
                     size_t fsSize, const char* entryPoint, Format colorFormat) {
  // 几何缓冲：创建时随带上传；uniform 64 字节（一个 mat4）稍后每帧 update
  vbo_ = device.createBuffer({sizeof(kVertices), BufferUsage::Vertex, kVertices});
  ibo_ = device.createBuffer({sizeof(kIndices), BufferUsage::Index, kIndices});
  ubo_ = device.createBuffer({64, BufferUsage::Uniform, nullptr});

  ShaderModuleDesc vsd;
  vsd.stage = ShaderStage::Vertex;
  vsd.code.assign(vsCode, vsCode + vsSize);
  vsd.entryPoint = entryPoint;
  vs_ = device.createShaderModule(vsd);

  ShaderModuleDesc fsd;
  fsd.stage = ShaderStage::Fragment;
  fsd.code.assign(fsCode, fsCode + fsSize);
  fsd.entryPoint = entryPoint;
  fs_ = device.createShaderModule(fsd);

  // 管线：binding0 stride24；location0=pos(float3@0)，location1=color(float3@12)；
  // 背面剔除；颜色格式须与渲染目标一致（swapchain 由调用方传入）
  PipelineDesc pd;
  pd.vertexShader = vs_;
  pd.fragmentShader = fs_;
  pd.vertexBindings = {{0, 24}};
  pd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0}, {1, Format::R32G32B32_FLOAT, 12, 0}};
  pd.cullMode = CullMode::Back;
  pd.colorFormat = colorFormat;
  pipeline_ = device.createPipeline(pd);

  return vbo_.valid() && ibo_.valid() && ubo_.valid() && vs_.valid() && fs_.valid() &&
         pipeline_.valid();
}

void CubeScene::render(Device& device, TargetHandle target, uint32_t width, uint32_t height,
                       float angleRad) {
  using namespace rd::math;
  // MVP：透视 45°（NDC z∈[0,1]，见 foundation/math.h 约定）× 视图（z=4 看向原点）
  //      × 模型（绕 (1,1,0) 轴旋转 angleRad）
  Mat4 mvp = perspective(radians(45.0f), float(width) / float(height), 0.1f, 100.0f) *
             lookAt(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0)) *
             rotate(Mat4(1.0f), angleRad, glm::normalize(Vec3(1, 1, 0)));
  device.updateBuffer(ubo_, &mvp, sizeof(mvp), 0);

  CommandBuffer* cmd = device.acquireCommandBuffer();
  cmd->beginRenderPass(target, {0.1f, 0.1f, 0.12f, 1.0f});  // 深蓝灰背景
  cmd->bindPipeline(pipeline_);
  cmd->bindUniformBuffer(0, ubo_, 0, 64);   // uniform slot 0 ↔ shader 中 MVP 块
  cmd->bindVertexBuffer(0, vbo_, 0);
  cmd->bindIndexBuffer(ibo_, 0, IndexType::UInt16);
  cmd->drawIndexed(36, 0, 0);
  cmd->endRenderPass();
  device.submit(cmd);
  device.waitIdle();  // P0 简化：帧串行，便于 readback/截图
}

/// 按依赖逆序释放（管线 → 着色器 → 缓冲）；对无效句柄安全。
void CubeScene::shutdown(Device& device) {
  device.destroyPipeline(pipeline_);
  device.destroyShaderModule(vs_);
  device.destroyShaderModule(fs_);
  device.destroyBuffer(vbo_);
  device.destroyBuffer(ibo_);
  device.destroyBuffer(ubo_);
}

} // namespace rd::demo
