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

/**
 * @brief 最近一次错误的可读描述（无错误时为空串）。
 * @note 返回指针由 engine 持有，下次错误时被覆盖；调用方勿释放。
 */
const char* rd_get_last_error(rd_engine* engine);

#ifdef __cplusplus
}
#endif
