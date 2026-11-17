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
            .filter { ["ipa", "apk"].contains($0.pathExtension.lowercased()) }
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
        // Shortcuts, Spotlight and Siri list one "Play <game>" per installed game.
        HydrogenShortcuts.updateAppShortcutParameters()
    }

    /// Copies (if needed) and extracts an .ipa (or an Android .apk) off the main thread.
    func importIPA(at url: URL, removeAfter: Bool = false) {
        if url.pathExtension.lowercased() == "apk" { importAPK(at: url, removeAfter: removeAfter); return }
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

    /// Copies and extracts an Android .apk off the main thread.
    func importAPK(at url: URL, removeAfter: Bool = false) {
        let name = url.lastPathComponent
        importing.append(name)
        let scoped = url.startAccessingSecurityScopedResource()
        let staged = URL.temporaryDirectory.appending(path: UUID().uuidString + ".apk")
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
                let info = try HVMEngine.importAPK(staged.path(percentEncoded: false), intoDirectory: root)
                // Kept like an .ipa, so Reinstall and Verify work for Android games too.
                let keep = UserDefaults.standard.object(forKey: "keepIpaForReinstall") as? Bool ?? true
                if keep, let sha256 = staged.sha256 {
                    let appSupport = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
                    let dir = appSupport.appendingPathComponent("HydrogenVM/IPAs/\(info.bundleIdentifier)", isDirectory: true)
                    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                    for old in (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [] {
                        try? FileManager.default.removeItem(at: old)
                    }
                    try FileManager.default.copyItem(at: staged, to: dir.appendingPathComponent("\(sha256).apk"))
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
    /// Default display mode for games that have no setting of their own (PlayerSettings).
    @AppStorage(PlayerSettings.scaleModeKey) private var defaultScaleMode = PlayerScaleMode.fit.rawValue
    @AppStorage("hvm.zombies") private var zombies = false
    @AppStorage(PlayerOverlay.settingKey) private var showDebugOverlay = PlayerOverlay.settingDefault
    @State private var store = LibraryStore()
    @State private var pickingFile = false
    @State private var running: InstalledApp?
    @State private var browsing: InstalledApp?
    /// The game whose page is open (tap a row); Play on the row or the page runs it.
    @State private var showing: InstalledApp?
    /// iPhone games (0) or Android games (1).
    @State private var platform = 0
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
                    ZStack {
                        LinenBackground()
                    ContentUnavailableView {
                        Label("No apps yet", systemImage: "shippingbox")
                    } description: {
                        Text("Import a decrypted 32-bit .ipa (iOS 3–10), or an Android .apk with 32-bit ARM code. You can also drop them into HydrogenVM's folder in the Files app.")
                    } actions: {
                        Button("Import a game") { pickingFile = true }
                            .buttonStyle(GlossButtonStyle(kind: .blue, height: 44))
                            .frame(maxWidth: 220)
                    }
                    .foregroundStyle(Gloss.ink)
                    }
                } else {
                    List {
                        Section {
                            Picker("Platform", selection: $platform) {
                                Text("iPhone").tag(0)
                                Text("Android").tag(1)
                            }
                            .pickerStyle(.segmented)
                            .listRowBackground(Color.clear)
                            .listRowInsets(EdgeInsets(top: 4, leading: 4, bottom: 4, trailing: 4))
                        }
                        if platform == 1 && !store.apps.contains(where: \.info.isAndroid) {
                            Section {
                                Text("Import an Android .apk with 32-bit ARM native code (lib/armeabi or armeabi-v7a). Android support is new: games built on EA's Blast engine (Dead Space) and Unity run so far.")
                                    .font(.subheadline).foregroundStyle(Gloss.inkSoft)
                            }
                        }
                        if !store.importing.isEmpty {
                            Section("Importing") {
                                ForEach(store.importing, id: \.self) { name in
                                    HStack { ProgressView(); Text(name).padding(.leading, 8) }
                                }
                            }
                        }
                        if !store.apps.filter({ $0.info.isAndroid == (platform == 1) }).isEmpty {
                        Section {
                            ForEach(store.apps.filter { $0.info.isAndroid == (platform == 1) }) { app in
                                LibraryAppRow(app: app) {
                                    if app.info.runnable { running = app }
                                } details: {
                                    showing = app
                                } browse: {
                                    browsing = app
                                } reset: {
                                    store.resetData(app)
                                } delete: {
                                    store.delete(app)
                                } reinstall: { url in
                                    store.importIPA(at: url)
                                }
                                .listRowBackground(Gloss.card)
                            }
                            .onDelete { idx in
                                let shown = store.apps.filter { $0.info.isAndroid == (platform == 1) }
                                idx.map { shown[$0] }.forEach(store.delete)
                            }
                        } header: {
                            Text("Your games")
                                .font(.system(size: 13, weight: .semibold))
                                .foregroundStyle(Gloss.sectionInk)
                                .shadow(color: Gloss.emboss, radius: 0, x: 0, y: 1)
                                .textCase(nil)
                        }
                        }
                    }
                    .listStyle(.insetGrouped)
                    .scrollContentBackground(.hidden)
                    .background(LinenBackground())
                    .refreshable { store.refresh() }
                }
            }
            .navigationTitle("HydrogenVM")
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(item: $showing) { app in
                GameDetailView(app: app) {
                    if app.info.runnable { running = app }
                } browse: {
                    browsing = app
                } reset: {
                    store.resetData(app)
                } delete: {
                    showing = nil
                    store.delete(app)
                }
            }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Menu("Options", systemImage: "gearshape") {
                        Button("Test All Games", systemImage: "play.grid.2x2") {
                            testingAllGames = true
                        }
                        Divider()
                        Toggle("Continue past missing functions", isOn: $keepGoing)
                        Toggle("Keep .ipa / .apk for reinstall", isOn: $keepIpaForReinstall)
                        Toggle("Show debug overlay while playing", isOn: $showDebugOverlay)
                        Text("Off: nothing of HydrogenVM is drawn over a game. Hold three fingers on the screen for a moment to open pause, volume, log and quit.")
                            .font(.footnote).foregroundStyle(.secondary)
                        Picker("Default display mode", selection: $defaultScaleMode) {
                            ForEach(PlayerScaleMode.allCases) { mode in
                                Text(mode.rawValue).tag(mode.rawValue)
                            }
                        }
                        Toggle("Debug: name freed objects (zombies)", isOn: $zombies)
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Import", systemImage: "plus") { pickingFile = true }
                }
            }
            .fileImporter(isPresented: $pickingFile,
                          allowedContentTypes: [UTType(filenameExtension: "ipa") ?? .zip,
                                                UTType(filenameExtension: "apk") ?? .zip, .zip],
                          allowsMultipleSelection: true) { result in
                if case .success(let urls) = result { urls.forEach { store.importIPA(at: $0) } }
            }
            .onOpenURL { url in if ["ipa", "apk"].contains(url.pathExtension.lowercased()) { store.importIPA(at: url) } }
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
            #if COLD_START_URL_TEST && targetEnvironment(simulator)
            ColdStartURLTest.record("error", value: error.message)
            #endif
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
                #if COLD_START_URL_TEST && targetEnvironment(simulator)
                if ColdStartURLTest.queued(url) {
                    ColdStartURLTest.record("routed", value: app.info.bundleIdentifier)
                }
                #endif
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
    let details: () -> Void
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

    /// "ipa" or "apk": the kind of file this game was imported from.
    private var packageExtension: String { app.info.isAndroid ? "apk" : "ipa" }

    private var storedIPAURL: URL? {
        guard let appSupport = try? FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: false) else { return nil }
        let ipaDir = appSupport.appendingPathComponent("HydrogenVM/IPAs/\(app.info.bundleIdentifier)", isDirectory: true)
        let files = (try? FileManager.default.contentsOfDirectory(at: ipaDir, includingPropertiesForKeys: nil)) ?? []
        return files.first { $0.pathExtension.lowercased() == packageExtension }
    }

    private func doVerify() {
        guard let ipa = storedIPAURL else {
            verifyError = "The original .\(packageExtension) isn't stored, so there is nothing to check against. Reinstall the game from its .\(packageExtension) to keep a copy."
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
        Button(action: details) {
            AppRow(app: app, play: app.info.runnable ? open : nil)
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
            Toggle(isOn: Binding(get: { RunModel.allowsInternet(for: app.info.bundleIdentifier) },
                                 set: { UserDefaults.standard.set($0, forKey: RunModel.internetKey(app.info.bundleIdentifier)) })) {
                Label("Allow Internet", systemImage: "globe")
            }
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
        .alert("Original .\(packageExtension) not stored", isPresented: $showingReinstallMissing) {
            Button("Choose .\(packageExtension)…") { pickingReinstallIPA = true }
            Button("Cancel", role: .cancel) {}
        } message: { Text("Reinstalling needs the game's original .\(packageExtension). Choose it from your files.") }
        .fileImporter(isPresented: $pickingReinstallIPA, allowedContentTypes: [UTType(filenameExtension: packageExtension) ?? .zip, .zip]) { result in
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
    /// Runs the game straight from the row; nil for games that cannot run here.
    var play: (() -> Void)? = nil

    /// One short status, in words a player reads at a glance.
    static func status(_ info: HVMAppInfo) -> (text: String, color: Color) {
        if info.runnable { return ("Ready", Gloss.ok) }
        if info.encrypted { return ("Encrypted", Gloss.warn) }
        if info.problem.localizedCaseInsensitiveContains("64") { return ("64-bit only", Gloss.bad) }
        return ("Can't run", Gloss.warn)
    }

    var body: some View {
        HStack(spacing: 12) {
            GlossIcon(path: app.info.iconPath, title: app.title, size: 48)
            VStack(alignment: .leading, spacing: 2) {
                Text(app.title).font(.system(size: 16, weight: .semibold)).foregroundStyle(Gloss.ink).lineLimit(1)
                Text([app.info.version.isEmpty ? nil : app.info.version,
                      app.info.isAndroid ? (app.info.engine.isEmpty ? "Android" : "Android · \(app.info.engine)")
                                         : (app.info.minimumOSVersion.isEmpty ? nil : "iOS \(app.info.minimumOSVersion)+")]
                        .compactMap { $0 }.joined(separator: " · "))
                    .font(.system(size: 13)).foregroundStyle(Gloss.inkSoft).lineLimit(1)
                if app.info.isAndroid && !app.info.engine.isEmpty && !app.info.engineSupported {
                    Label("\(app.info.engine) isn't supported yet", systemImage: "exclamationmark.triangle")
                        .font(.system(size: 12)).foregroundStyle(Gloss.warn).lineLimit(1)
                }
            }
            Spacer(minLength: 8)
            let s = Self.status(app.info)
            Text(s.text).font(.system(size: 12, weight: .semibold)).foregroundStyle(s.color)
            if let play {
                Button(action: play) {
                    Image(systemName: "play.fill")
                        .font(.system(size: 14, weight: .bold))
                        .frame(width: 44, height: 44)
                        .foregroundStyle(Gloss.link)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel("Play \(app.title)")
            } else {
                Image(systemName: "chevron.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(Gloss.chevron)
                    .accessibilityHidden(true)
            }
        }
        .padding(.vertical, 4)
        .contentShape(.rect)
        .opacity(app.info.runnable ? 1 : 0.85)
    }
}
