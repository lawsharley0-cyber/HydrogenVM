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
            // A `hydrogenvm://run/<id>` link from a Home Screen icon arrives here, both when the
            // app is already running and when the link is what launched it: a scene-based app
            // receives the launch URL through the scene's connection options, which SwiftUI
            // delivers as this URL action. The app delegate's `configurationForConnecting` is not
            // used: its connection options are not the URL handoff, and returning a scene
            // configuration there replaces SwiftUI's own scene delegate.
            // The link is queued before the library resolves it, and the library stays black
            // while one is pending, so a game link never flashes the library.
            // `.ipa` imports keep going to the library's own `.onOpenURL`.
            .onOpenURL { url in
                guard GameLink.gameID(from: url) != nil else { return }
                LaunchRouter.shared.open(url)
                #if COLD_START_URL_TEST && targetEnvironment(simulator)
                // The contract with tools/test_cold_start_url.sh: the link arrived and is queued.
                ColdStartURLTest.record("queued", value: url.absoluteString)
                #endif
            }
        }
    }
}
