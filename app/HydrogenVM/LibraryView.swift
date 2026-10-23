import SwiftUI
import UniformTypeIdentifiers

/// Installed apps live in Documents/Apps/<bundle id>/<Name>.app, with the
/// app's sandbox (Documents/, Library/, tmp/) beside the bundle. Any .ipa
/// dropped into Documents through Files or Finder is imported on refresh.
struct InstalledApp: Identifiable, Hashable {
    let info: HVMAppInfo
    var id: String { info.bundlePath }
    /// Directory that contains the `.app` plus the guest sandbox (`Documents/`, `Library/`, `tmp/`).
    var homeURL: URL { URL(filePath: info.bundlePath).deletingLastPathComponent() }
    /// Guest sandbox root. Same directory as `homeURL`.
    var dataDir: URL { homeURL }
    /// The installed `.app` bundle.
    var bundleDir: URL { URL(filePath: info.bundlePath) }
    /// Name shown in the library: the display name, or the bundle name when that is empty.
    var title: String { info.displayName.isEmpty ? info.name : info.displayName }
}

@Observable
@MainActor
final class LibraryStore {
    private(set) var apps: [InstalledApp] = []
    private(set) var importing: [String] = []
    var lastError: String?

    static var appsDir: URL { URL.documentsDirectory.appending(path: "Apps", directoryHint: .isDirectory) }

    init() { refresh() }

    func refresh() {
        let fm = FileManager.default
        try? fm.createDirectory(at: Self.appsDir, withIntermediateDirectories: true)
        // Pick up .ipa files dropped into Documents (or the old Documents/Library).
        let dropped = [URL.documentsDirectory, URL.documentsDirectory.appending(path: "Library")]
            .flatMap { (try? fm.contentsOfDirectory(at: $0, includingPropertiesForKeys: nil)) ?? [] }
            .filter { $0.pathExtension.lowercased() == "ipa" }
        for ipa in dropped { importIPA(at: ipa, removeAfter: true) }
        reloadList()
    }

    private func reloadList() {
        let fm = FileManager.default
        var found: [InstalledApp] = []
        for dir in (try? fm.contentsOfDirectory(at: Self.appsDir, includingPropertiesForKeys: nil)) ?? [] {
            for item in (try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
            where item.pathExtension == "app" {
                if let info = HVMEngine.appInfo(atPath: item.path(percentEncoded: false)) { found.append(InstalledApp(info: info)) }
            }
        }
        apps = found.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
    }

    /// Copies (if needed) and extracts an .ipa off the main thread.
    func importIPA(at url: URL, removeAfter: Bool = false) {
        let name = url.lastPathComponent
        importing.append(name)
        let scoped = url.startAccessingSecurityScopedResource()
        // Security-scoped URLs only live while we hold access, so stage a copy first.
        let staged = URL.temporaryDirectory.appending(path: UUID().uuidString + ".ipa")
        do {
            try FileManager.default.copyItem(at: url, to: staged)
        } catch {
            if scoped { url.stopAccessingSecurityScopedResource() }
            importing.removeAll { $0 == name }
            lastError = "\(name): \(error.localizedDescription)"
            return
        }
        if scoped { url.stopAccessingSecurityScopedResource() }
        if removeAfter { try? FileManager.default.removeItem(at: url) }
        let root = Self.appsDir.path(percentEncoded: false)
        Task.detached {
            var message: String?
            do { _ = try HVMEngine.importIPA(staged.path(percentEncoded: false), intoDirectory: root) }
            catch { message = (error as NSError).localizedDescription }
            try? FileManager.default.removeItem(at: staged)
            let failure = message
            await MainActor.run {
                self.importing.removeAll { $0 == name }
                if let failure { self.lastError = "\(name): \(failure)" }
                self.reloadList()
            }
        }
    }

    func delete(_ app: InstalledApp) {
        try? FileManager.default.removeItem(at: app.homeURL)
        reloadList()
    }

    func resetData(_ app: InstalledApp) {
        for sub in ["Documents", "Library", "tmp"] {
            try? FileManager.default.removeItem(at: app.homeURL.appending(path: sub))
        }
    }
}

struct AppIcon: View {
    let path: String?
    var body: some View {
        Group {
            if let path, let image = AppIcon.load(path) {
                Image(uiImage: image).resizable()
            } else {
                Image(systemName: "app.dashed").resizable().padding(10).foregroundStyle(.secondary)
            }
        }
        .frame(width: 52, height: 52)
        .clipShape(.rect(cornerRadius: 12))
    }

