import BackgroundTasks
import SwiftUI
import UIKit
import UniformTypeIdentifiers

extension HVMAppInfo: @unchecked Sendable {}

/// Tracks one active archive import (`.ipa`, `.apk`, or `.obb`) for the library progress banner.
struct ImportProgressItem: Identifiable, Equatable, Sendable {
    let id: String
    let fileName: String
    let totalBytes: Int64
    var completedBytes: Int64
    var phase: String
    var backgroundContinued: Bool

    var fractionCompleted: Double {
        guard totalBytes > 0 else { return 0 }
        return min(max(Double(completedBytes) / Double(totalBytes), 0), 1)
    }
}

/// Coordinates iOS 26 `BGContinuedProcessingTask` registration and execution for large imports (> ~200 MB),
/// falling back cleanly to `UIApplication.beginBackgroundTask` when unavailable or on older iOS.
@MainActor
final class BackgroundImportCoordinator {
    static let shared = BackgroundImportCoordinator()
    static let largeImportThresholdBytes: Int64 = 200 * 1024 * 1024
    static let taskIdentifierPrefix = "com.lfull.HydrogenVM.import."

    private var pendingHandlers: [String: @Sendable (Progress) async -> Bool] = [:]

    /// Starts a background-protected import. For archives >= ~200 MB on iOS 26+, requests a
    /// `BGContinuedProcessingTaskRequest` so iOS shows system background progress and keeps running
    /// across app switches; falls back to `beginBackgroundTask` if submission fails or for smaller files.
    func runImport(
        id: String,
        title: String,
        subtitle: String,
        totalBytes: Int64,
        onProgress: @escaping @MainActor (Int64, String, Bool) -> Void,
        work: @escaping @Sendable (@escaping @Sendable (Int64, String) -> Void) async -> Bool
    ) {
        let wantsContinued = totalBytes >= Self.largeImportThresholdBytes
        if wantsContinued, #available(iOS 26.0, *) {
            let identifier = "\(Self.taskIdentifierPrefix)\(id)"
            let registered = BGTaskScheduler.shared.register(forTaskWithIdentifier: identifier, using: nil) { [weak self] task in
                guard let continued = task as? BGContinuedProcessingTask else {
                    task.setTaskCompleted(success: false)
                    return
                }
                Task { @MainActor [weak self] in
                    guard let self, let handler = self.pendingHandlers.removeValue(forKey: identifier) else {
                        continued.setTaskCompleted(success: false)
                        return
                    }
                    continued.progress.totalUnitCount = max(1, totalBytes)
                    var cancelled = false
                    continued.expirationHandler = {
                        cancelled = true
                    }
                    let ok = await handler(continued.progress)
                    continued.setTaskCompleted(success: ok && !cancelled)
                }
            }

            if registered {
                pendingHandlers[identifier] = { sysProgress in
                    await work { bytes, phase in
                        sysProgress.completedUnitCount = min(sysProgress.totalUnitCount, max(0, bytes))
                        Task { @MainActor in
                            onProgress(bytes, phase, true)
                        }
                    }
                }
                let request = BGContinuedProcessingTaskRequest(
                    identifier: identifier,
                    title: title,
                    subtitle: subtitle
                )
                request.strategy = .queue
                do {
                    try BGTaskScheduler.shared.submit(request)
                    onProgress(0, "Continuing in background…", true)
                    return
                } catch {
                    pendingHandlers.removeValue(forKey: identifier)
                }
            }
        }

