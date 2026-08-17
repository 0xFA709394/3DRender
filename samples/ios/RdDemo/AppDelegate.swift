import UIKit

/// 示例 App 入口：全屏 RenderView（Metal 后端）作为唯一内容。
@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
    ) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = UIViewController()
        // RenderView 的 surface/渲染生命周期由其自身 didMoveToWindow 驱动
        let renderView = RenderView(frame: UIScreen.main.bounds)
        window.rootViewController?.view = renderView
        window.makeKeyAndVisible()
        // surface 就绪(loadModel 内部等 engine 创建)后加载演示模型
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            if let path = Bundle.main.path(forResource: "DamagedHelmet", ofType: "glb") {
                renderView.loadModel(path)
            } else {
                print("RD: bundle 内无 DamagedHelmet.glb")
            }
        }
        self.window = window
        return true
    }
}
