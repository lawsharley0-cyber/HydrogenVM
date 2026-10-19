import SwiftUI
import UIKit

/// Lets the Run screen pin the interface to portrait (the emulated device draws
/// its own rotation inside a portrait framebuffer, like iOS 6 did).
@MainActor
final class OrientationLock {
    static let shared = OrientationLock()
    private(set) var mask: UIInterfaceOrientationMask = .all

    func lock(_ mask: UIInterfaceOrientationMask) {
        self.mask = mask
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: mask)) { _ in }
            scene.keyWindow?.rootViewController?.setNeedsUpdateOfSupportedInterfaceOrientations()
        }
    }
}

final class AppDelegate: NSObject, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
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

    var body: some Scene {
        WindowGroup {
            TabView {
                Tab("Library", systemImage: "square.grid.2x2") { LibraryView() }
                Tab("Diagnostics", systemImage: "stethoscope") { DiagnosticsView() }
            }
        }
    }
}
