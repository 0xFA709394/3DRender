# P2-3 设计:骨骼动画(GPU 蒙皮 + clip 播放 + 交叉淡入)

日期:2026-08-17
状态:已确认(用户审阅通过)
前置:P2-1(阴影+多光源)、P2-2(后处理链)已完成

## 0. 目标与范围

P2 第三个子项目:glTF 骨骼动画。

1. **GPU 蒙皮**:joint matrix palette 存 uniform(JointUBO slot3,128 骨上限);
   顶点 joints/weights 属性(loader 端转 float,零 RHI 改动)。
2. **clip 播放 + 交叉淡入**:sampler 线性插值(rotation slerp)、loop、
  双 clip 交叉淡入过渡。
3. **节点层级导入**:蒙皮所需的 nodes/skins/animations 全量解析进 ModelAsset。
4. **测试资产运行时程序生成**(2 骨 quad + 旋转 clip,与 ktx2 同模式)。

非目标:texture palette 存储(后续扩展)、>128 骨自动拆 drawcall(超出告警截断)、
morph target、STEP/CUBIC 插值(STEP 退化保持,CUBIC 按线性)、IK、动画事件。

## 1. loader:节点层级 + skins + animations + 80B 布局

- `ModelAsset` 扩展(非蒙皮模型以下均为空,行为不变):
  - `AnimNodeData nodes[]`:parent 下标、TRS(translation/rotation quat/scale)、
    mesh 下标(-1 无)。
  - `SkinData skins[]`:joints(node 下标表)、inverseBindMatrices、skeletonRoot。
  - `AnimClip animations[]`:channels(node 下标、path(translation/rotation/scale)、
    times、values 扁平序列)、duration。
- MeshData:蒙皮 mesh 顶点扩为 80B 交错
  (pos3@0|normal3@12|tangent4@24|uv2@40|joints4f@48|weights4f@64,location 0-5);
  JOINTS_0(u8/u16)与 WEIGHTS_0(u8/u16 归一化或 float)统一转 float。
  `MeshData.skinned = true` 标记。
- 非蒙皮模型保持 48B 布局,现有 golden 零回归。
- 顶点布局约定更新(AGENTS.md 同步):蒙皮=80B 六属性;非蒙皮=48B 四属性。

## 2. scene::Animator

```cpp
namespace rd::scene {
class Animator {
public:
  /// 绑定 ModelAsset(须含 skins/animations;nodes 非空)。
  bool bind(const ModelAsset& model);
  void play(uint32_t clipIndex);                        ///< 立即切换
  void playWithFade(uint32_t clipIndex, float fadeSec); ///< 交叉淡入
  void pause(bool p);                                    ///< 暂停/继续
  /// 推进时间并计算:节点全局矩阵 + jointMatrices(skin 0)。
  void update(float dt);
  /// 当前关节矩阵(globalJoint × inverseBind),供渲染层上传 JointUBO。
  const std::vector<math::Mat4>& jointMatrices() const;
  /// 节点全局矩阵(mesh 节点变换用)。
  const std::vector<math::Mat4>& nodeGlobals() const;
  bool playing() const;
};
}
```

- 采样:time 取模 loop;times 二分查找区间,translation/scale 线性、rotation slerp。
- 交叉淡入:新旧 clip 同时采样,按 fade 权重 lerp/slerp 混合;fade 结束后旧 clip 停采样。
- 单测:合成 clip 中点采样、淡入权重曲线、joint matrix = global × IBM 正确性。

## 3. renderer:skinned 管线 + 关节调色板

- `pbr_forward_skinned.vert`(新):location 4=joints4f,5=weights4f;
  `JointUBO(binding 3): mat4 joints[128]`;skin = Σ w_i·joints[int(j_i)];
  法线/切线同矩阵(mat3);frag 复用 pbr_forward.frag。
- Renderer:
  - 共享 JointUBO:64KB(8 项 × 8KB,per-item 偏移 256B 对齐);
    帧内 per-item 关节 updateBuffer;超 8 项告警截断。
  - submit 按 mesh 资源 stride 识别蒙皮(80B)→ skinned 管线;
    关节来源:scene 层经 `submit(mesh, world, joints)` 重载传入 palette 指针。
  - RenderContext 加 jointUbo/jointOffset;MeshRenderable 蒙皮路径绑 slot3。
- GLES 块名表加 `JointUBO→3`。
- golden:弯折 quad 双后端(运行时生成资产;t=弯折中点)。

## 4. C API + engine

- `rd_engine_play_animation(engine, index)`、`rd_engine_crossfade_animation(engine, index, fadeSeconds)`、
  `rd_engine_pause_animation(engine, int paused)`。
- load_gltf 含 animations 时自动播放 clip 0(loop);engine 持 Animator。
- render_frame dt 驱动 animator.update → 关节 palette 经 submit 传入。

## 5. 错误处理

- 蒙皮 mesh 缺 JOINTS/WEIGHTS:退化按 48B 处理 + 警告(joints 全 0 → 根骨)。
- 骨数 >128:截断 + 警告(拆 drawcall 归后续)。
- clip 下标越界:记警告 no-op。
- 无 skins 的模型调 play:警告 no-op。

## 6. 测试与验证

| 层 | 内容 |
|---|---|
| 单测 | loader 蒙皮解析(nodes/skins/animations/80B 顶点)、Animator 采样/淡入/jointMatrices |
| golden | 弯折 quad(双后端;t=弯折中点确定性) |
| 回归 | 非蒙皮 golden 全不变(48B 布局不动) |
| 手动 | interactive;Android 构建验证(GLES) |

## 7. 实施顺序

1. loader 扩展(nodes/skins/animations/80B)+ 解析单测
2. Animator + 单测
3. renderer skinned 管线 + JointUBO + golden
4. C API + engine 集成
5. AGENTS.md 收尾