    /// Old app icons are often Xcode-crushed (CgBI) PNGs that UIImage can't read;
    /// the engine's decoder handles both, via the bridge.
    static func load(_ path: String) -> UIImage? {
        if let img = UIImage(contentsOfFile: path) { return img }
        if let cg = HVMEngine.decodeImage(atPath: path) { return UIImage(cgImage: cg) }
        return nil
    }
}

struct LibraryView: View {
    @State private var store = LibraryStore()
    @State private var pickingFile = false
    @State private var running: InstalledApp?
    @State private var browsing: InstalledApp?
    @State private var keepGoing = false
    @State private var testingAllGames = false

    var body: some View {
        NavigationStack {
            Group {
                if store.apps.isEmpty && store.importing.isEmpty {
                    ContentUnavailableView {
                        Label("No apps yet", systemImage: "shippingbox")
                    } description: {
                        Text("Import a decrypted 32-bit .ipa (iOS 3–10). You can also drop .ipa files into HydrogenVM's folder in the Files app.")
                    } actions: {
                        Button("Import .ipa") { pickingFile = true }.buttonStyle(.borderedProminent)
                    }
                } else {
                    List {
                        if !store.importing.isEmpty {
                            Section("Importing") {
                                ForEach(store.importing, id: \.self) { name in
                                    HStack { ProgressView(); Text(name).padding(.leading, 8) }
                                }
                            }
                        }
                        Section {
                            ForEach(store.apps) { app in
                                LibraryAppRow(app: app) {
                                    if app.info.runnable { running = app }
                                } browse: {
                                    browsing = app
                                } reset: {
                                    store.resetData(app)
                                } delete: {
                                    store.delete(app)
                                }
                            }
                            .onDelete { idx in idx.map { store.apps[$0] }.forEach(store.delete) }
                        }
                    }
                    .refreshable { store.refresh() }
                }
            }
            .navigationTitle("HydrogenVM")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Menu("Options", systemImage: "gearshape") {
                        Button("Test All Games", systemImage: "play.grid.2x2") {
                            testingAllGames = true
                        }
                        Divider()
                        Toggle("Continue past missing functions", isOn: $keepGoing)
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Import", systemImage: "plus") { pickingFile = true }
                }
            }
            .fileImporter(isPresented: $pickingFile,
                          allowedContentTypes: [UTType(filenameExtension: "ipa") ?? .zip, .zip],
                          allowsMultipleSelection: true) { result in
                if case .success(let urls) = result { urls.forEach { store.importIPA(at: $0) } }
            }
            .onOpenURL { url in if url.pathExtension.lowercased() == "ipa" { store.importIPA(at: url) } }
            .alert("Import failed", isPresented: Binding(get: { store.lastError != nil }, set: { if !$0 { store.lastError = nil } })) {
                Button("OK") { store.lastError = nil }
            } message: { Text(store.lastError ?? "") }
            .task {
                // Headless Test All Games CI hook
                if ProcessInfo.processInfo.environment["HVM_TEST_ALL_GAMES"] != nil {
                    store.refresh()
                    let runner = BatchTestRunner()
                    let seconds = Int(ProcessInfo.processInfo.environment["HVM_TEST_ALL_GAMES_SECONDS"] ?? "10") ?? 10
                    let modeStr = (ProcessInfo.processInfo.environment["HVM_TEST_ALL_GAMES_MODE"] ?? "both").lowercased()
                    let mode = BatchRendererMode(rawValue: modeStr) ?? .both
                    let options = BatchTestOptions(secondsPerApp: seconds, rendererMode: mode, keepGoing: true)
                    RunModel.console("[hvm-batch-start] starting headless batch test for \(store.apps.filter(\.info.runnable).count) apps")
                    runner.startNewBatch(apps: store.apps, options: options)

                    while runner.status == .running {
                        try? await Task.sleep(for: .milliseconds(500))
                    }

                    let reportsDir = BatchTestRunner.reportsDir
                    try? FileManager.default.createDirectory(at: reportsDir, withIntermediateDirectories: true)

                    let jsonDict = runner.buildCompatReportJSON()
                    if let jsonData = try? JSONSerialization.data(withJSONObject: jsonDict, options: [.prettyPrinted, .sortedKeys]) {
                        try? jsonData.write(to: reportsDir.appending(path: "batch_report.json"), options: .atomic)
                        try? jsonData.write(to: reportsDir.appending(path: "latest_batch_report.json"), options: .atomic)
                    }

                    let mdText = runner.buildMarkdownReport()
                    try? mdText.write(to: reportsDir.appending(path: "batch_report.md"), options: .atomic)
                    try? mdText.write(to: reportsDir.appending(path: "latest_batch_report.md"), options: .atomic)

                    RunModel.console("[hvm-batch-exit] batch test finished with \(runner.results.count) results")
                    return
                }

                // CI hook (see RunModel.automation): launch an installed app by bundle id.
                if let id = ProcessInfo.processInfo.environment["HVM_AUTORUN"], running == nil {
                    store.refresh()
                    if let app = store.apps.first(where: { $0.info.bundleIdentifier == id }) {
                        running = app
                    } else {
                        RunModel.console("[hvm-exit] autorun: no installed app with bundle id \(id)")
                    }
                }
            }
            .fullScreenCover(item: $running) { app in
                RunView(app: app, keepGoing: keepGoing)
            }
            .sheet(item: $browsing) { app in
                FileBrowserView(app: app)
            }
            .sheet(isPresented: $testingAllGames) {
                BatchTestView(installedApps: store.apps)
            }
        }
    }
}

