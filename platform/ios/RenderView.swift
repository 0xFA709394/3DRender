import QuartzCore
import UIKit

/// 3D 渲染视图（iOS 平台容器，Metal 后端）。
///
/// 线程模型：engine 调用全在主线程（与 MTKView 惯例一致），
/// 由 CADisplayLink 垂直同步驱动渲染循环。
///
/// 生命周期：didMoveToWindow（挂上窗口）→ startEngine；
/// 移出窗口 → stopEngine。layoutSubviews 同步 drawableSize 并通知 resize。
@objc public final class RenderView: UIView {
    /// 以 CAMetalLayer 作为 backing layer（UIView 的 Metal 渲染标准做法）。
    override public class var layerClass: AnyClass { CAMetalLayer.self }

    private var engine: OpaquePointer?       ///< rd_engine 实例
    private var link: CADisplayLink?         ///< 垂直同步回调
    private var lastTimestamp: CFTimeInterval = 0  ///< 上一帧时间戳（算 dt 用）
    private var recordFile: UnsafeMutablePointer<FILE>?  ///< 输入录制(非空则写)
    private var recordStart: CFTimeInterval = 0   ///< 录制起始时刻(时间戳基准)

    override public init(frame: CGRect) {
        super.init(frame: frame)
        // 按屏幕物理像素渲染（@2x/@3x），否则 drawableSize 偏小导致模糊
        contentScaleFactor = UIScreen.main.nativeScale
        isMultipleTouchEnabled = true
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        contentScaleFactor = UIScreen.main.nativeScale
        isMultipleTouchEnabled = true
    }

