import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        droid_bridge_raise_memory_limit()
        Log.info("launch: JIT available=\(droid_bridge_jit_available()) debugger=\(droid_bridge_debugger_attached())")
        var vaTotal: Int32 = 0, vaLargest: Int32 = 0
        droid_bridge_address_space(&vaTotal, &vaLargest)
        Log.info("address space: \(vaTotal) GiB reservable, largest piece \(vaLargest) GiB, RAM \(ProcessInfo.processInfo.physicalMemory >> 30) GiB")
        return true
    }

    /// The image download runs on a background URLSession; iOS relaunches the app to deliver its events.
    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void) {
        Downloader.shared.backgroundCompletion = completionHandler
        Downloader.shared.attach()
    }

    func application(_ application: UIApplication, configurationForConnecting connectingSceneSession: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        UISceneConfiguration(name: "Default", sessionRole: connectingSceneSession.role)
    }
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options connectionOptions: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        let w = UIWindow(windowScene: ws)
        w.rootViewController = MainViewController()
        w.makeKeyAndVisible()
        window = w
        handle(urls: connectionOptions.urlContexts)
    }

    func scene(_ scene: UIScene, openURLContexts URLContexts: Set<UIOpenURLContext>) {
        handle(urls: URLContexts)
    }

    /// Files opened with "Open in Android" / the share extension's hand-off URL.
    private func handle(urls: Set<UIOpenURLContext>) {
        for ctx in urls {
            let url = ctx.url
            if url.isFileURL {
                let dest = Paths.inbox.appendingPathComponent(url.lastPathComponent)
                let scoped = url.startAccessingSecurityScopedResource()
                try? FileManager.default.removeItem(at: dest)
                do { try FileManager.default.copyItem(at: url, to: dest) } catch { Log.info("open-in copy failed: \(error)") }
                if scoped { url.stopAccessingSecurityScopedResource() }
            }
        }
        Inbox.shared.process()
    }

    func sceneDidBecomeActive(_ scene: UIScene) {
        // Keep the screen on while Android is in front, like a game would.
        UIApplication.shared.isIdleTimerDisabled = true
        VirtualMachine.shared.enterForeground()
        Inbox.shared.process()
        FileBridge.shared.syncNow()
    }

    func sceneDidEnterBackground(_ scene: UIScene) {
        FileBridge.shared.syncNow()
        // iOS suspends us shortly after this; pause vCPUs so the guest doesn't see a stalled
        // CPU (RCU stall warnings) when we come back, and save Android's state in case iOS
        // ends the app while it is in the background.
        VirtualMachine.shared.enterBackground()
    }
}
