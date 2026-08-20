package com.rd.sample

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.widget.Button
import android.widget.FrameLayout
import com.rd.renderer.RenderView
import java.io.File

/**
 * 示例 Activity：全屏 RenderView。
 * 后端由 intent extra "backend" 选择（0=Vulkan，其他=GLES，默认 GLES），
 * 便于 adb 启动时切换验证：`am start -n com.rd.sample/.MainActivity --ei backend 0`。
 */
class MainActivity : Activity() {
    private lateinit var renderView: RenderView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val backend = if (intent.getIntExtra("backend", 2) == 0)
            RenderView.Backend.VULKAN else RenderView.Backend.GLES
        renderView = RenderView(this)
        renderView.setBackend(backend)
        setContentView(
            renderView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ),
        )
        // assets → filesDir(内核 v1 只支持文件路径);surface 就绪后 loadModel 内部投递
        val dst = File(filesDir, "DamagedHelmet.glb")
        try {
            if (!dst.exists()) assets.open("DamagedHelmet.glb").use { input ->
                dst.outputStream().use { input.copyTo(it) }
            }
            renderView.loadModel(dst.absolutePath)
        } catch (t: Throwable) {
            Log.w("RdSample", "演示模型缺失,仅显示清屏背景", t)
        }
        // cesium_man 资产存在时(开发者经 fetch_assets.sh 下载并拷贝)一并入队
        val cesium = File(filesDir, "CesiumMan.glb")
        val hasCesium = try {
            if (!cesium.exists()) assets.open("CesiumMan.glb").use { input ->
                cesium.outputStream().use { input.copyTo(it) }
            }
            true
        } catch (_: Throwable) { cesium.exists() }

        // 「切换」按钮:画质档轮换(High→Mid→Low)+ 模型轮换(cesium_man 存在时)
        data class DemoState(val quality: Int, val model: File, val label: String)
        val states = mutableListOf(
            DemoState(1, dst, "High"),
            DemoState(2, dst, "Mid"),
            DemoState(3, dst, "Low"),
        )
        if (hasCesium) states.add(DemoState(1, cesium, "High+骨骼动画"))
        var index = 0
        var recording = false
        val button = Button(this).apply {
            text = "切换"
            setOnClickListener {
                index = (index + 1) % states.size
                val st = states[index]
                renderView.setQuality(st.quality)
                renderView.loadModel(st.model.absolutePath)
                Log.i("RdSample", "demo 状态 -> ${st.label}")
            }
            // 长按 2s = 输入录制开关(写 filesDir/rd_input.log,与 host 回放同格式)
            setOnLongClickListener {
                recording = !recording
                renderView.setInputRecording(
                    if (recording) File(filesDir, "rd_input.log").absolutePath else "")
                Log.i("RdSample", "输入录制 -> $recording")
                true
            }
        }
        addContentView(
            button,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM or Gravity.END,
            ).apply { setMargins(0, 0, 48, 48) },
        )
    }
}
