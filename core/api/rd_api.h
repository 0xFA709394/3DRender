/**
 * @file rd_api.h
 * @brief 渲染引擎 C API（平台层/绑定的唯一入口）。
 *
 * 线程约定：同一 engine 的所有调用必须在同一线程（平台层的"渲染线程"）。
 *   Android：RenderView 的专用渲染线程；iOS：主线程（MTKView 惯例）。
 *
 * 错误处理：不使用异常/错误码之外的机制；创建失败返回 nullptr，其余失败
 * 返回 rd_result_t 错误码，细节可经 rd_get_last_error 与日志获取。
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 引擎实例（不透明句柄，由 rd_engine_create 创建、rd_engine_destroy 销毁）。
typedef struct rd_engine rd_engine;

/// 渲染后端选择（与内核 rd::Backend 一一对应）。
typedef enum rd_backend {
  RD_BACKEND_VULKAN = 0,  ///< Vulkan（Android 原生；macOS 经 MoltenVK）
  RD_BACKEND_METAL = 1,   ///< Metal（iOS/macOS）
  RD_BACKEND_GLES = 2,    ///< OpenGL ES 3（Android）
} rd_backend_t;

/// API 结果码。
typedef enum rd_result {
  RD_OK = 0,                ///< 成功
  RD_ERROR_INVALID_ARG = 1, ///< 参数非法（空指针/尺寸为 0 等）
  RD_ERROR_SURFACE = 2,     ///< 表面/swapchain 创建失败
  RD_ERROR_SHADER = 3,      ///< 内嵌 shader 缺失或不可用
  RD_ERROR_SCENE = 4,       ///< 场景初始化失败（资源/管线创建失败）
  RD_ERROR_ASSET = 5,       ///< 资产加载/解析失败
} rd_result_t;

/**
 * @brief 创建引擎实例（创建 GPU 设备，不创建任何渲染资源）。
 * @param backend 期望的后端；该后端不可用（未编译/无设备）时返回 nullptr（细节见日志）。
 */
rd_engine* rd_engine_create(rd_backend_t backend);
/// 销毁引擎：等 GPU 空闲，释放场景资源与 swapchain 后释放设备。传 nullptr 安全。
void rd_engine_destroy(rd_engine* engine);

/**
 * @brief 设置渲染表面（创建 swapchain；首次调用还会初始化演示场景）。
 * @param native_window Android=ANativeWindow*，iOS=CAMetalLayer*。
 *        重复调用会销毁旧 swapchain 再创建。engine 内部会 retain 窗口句柄。
 * @note 必须在 rd_engine_render_frame 之前调用；Vulkan 后端可能在此按表面格式
 *       重建 render pass，故须在场景初始化（本函数内首次完成）之前设置表面。
 */
rd_result_t rd_engine_set_surface(rd_engine* engine, void* native_window, uint32_t width,
                                  uint32_t height);
/// 清除表面（销毁 swapchain）；之后 render_frame 成为安全 no-op。
void rd_engine_clear_surface(rd_engine* engine);
/// 表面尺寸变化（如旋转/分屏）；无表面或尺寸为 0 时安全忽略。
void rd_engine_resize(rd_engine* engine, uint32_t width, uint32_t height);

/**
 * @brief 渲染一帧并上屏。无 surface 或场景未就绪时为安全 no-op（偶发记警告）。
 * @param dt_seconds 距上一帧的秒数，驱动 Orbit 惯性等动画。
 */
void rd_engine_render_frame(rd_engine* engine, float dt_seconds);

/// 画质档位（AUTO=caps 启发式默认，引擎初始状态）。
typedef enum rd_quality {
  RD_QUALITY_AUTO = 0,
  RD_QUALITY_HIGH = 1,
  RD_QUALITY_MID = 2,
  RD_QUALITY_LOW = 3,
} rd_quality_t;

/**
 * @brief 设置画质档位；立即生效（下一次 render_frame 应用分辨率/MSAA/IBL 变化）。
 * @note 线程约定同 render_frame。
 */
rd_result_t rd_engine_set_quality(rd_engine* engine, rd_quality_t quality);
/// 当前生效档（AUTO 时返回启发式解析结果，不会返回 AUTO）；空引擎返回 RD_QUALITY_LOW。
rd_quality_t rd_engine_get_quality(rd_engine* engine);

