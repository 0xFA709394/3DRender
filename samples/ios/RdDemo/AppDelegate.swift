import UIKit

/// 示例 App 入口：全屏 RenderView（Metal 后端）作为唯一内容。
@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
    private var renderView: RenderView?
    private var stateIndex = 0

    /// 状态机:画质档轮换(helmet 三档)+ 模型轮换(bundle 内全部 glb,High 档)。
    private struct DemoState {
        let quality: Int      // 1=High,2=Mid,3=Low
        let model: String     // bundle 资源名
        let label: String
    }
    /// 模型友好名(bundle 扫描顺序即轮换顺序)。
    private static let modelLabels: [String: String] = [
        "CesiumMan": "骨骼动画", "Fox": "Fox 骨骼动画", "Duck": "Duck",
        "BoomBox": "BoomBox", "WaterBottle": "WaterBottle", "Corset": "Corset",
        "Lantern": "Lantern",
    ]
    private func states() -> [DemoState] {
        var s = [DemoState(quality: 1, model: "DamagedHelmet", label: "High"),
                 DemoState(quality: 2, model: "DamagedHelmet", label: "Mid"),
                 DemoState(quality: 3, model: "DamagedHelmet", label: "Low")]
        // bundle 内全部 glb(除 helmet)按 High 档追加
        let extras = Bundle.main.paths(forResourcesOfType: "glb", inDirectory: nil)
            .map { (($0 as NSString).lastPathComponent as NSString).deletingPathExtension }
            .sorted()
        for name in extras where name != "DamagedHelmet" {
            s.append(DemoState(quality: 1, model: name,
                               label: AppDelegate.modelLabels[name] ?? name))
        }
        return s
    }
    @objc private func onSwitchTap() {
        stateIndex = (stateIndex + 1) % states().count
        applyCurrentState()
    }

    private var recording = false
    @objc private func onRecordLongPress(_ g: UILongPressGestureRecognizer) {
        guard g.state == .began, let renderView else { return }
        recording.toggle()
        if (recording) {
            let docs = FileManager.default.urls(for: .documentDirectory,
                                                in: .userDomainMask).first!
            renderView.setInputRecording(docs.appendingPathComponent("rd_input.log").path)
        } else {
            renderView.setInputRecording("")
        }
    }
    private func applyCurrentState() {
        guard let renderView else { return }
        let st = states()[stateIndex]
        renderView.setQuality(st.quality)
        if let path = Bundle.main.path(forResource: st.model, ofType: "glb") {
            renderView.loadModel(path)
        }
        print("RD: demo 状态 -> \(st.label)/\(st.model)")
    }

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
    ) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = UIViewController()
        // RenderView 的 surface/渲染生命周期由其自身 didMoveToWindow 驱动
        let renderView = RenderView(frame: UIScreen.main.bounds)
        window.rootViewController?.view = renderView
        // 「切换」按钮:画质档轮换(High→Mid→Low)+ 模型轮换(helmet↔cesium_man 若在包内)
        let button = UIButton(type: .system)
        button.setTitle("切换", for: .normal)
        button.backgroundColor = UIColor(white: 0, alpha: 0.35)
        button.setTitleColor(.white, for: .normal)
        button.layer.cornerRadius = 8
        let rootView = window.rootViewController!.view!
        rootView.addSubview(button)
        button.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            button.trailingAnchor.constraint(equalTo: rootView.trailingAnchor, constant: -20),
            button.bottomAnchor.constraint(equalTo: rootView.safeAreaLayoutGuide.bottomAnchor,
                                           constant: -20),
            button.widthAnchor.constraint(equalToConstant: 88),
            button.heightAnchor.constraint(equalToConstant: 36),
        ])
        button.addTarget(self, action: #selector(onSwitchTap), for: .touchUpInside)
        // 长按「切换」2s = 输入录制开关(写 Documents/rd_input.log,与 host 回放同格式)
        let longPress = UILongPressGestureRecognizer(target: self,
                                                     action: #selector(onRecordLongPress(_:)))
        longPress.minimumPressDuration = 2.0
        button.addGestureRecognizer(longPress)
        self.renderView = renderView
        window.makeKeyAndVisible()
        // surface 就绪(loadModel 内部等 engine 创建)后加载演示模型
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.applyCurrentState()
        }
        self.window = window
        return true
    }
}
