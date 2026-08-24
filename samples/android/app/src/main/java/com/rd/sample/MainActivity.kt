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
        // assets → filesDir(内核 v1 只支持文件路径);遍历全部 glb(泛化模型集)
        fun copyAsset(name: String): File? = try {
            val dst = File(filesDir, name)
            if (!dst.exists()) assets.open(name).use { input ->
                dst.outputStream().use { input.copyTo(it) }
            }
            dst
        } catch (_: Throwable) { null }
        val models = (assets.list("") ?: emptyArray())
            .filter { it.endsWith(".glb") }
            .sorted()
            .mapNotNull { name -> copyAsset(name)?.let { name.removeSuffix(".glb") to it } }
        val helmet = models.firstOrNull { it.first == "DamagedHelmet" }?.second
        helmet?.let { renderView.loadModel(it.absolutePath) }

        // 「切换」按钮:画质档轮换(helmet 三档)+ 全部模型轮换(High 档)
        data class DemoState(val quality: Int, val model: File, val label: String)
        val states = mutableListOf<DemoState>()
        helmet?.let {
            states += DemoState(1, it, "High")
            states += DemoState(2, it, "Mid")
            states += DemoState(3, it, "Low")
        }
        for ((name, f) in models) {
            if (name == "DamagedHelmet") continue
            val label = when (name) {
                "CesiumMan", "Fox" -> "$name 骨骼动画"
                else -> name
            }
            states += DemoState(1, f, label)
        }
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
