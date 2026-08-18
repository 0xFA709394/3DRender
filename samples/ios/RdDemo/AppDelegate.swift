import UIKit

/// 示例 App 入口：全屏 RenderView（Metal 后端）作为唯一内容。
@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?
    private var renderView: RenderView?
    private var stateIndex = 0

    /// 状态机:画质档轮换 + 模型轮换(cesium_man 存在时)。
    private struct DemoState {
        let quality: Int      // 1=High,2=Mid,3=Low
        let model: String     // bundle 资源名
        let label: String
    }
    private func states() -> [DemoState] {
        var s = [DemoState(quality: 1, model: "DamagedHelmet", label: "High"),
                 DemoState(quality: 2, model: "DamagedHelmet", label: "Mid"),
                 DemoState(quality: 3, model: "DamagedHelmet", label: "Low")]
        if Bundle.main.path(forResource: "CesiumMan", ofType: "glb") != nil {
            s.append(DemoState(quality: 1, model: "CesiumMan", label: "High+骨骼动画"))
        }
        return s
    }
    @objc private func onSwitchTap() {
        stateIndex = (stateIndex + 1) % states().count
        applyCurrentState()
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