        // Fallback path: standard foreground/background-task continuation.
        var bgTask: UIBackgroundTaskIdentifier = .invalid
        bgTask = UIApplication.shared.beginBackgroundTask(withName: "Import \(title)") {
            if bgTask != .invalid {
                UIApplication.shared.endBackgroundTask(bgTask)
                bgTask = .invalid
            }
        }
        Task.detached {
            _ = await work { bytes, phase in
                Task { @MainActor in
                    onProgress(bytes, phase, false)
                }
            }
            await MainActor.run {
                if bgTask != .invalid {
                    UIApplication.shared.endBackgroundTask(bgTask)
                    bgTask = .invalid
                }
            }
        }
    }
}

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
    /// Android packages imported through the .apk path (plain or split archives).
    nonisolated static let androidPackageExtensions: Set<String> = ["apk", "xapk", "apks", "apkm"]
    /// Every file kind the library imports.
    nonisolated static let importExtensions: Set<String> = androidPackageExtensions.union(["ipa", "obb"])
    /// The file picker's types, built once (a long literal of `UTType(...) ?? .zip` here was too slow
    /// for the Swift type checker).
    nonisolated static let importContentTypes: [UTType] = {
        var types: [UTType] = []
        for ext in ["ipa", "apk", "xapk", "apks", "apkm"] { types.append(UTType(filenameExtension: ext) ?? .zip) }
        types.append(UTType(filenameExtension: "obb") ?? .data)
        types.append(.zip)
        types.append(.data)
        return types
    }()
    private(set) var apps: [InstalledApp] = []
    private(set) var importing: [String] = []
    private(set) var importItems: [ImportProgressItem] = []
    var lastError: String?

    static var appsDir: URL { URL.documentsDirectory.appending(path: "Apps", directoryHint: .isDirectory) }

    init() { refresh() }

    func refresh() {
        AppIcon.clearCache()
        let fm = FileManager.default
        try? fm.createDirectory(at: Self.appsDir, withIntermediateDirectories: true)
        // Pick up .ipa, .apk, and .obb files dropped into Documents (or the old Documents/Library).
        let dropped = [URL.documentsDirectory, URL.documentsDirectory.appending(path: "Library")]
            .flatMap { (try? fm.contentsOfDirectory(at: $0, includingPropertiesForKeys: nil)) ?? [] }
            .filter { Self.importExtensions.contains($0.pathExtension.lowercased()) }
        for archive in dropped { importIPA(at: archive, removeAfter: true) }
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

    private func beginProgress(id: String, name: String, totalBytes: Int64) {
        importing.append(name)
        importItems.append(ImportProgressItem(
            id: id,
            fileName: name,
            totalBytes: max(1, totalBytes),
            completedBytes: 0,
            phase: "Staging \(name)…",
            backgroundContinued: totalBytes >= BackgroundImportCoordinator.largeImportThresholdBytes
        ))
    }

    private func updateProgress(id: String, completedBytes: Int64, phase: String, backgroundContinued: Bool) {
        guard let idx = importItems.firstIndex(where: { $0.id == id }) else { return }
        importItems[idx].completedBytes = min(importItems[idx].totalBytes, max(0, completedBytes))
        importItems[idx].phase = phase
        importItems[idx].backgroundContinued = backgroundContinued || importItems[idx].backgroundContinued
    }

    private func endProgress(id: String, name: String, failure: String?) {
        importing.removeAll { $0 == name }
        importItems.removeAll { $0.id == id }
        if let failure { lastError = "\(name): \(failure)" }
        reloadList()
    }

    /// Copies a file in 4 MB chunks while reporting byte progress.
    nonisolated private static func copyWithProgress(
        from source: URL,
        to destination: URL,
        baseBytes: Int64 = 0,
        phase: String,
        report: @Sendable (Int64, String) -> Void
    ) throws -> Int64 {
        let fm = FileManager.default
        if fm.fileExists(atPath: destination.path(percentEncoded: false)) {
            try fm.removeItem(at: destination)
        }
        let input = try FileHandle(forReadingFrom: source)
        defer { try? input.close() }
        fm.createFile(atPath: destination.path(percentEncoded: false), contents: nil)
        let output = try FileHandle(forWritingTo: destination)
        defer { try? output.close() }

        let chunkSize = 4 * 1024 * 1024
        var copied: Int64 = 0
        while true {
            let chunk = try input.read(upToCount: chunkSize) ?? Data()
            if chunk.isEmpty { break }
            try output.write(contentsOf: chunk)
            copied += Int64(chunk.count)
            report(baseBytes + copied, phase)
        }
        return copied
    }

    /// Copies (if needed) and extracts an .ipa, Android .apk, or Android .obb expansion file off the main thread.
    /// Archives over ~200 MB use `BGContinuedProcessingTask` on iOS 26+.
    func importIPA(at url: URL, removeAfter: Bool = false) {
        let ext = url.pathExtension.lowercased()
        // Split archives (.xapk/.apks/.apkm) hold a base .apk plus splits; the loader tells them
        // apart from a plain .apk by their contents, so they take the same path.
        if Self.androidPackageExtensions.contains(ext) { importAPK(at: url, removeAfter: removeAfter); return }
        if ext == "obb" { importOBB(at: url, removeAfter: removeAfter); return }

        let name = url.lastPathComponent
        let jobID = UUID().uuidString.lowercased()
        let scoped = url.startAccessingSecurityScopedResource()
        let fileSize = Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        // Security-scoped URLs only live while we hold access, so stage a copy first.
        let staged = URL.temporaryDirectory.appending(path: jobID + ".ipa")
        beginProgress(id: jobID, name: name, totalBytes: max(1, fileSize * 2))
        do {
            try FileManager.default.copyItem(at: url, to: staged)
        } catch {
            if scoped { url.stopAccessingSecurityScopedResource() }
            endProgress(id: jobID, name: name, failure: error.localizedDescription)
            return
        }
        if scoped { url.stopAccessingSecurityScopedResource() }
        if removeAfter { try? FileManager.default.removeItem(at: url) }

        let root = Self.appsDir.path(percentEncoded: false)
        let stagedSize = max(1, fileSize)
        BackgroundImportCoordinator.shared.runImport(
            id: jobID,
            title: "Importing \(name)",
            subtitle: "Extracting iOS app archive",
            totalBytes: stagedSize,
            onProgress: { [weak self] bytes, phase, bg in
                self?.updateProgress(id: jobID, completedBytes: bytes, phase: phase, backgroundContinued: bg)
            }
        ) { report in
            var message: String?
            do {
                report(stagedSize / 3, "Extracting \(name)…")
                let info = try HVMEngine.importIPA(staged.path(percentEncoded: false), intoDirectory: root)
                report((stagedSize * 2) / 3, "Archiving \(info.displayName.isEmpty ? name : info.displayName)…")
                // @AppStorage defaults to on but writes nothing until toggled, so absent means on.
                let keepIpa = UserDefaults.standard.object(forKey: "keepIpaForReinstall") as? Bool ?? true
                if keepIpa {
                    let bundleId = info.bundleIdentifier
                    if let sha256 = staged.sha256 {
                        let appSupport = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
                        let ipaDir = appSupport.appendingPathComponent("HydrogenVM/IPAs/\(bundleId)", isDirectory: true)
                        try FileManager.default.createDirectory(at: ipaDir, withIntermediateDirectories: true)
                        let targetIPA = ipaDir.appendingPathComponent("\(sha256).ipa")
                        _ = try Self.copyWithProgress(from: staged, to: targetIPA, baseBytes: (stagedSize * 2) / 3, phase: "Saving reinstall copy…", report: report)
                    }
                }
                report(stagedSize, "Finished \(name)")
            } catch {
                message = (error as NSError).localizedDescription
            }
            try? FileManager.default.removeItem(at: staged)
            let failure = message
            await MainActor.run {
                self.endProgress(id: jobID, name: name, failure: failure)
            }
            return failure == nil
        }
    }

    /// Copies and extracts an Android .apk off the main thread.
    func importAPK(at url: URL, removeAfter: Bool = false) {
        let name = url.lastPathComponent
        let jobID = UUID().uuidString.lowercased()
        let scoped = url.startAccessingSecurityScopedResource()
        let fileSize = Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        let staged = URL.temporaryDirectory.appending(path: jobID + ".apk")
        beginProgress(id: jobID, name: name, totalBytes: max(1, fileSize))
        do {
            try FileManager.default.copyItem(at: url, to: staged)
        } catch {
            if scoped { url.stopAccessingSecurityScopedResource() }
            endProgress(id: jobID, name: name, failure: error.localizedDescription)
            return
        }
        if scoped { url.stopAccessingSecurityScopedResource() }
        if removeAfter { try? FileManager.default.removeItem(at: url) }

        let root = Self.appsDir.path(percentEncoded: false)
        let stagedSize = max(1, fileSize)
        BackgroundImportCoordinator.shared.runImport(
            id: jobID,
            title: "Importing \(name)",
            subtitle: "Extracting Android package",
            totalBytes: stagedSize,
            onProgress: { [weak self] bytes, phase, bg in
                self?.updateProgress(id: jobID, completedBytes: bytes, phase: phase, backgroundContinued: bg)
            }
        ) { report in
            var message: String?
            do {
                report(stagedSize / 3, "Extracting \(name)…")
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
                    _ = try Self.copyWithProgress(
                        from: staged,
                        to: dir.appendingPathComponent("\(sha256).apk"),
                        baseBytes: (stagedSize * 2) / 3,
                        phase: "Saving reinstall copy…",
                        report: report
                    )
                }
                report(stagedSize, "Finished \(name)")
            } catch {
                message = (error as NSError).localizedDescription
            }
            try? FileManager.default.removeItem(at: staged)
            let failure = message
            await MainActor.run {
                self.endProgress(id: jobID, name: name, failure: failure)
            }
            return failure == nil
        }
    }

    /// Imports an Android `.obb` expansion file (`main.<ver>.<pkg>.obb` or `patch.<ver>.<pkg>.obb`)
    /// into the matching game's `Library/Caches/sdcard/Android/obb/<pkg>/` directory.
    func importOBB(at url: URL, removeAfter: Bool = false) {
        let name = url.lastPathComponent
        let jobID = UUID().uuidString.lowercased()
        let scoped = url.startAccessingSecurityScopedResource()
        let fileSize = Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        let staged = URL.temporaryDirectory.appending(path: jobID + "-" + name)
        beginProgress(id: jobID, name: name, totalBytes: max(1, fileSize))
        do {
            try FileManager.default.copyItem(at: url, to: staged)
        } catch {
            if scoped { url.stopAccessingSecurityScopedResource() }
            endProgress(id: jobID, name: name, failure: error.localizedDescription)
            return
        }
        if scoped { url.stopAccessingSecurityScopedResource() }
        if removeAfter { try? FileManager.default.removeItem(at: url) }

        let packageID = Self.parseOBBPackageIdentifier(from: name)
            ?? apps.first(where: \.info.isAndroid)?.info.bundleIdentifier
        guard let packageID else {
            try? FileManager.default.removeItem(at: staged)
            endProgress(id: jobID, name: name, failure: "Install the Android game (.apk) first or name the file main.<version>.<package>.obb.")
            return
        }

        let obbDir = Self.appsDir
            .appending(path: packageID, directoryHint: .isDirectory)
            .appending(path: "Library/Caches/sdcard/Android/obb/\(packageID)", directoryHint: .isDirectory)
        let targetOBB = obbDir.appending(path: name)
        let stagedSize = max(1, fileSize)

        BackgroundImportCoordinator.shared.runImport(
            id: jobID,
            title: "Importing \(name)",
            subtitle: "Copying OBB expansion data for \(packageID)",
            totalBytes: stagedSize,
            onProgress: { [weak self] bytes, phase, bg in
                self?.updateProgress(id: jobID, completedBytes: bytes, phase: phase, backgroundContinued: bg)
            }
        ) { report in
            var message: String?
            do {
                try FileManager.default.createDirectory(at: obbDir, withIntermediateDirectories: true)
                _ = try Self.copyWithProgress(
                    from: staged,
                    to: targetOBB,
                    baseBytes: 0,
                    phase: "Copying \(name)…",
                    report: report
                )
            } catch {
                message = (error as NSError).localizedDescription
            }
            try? FileManager.default.removeItem(at: staged)
            let failure = message
            await MainActor.run {
                self.endProgress(id: jobID, name: name, failure: failure)
            }
            return failure == nil
        }
    }

    /// Extracts `<package>` from standard OBB filenames (`main.123.com.example.game.obb` or `patch.123.com.example.game.obb`).
    nonisolated static func parseOBBPackageIdentifier(from fileName: String) -> String? {
        let lower = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard lower.lowercased().hasSuffix(".obb") else { return nil }
        let stem = String(lower.dropLast(4))
        let parts = stem.split(separator: ".")
        if parts.count >= 4,
           ["main", "patch"].contains(parts[0].lowercased()),
           parts[1].allSatisfy(\.isNumber) {
            return parts.dropFirst(2).joined(separator: ".")
        }
        return nil
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

/// Adaptive Gloss banner displayed at the top of the library while archives (especially large >200 MB
/// `.ipa` / `.apk` / `.obb` packages) are importing in the foreground or via `BGContinuedProcessingTask`.
struct ImportProgressBanner: View {
    let items: [ImportProgressItem]

    private static func formatBytes(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    var body: some View {
        GlossCard {
            VStack(alignment: .leading, spacing: 10) {
                ForEach(Array(items.enumerated()), id: \.element.id) { index, item in
                    if index > 0 { GlossRule() }
                    VStack(alignment: .leading, spacing: 6) {
                        HStack(spacing: 8) {
                            ProgressView()
                                .controlSize(.small)
                            Text(item.fileName)
                                .font(.system(size: 14, weight: .semibold))
                                .foregroundStyle(Gloss.ink)
                                .lineLimit(1)
                            Spacer(minLength: 4)
                            if item.backgroundContinued {
                                Text("Background")
                                    .font(.system(size: 11, weight: .semibold))
                                    .foregroundStyle(Gloss.link)
                                    .padding(.horizontal, 7)
                                    .padding(.vertical, 2)
                                    .background(Gloss.link.opacity(0.14), in: Capsule())
                            }
                            Text("\(Int((item.fractionCompleted * 100).rounded()))%")
                                .font(.system(size: 12, weight: .bold).monospacedDigit())
                                .foregroundStyle(Gloss.inkSoft)
                        }
                        ProgressView(value: item.fractionCompleted)
                            .tint(Gloss.link)
                        HStack {
                            Text(item.phase)
                                .font(.system(size: 12))
                                .foregroundStyle(Gloss.inkSoft)
                                .lineLimit(1)
                            Spacer()
                            if item.totalBytes > 1 {
                                Text("\(Self.formatBytes(item.completedBytes)) / \(Self.formatBytes(item.totalBytes))")
                                    .font(.system(size: 11).monospacedDigit())
                                    .foregroundStyle(Gloss.inkSoft)
                            }
                        }
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 10)
                }
            }
        }
        .accessibilityIdentifier("library-import-progress-banner")
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

    /// Decoded icons by file path. Reading and decoding an icon on every redraw made the
    /// library stutter while scrolling; each icon is now decoded once.
    nonisolated(unsafe) private static let cache = NSCache<NSString, UIImage>()

    /// The icon if it is already decoded; never touches the disk (safe in a view's body).
    nonisolated static func cached(_ path: String) -> UIImage? { cache.object(forKey: path as NSString) }

    /// Old app icons are often Xcode-crushed (CgBI) PNGs that UIImage can't read;
    /// the engine's decoder handles both, via the bridge. Decoded once, ready to draw.
    nonisolated static func load(_ path: String) -> UIImage? {
        if let hit = cached(path) { return hit }
        var image = UIImage(contentsOfFile: path)
        if image == nil, let cg = HVMEngine.decodeImage(atPath: path) { image = UIImage(cgImage: cg) }
        guard let image else { return nil }
        let ready = image.preparingForDisplay() ?? image
        cache.setObject(ready, forKey: path as NSString)
        return ready
    }

    /// Forget decoded icons (after an import or reinstall replaced one).
    nonisolated static func clearCache() { cache.removeAllObjects() }
}

struct LibraryView: View {
    @AppStorage("keepIpaForReinstall") private var keepIpaForReinstall = true
    /// Default display mode for games that have no setting of their own (PlayerSettings).
    @AppStorage(PlayerSettings.scaleModeKey) private var defaultScaleMode = PlayerScaleMode.fit.rawValue
    @AppStorage("hvm.zombies") private var zombies = false
    @AppStorage("player.safeModeAlwaysRetry") private var safeModeAlwaysRetry = false
    @AppStorage(PlayerOverlay.settingKey) private var showDebugOverlay = PlayerOverlay.settingDefault
    @State private var store = LibraryStore()
    @State private var pickingFile = false
    @State private var detailedRecording = false
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
    /// "icons": the Home-screen grid (default); "list": one row per game.
    @AppStorage("library.layout") private var layout = "icons"
    /// Bundle ids of the games played last, newest first (RecentlyPlayed).
    @AppStorage(RecentlyPlayed.key) private var recentRaw = ""

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
                        Text("Import a decrypted 32-bit .ipa (iOS 3–10), or an Android .apk / .obb with 32-bit ARM code. You can also drop them into HydrogenVM's folder in the Files app.")
                    } actions: {
                        Button("Import a game") { pickingFile = true }
                            .buttonStyle(GlossButtonStyle(kind: .blue, height: 44))
                            .frame(maxWidth: 220)
                    }
                    .foregroundStyle(Gloss.ink)
                    }
                } else if layout == "icons" {
                    iconGrid
                } else {
                    VStack(spacing: 0) {
                        if !store.importItems.isEmpty {
                            ImportProgressBanner(items: store.importItems)
                                .padding(.top, 10)
                                .padding(.bottom, 4)
                        }
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
                                    Text("Import an Android .apk with 32-bit ARM native code (lib/armeabi or armeabi-v7a) and optional .obb expansion archives. Android support is new: games built on EA's Blast engine (Dead Space) and Unity run so far.")
                                        .font(.subheadline).foregroundStyle(Gloss.inkSoft)
                                }
                            }
                            if !store.apps.filter({ $0.info.isAndroid == (platform == 1) }).isEmpty {
                            Section {
                                ForEach(store.apps.filter { $0.info.isAndroid == (platform == 1) }) { app in
                                    LibraryAppRow(app: app) {
                                        if app.info.runnable { running = app }
                                    } record: {
                                        detailedRecording = true
                                        running = app
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
                    }
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
                        Toggle("Always retry crashed games in Safe Mode", isOn: $safeModeAlwaysRetry)
                        Picker("Library layout", selection: $layout) {
                            Label("Icons", systemImage: "square.grid.3x3").tag("icons")
                            Label("List", systemImage: "list.bullet").tag("list")
                        }
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Import", systemImage: "plus") { pickingFile = true }
                }
            }
            .fileImporter(isPresented: $pickingFile,
                          allowedContentTypes: LibraryStore.importContentTypes,
                          allowsMultipleSelection: true) { result in
                if case .success(let urls) = result { urls.forEach { store.importIPA(at: $0) } }
            }
            .onOpenURL { url in if LibraryStore.importExtensions.contains(url.pathExtension.lowercased()) { store.importIPA(at: url) } }
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
            .fullScreenCover(item: $running, onDismiss: { detailedRecording = false }) { app in
                RunView(app: app, keepGoing: keepGoing, detailedRecording: detailedRecording)
                    .alert("Can't open that game", isPresented: Binding(get: { router.message != nil }, set: { if !$0 { router.message = nil } })) {
                        Button("OK") { router.message = nil }
                    } message: { Text(router.message ?? "") }
            }
            .onChange(of: router.pending) { _, _ in openPendingLink() }
            .onChange(of: running) { _, app in
                if let app { recentRaw = RecentlyPlayed.noted(app.info.bundleIdentifier, in: recentRaw) }
            }
            .task { openPendingLink() }
            .sheet(item: $browsing) { app in
                FileBrowserView(app: app)
            }
            .sheet(isPresented: $testingAllGames) {
                BatchTestView(installedApps: store.apps)
            }
        }
    }

    /// The games of the chosen platform.
    private var shownApps: [InstalledApp] { store.apps.filter { $0.info.isAndroid == (platform == 1) } }

    private var platformPicker: some View {
        Picker("Platform", selection: $platform) {
            Text("iPhone").tag(0)
            Text("Android").tag(1)
        }
        .pickerStyle(.segmented)
    }

    /// One game, as a Home-screen icon or a list row, with the same actions either way.
    private func libraryRow(_ app: InstalledApp, tile: Bool) -> some View {
        LibraryAppRow(app: app, tile: tile) {
            if app.info.runnable { running = app }
        } record: {
            detailedRecording = true
            running = app
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
    }

    private var gameCountText: String {
        let n = shownApps.count
        return "\(n) \(platform == 1 ? "Android" : "iPhone") game\(n == 1 ? "" : "s")"
    }

    /// The library as an iPhone OS 6 Home screen: icons in rows, then the recently played shelf.
    private var iconGrid: some View {
        ScrollView {
            VStack(spacing: 0) {
                if !store.importItems.isEmpty {
                    ImportProgressBanner(items: store.importItems)
                        .padding(.top, 10)
                }
                platformPicker
                    .padding(.horizontal, 12)
                    .padding(.top, 12)
                if platform == 1 && shownApps.isEmpty {
                    Text("Import an Android .apk with 32-bit ARM native code (lib/armeabi or armeabi-v7a) and optional .obb expansion archives. Android support is new: games built on EA's Blast engine (Dead Space) and Unity run so far.")
                        .font(.subheadline).foregroundStyle(Gloss.inkSoft)
                        .padding(.horizontal, 20).padding(.top, 16)
                }
                if !shownApps.isEmpty {
                    HStack(alignment: .firstTextBaseline) {
                        Text("Your games")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(Gloss.sectionInk)
                            .accessibilityAddTraits(.isHeader)
                        Spacer()
                        Text(gameCountText).font(.system(size: 12)).foregroundStyle(Gloss.inkSoft)
                    }
                    .shadow(color: Gloss.emboss, radius: 0, x: 0, y: 1)
                    .padding(.horizontal, 20)
                    .padding(.top, 16)
                    .padding(.bottom, 10)
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 80), spacing: 4)], spacing: 18) {
                        ForEach(shownApps) { app in libraryRow(app, tile: true) }
                    }
                    .padding(.horizontal, 10)
                    LibraryLegend().padding(.top, 18)
                }
            }
            .padding(.bottom, 16)
        }
        .background(LinenBackground())
        .refreshable { store.refresh() }
        .safeAreaInset(edge: .bottom) {
            let recent = RecentlyPlayed.apps(recentRaw, from: shownApps)
            if !recent.isEmpty {
                RecentShelf(apps: recent) { app in
                    if app.info.runnable { running = app } else { showing = app }
                }
                .padding(.horizontal, 10)
                .padding(.bottom, 6)
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
    /// Drawn as a Home-screen icon (the icon layout) instead of a list row.
    var tile: Bool = false
    let open: () -> Void
    let record: () -> Void
    let details: () -> Void
    let browse: () -> Void
    let reset: () -> Void
    let delete: () -> Void
    let reinstall: (URL) -> Void

    @State private var confirmRecording = false
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
            if tile {
                LibraryTile(app: app)
            } else {
                AppRow(app: app, play: app.info.runnable ? open : nil)
            }
        }
        .buttonStyle(.plain)
        .swipeActions(edge: .leading) {
            Button("Files", systemImage: "folder", action: browse)
                .tint(.blue)
        }
        .contextMenu {
            if app.info.runnable {
                Button("Record detailed replay", systemImage: "record.circle") { confirmRecording = true }
            }
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
        .confirmationDialog("Record this launch?", isPresented: $confirmRecording, titleVisibility: .visible) {
            Button("Record and play", action: record)
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The private replay can contain saves and network replies. GitHub reports include only the input recording.")
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
