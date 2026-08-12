package com.rd.sample

import android.app.Activity
import android.os.Bundle
import android.widget.FrameLayout
import com.rd.renderer.RenderView

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
    }
}
