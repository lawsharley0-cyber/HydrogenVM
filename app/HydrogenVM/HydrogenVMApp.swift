import SwiftUI
import UIKit

final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        HVMCrashJournal.installSignalHandlers()
        HVMEngine.installPlatformServices()
        return true
    }

    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        MainActor.assumeIsolated { OrientationLock.shared.mask }
    }

    /// A cold start from a Home Screen icon. A scene-based app receives the link in the
    /// scene's connection options, before any SwiftUI view exists, so it can be queued here
    /// and the library stays hidden until it has been routed. A link that arrives while
    /// the app is already running comes through `.onOpenURL` instead. Not verified on a device.
    ///
    /// Check both `urlContexts` and `userActivities` (some system paths deliver a browsing
    /// activity). The returned configuration uses an explicit role-matched scene config so
    /// connection options are delivered to the scene lifecycle.
    func application(_ application: UIApplication,
                     configurationForConnecting connectingSceneSession: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        if let url = Self.gameLink(from: options) {
            // Queue before any SwiftUI view exists. MainActor is required for LaunchRouter.
            if Thread.isMainThread {
                Self.queueColdStart(url)
            } else {
                DispatchQueue.main.sync { Self.queueColdStart(url) }
            }
        }
        let config = UISceneConfiguration(name: "Default Configuration",
                                          sessionRole: connectingSceneSession.role)
        // Keep the system/SwiftUI scene delegate path; do not install a custom class that
        // would replace the WindowGroup hosting.
        return config
    }

    private static func gameLink(from options: UIScene.ConnectionOptions) -> URL? {
        if let url = options.urlContexts.first?.url,
           url.scheme?.lowercased() == GameLink.scheme {
            return url
        }
        for activity in options.userActivities {
            if let url = activity.webpageURL,
               url.scheme?.lowercased() == GameLink.scheme {
                return url
            }
        }
        return nil
    }

    private static func queueColdStart(_ url: URL) {
        LaunchRouter.shared.open(url)
        #if COLD_START_URL_TEST && targetEnvironment(simulator)
        // Record even if pending was already set; the marker is the contract with the test.
        ColdStartURLTest.record("queued", value: url.absoluteString)
        #endif
    }
}

@main
struct HydrogenVMApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    @State private var recoveredCrash: CrashReport?

    var body: some Scene {
        WindowGroup {
            TabView {
                Tab("Library", systemImage: "square.grid.2x2") { LibraryView() }
                Tab("Diagnostics", systemImage: "stethoscope") { DiagnosticsView() }
            }
            .task { recoveredCrash = CrashReport.recover() }
            .onOpenURL { url in
                #if COLD_START_URL_TEST && targetEnvironment(simulator)
                // The regression must use the queued scene URL, not a later delivery.
                #else
                LaunchRouter.shared.open(url)
                #endif
            }
            .sheet(item: $recoveredCrash) { CrashReportView(report: $0) }
        }
    }
}
