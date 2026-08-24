package com.rd.renderer

import android.annotation.SuppressLint
import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.util.AttributeSet
import android.view.Choreographer
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * 3D 渲染视图（Android 平台容器）。所有 engine 调用都在专用渲染线程
 * （满足 rd_engine 单线程约定）。
 *
 * 用法：setBackend() → 加入布局即可；surface 生命周期全自动。
 *
 * 线程模型：
 * - surfaceCreated：新建专用 HandlerThread（"rd-render"）；
 * - surfaceChanged：在渲染线程上创建 engine + 设置表面，然后启动
 *   Choreographer 垂直同步帧回调驱动渲染循环；
 * - surfaceDestroyed：在渲染线程上销毁 engine 后退出线程。
 * 渲染线程每次 surfaceCreated 新建（HandlerThread quit 后不可复用）。
 */
class RenderView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    /** 后端选择；value 与 rd_backend_t 一致（0=Vulkan，2=GLES）。 */
    enum class Backend(val value: Int) { VULKAN(0), GLES(2) }

    private var renderThread: HandlerThread? = null // 每次 surfaceCreated 新建（quit 后不可复用）
    private lateinit var renderHandler: Handler
    private var enginePtr: Long = 0      ///< native engine 指针（0=未创建/已销毁）
    private var backend = Backend.GLES   ///< 默认 GLES（兼容性最好）
    private var running = false          ///< 帧回调是否激活
    private var lastFrameNanos = 0L      ///< 上一帧时间戳（算 dt 用）

    /** 垂直同步帧回调：计算 dt 并驱动 rd_engine_render_frame，然后挂下一帧。 */
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!running) return
            // 首帧无历史时间戳，按 60fps 给个默认 dt
            val dt = if (lastFrameNanos == 0L) 1f / 60f
            else (frameTimeNanos - lastFrameNanos) / 1_000_000_000f
            lastFrameNanos = frameTimeNanos
            if (enginePtr != 0L) nativeRenderFrame(enginePtr, dt)
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    init {
        holder.addCallback(this)
    }

    /** 选择后端；必须在 surface 就绪（engine 创建）之前调用。 */
    fun setBackend(value: Backend) {
        check(enginePtr == 0L) { "setBackend 须在 surface 就绪前调用" }
        backend = value
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val thread = HandlerThread("rd-render")
        thread.start()
        renderThread = thread
        renderHandler = Handler(thread.looper)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        val surface = holder.surface
        // 全部 engine 操作投递到渲染线程执行
        renderHandler.post {
            if (enginePtr == 0L) {
                enginePtr = nativeCreate(backend.value)
                nativeSetCacheDir(enginePtr, context.cacheDir.absolutePath + "/rd_cache")
                if (enginePtr != 0L &&
                    nativeSetSurface(enginePtr, surface, width, height) == 0) {
                    running = true
                    lastFrameNanos = 0L
                    Choreographer.getInstance().postFrameCallback(frameCallback)
                }
            } else {
                // 已初始化：纯尺寸变化（旋转/分屏）
                nativeResize(enginePtr, width, height)
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // 在渲染线程上停帧回调并销毁 engine，然后安全退出线程
        renderHandler.post {
            running = false
            Choreographer.getInstance().removeFrameCallback(frameCallback)
            if (enginePtr != 0L) {
                nativeClearSurface(enginePtr)
                nativeDestroy(enginePtr)
                enginePtr = 0L
            }
            renderThread?.quitSafely()
            renderThread = null
        }
    }

    // ---- 手势 → Orbit(事件在 UI 线程到达,投递到渲染线程喂给 engine)----
    /** 物理像素换算(C API 约定像素坐标,与 surface 尺寸一致) */
    private val pxScale: Float get() = resources.displayMetrics.density

    // ---- 输入录制(与 host 回放同格式:归一化坐标)----
    private var recordWriter: java.io.BufferedWriter? = null
    private var recordStartNanos = 0L

    /** 输入录制开关;path 空串=停止。 */
    fun setInputRecording(path: String) {
        if (path.isEmpty()) {
            recordWriter?.close()
            recordWriter = null
            return
        }
        recordWriter = java.io.File(path).bufferedWriter()
        recordStartNanos = System.nanoTime()
        recordWriter?.write("# viewport $width $height\n")
        recordWriter?.flush()
    }

    private fun recordEvent(action: String, id: Int, nx: Float, ny: Float) {
        val w = recordWriter ?: return
        val t = (System.nanoTime() - recordStartNanos) / 1_000_000
        w.write("$t $action $id $nx $ny\n")
        w.flush()
    }

    private val scaleDetector = ScaleGestureDetector(context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean {
                val ratio = d.scaleFactor
                renderHandler.post { if (enginePtr != 0L) nativeOnPinch(enginePtr, ratio) }
                return true
            }
        })

    private val gestureDetector = GestureDetector(context,
        object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
                val x = e.x * pxScale
                val y = e.y * pxScale
                renderHandler.post { if (enginePtr != 0L) nativeOnDoubleTap(enginePtr, x, y) }
                return true
            }
            override fun onDown(e: MotionEvent): Boolean = true
        })

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(e)
        gestureDetector.onTouchEvent(e)
        val action = when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> 0
            MotionEvent.ACTION_MOVE -> 1
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> 2
            MotionEvent.ACTION_CANCEL -> 3
            else -> -1
        }
        if (action >= 0) {
            val i = e.actionIndex
            // 录制(归一化坐标;与 host 回放同格式)
            if (recordWriter != null) {
                val actionName = when (action) {
                    0 -> "down"; 1 -> "move"; else -> "up"
                }
                if (action == 1) {
                    for (k in 0 until e.pointerCount)
                        recordEvent(actionName, e.getPointerId(k),
                                    e.getX(k) / width, e.getY(k) / height)
                } else {
                    recordEvent(actionName, e.getPointerId(i),
                                e.getX(i) / width, e.getY(i) / height)
                }
            }
            renderHandler.post {
                if (enginePtr == 0L) return@post
                if (action == 1) {
                    // MOVE 需遍历全部指针(每个都喂 MOVE)
                    for (k in 0 until e.pointerCount)
                        nativeOnPointer(enginePtr, 1, e.getPointerId(k),
                                        e.getX(k) * pxScale, e.getY(k) * pxScale)
                } else {
                    nativeOnPointer(enginePtr, action, e.getPointerId(i),
                                    e.getX(i) * pxScale, e.getY(i) * pxScale)
                }
            }
        }
        return true
    }

    /** 加载 glTF 模型(渲染线程执行;surface 就绪后调用)。 */
    fun loadModel(path: String) {
        renderHandler.post { if (enginePtr != 0L) nativeLoadGltf(enginePtr, path) }
    }

    /** 设置画质档(1=HIGH,2=MID,3=LOW,0=AUTO;渲染线程执行)。 */
    fun setQuality(tier: Int) {
        renderHandler.post { if (enginePtr != 0L) nativeSetQuality(enginePtr, tier) }
    }

    // ---- JNI native 方法（实现在 platform/android/jni/rd_jni.cpp）----
    private external fun nativeCreate(backend: Int): Long
    private external fun nativeSetCacheDir(ptr: Long, path: String)
    private external fun nativeDestroy(ptr: Long)
    private external fun nativeSetSurface(ptr: Long, surface: Surface, width: Int, height: Int): Int
    private external fun nativeClearSurface(ptr: Long)
    private external fun nativeResize(ptr: Long, width: Int, height: Int)
    private external fun nativeRenderFrame(ptr: Long, dt: Float)
    private external fun nativeOnPointer(ptr: Long, action: Int, id: Int, x: Float, y: Float)
    private external fun nativeOnPinch(ptr: Long, ratio: Float)
    private external fun nativeOnDoubleTap(ptr: Long, x: Float, y: Float)
    private external fun nativeLoadGltf(ptr: Long, path: String): Int
    private external fun nativeSetQuality(ptr: Long, tier: Int)

    companion object {
        init { System.loadLibrary("rd_jni") }
    }
}
