// ============================================================================
// Android JNI 桥接层：com.rd.renderer.RenderView 的 native 方法实现。
//
// - engine 指针以 jlong 在 Java/native 间传递（Java 侧存为 Long，0 = 空）。
// - 所有方法由 RenderView 的专用渲染线程调用（满足 rd_engine 单线程约定）。
// - 链接时加 -Wl,-z,max-page-size=16384（Android 15+ 的 16KB 页对齐要求，
//   见 samples/android/app/build.gradle 或 rd_jni 的 CMake 配置）。
// ============================================================================
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include "api/rd_api.h"
#include "foundation/log.h"

extern "C" {

/// 创建引擎。backend：0=Vulkan，2=GLES（与 rd_backend_t 一致）。返回 engine 指针（0=失败）。
JNIEXPORT jlong JNICALL Java_com_rd_renderer_RenderView_nativeCreate(JNIEnv*, jobject,
                                                                     jint backend) {
  jlong result = reinterpret_cast<jlong>(rd_engine_create(static_cast<rd_backend_t>(backend)));
  RD_LOGI("rd_jni", "nativeCreate backend=%d -> %lld", int(backend), (long long)result);
  return result;
}

/// 销毁引擎（ptr 为 nativeCreate 返回值）。
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeDestroy(JNIEnv*, jobject, jlong ptr) {
  rd_engine_destroy(reinterpret_cast<rd_engine*>(ptr));
}

/// 设置渲染表面：Java Surface → ANativeWindow。
/// ANativeWindow_fromSurface 会 acquire 一次引用；调用后本函数 release，
/// 因为 engine 内部（swapchain）已 acquire 了自己的引用——净引用数不变。
JNIEXPORT jint JNICALL Java_com_rd_renderer_RenderView_nativeSetSurface(JNIEnv* env, jobject,
                                                                        jlong ptr, jobject surface,
                                                                        jint w, jint h) {
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (!window) return RD_ERROR_INVALID_ARG;
  jint result = rd_engine_set_surface(reinterpret_cast<rd_engine*>(ptr), window,
                                      static_cast<uint32_t>(w), static_cast<uint32_t>(h));
  RD_LOGI("rd_jni", "nativeSetSurface %dx%d -> %d", int(w), int(h), int(result));
  ANativeWindow_release(window); // engine 内部已 acquire 自己的引用
  return result;
}

/// 清除表面（surfaceDestroyed 时调用）。
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeClearSurface(JNIEnv*, jobject,
                                                                          jlong ptr) {
  rd_engine_clear_surface(reinterpret_cast<rd_engine*>(ptr));
}

/// 表面尺寸变化。
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeResize(JNIEnv*, jobject, jlong ptr,
                                                                    jint w, jint h) {
  rd_engine_resize(reinterpret_cast<rd_engine*>(ptr), static_cast<uint32_t>(w),
                   static_cast<uint32_t>(h));
}

/// 渲染一帧。dt 单位秒（由 Choreographer 帧间隔算出）。约每秒记一次帧日志（节流）。
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeRenderFrame(JNIEnv*, jobject,
                                                                         jlong ptr, jfloat dt) {
  static int frameCount = 0;
  if (frameCount++ % 60 == 0) RD_LOGI("rd_jni", "renderFrame #%d dt=%f", frameCount, double(dt));
  rd_engine_render_frame(reinterpret_cast<rd_engine*>(ptr), dt);
}

/// 指针事件（action: 0=DOWN,1=MOVE,2=UP,3=CANCEL;坐标为物理像素）。
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeOnPointer(
    JNIEnv*, jobject, jlong ptr, jint action, jint id, jfloat x, jfloat y) {
  rd_engine_on_pointer(reinterpret_cast<rd_engine*>(ptr),
                       static_cast<rd_pointer_action_t>(action), id, x, y);
}

/// 双指比例缩放（ratio>1 放大→拉近）。
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeOnPinch(JNIEnv*, jobject,
                                                                     jlong ptr, jfloat ratio) {
  rd_engine_on_pinch(reinterpret_cast<rd_engine*>(ptr), ratio);
}

/// 双击重置取景。
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeOnDoubleTap(JNIEnv*, jobject,
                                                                         jlong ptr, jfloat x,
                                                                         jfloat y) {
  rd_engine_on_double_tap(reinterpret_cast<rd_engine*>(ptr), x, y);
}

/// 同步加载 glTF 模型（返回 rd_result_t;0=成功）。
JNIEXPORT jint JNICALL Java_com_rd_renderer_RenderView_nativeLoadGltf(JNIEnv* env, jobject,
                                                                      jlong ptr, jstring path) {
  const char* p = env->GetStringUTFChars(path, nullptr);
  const jint r = jint(rd_engine_load_gltf(reinterpret_cast<rd_engine*>(ptr), p));
  RD_LOGI("rd_jni", "nativeLoadGltf -> %d", int(r));
  env->ReleaseStringUTFChars(path, p);
  return r;
}

/// 设置画质档(0=AUTO,1=HIGH,2=MID,3=LOW)。
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeSetQuality(JNIEnv*, jobject,
                                                                        jlong ptr, jint q) {
  rd_engine_set_quality(reinterpret_cast<rd_engine*>(ptr), static_cast<rd_quality_t>(q));
}

} // extern "C"
