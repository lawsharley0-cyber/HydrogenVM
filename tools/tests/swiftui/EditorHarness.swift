import SwiftUI

// Only the isolated test app uses this model; no emulator or guest assets run.
struct InstalledApp {
    let dataDir: URL
    let bundleDir: URL
    let title: String
    var homeURL: URL { dataDir }
    var info: HarnessAppInfo { HarnessAppInfo(bundlePath: bundleDir.path) }
}

class HVMCrashJournal {
    static func installSignalHandlers() {}
    static func openSignalFile(_ path: String) {}
    static func closeSignalFile() {}
    static func availableMemory() -> UInt64 { return 1024 * 1024 * 1024 }
}

class HVMSession {
    static var emulatorCommit: String { "harness" }
}

class HVMEngine {
    static func decodeImageAtPath(_ path: String) -> CGImage? { return nil }
}

@main
struct EditorHarness: App {
    var body: some Scene { WindowGroup { HarnessHome() } }
}

struct HarnessHome: View {
    @State private var showPlayer = false
    @State private var file: FileItem?
    @State private var nestedEnabled = false
    @State private var diskText = ""
    @State private var prepared = false
    private let root = URL.documentsDirectory.appendingPathComponent("EditorFixtures")

    var body: some View {
        NavigationStack {
            List {
                Button("Open Player") { showPlayer = true }
                Button("Open JSON") { open("sample.json") }
                Button("Open Text") { open("sample.txt") }
                Text("nested=\(nestedEnabled ? "true" : "false")").accessibilityIdentifier("disk-json")
                Text(diskText).accessibilityIdentifier("disk-text")
            }
            .navigationTitle("Editor Test Fixtures")
        }
        .fullScreenCover(isPresented: $showPlayer) {
            PlayerHarness(app: InstalledApp(dataDir: root, bundleDir: root.appendingPathComponent("Harness.app"), title: "Synthetic Player"))
        }
        .onAppear {
            guard !prepared else { return }
            prepared = true
            UserDefaults.standard.set("Aspect Fit", forKey: "player.scaleMode")
            UserDefaults.standard.set("Bilinear", forKey: "player.filter")
            UserDefaults.standard.set(false, forKey: "fileEditor.autoSave")
            do {
                try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
                try Data("{\"nested\":{\"enabled\":false}}".utf8).write(to: root.appendingPathComponent("sample.json"), options: .atomic)
                try Data("original".utf8).write(to: root.appendingPathComponent("sample.txt"), options: .atomic)
                refresh()
            } catch { diskText = error.localizedDescription }
        }
        .sheet(item: $file, onDismiss: refresh) { item in FileEditorSheet(item: item) }
    }

    private func open(_ name: String) {
        file = FileItem(url: root.appendingPathComponent(name), isDirectory: false, size: 0, modified: nil)
    }

    private func refresh() {
        if let data = try? Data(contentsOf: root.appendingPathComponent("sample.json")),
           let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           let nested = object["nested"] as? [String: Any] {
            nestedEnabled = nested["enabled"] as? Bool ?? false
        }
        diskText = (try? String(contentsOf: root.appendingPathComponent("sample.txt"), encoding: .utf8)) ?? "read failed"
    }
}
