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

    /// A Home Screen icon opened while HydrogenVM is running or in the background.
    /// On a cold start this arrives before the first SwiftUI body, so the library is
    /// never drawn first; `.onOpenURL` covers the same case for the scene lifecycle.
    func application(_ app: UIApplication, open url: URL,
                     options: [UIApplication.OpenURLOptionsKey: Any] = [:]) -> Bool {
        // Only game links: .ipa imports keep using the library's own .onOpenURL.
        guard url.scheme?.lowercased() == GameLink.scheme else { return false }
        MainActor.assumeIsolated { LaunchRouter.shared.open(url) }
        return true
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