    override public func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            startEngine()
        } else {
            stopEngine()
        }
    }

    override public func layoutSubviews() {
        super.layoutSubviews()
        guard let engine else { return }
        // 尺寸变化：同步 layer 的 drawableSize（物理像素）并通知 engine
        let scale = contentScaleFactor
        let w = UInt32(max(1, bounds.width * scale))
        let h = UInt32(max(1, bounds.height * scale))
        (layer as? CAMetalLayer)?.drawableSize = CGSize(width: Int(w), height: Int(h))
        rd_engine_resize(engine, w, h)
    }

    /// 创建 engine + 设置表面（CAMetalLayer 指针）+ 启动 DisplayLink。
    /// passUnretained：layer 由 self 持有，engine 使用期间 view 必定存活，无需 retain。
    private func startEngine() {
        guard engine == nil else { return }
        guard let created = rd_engine_create(RD_BACKEND_METAL) else {
            print("RD: rd_engine_create 失败")
            return
        }
        let metalLayer = unsafeDowncast(layer, to: CAMetalLayer.self)
        let scale = contentScaleFactor
        let w = UInt32(max(1, bounds.width * scale))
        let h = UInt32(max(1, bounds.height * scale))
        let layerPtr = Unmanaged.passUnretained(metalLayer).toOpaque()
        // 缓存目录:Library/Caches/rd_cache(IBL 预滤波 + pipeline 缓存,二次启动提速)
        if let caches = FileManager.default.urls(for: .cachesDirectory,
                                                 in: .userDomainMask).first {
            rd_engine_set_cache_dir(created, caches.appendingPathComponent("rd_cache").path)
        }
        let result = rd_engine_set_surface(created, layerPtr, w, h)
        print("RD: set_surface \(w)x\(h) -> \(result)")
        guard result == RD_OK else {
            rd_engine_destroy(created)
            return
        }
        engine = created
        // 双击重置取景
        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(onDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2
        addGestureRecognizer(doubleTap)
        // common mode：滚动/追踪期间也不断帧
        let displayLink = CADisplayLink(target: self, selector: #selector(tick(_:)))
        displayLink.add(to: .main, forMode: .common)
        link = displayLink
        print("RD: render loop started")
    }

    /// 加载 glTF 模型（主线程；启动后调用一次即可）。成功返回 true。
    @discardableResult
    public func loadModel(_ path: String) -> Bool {
        guard let engine else { return false }
        let r = rd_engine_load_gltf(engine, path)
        print("RD: load_gltf -> \(r)")
        return r == RD_OK
    }

    /// 设置画质档（0=AUTO,1=HIGH,2=MID,3=LOW;主线程）。
    public func setQuality(_ tier: Int) {
        guard let engine else { return }
        let q: rd_quality_t
        switch tier {
        case 1: q = RD_QUALITY_HIGH
        case 2: q = RD_QUALITY_MID
        case 3: q = RD_QUALITY_LOW
        default: q = RD_QUALITY_AUTO
        }
        rd_engine_set_quality(engine, q)
        print("RD: set_quality -> \(tier)")
    }

    // ---- 触摸 → Orbit（rd_engine 主线程约定，直接调用）----
    /// UITouch → 稳定指针 id（按 touch 对象标识散列，跟踪期内稳定）
    private func touchId(_ touch: UITouch) -> Int32 {
        Int32(truncatingIfNeeded: ObjectIdentifier(touch).hashValue)
    }
    private func forwardTouches(_ touches: Set<UITouch>, action: rd_pointer_action_t) {
        guard let engine else { return }
        let scale = contentScaleFactor  // 逻辑点 → 物理像素
        for t in touches {
            let p = t.location(in: self)
            // 录制(归一化坐标,与 host 回放同格式)
            if let f = recordFile {
                let actionName: String
                switch action {
                case RD_POINTER_DOWN: actionName = "down"
                case RD_POINTER_MOVE: actionName = "move"
                case RD_POINTER_UP, RD_POINTER_CANCEL: actionName = "up"
                default: actionName = "move"
                }
                let tMs = Int64((CACurrentMediaTime() - recordStart) * 1000)
                let nx = Float(p.x / bounds.width), ny = Float(p.y / bounds.height)
                let line = "\(tMs) \(actionName) \(touchId(t)) \(nx) \(ny)\n"
                fwrite(line, 1, line.utf8.count, f)
            }
            rd_engine_on_pointer(engine, action, touchId(t),
                                 Float(p.x * scale), Float(p.y * scale))
        }
    }

    /// 输入录制开关;path 空串=停止。与 host --record 同格式(归一化坐标)。
    public func setInputRecording(_ path: String) {
        if path.isEmpty {
            if let f = recordFile { fclose(f) }
            recordFile = nil
            print("RD: 录制停止")
            return
        }
        recordFile = fopen(path, "wb")
        if let f = recordFile {
            recordStart = CACurrentMediaTime()
            let hdr = "# viewport \(Int(bounds.width)) \(Int(bounds.height))\n"
            fwrite(hdr, 1, hdr.utf8.count, f)
        }
        print("RD: 录制开始 -> \(path)")
    }
    override public func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwardTouches(touches, action: RD_POINTER_DOWN)
    }
    override public func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwardTouches(touches, action: RD_POINTER_MOVE)
    }
    override public func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwardTouches(touches, action: RD_POINTER_UP)
    }
    override public func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwardTouches(touches, action: RD_POINTER_CANCEL)
    }
    @objc private func onDoubleTap(_ g: UITapGestureRecognizer) {
        guard let engine else { return }
        let p = g.location(in: self)
        rd_engine_on_double_tap(engine, Float(p.x), Float(p.y))
    }

    /// 垂直同步回调：计算 dt 并渲染一帧。
    @objc private func tick(_ displayLink: CADisplayLink) {
        guard let engine else { return }
        // 首帧无历史时间戳，按 60fps 给个默认 dt
        let dt = lastTimestamp == 0 ? Float(1.0 / 60.0) : Float(displayLink.timestamp - lastTimestamp)
        lastTimestamp = displayLink.timestamp
        rd_engine_render_frame(engine, dt)
    }

    /// 停 DisplayLink 并销毁 engine（clear_surface → destroy 的顺序与 Android 侧一致）。
    private func stopEngine() {
        link?.invalidate()
        link = nil
        if let engine {
            rd_engine_clear_surface(engine)
            rd_engine_destroy(engine)
        }
        engine = nil
    }
}
