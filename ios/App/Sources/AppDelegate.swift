import SwiftUI
import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        AppModel.shared.start()
        return true
    }
}

/// Phone screen. Shows the settings/status UI, or a black "Playing on external display" view
/// while an external display is connected (spec 6.6).
final class PhoneSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        let w = UIWindow(windowScene: ws)
        w.rootViewController = UIHostingController(rootView: PhoneRoot(model: AppModel.shared))
        w.makeKeyAndVisible()
        window = w
    }

    func sceneDidEnterBackground(_ scene: UIScene) {
        logW("lifecycle: background")
        Log.shared.flush()
    }

    func sceneWillEnterForeground(_ scene: UIScene) {
        logW("lifecycle: foreground")
    }
}

struct PhoneRoot: View {
    @ObservedObject var model: AppModel
    var body: some View {
        if model.externalDisplayConnected && model.phase == .ready {
            ZStack {
                Color.black.ignoresSafeArea()
                Text("Playing on external display").foregroundStyle(.white)
            }
        } else {
            RootView(model: model)
        }
    }
}

/// External display (UIWindowSceneSessionRoleExternalDisplayNonInteractive). The presenter
/// (step 7) will own a CAMetalLayer here; until then the screen is black.
final class ExternalDisplaySceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        let w = UIWindow(windowScene: ws)
        let vc = UIViewController()
        vc.view.backgroundColor = .black
        w.rootViewController = vc
        w.isHidden = false
        window = w
        let mode = ws.screen.currentMode?.size ?? .zero
        logW("display: external connected, \(Int(mode.width))x\(Int(mode.height))")
        UIApplication.shared.isIdleTimerDisabled = true
        DispatchQueue.main.async { AppModel.shared.externalDisplayConnected = true }
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        logW("display: external disconnected")
        UIApplication.shared.isIdleTimerDisabled = false
        DispatchQueue.main.async { AppModel.shared.externalDisplayConnected = false }
    }
}
