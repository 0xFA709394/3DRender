# 阶段二 a:渲染框架骨架 + 深度 + glTF unlit 渲染打通 设计文档

- **日期**: 2026-08-13
- **状态**: 已评审
- **前置文档**: `docs/superpowers/specs/2026-08-13-rhi-foundation-hardening-design.md`(阶段一)、`docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md`(总体设计)、`docs/superpowers/plans/2026-08-09-p1-1-pbr-core.md`(旧 P1 计划,参考吸收)
- **目标平台**: iOS / Android(鸿蒙维持预埋);host(Metal/Vulkan)驱动验证

## 1. 定位与已确认决策

阶段二总目标：借鉴 GGUI 五元组/帧流程设计 renderer/scene/resource 层，与 P1(glTF+PBR/IBL）合并实施。拆分为：

| spec | 内容 | 验收 |
|---|---|---|
| **2a(本文档)** | 框架骨架(三层架构 + 帧流程)+ RHI 深度附件 + glTF 静态 unlit 渲染打通 | BoxTextured.glb 离屏渲染 golden 双后端一致;全部单测/契约测试绿 |
| 2b(后续) | PBR/IBL:uber-shader + 环境生成(SH/预滤波/LUT)+ 切线 | DamagedHelmet golden 双后端一致 |
| 2c(后续) | 相机 Orbit 手势(平台输入转发)+ 画质分级初版 + KTX2 纹理 | 双端 demo 手势可交互;画质档位生效;KTX2 资产渲染 |

### 已确认决策

| 决策点 | 结论 |
|---|---|
| 架构 | **方案 A:GGUI 五元组移动版**——Renderer 立即模式执行,scene 保留模式组织,resource 持久持有重资产 |
| pass 组织 | 固定 pass 序列 + Renderable 级 prepass 钩子(2b IBL 预滤波用);RenderGraph 推迟到 P2 |
| 旧 P1 计划 | 参考吸收(cgltf/切线/环境生成/golden 思路);其 RHI 部分已被阶段一超越 |
| 深度 | 本阶段落地离屏深度附件;CompareOp 枚举预留(默认 Less,Reverse-Z 切换留作一行改动);swapchain 暂不带深度 |
| 顶点布局 | pos(3)+normal(3)+uv(2) 交错 stride 32;tangent 与 mesh_utils 归 2b |
| rd_engine/demo | 保持不变(继续转 cube);新骨架由 tests + tools/render_test 驱动 |

### 非目标(YAGNI)

- 不做 RenderGraph / ECS / 场景双缓冲线程模型(总体设计 §3.1 的三线程制归阶段三)
- 不做骨骼动画、阴影、后处理(P2)
- 不做 KTX2/Draco/异步加载(2c)
- 不做切线计算/IBL/PBR(2b)
- swapchain 深度附件(离屏先行;上屏深度随 P2 需求再评)

## 2. 分层结构

```
core/
  renderer/
    renderer.{h,cpp}        # 帧流程:render queue + pass 序列 + prepass 钩子 + 场景 UBO
    renderable.{h,cpp}      # Renderable 基类(配置 + record 命令)
    mesh_renderable.{h,cpp} # MeshRenderable:引用持久 GPU 资源 + 材质槽
  scene/
    node.{h,cpp}            # Node:局部 TRS + 子节点,world 矩阵遍历
    camera.{h,cpp}          # Camera:pos/lookat/up/fov/aspect;view/proj 矩阵
    scene.{h,cpp}           # Scene:root + 遍历产出 render items 投给 Renderer
  resource/
    gltf_loader.{h,cpp}     # cgltf(FetchContent)→ ModelAsset
    image_codec.{h,cpp}     # stb_image 解码/编码(tests/common/image.cpp 改为调用)
```

依赖方向只允许向下：scene/renderer 依赖 rhi/foundation;resource 不依赖 renderer(scene 组装时把资源交给 renderer);renderer 不知道资产从哪来。

**与 GGUI 的关键差异(有意为之)**:GGUI 每帧重建 VBO(轻数据)并依赖底层延迟释放;本框架模型是重资产——`MeshRenderable` **只持引用不拥有** GPU 资源,资源由 resource 层 `MeshRenderResource` 持久持有;帧级生命的仅是 render queue 中的配置对象,销毁安全由阶段一 N 帧退休队列兜底。

## 3. 核心机制

### 3.1 帧流程

```
device.beginFrame()
renderer.beginScene(camera, clearColor)   // view/proj → 场景 UBO(hostWrite 缓冲)
scene.collect(renderer)                   // 遍历 Node 树:MeshNode → renderer.submit(MeshRenderable)
renderer.endScene(cmd)                    // pass 序列:prepass 钩子(2b 用) → MainPass(depth 开)
device.submit(cmd) → present(sc) / readback(离屏)
device.endFrame()
```

- `Renderer` 持有:render queue(`std::vector<Renderable*>` 借用指针)+ 场景 UBO(每帧 map/memcpy)+ 帧内 renderable 配置对象池(帧末清空)。
- `Renderable` 基类:`record(cmd, sceneUBO)` 纯虚 + `prepass(cmd)` 默认空(2b IBL 预滤波钩子的挂载点,对齐 GGUI 两阶段录制)。
- `MeshRenderable`:持久资源引用(vbo/ibo/纹理句柄,不拥有)+ world 矩阵 + 材质槽(2a: baseColor 纹理 + sampler);record = bindPipeline(按材质键缓存)+ bindUniformBuffer(mvp)+ bindVertexBuffer/bindIndexBuffer/bindTexture + drawIndexed。

### 3.2 RHI 深度附件(阶段一预留落地)

- `OffscreenTargetDesc.depth=true` 三后端落地:
  - Vulkan:新增 depth 图像(D32)+ attachment;render pass 由单例改为**有/无深度两实例**,`createOffscreenTarget` 按 desc.depth 选用,pipeline 创建按目标深度性选用(管线与 render pass 兼容);
  - Metal:`MTLPixelFormatDepth32Float` 附件(离屏纹理,Private);
  - GLES:`DEPTH_COMPONENT24` renderbuffer/FBO 挂载(ES3 通用)+ `glEnable(GL_DEPTH_TEST)`。
- `PipelineDesc.depthTest/depthWrite` 放开(移除阶段一的拒绝守卫);新增 `DepthCompareOp{Less, Greater}` 枚举(默认 Less;**Reverse-Z 预留**:切 Greater + clearDepth 0.0 + 投影 near/far 对调即完成,2b 评估启用)。
- `beginRenderPass(target, clear)` 的 clear 扩展 depth 分量(默认 1.0f):`ClearColor` 加 `float depth = 1.0f`(Vulkan clearValues[2];Metal depthAttachment.clearDepth;GLES glClearDepthf + GL_DEPTH_BUFFER_BIT)。

### 3.3 resource 层

- `image_codec`:stb_image/stb_image_write 迁入 `core/resource`(tests/common/image.{h,cpp} 改为薄封装调用之);`decodeRGBA8(data,size)→{w,h,pixels}` / `encodePNG`。
- `GltfLoader`(cgltf v1.14,FetchContent):
  - `loadFromFile(path) → ModelAsset`;失败(不存在/解析错/无 mesh)返回空 + 日志。
  - 顶点布局 **pos(3f)+normal(3f)+uv(2f) 交错 stride 32**;属性缺失(无 uv/法线)补 0。索引:cgltf u16/u32 自适应(`IndexType` 相应选择,32 位索引阶段一已支持)。
  - 内嵌纹理图像(buffer view/uri)→ image_codec → RGBA8;解码失败 → 1x1 灰色占位(优雅降级,总体设计 §3.4)。
  - `ModelAsset { meshes: [{name, vertices, indices, indexType, baseColorImage}], }`——纯 CPU 数据,不碰 GPU。
- `MeshRenderResource`:ModelAsset → 持久 GPU 资源集(device-local vbo/ibo + 纹理 + 默认 sampler);由 scene 层或调用方持有。

### 3.4 scene 层

- `Node`:`TRS`(平移/旋转四元数或欧拉/缩放)→ 局部矩阵;`children` 列表;`worldMatrix()` 父链复合。2a 不做组件化,`MeshNode : Node` 挂 `shared_ptr<MeshRenderResource>` + 材质。
- `Camera`:`position/lookat/up/fovY/aspect/zNear/zFar`;`viewMatrix()`(glm::lookAt)、`projMatrix()`(glm::perspective,GLM_FORCE_DEPTH_ZERO_TO_ONE 既定约定)。
- `Scene`:`root` Node;`collect(renderer)` 深度优先遍历,跳过不可见节点(2a 全部可见)。

### 3.5 unlit 渲染与 shader

- `shaders/unlit.vert`:location0=pos,1=normal(不用),2=uv;UBO{mat4 mvp} binding0。
- `shaders/unlit.frag`:baseColor 纹理 slot0(binding4/texN)采样直出。
- 材质键:2a 仅一个 unlit 管线(按 colorFormat 区分),走阶段一管线缓存。

## 4. 测试策略

| 层级 | 内容 | 通过标准 |
|---|---|---|
| 单测 | gltf_loader:BoxTextured(mesh 数/顶点数/索引数/纹理尺寸)+ DamagedHelmet(多 mesh/32 位索引);Node TRS/world 复合;Camera view/proj | 断言通过 |
| golden(新) | 深度遮挡(交叠三角形,depthTest 开/关对比);BoxTextured 固定机位 unlit 渲染 | Metal/Vulkan 双后端容差比对通过 |
| 回归 | 既有 58 项测试(含 cube/纹理/blend/instancing/cube-target golden) | 全绿 |
| 工具 | `tools/render_test --model <glb> --backend metal|vulkan --out x.png` 离屏出图 | img_check 通过 |

## 5. 提交节奏(6 个 commit)

1. `feat(rhi): 离屏深度附件三后端 + depthTest/Write 放开 + CompareOp 预留 + 深度 golden`
2. `feat(resource): image_codec 迁入 + gltf_loader(cgltf)+ 单测`
3. `feat(scene): Node/Camera/Scene 骨架 + 单测`
4. `feat(renderer): Renderer/Renderable/MeshRenderable 帧流程骨架`
5. `feat(renderer): unlit shader + BoxTextured golden + render_test --model`
6. `docs: AGENTS.md 同步 + 旧 P1 计划取代标注`

## 6. 对 2b/2c 的预埋

- `Renderable::prepass` 空钩子 → 2b IBL 预滤波 pass;
- `DepthCompareOp::Greater` + clearDepth 字段 → Reverse-Z 一行切换;
- 顶点布局已含 normal;tangent 扩展 = stride 32→48 + mesh_utils(2b);
- `MeshRenderable` 材质槽 → 2b uber-shader 的多纹理/参数;
- `Camera` 交互方法与画质分级(分辨率缩放)挂点 → 2c。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| Vulkan render pass 双实例管理错误(pipeline/pass 不匹配) | createOffscreenTarget 记录 depth 性,beginRenderPass 按 target 选 pass;契约测试覆盖有无深度两路 |
| cgltf 真实模型数据多样性(缺 uv/法线、非交错、稀疏 accessor) | 加载期规整为固定交错布局 + 缺省补 0;DamagedHelmet 作为复杂样本进单测 |
| 深度精度/翻转约定三后端不一致 | 深度 golden 双后端容差比对;CompareOp 单测 |
| 骨架过度设计(2b 用不上) | 每个抽象对齐 GGUI 已验证形态 + 只实现 2a 所需最小面;prepass 等钩子不提前实现功能体 |
