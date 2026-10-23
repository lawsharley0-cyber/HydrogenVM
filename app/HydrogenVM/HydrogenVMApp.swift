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
            .sheet(item: $recoveredCrash) { CrashReportView(report: $0) }
        }
    }
}
