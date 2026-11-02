import SwiftUI
import UniformTypeIdentifiers

extension HVMAppInfo: @unchecked Sendable {}

/// Installed apps live in Documents/Apps/<bundle id>/<Name>.app, with the
/// app's sandbox (Documents/, Library/, tmp/) beside the bundle. Any .ipa
/// dropped into Documents through Files or Finder is imported on refresh.
struct InstalledApp: Identifiable, Hashable, Sendable {
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
            do {
                let info = try HVMEngine.importIPA(staged.path(percentEncoded: false), intoDirectory: root)
                // @AppStorage defaults to on but writes nothing until toggled, so absent means on.
                let keepIpa = UserDefaults.standard.object(forKey: "keepIpaForReinstall") as? Bool ?? true
                if keepIpa {
                    let bundleId = info.bundleIdentifier
                    if let sha256 = staged.sha256 {
                        let appSupport = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
                        let ipaDir = appSupport.appendingPathComponent("HydrogenVM/IPAs/\(bundleId)", isDirectory: true)
                        try FileManager.default.createDirectory(at: ipaDir, withIntermediateDirectories: true)
                        let targetIPA = ipaDir.appendingPathComponent("\(sha256).ipa")
                        // Replace if exists (e.g. reinstall over same version)
                        try? FileManager.default.removeItem(at: targetIPA)
                        try FileManager.default.copyItem(at: staged, to: targetIPA)
                    }
                }
            }
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
        if let appSupport = try? FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: false) {
            let ipaDir = appSupport.appendingPathComponent("HydrogenVM/IPAs/\(app.info.bundleIdentifier)", isDirectory: true)
            try? FileManager.default.removeItem(at: ipaDir)
        }
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
    @AppStorage("keepIpaForReinstall") private var keepIpaForReinstall = true
    @AppStorage("hvm.zombies") private var zombies = false
    @State private var store = LibraryStore()
    @State private var pickingFile = false
    @State private var running: InstalledApp?
    @State private var browsing: InstalledApp?
    @State private var keepGoing = false
    @State private var testingAllGames = false
    /// A `hydrogenvm://run/<id>` link (Home Screen icon) waiting to be opened.
    @State private var router = LaunchRouter.shared

    var body: some View {
        NavigationStack {
            Group {
                if router.pending != nil {
                    // A Home Screen link is opening a game: stay black, never flash the library.
                    Color.black.ignoresSafeArea()
                } else if store.apps.isEmpty && store.importing.isEmpty {
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
                                } reinstall: { url in
                                    store.importIPA(at: url)
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
                        Toggle("Keep .ipa for reinstall", isOn: $keepIpaForReinstall)
                        Toggle("Debug: name freed objects (zombies)", isOn: $zombies)
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
            // While a game is running its full-screen cover hides this view, so the message is
            // shown from the cover instead (see `.fullScreenCover` below).
            .alert("Can't open that game", isPresented: Binding(get: { router.message != nil && running == nil }, set: { if !$0 { router.message = nil } })) {
                Button("OK") { router.message = nil }
            } message: { Text(router.message ?? "") }
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
                    try? mdText.write(to: reportsDir.appending(path: "batch_report.md"), atomically: true, encoding: .utf8)
                    try? mdText.write(to: reportsDir.appending(path: "latest_batch_report.md"), atomically: true, encoding: .utf8)

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
                    .alert("Can't open that game", isPresented: Binding(get: { router.message != nil }, set: { if !$0 { router.message = nil } })) {
                        Button("OK") { router.message = nil }
                    } message: { Text(router.message ?? "") }
            }
            .onChange(of: router.pending) { _, _ in openPendingLink() }
            .task { openPendingLink() }
            .sheet(item: $browsing) { app in
                FileBrowserView(app: app)
            }
            .sheet(isPresented: $testingAllGames) {
                BatchTestView(installedApps: store.apps)
            }
        }
    }

    /// Open the game a Home Screen icon asked for. Unknown or uninstalled ids get a
    /// clear message instead of an empty run screen.
    private func openPendingLink() {
        guard let url = router.pending else { return }
        router.pending = nil
        switch GameLink.resolve(url, installed: store.apps.map { $0.info.bundleIdentifier }) {
        case .failure(let error):
            router.message = error.message
        case .success(let id):
            guard let app = store.apps.first(where: { $0.info.bundleIdentifier == id }) else {
                router.message = GameLinkError.unknownGame(id).message
                return
            }
            guard app.info.runnable else {
                router.message = "\(app.title) is installed but cannot run here: \(app.info.problem)"
                return
            }
            switch GameLink.startDecision(wanted: app.info.bundleIdentifier, running: running?.info.bundleIdentifier) {
            case .start:
                running = app
            case .alreadyRunning:
                break
            case .busy:
                router.message = "Close \(running?.title ?? "the current game") first, then open \(app.title) again."
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
    let reinstall: (URL) -> Void

    @State private var showingReinstallConfirm = false
    @State private var showingReinstallMissing = false
    @State private var pickingReinstallIPA = false
    @State private var verifyResult: VerificationResult? = nil
    @State private var showingVerifyResult = false
    @State private var verifyError: String? = nil
    @State private var showingVerifyError = false
    @State private var makingClip = false

    private var storedIPAURL: URL? {
        guard let appSupport = try? FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: false) else { return nil }
        let ipaDir = appSupport.appendingPathComponent("HydrogenVM/IPAs/\(app.info.bundleIdentifier)", isDirectory: true)
        let files = (try? FileManager.default.contentsOfDirectory(at: ipaDir, includingPropertiesForKeys: nil)) ?? []
        return files.first { $0.pathExtension == "ipa" }
    }

    private func doVerify() {
        guard let ipa = storedIPAURL else {
            verifyError = "Stored .ipa missing. Cannot verify."
            showingVerifyError = true
            return
        }
        do {
            let res = try verifyAppFiles(appBundleURL: app.bundleDir, ipaURL: ipa)
            verifyResult = res
            showingVerifyResult = true
        } catch {
            verifyError = "Verification failed: \(error.localizedDescription)"
            showingVerifyError = true
        }
    }

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
            Button("Add to Home Screen", systemImage: "plus.app") { makingClip = true }
            Button("Verify files", systemImage: "checkmark.seal", action: doVerify)
            Button("Reinstall", systemImage: "arrow.down.app") {
                if storedIPAURL != nil {
                    showingReinstallConfirm = true
                } else {
                    showingReinstallMissing = true
                }
            }
            Button("Reset App Data", systemImage: "arrow.counterclockwise", action: reset)
            Button("Delete", systemImage: "trash", role: .destructive, action: delete)
        }
        .confirmationDialog("Reinstall \(app.title)?", isPresented: $showingReinstallConfirm, titleVisibility: .visible) {
            Button("Reinstall", role: .destructive) {
                if let ipa = storedIPAURL { reinstall(ipa) }
            }
            Button("Cancel", role: .cancel) {}
        } message: { Text("Your saves are kept.") }
        .alert("Stored .ipa missing", isPresented: $showingReinstallMissing) {
            Button("Choose .ipa…") { pickingReinstallIPA = true }
            Button("Cancel", role: .cancel) {}
        } message: { Text("Reinstalling needs the original .ipa. Choose it from your files.") }
        .fileImporter(isPresented: $pickingReinstallIPA, allowedContentTypes: [UTType(filenameExtension: "ipa") ?? .zip, .zip]) { result in
            if case .success(let url) = result { reinstall(url) }
        }
        .sheet(isPresented: $makingClip) { AddToHomeScreenSheet(app: app) }
        .alert("Verify failed", isPresented: $showingVerifyError) {
            Button("OK", role: .cancel) {}
        } message: { Text(verifyError ?? "") }
        .alert(verifyResult?.mismatched.isEmpty == true && verifyResult?.missing.isEmpty == true ? "All \(verifyResult?.matchCount ?? 0) files match" : "Verification Failed", isPresented: $showingVerifyResult, presenting: verifyResult) { res in
            if !res.mismatched.isEmpty || !res.missing.isEmpty {
                Button("Reinstall") {
                    if storedIPAURL != nil { showingReinstallConfirm = true } else { showingReinstallMissing = true }
                }
                Button("Cancel", role: .cancel) {}
            } else {
                Button("OK", role: .cancel) {}
            }
        } message: { res in
            if !res.mismatched.isEmpty || !res.missing.isEmpty {
                let m1 = res.missing.isEmpty ? "" : "\(res.missing.count) missing"
                let m2 = res.mismatched.isEmpty ? "" : "\(res.mismatched.count) mismatched"
                let both = [m1, m2].filter { !$0.isEmpty }.joined(separator: ", ")
                Text("\(both). Your saves are kept.")
            } else {
                Text("Your saves are kept.")
            }
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
