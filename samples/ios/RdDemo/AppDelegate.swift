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
        window.rootViewController?.view = RenderView(frame: UIScreen.main.bounds)
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}
