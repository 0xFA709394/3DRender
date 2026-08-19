# 场景库扩充(6 新场景)实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-19-demo-scenes-2-design.md` 实现 6 新场景:4 纯程序(emissive_bloom/normal_map_wall/shadow_gallery/ktx2_gallery)+ alpha_blend(引擎混合支持)+ fox_anim(资产)。

**通用约定:**
- 构建+测试:`export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && ./scripts/check.sh`;单跑 `ctest --test-dir build --output-on-failure -R <正则>`
- golden 更新:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R DemoScenes` 后**像素核对**再提交
- 提交规范:每 Task 一个 commit,`<type>(<scope>): 描述`
- 场景质量档:DemoScene 新增 `const QualityPreset* quality` 指针(默认 nullptr=legacy 档;Bloom/阴影场景显式 High)
- 资源创建须在 acquireCommandBuffer 之前

---

### Task 1: 4 纯程序场景 + golden

**Files:**
- Modify: `tools/render_test/scenes.h`(DemoScene.quality)、`tools/render_test/scenes.cpp`(4 新场景)
- Golden: 新增 `demo_bloom_metal/vulkan.png`、`demo_normal_metal/vulkan.png`、`demo_shadow_metal/vulkan.png`、`demo_ktx2_metal/vulkan.png`

- [ ] **Step 1: DemoScene.quality 字段**

`scenes.h` 的 DemoScene 追加:

```cpp
  /// 可选画质档(默认 nullptr=legacy 档;Bloom/阴影场景显式指定)。
  const QualityPreset* quality = nullptr;
```

`scenes.cpp` 的 buildDemoScene 末尾(灯光应用处):

```cpp
  if (out.quality) renderer.setQuality(*out.quality);
```

- [ ] **Step 2: emissive_bloom**

`scenes.cpp` 新增 builder:

```cpp
void buildEmissiveBloom(Device& dev, DemoScene& out) {
  static const QualityPreset kHigh = {1.0f, 4, 256, 6, 4096, 2048, 1, 0};
  out.quality = &kHigh;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      auto mesh = primitives::makeSphere(0.3f, 32, 16);
      mesh.material.roughnessFactor = 0.4f;
      mesh.material.metallicFactor = 0.0f;
      mesh.material.baseColorFactor[0] = mesh.material.baseColorFactor[1] =
          mesh.material.baseColorFactor[2] = 0.15f;
      // 自发光沿对角递增(0.5→8),按 (i+j) 映射
      const float e = 0.5f + float(i + j) / 6.0f * 7.5f;
      mesh.material.emissiveFactor[0] = e * (i % 2 ? 1.0f : 0.25f);
      mesh.material.emissiveFactor[1] = e * (j % 2 ? 1.0f : 0.4f);
      mesh.material.emissiveFactor[2] = e * ((i + j) % 3 ? 0.6f : 1.0f);
      uploadInto(dev, wrapMesh(std::move(mesh)), out,
                 glm::translate(math::Mat4(1.0f),
                                math::Vec3((i - 1.5f) * 0.85f, (j - 1.5f) * 0.85f, 0)));
    }
  out.camera.lookAt({0, 0, 5.0f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 2.0f;
}
```

- [ ] **Step 3: normal_map_wall(程序化法线贴图)**

builder(法线贴图在 scenes.cpp 内生成):

```cpp
/// 程序化砖墙法线贴图:8 行砖+错缝,砖面凸起/灰浆凹槽 → 高度场转法线。
ImageData makeBrickNormalMap() {
  const uint32_t W = 256, H = 256;
  std::vector<float> height(size_t(W) * H, 0.0f);
  const uint32_t rowH = H / 8, brickW = W / 4;
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const uint32_t row = y / rowH;
      const uint32_t off = (row % 2) ? brickW / 2 : 0;
      const bool mortarY = (y % rowH) < 3;
      const bool mortarX = ((x + off) % brickW) < 3;
      height[size_t(y) * W + x] = (mortarY || mortarX) ? 0.0f : 1.0f;
    }
  ImageData img;
  img.width = W;
  img.height = H;
  img.pixels.resize(size_t(W) * H * 4);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      const float hl = height[size_t(y) * W + (x ? x - 1 : 0)];
      const float hr = height[size_t(y) * W + (x + 1 < W ? x + 1 : x)];
      const float hd = height[size_t(y ? y - 1 : 0) * W + x];
      const float hu = height[size_t(y + 1 < H ? y + 1 : y) * W + x];
      const float nx = (hl - hr) * 2.0f, ny = (hd - hu) * 2.0f, nz = 1.0f;
      const float inv = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
      uint8_t* p = img.pixels.data() + (size_t(y) * W + x) * 4;
      p[0] = uint8_t((nx * inv * 0.5f + 0.5f) * 255);
      p[1] = uint8_t((ny * inv * 0.5f + 0.5f) * 255);
      p[2] = uint8_t((nz * inv * 0.5f + 0.5f) * 255);
      p[3] = 255;
    }
  return img;
}

