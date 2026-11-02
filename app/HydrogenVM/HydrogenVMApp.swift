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
    func application(_ application: UIApplication,
                     configurationForConnecting connectingSceneSession: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        // Only game links: .ipa imports keep using the library's own .onOpenURL.
        if let url = options.urlContexts.first?.url, url.scheme?.lowercased() == GameLink.scheme {
            MainActor.assumeIsolated { LaunchRouter.shared.open(url) }
        }
        return UISceneConfiguration(name: nil, sessionRole: connectingSceneSession.role)
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
            .onOpenURL { LaunchRouter.shared.open($0) }
            .sheet(item: $recoveredCrash) { CrashReportView(report: $0) }
        }
    }
}