/// One library row. Split out of `LibraryView.body` so Swift 6 can type-check the button, swipe action, and context menu separately.
private struct LibraryAppRow: View {
    let app: InstalledApp
    let open: () -> Void
    let browse: () -> Void
    let reset: () -> Void
    let delete: () -> Void

    var body: some View {
        Button(action: open) {
            AppRow(app: app)
        }
        .buttonStyle(.plain)
        .swipeActions(edge: .leading) {
            Button("Files", systemImage: "folder", action: browse)
                .tint(.blue)
        }
        .contextMenu {
            Button("Files", systemImage: "folder", action: browse)
            Button("Reset App Data", systemImage: "arrow.counterclockwise", action: reset)
            Button("Delete", systemImage: "trash", role: .destructive, action: delete)
        }
    }
}

struct AppRow: View {
    let app: InstalledApp

    private var statusBadge: (text: String, color: Color, icon: String) {
        if app.info.runnable {
            return ("Ready · ARMv7", .green, "checkmark.circle.fill")
        } else if app.info.encrypted {
            return ("Encrypted · FairPlay Unsupported", .orange, "lock.fill")
        } else if app.info.problem.localizedCaseInsensitiveContains("64") {
            return ("64-Bit Only · Unsupported", .red, "xmark.octagon.fill")
        } else {
            let desc = app.info.problem.isEmpty ? "Unsupported" : app.info.problem
            return (desc, .orange, "exclamationmark.triangle.fill")
        }
    }

    var body: some View {
        HStack(spacing: 14) {
            AppIcon(path: app.info.iconPath)
            VStack(alignment: .leading, spacing: 4) {
                Text(app.title).font(.headline)
                Text([app.info.version.isEmpty ? nil : "v\(app.info.version)",
                      app.info.minimumOSVersion.isEmpty ? nil : "iOS \(app.info.minimumOSVersion)+",
                      app.info.bundleIdentifier].compactMap { $0 }.joined(separator: " · "))
                    .font(.caption).foregroundStyle(.secondary).lineLimit(1)

                let badge = statusBadge
                HStack(spacing: 4) {
                    Image(systemName: badge.icon)
                        .font(.caption2)
                    Text(badge.text)
                        .font(.caption2.weight(.semibold))
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(badge.color.opacity(0.15), in: Capsule())
                .foregroundStyle(badge.color)
            }
            Spacer()
            if app.info.runnable { Image(systemName: "play.circle.fill").font(.title2).foregroundStyle(.tint) }
        }
        .contentShape(.rect)
        .opacity(app.info.runnable ? 1 : 0.85)
    }
}