void buildNormalMapWall(Device& dev, DemoScene& out) {
  auto mesh = primitives::makePlane(4.0f, 2.0f);
  mesh.material.normal = makeBrickNormalMap();
  mesh.material.normalScale = 1.0f;
  mesh.material.roughnessFactor = 0.85f;
  mesh.material.metallicFactor = 0.0f;
  mesh.material.baseColorFactor[0] = 0.72f;
  mesh.material.baseColorFactor[1] = 0.45f;
  mesh.material.baseColorFactor[2] = 0.35f;
  // 平面立起面向 +Z(绕 X 轴 -90°),加斜向方向光
  uploadInto(dev, wrapMesh(std::move(mesh)), out,
             glm::rotate(math::Mat4(1.0f), -1.5707963f, math::Vec3(1, 0, 0)));
  LightData dl;
  dl.type = LightType::Directional;
  const float n = std::sqrt(0.5f * 0.5f + 0.5f * 0.5f + 0.5f * 0.5f);
  dl.direction[0] = 0.5f / n;
  dl.direction[1] = 0.5f / n;
  dl.direction[2] = 0.5f / n;
  dl.color[0] = dl.color[1] = dl.color[2] = 3.0f;
  out.lights.push_back(dl);
  out.camera.lookAt({0, 0, 4.0f}, {0, 0, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 2.5f;
}
```

- [ ] **Step 4: shadow_gallery**

```cpp
void buildShadowGallery(Device& dev, DemoScene& out) {
  static const QualityPreset kHigh = {1.0f, 4, 256, 6, 4096, 2048, 1, 0};
  out.quality = &kHigh;
  // 地面 + 抬升平台 + 悬空盒;斜向方向光 + 阴影取景
  uploadInto(dev, wrapMesh(primitives::makePlane(10.0f, 2.0f)), out, math::Mat4(1.0f));
  uploadInto(dev, wrapMesh(primitives::makeBox(1.6f, 0.2f, 1.6f)), out,
             glm::translate(math::Mat4(1.0f), math::Vec3(1.2f, 0.8f, -0.5f)));
  uploadInto(dev, wrapMesh(primitives::makeBox(0.7f, 0.7f, 0.7f)), out,
             glm::translate(math::Mat4(1.0f), math::Vec3(0.0f, 1.8f, 0.4f)));
  LightData dl;
  dl.type = LightType::Directional;
  const float n = std::sqrt(0.5f * 0.5f + 1.0f + 0.3f * 0.3f);
  dl.direction[0] = 0.5f / n;
  dl.direction[1] = 1.0f / n;
  dl.direction[2] = 0.3f / n;
  dl.color[0] = dl.color[1] = dl.color[2] = 3.0f;
  out.lights.push_back(dl);
  out.framingRadius = 3.5f;
  out.camera.lookAt({0, 2.5f, 6.0f}, {0, 0.8f, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
}
```

- [ ] **Step 5: ktx2_gallery**

```cpp
/// 彩色渐变图源(PNG 路径与 KTX2 路径共用)。
ImageData makeGradient() {
  const uint32_t S = 256;
  ImageData img;
  img.width = S;
  img.height = S;
  img.pixels.resize(size_t(S) * S * 4);
  for (uint32_t y = 0; y < S; ++y)
    for (uint32_t x = 0; x < S; ++x) {
      uint8_t* p = img.pixels.data() + (size_t(y) * S + x) * 4;
      p[0] = uint8_t(x);
      p[1] = uint8_t(y);
      p[2] = uint8_t(((x / 32) ^ (y / 32)) & 1 ? 220 : 60);
      p[3] = 255;
    }
  return img;
}

void buildKtx2Gallery(Device& dev, Renderer& renderer, DemoScene& out) {
  auto img = makeGradient();
  // 右:KTX2(libktx 现场编码 → 按 caps 转码目标)——复用 ktx2_gen 的编码路径
  // (scenes.cpp 内联:makeTestKtx2 的自定义版或直接写 ktxTexture2 流程;
  //  简化:借 tests/common/ktx2_gen::makeTestKtx2 棋盘图,左图用同棋盘 PNG 解码)
  // ——实现注:为左右一致,两边都用 ktx2_gen 棋盘源;
  //   左:libktx 解码为 RGBA(decodeKtx2 Rgba32);右:按 caps 转码 ASTC/ETC2
  const auto ktxBytes = test::makeTestKtx2(256);
  ImageData left = decodeKtx2(ktxBytes.data(), ktxBytes.size(), Ktx2Target::Rgba32) 的
    包装(把 Ktx2Image 转 ImageData;写个 toImageData 辅助)
  const bool astc = dev.caps().supports(Capability::texture_compression_astc);
  const bool etc2 = dev.caps().supports(Capability::texture_compression_etc2);
  auto right = decodeKtx2(ktxBytes.data(), ktxBytes.size(),
                          pickTranscodeTarget(astc, etc2));
  // 左右 quad 各自上传纹理:mesh.material.baseColor = toImageData(left/right)
  ...两个 quad 并排(左 x=-1.1,右 x=+1.1),相机正面近景
}
```

实现注:`Ktx2Image → ImageData` 转换辅助(toImageData,字段直搬;
ImageData.format/mipLevels 直通,MeshRenderResource 已支持压缩上传)。

- [ ] **Step 6: 注册 + golden**

`demoSceneNames` 的 kNames 追加 4 名;buildDemoScene 分支接入
(ktx2_gallery 的 builder 签名多一个 renderer 参数——给 builder 传 renderer)。

golden 沿用 demo_scenes_test 的 runGolden 模式,4 场景各 2 用例。
Run: `./scripts/check.sh 2>&1 | tail -6`(golden 缺失报错)
Run: `RD_UPDATE_GOLDENS=1 ctest --test-dir build --output-on-failure -R DemoScenes`
**像素核对** 8 张新 golden:辉光光晕、砖墙立体感、阴影投射、左右棋盘一致感;
双后端 direct≈0。再全量回归。
Commit: `feat(tools): 4 程序场景(emissive_bloom/normal_map_wall/shadow_gallery/ktx2_gallery)+ golden`

---

### Task 2: alpha_blend(引擎混合支持)+ 场景 golden

**Files:**
- Modify: `core/resource/gltf_loader.h`(`MaterialData.alphaBlend`)、`gltf_loader.cpp`(alphaMode 解析)
- Modify: `core/renderer/renderer.h`、`core/renderer/renderer.cpp`(混合管线 + 排序)
- Modify: `core/renderer/renderable.h`(RenderContext.blendPipeline)、`core/renderer/mesh_renderable.cpp`(blend 路径)
- Test: `tests/resource/gltf_test.cpp`(alphaMode 用例)、golden `demo_blend_metal/vulkan.png`

- [ ] **Step 1: loader alphaMode + 测试**

`MaterialData` 追加:`bool alphaBlend = false;  // KHR alphaMode=BLEND`
`readMaterial`:`m.alphaBlend = mat->alpha_mode == cgltf_alpha_mode_blend;`
(MASK 记警告按 OPAQUE)
gltf_test 追加:合成含 `"alphaMode": "BLEND"` 材质的最小 gltf,断言 alphaBlend=true。

- [ ] **Step 2: renderer 混合管线 + 排序**

`renderer.h` 成员:`PipelineHandle blendPipeline_;`
`ensureScenePipelines` 内追加(pbr 管线之后):

```cpp
  PipelineDesc bpd = ppd;  // 与 pbr 同布局/格式/采样数
  bpd.blend.enable = true;
  bpd.blend.srcColor = BlendFactor::SrcAlpha;
  bpd.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
  bpd.blend.srcAlpha = BlendFactor::One;
  bpd.blend.dstAlpha = BlendFactor::OneMinusSrcAlpha;
  bpd.depthWrite = false;
  if (blendPipeline_.valid()) dev_->destroyPipeline(blendPipeline_);
  blendPipeline_ = dev_->createPipeline(bpd);
```

`renderable.h` RenderContext 追加:`PipelineHandle blendPipeline;`
`mesh_renderable.cpp` pbr 分支前加:

```cpp
    if (g.material.alphaBlend && !g.skinned) {  // blend 路径(skinned 暂按 opaque)
      cmd->bindPipeline(ctx.blendPipeline);
      // UBO/纹理绑定同 pbr 路径(复制绑定段)
      ...
      continue;  // 跳过常规 pbr 绑定
    }
```

`renderer.cpp` endScene 排序(ItemUBO 填充前):

```cpp
  // opaque/blend 分区;blend 按视距远→近排序(正确透明叠加)
  const math::Vec3 eyePos = cameraEye_;  // beginScene 存 camera.eye()
  std::vector<uint32_t> order(count);
  for (uint32_t i = 0; i < count; ++i) order[i] = i;
  auto isBlend = [&](uint32_t i) {
    const auto* r = static_cast<const MeshRenderable*>(queue_[i].get());
    return !r->meshData().empty() && r->meshData()[0].material.alphaBlend;
  };
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const bool ba = isBlend(a), bb = isBlend(b);
    if (ba != bb) return !ba;  // opaque 先
    const auto da = glm::dot(worldStack_[a][3] - math::Vec4(eyePos, 1),
                             worldStack_[a][3] - math::Vec4(eyePos, 1));
    const auto db = glm::dot(worldStack_[b][3] - math::Vec4(eyePos, 1),
                             worldStack_[b][3] - math::Vec4(eyePos, 1));
    return da > db;  // 远→近
  });
```

ItemUBO 填充与绘制循环改按 `order[i]` 下标访问(注意 ItemUBO 的 per-item 槽位
必须与渲染顺序一致——**按 order 顺序重新填 UBO**(第 i 个槽 = order[i] 项);
jointSlot_ 同步按原下标对齐(jointSlot_[order[i]])。
beginScene 存 `cameraEye_ = camera.eye()`(Camera::eye 已有)。

- [ ] **Step 3: alpha_blend 场景 + golden**

```cpp
void buildAlphaBlend(Device& dev, DemoScene& out) {
  // 后排:3 彩色盒;前排:3 玻璃板(alpha 0.3/0.5/0.8)
  const float cols[3][3] = {{0.9f, 0.3f, 0.3f}, {0.3f, 0.9f, 0.4f}, {0.3f, 0.5f, 0.95f}};
  for (int i = 0; i < 3; ++i) {
    auto mesh = primitives::makeBox(0.9f, 0.9f, 0.9f);
    memcpy(mesh.material.baseColorFactor, cols[i], 12);
    mesh.material.roughnessFactor = 0.4f;
    uploadInto(dev, wrapMesh(std::move(mesh)), out,
               glm::translate(math::Mat4(1.0f),
                              math::Vec3((i - 1) * 1.2f, 0, -1.0f - i * 0.6f)));
  }
  const float alpha[3] = {0.8f, 0.5f, 0.3f};
  for (int i = 0; i < 3; ++i) {
    auto mesh = primitives::makePlane(1.6f, 1.0f);
    memcpy(mesh.material.baseColorFactor, cols[i], 12);
    mesh.material.baseColorFactor[3] = alpha[i];
    mesh.material.alphaBlend = true;  // 引擎混合路径
    uploadInto(dev, wrapMesh(std::move(mesh)), out,
               glm::rotate(math::Mat4(1.0f), -1.5707963f, math::Vec3(1, 0, 0)) *
                   glm::translate(math::Mat4(1.0f),
                                  math::Vec3((i - 1) * 1.3f, 0, 0.5f + i * 0.3f)));
  }
  out.camera.lookAt({0, 0.5f, 5.0f}, {0, 0, -0.5f}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
  out.framingRadius = 3.0f;
}
```

golden `demo_blend_metal/vulkan.png`(排序正确性:近处玻璃应透出远处盒)。
Commit: `feat(renderer): glTF alphaMode BLEND 混合管线 + 排序 + alpha_blend 场景`

---

### Task 3: fox_anim 接入

**Files:**
- Modify: `scripts/fetch_assets.sh`、`tools/render_test/scenes.cpp`

- [ ] **Step 1: fetch + 场景**

fetch_assets.sh 追加(404 回退):

```bash
dl Fox.glb Fox/glTF-Binary/Fox.glb || dl Fox.gltf.zip Fox/glTF-Embedded/Fox.gltf || true
```

(若 Binary 404:换 `Fox/glTF-Embedded/Fox.gltf`(内嵌资源单文件)——执行时验证)
`demoSceneNames` 追加 `fox_anim`;buildDemoScene:
`buildFamousGlb(dev, out, modelStorage, "Fox.glb", true)`(或 Fox.gltf,按下载结果)。

- [ ] **Step 2: 下载 + 验证**

Run: `./scripts/fetch_assets.sh`
Run: `./build/tools/render_test/render_test --scene fox_anim --out /tmp/fox.png` +
`img_check /tmp/fox.png --min-coverage 0.03` PASS
Commit: `feat(tools): fox_anim 真骨骼动画场景接入`

---

### Task 4: AGENTS.md 收尾 + 全量回归

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: AGENTS.md 更新**

- 「当前状态」追加场景库扩充(6 新场景名)。
- 「代码约定」追加:

```markdown
- 混合:glTF alphaMode=BLEND → MaterialData.alphaBlend;混合管线(BlendDesc srcAlpha/
  oneMinusSrcAlpha,depthWrite 关);endScene opaque 先、blend 按视距远→近;
  blend 项不参与蒙皮路径(按 opaque 处理)
```

- [ ] **Step 2: 全量回归**(check.sh 全绿 + Android/iOS 构建成功)

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md
git commit -m "docs: 场景库扩充收尾(AGENTS.md)"
```

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| MASK alphaMode | 暂不支持,警告按 OPAQUE(alpha cutoff 归后续) |
| blend+skinned 组合 | 蒙皮路径忽略 alphaBlend(按 opaque;罕见组合) |
| 排序粒度 | per-item(非 per-mesh/三角形)——多 mesh 物件内部顺序不保证;够 demo |
| ktx2 左右图 | 用 ktx2_gen 棋盘源保证一致;KTX2 有损差异即展示内容 |