/// 指针动作（触摸/鼠标）。
typedef enum rd_pointer_action {
  RD_POINTER_DOWN = 0,
  RD_POINTER_MOVE = 1,
  RD_POINTER_UP = 2,
  RD_POINTER_CANCEL = 3,
} rd_pointer_action_t;

/**
 * @brief 指针事件（像素坐标，origin 左上；id 区分多指，引擎跟踪前 2 个）。
 * 单指=旋转；双指=pinch 缩放+平移。空引擎安全忽略。
 */
void rd_engine_on_pointer(rd_engine* engine, rd_pointer_action_t action,
                          int32_t pointer_id, float x, float y);
/// host 滚轮缩放（deltaY>0 拉近）。
void rd_engine_on_scroll(rd_engine* engine, float delta_y);
/// 双指比例缩放（Android 探测器路径；ratio>1 放大→拉近）。
void rd_engine_on_pinch(rd_engine* engine, float ratio);
/// 双击重置取景。
void rd_engine_on_double_tap(rd_engine* engine, float x, float y);

/**
 * @brief 同步加载 glb/gltf 模型并替换场景内容（成功后 Orbit 自动取景）。
 * 纹理解码按当前画质档的尺寸上限与设备压缩格式 caps 自动选择。
 * @return RD_OK / RD_ERROR_INVALID_ARG（空参）/ RD_ERROR_ASSET（解析/上传失败，
 *         细节见 rd_get_last_error）。v1 为同步加载（异步留 P2）。
 */
rd_result_t rd_engine_load_gltf(rd_engine* engine, const char* path);

/// 清空全部手动灯光（清空后回落 glTF 灯/默认灯）。
void rd_engine_clear_lights(rd_engine* engine);
/// 加方向光（dir=指向光源方向，无需归一化；color×intensity）。
void rd_engine_add_dir_light(rd_engine* engine, float dx, float dy, float dz,
                             float r, float g, float b, float intensity);
/// 加点光（range≤0 视为无限）。
void rd_engine_add_point_light(rd_engine* engine, float px, float py, float pz,
                               float range, float r, float g, float b, float intensity);
/// 加聚光灯（内外锥角单位：度）。
void rd_engine_add_spot_light(rd_engine* engine, float px, float py, float pz,
                              float dx, float dy, float dz, float innerDeg,
                              float outerDeg, float range, float r, float g, float b,
                              float intensity);
/// 阴影总开关（默认 1；Low 画质档自动关，与本开关为与关系）。
void rd_engine_set_shadow_enabled(rd_engine* engine, int enabled);

/// 播放动画 clip（立即切换，loop）。越界/无动画记警告 no-op。
void rd_engine_play_animation(rd_engine* engine, int32_t clip_index);
/// 交叉淡入到目标 clip（fade_seconds 秒过渡）。
void rd_engine_crossfade_animation(rd_engine* engine, int32_t clip_index,
                                   float fade_seconds);
/// 暂停/继续动画。
void rd_engine_pause_animation(rd_engine* engine, int32_t paused);

/// 拾取结果（C 结构；mesh_name 截断 63B）。
typedef struct rd_pick_result {
  int32_t hit;          ///< 0/1
  int32_t mesh_index;   ///< meshes[] 下标(-1=未中)
  float distance;       ///< 命中距离
  float px, py, pz;     ///< 命中点(世界)
  char mesh_name[64];   ///< mesh 名
} rd_pick_result_t;

/**
 * @brief 射线拾取（像素坐标，与输入事件同一坐标系）。
 * 无模型/未命中/空引擎返回 hit=0；蒙皮模型按绑定姿态（已知限制）。
 * @note 线程约定同 render_frame。
 */
rd_pick_result_t rd_engine_pick(rd_engine* engine, float x, float y);

/**
 * @brief 最近一次错误的可读描述（无错误时为空串）。
 * @note 返回指针由 engine 持有，下次错误时被覆盖；调用方勿释放。
 */
const char* rd_get_last_error(rd_engine* engine);

#ifdef __cplusplus
}
#endif
