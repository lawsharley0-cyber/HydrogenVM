import SwiftUI
import UniformTypeIdentifiers
import SQLite3
import zlib

// Per-app file explorer & advanced Game Save Editor:
// - Save Editor Dashboard: auto-discovers game saves, NSUserDefaults plists, SQLite DBs,
//   JSON/INI configs, and binary saves, plus one-tap Save Snapshot Backup & Restore.
// - Plist & NSKeyedArchiver Editor: preserves CFKeyedArchiverUID references, inspects
//   $objects graphs, and decodes/edits nested Data plists/JSON/text in-place.
// - Interactive SQLite3 Database Editor: browse tables, edit cells in-place, run custom SQL.
// - Structured JSON Tree Editor & INI/CFG Key-Value Editor.
// - Binary / Hex Editor with Value Inspector (Int8/16/32/64, UInt, Float, Double, String),
//   Cheat Value Scanner, and transparent Zlib/Gzip decompression & recompression.

// MARK: - Model & File Detection

struct FileItem: Identifiable, Hashable {
    let url: URL
    let isDirectory: Bool
    let size: Int64
    let modified: Date?
    var id: URL { url }
    var name: String { url.lastPathComponent }
}

enum FileKind: Equatable {
    case folder, image, plist, sqlite, json, ini, text, compressed, binary

    static let textExtensions: Set<String> = [
        "txt", "xml", "lua", "js", "csv", "yml", "yaml", "log", "html", "htm", "css",
        "md", "dat.txt", "sh", "lst", "tsv", "fnt", "atlas", "plist.txt", "glsl", "vsh", "fsh",
    ]
    static let iniExtensions: Set<String> = [
        "ini", "cfg", "conf", "properties", "strings",
    ]
    static let sqliteExtensions: Set<String> = [
        "sqlite", "sqlite3", "db", "sqlitedb", "db3",
    ]
    static let imageExtensions: Set<String> = ["png", "jpg", "jpeg", "gif", "bmp", "tif", "tiff"]

    init(url: URL, isDirectory: Bool) {
        if isDirectory { self = .folder; return }
        let ext = url.pathExtension.lowercased()
        if Self.imageExtensions.contains(ext) { self = .image }
        else if Self.sqliteExtensions.contains(ext) || FileKind.looksLikeSQLite(url) { self = .sqlite }
        else if ext == "plist" || FileKind.looksLikePlist(url) { self = .plist }
        else if ext == "json" || FileKind.looksLikeJSON(url) { self = .json }
        else if Self.iniExtensions.contains(ext) { self = .ini }
        else if Self.textExtensions.contains(ext) || FileKind.looksLikeText(url) { self = .text }
        else if FileKind.looksLikeCompressed(url) { self = .compressed }
        else { self = .binary }
    }

    var symbol: String {
        switch self {
        case .folder: "folder.fill"
        case .image: "photo"
        case .plist: "list.bullet.rectangle.portrait"
        case .sqlite: "cylinder.split.1x2.fill"
        case .json: "curlybraces.square"
        case .ini: "slider.horizontal.3"
        case .text: "doc.text"
        case .compressed: "archivebox.fill"
        case .binary: "memorychip"
        }
    }

    var badgeLabel: String {
        switch self {
        case .folder: "Folder"
        case .image: "Image"
        case .plist: "Plist"
        case .sqlite: "SQLite DB"
        case .json: "JSON"
        case .ini: "Config"
        case .text: "Text"
        case .compressed: "Zlib/Gzip"
        case .binary: "Binary"
        }
    }

    private static func head(_ url: URL, _ n: Int) -> Data {
        guard let h = try? FileHandle(forReadingFrom: url) else { return Data() }
        defer { try? h.close() }
        return (try? h.read(upToCount: n)) ?? Data()
    }

    static func looksLikeSQLite(_ url: URL) -> Bool {
        let d = head(url, 16)
        return d.starts(with: Array("SQLite format 3".utf8))
    }

    static func looksLikePlist(_ url: URL) -> Bool {
        let d = head(url, 64)
        if d.starts(with: Array("bplist".utf8)) { return true }
        guard let s = String(data: d, encoding: .utf8) else { return false }
        return s.contains("<plist") || s.contains("<!DOCTYPE plist")
    }

    static func looksLikeJSON(_ url: URL) -> Bool {
        let d = head(url, 512)
        guard !d.isEmpty, !d.contains(0), let s = String(data: d, encoding: .utf8) else { return false }
        let trimmed = s.trimmingCharacters(in: .whitespacesAndNewlines)
        return (trimmed.hasPrefix("{") || trimmed.hasPrefix("[")) && (trimmed.contains(":") || trimmed.contains("\""))
    }

    static func looksLikeCompressed(_ url: URL) -> Bool {
        SaveCompressionFormat.detect(head(url, 4)) != .none
    }

    static func looksLikeText(_ url: URL) -> Bool {
        let d = head(url, 4096)
        if d.isEmpty { return true }
        if d.contains(0) { return false }
        return String(data: d, encoding: .utf8) != nil
    }
}

// MARK: - Compression Helpers (Zlib / Gzip)

// MARK: - Directory Model

@MainActor
@Observable
final class DirectoryModel {
    let url: URL
    private(set) var items: [FileItem] = []
    var error: String?

    init(url: URL) { self.url = url }

    func reload() {
        let keys: [URLResourceKey] = [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey, .isSymbolicLinkKey]
        do {
            let urls = try FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: keys, options: [])
            items = urls.map { u in
                let v = try? u.resourceValues(forKeys: Set(keys))
                return FileItem(url: u, isDirectory: v?.isDirectory ?? false, size: Int64(v?.fileSize ?? 0),
                                modified: v?.contentModificationDate)
            }
            .sorted { a, b in
                if a.isDirectory != b.isDirectory { return a.isDirectory }
                return a.name.localizedStandardCompare(b.name) == .orderedAscending
            }
        } catch {
            items = []
            self.error = error.localizedDescription
        }
    }

    private func run(_ what: () throws -> Void) {
        do { try what() } catch { self.error = error.localizedDescription }
        reload()
    }

    func delete(_ item: FileItem) { run { try FileManager.default.removeItem(at: item.url) } }

    func rename(_ item: FileItem, to name: String) {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard validName(clean), clean != item.name else { return }
        run { try FileManager.default.moveItem(at: item.url, to: url.appending(path: clean)) }
    }

    func duplicate(_ item: FileItem) {
        run { try FileManager.default.copyItem(at: item.url, to: freeName(for: item.name, suffix: " copy")) }
    }

    func newFolder(named name: String) {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard validName(clean) else { return }
        run { try FileManager.default.createDirectory(at: freeName(for: clean, suffix: ""), withIntermediateDirectories: false) }
    }

    func newFile(named name: String) {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard validName(clean) else { return }
        run { try Data().write(to: freeName(for: clean, suffix: "")) }
    }

    func importFiles(_ urls: [URL]) {
        run {
            for src in urls {
                let scoped = src.startAccessingSecurityScopedResource()
                defer { if scoped { src.stopAccessingSecurityScopedResource() } }
                let dst = freeName(for: src.lastPathComponent, suffix: "")
                try FileManager.default.copyItem(at: src, to: dst)
            }
        }
    }

    private func validName(_ name: String) -> Bool {
        guard !name.isEmpty, name != ".", name != "..", !name.contains("/"), !name.contains("\0") else {
            error = "Use a single file or folder name without path separators."
            return false
        }
        return true
    }

    private func freeName(for name: String, suffix: String) -> URL {
        let base = (name as NSString).deletingPathExtension
        let ext = (name as NSString).pathExtension
        var candidate = url.appending(path: suffix.isEmpty ? name : base + suffix + (ext.isEmpty ? "" : "." + ext))
        var n = 2
        while FileManager.default.fileExists(atPath: candidate.path(percentEncoded: false)) {
            candidate = url.appending(path: base + suffix + " \(n)" + (ext.isEmpty ? "" : "." + ext))
            n += 1
        }
        return candidate
    }
}

// MARK: - Root Container View

struct FileBrowserView: View {
    let app: InstalledApp
    @Environment(\.dismiss) private var dismiss

    enum Root: String, CaseIterable, Identifiable {
        case saveEditor = "Save Editor"
        case documents = "Documents"
        case library = "Library"
        case tmp = "tmp"
        case bundle = "App Bundle"
        var id: String { rawValue }
        var icon: String {
            switch self {
            case .saveEditor: "wand.and.stars"
            case .documents: "doc.on.doc"
            case .library: "books.vertical"
            case .tmp: "clock.arrow.circlepath"
            case .bundle: "shippingbox"
            }
        }
    }
    @State private var root: Root = .saveEditor

    private func rootURL(_ r: Root) -> URL {
        switch r {
        case .saveEditor: app.dataDir
        case .documents: app.dataDir.appending(path: "Documents")
        case .library: app.dataDir.appending(path: "Library")
        case .tmp: app.dataDir.appending(path: "tmp")
        case .bundle: app.bundleDir
        }
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                Picker("Location", selection: $root) {
                    ForEach(Root.allCases) { r in
                        Label(r.rawValue, systemImage: r.icon).tag(r)
                    }
                }
                .pickerStyle(.segmented)
                .padding(.horizontal)
                .padding(.vertical, 8)

                if root == .saveEditor {
                    SaveEditorDashboardView(app: app)
                } else {
                    DirectoryView(url: rootURL(root), title: root.rawValue, isRoot: true)
                        .id(root)
                }
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}

// MARK: - Save Editor Dashboard & Snapshot Manager

struct DiscoveredSaveFile: Identifiable, Hashable {
    let item: FileItem
    let relativePath: String
    let category: Category
    let kind: FileKind
    var id: URL { item.url }

    enum Category: String, CaseIterable {
        case keychain = "Keychain Game Data & Credentials"
        case preferences = "NSUserDefaults & Preferences"
        case databases = "Databases (SQLite)"
        case saveFiles = "Game Save Files & Worlds"
        case configs = "JSON & Config Files"
    }
}

struct SaveSnapshot: Identifiable {
    let url: URL
    let name: String
    let created: Date
    var id: URL { url }
}

struct SaveEditorDashboardView: View {
    let app: InstalledApp
    @State private var editingFile: FileItem?
    @State private var discovered: [DiscoveredSaveFile] = []
    @State private var snapshots: [SaveSnapshot] = []
    @State private var searchText = ""
    @State private var statusMessage: String?
    @State private var newSnapshotLabel = ""
    @State private var showingSnapshotPrompt = false

    private var backupsDir: URL {
        app.dataDir.appending(path: "SaveBackups")
    }

    private var filteredFiles: [DiscoveredSaveFile] {
        guard !searchText.isEmpty else { return discovered }
        return discovered.filter {
            $0.item.name.localizedCaseInsensitiveContains(searchText) ||
            $0.relativePath.localizedCaseInsensitiveContains(searchText) ||
            $0.kind.badgeLabel.localizedCaseInsensitiveContains(searchText)
        }
    }

    var body: some View {
        List {
            if let msg = statusMessage {
                Section {
                    HStack {
                        Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                        Text(msg).font(.subheadline)
                        Spacer()
                        Button("Dismiss") { statusMessage = nil }.font(.caption)
                    }
                }
            }

            // Snapshot Backup & Restore Section
            Section {
                Button {
                    newSnapshotLabel = "Snapshot \(snapshots.count + 1)"
                    showingSnapshotPrompt = true
                } label: {
                    Label("Create Save Snapshot Backup", systemImage: "camera.on.rectangle.fill")
                        .fontWeight(.medium)
                }

                if snapshots.isEmpty {
                    Text("No snapshots yet. Create a snapshot before modifying save files so you can restore in one tap.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(snapshots) { snap in
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(snap.name).font(.subheadline.weight(.medium))
                                Text(snap.created.formatted(date: .abbreviated, time: .shortened))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Restore") {
                                restoreSnapshot(snap)
                            }
                            .buttonStyle(.borderedProminent)
                            .controlSize(.small)
                        }
                        .swipeActions {
                            Button("Delete", role: .destructive) {
                                deleteSnapshot(snap)
                            }
                        }
                    }
                }
            } header: {
                Text("Save State Snapshots (Documents + Library)")
            }

            Section {
                HStack(alignment: .top, spacing: 10) {
                    Image(systemName: "info.circle.fill").foregroundStyle(.tint)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Important: Close the game before saving edits").font(.caption.weight(.semibold))
                        Text("If the game is still running in memory, it may overwrite save edits with RAM state upon exit.").font(.caption2).foregroundStyle(.secondary)
                    }
                }
            }

            // Discovered Save Files by Category
            if discovered.isEmpty {
                ContentUnavailableView(
                    "No Save Files Found Yet",
                    systemImage: "tray",
                    description: Text("Launch \(app.title) first so it creates its save files, preferences, or databases in Documents and Library.")
                )
            } else {
                ForEach(DiscoveredSaveFile.Category.allCases, id: \.rawValue) { cat in
                    let group = filteredFiles.filter { $0.category == cat }
                    if !group.isEmpty {
                        Section(cat.rawValue) {
                            ForEach(group) { entry in
                                Button { editingFile = entry.item } label: {
                                    HStack(spacing: 12) {
                                        Image(systemName: entry.kind.symbol)
                                            .font(.title3)
                                            .foregroundStyle(.tint)
                                            .frame(width: 28)
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(entry.item.name)
                                                .font(.body.weight(.medium))
                                                .lineLimit(1)
                                            Text(entry.relativePath)
                                                .font(.caption2)
                                                .foregroundStyle(.secondary)
                                                .lineLimit(1)
                                        }
                                        Spacer()
                                        VStack(alignment: .trailing, spacing: 2) {
                                            Text(entry.kind.badgeLabel)
                                                .font(.caption2.weight(.semibold))
                                                .padding(.horizontal, 6)
                                                .padding(.vertical, 2)
                                                .background(.tint.opacity(0.15), in: Capsule())
                                                .foregroundStyle(.tint)
                                            Text(ByteCountFormatter.string(fromByteCount: entry.item.size, countStyle: .file))
                                                .font(.caption2)
                                                .foregroundStyle(.secondary)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        .navigationTitle("\(app.title) — Save Editor")
        .searchable(text: $searchText, prompt: "Search saves, plists, databases…")
        .sheet(item: $editingFile, onDismiss: scanSandbox) { item in
            FileEditorSheet(item: item)
        }
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    scanSandbox()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
            }
        }
        .alert("New Save Snapshot", isPresented: $showingSnapshotPrompt) {
            TextField("Label", text: $newSnapshotLabel)
            Button("Create") { createSnapshot(label: newSnapshotLabel) }
            Button("Cancel", role: .cancel) {}
        }
        .onAppear { scanSandbox() }
    }

    private func scanSandbox() {
        var found: [DiscoveredSaveFile] = []
        let fm = FileManager.default
        let roots = ["Documents", "Library"]
        let basePath = app.dataDir.path(percentEncoded: false)
        let keys: [URLResourceKey] = [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey]

        for r in roots {
            let dirURL = app.dataDir.appending(path: r)
            guard let enumerator = fm.enumerator(
                at: dirURL,
                includingPropertiesForKeys: keys,
                options: [.skipsHiddenFiles]
            ) else { continue }

            for case let fileURL as URL in enumerator {
                let vals = try? fileURL.resourceValues(forKeys: Set(keys))
                if vals?.isDirectory == true { continue }
                let size = Int64(vals?.fileSize ?? 0)
                let item = FileItem(url: fileURL, isDirectory: false, size: size, modified: vals?.contentModificationDate)
                let kind = FileKind(url: fileURL, isDirectory: false)
                if kind == .image { continue }

                var rel = fileURL.path(percentEncoded: false)
                if rel.hasPrefix(basePath) {
                    rel = String(rel.dropFirst(basePath.count)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
                }

                let category: DiscoveredSaveFile.Category
                if rel.contains("Keychains") || fileURL.lastPathComponent == "hvm-keychain.plist" {
                    category = .keychain
                } else if rel.contains("Library/Preferences") || fileURL.pathExtension.lowercased() == "plist" {
                    category = .preferences
                } else if kind == .sqlite {
                    category = .databases
                } else if kind == .json || kind == .ini || kind == .text {
                    category = .configs
                } else {
                    category = .saveFiles
                }
                found.append(DiscoveredSaveFile(item: item, relativePath: rel, category: category, kind: kind))
            }
        }
        discovered = found.sorted { a, b in
            if a.category != b.category { return a.category.rawValue < b.category.rawValue }
            return a.relativePath.localizedStandardCompare(b.relativePath) == .orderedAscending
        }
        loadSnapshots()
    }

    private func loadSnapshots() {
        let fm = FileManager.default
        let keys: [URLResourceKey] = [.creationDateKey, .isDirectoryKey]
        guard let urls = try? fm.contentsOfDirectory(at: backupsDir, includingPropertiesForKeys: keys) else {
            snapshots = []
            return
        }
        snapshots = urls.compactMap { u -> SaveSnapshot? in
            let v = try? u.resourceValues(forKeys: Set(keys))
            guard v?.isDirectory == true, !u.lastPathComponent.hasPrefix(".") else { return nil }
            return SaveSnapshot(url: u, name: u.lastPathComponent, created: v?.creationDate ?? Date())
        }.sorted { $0.created > $1.created }
    }

    private func createSnapshot(label: String) {
        let cleanLabel = label.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "/", with: "-")
        let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let folderName = (cleanLabel.isEmpty ? stamp : "\(cleanLabel) (\(stamp))") + "-" + UUID().uuidString.prefix(8)
        let dest = backupsDir.appending(path: folderName)
        do {
            try SaveSnapshotStore.create(home: app.dataDir, destination: dest)
            statusMessage = "Created snapshot '\(folderName)'"
            loadSnapshots()
        } catch {
            statusMessage = "Snapshot error: \(error.localizedDescription)"
        }
    }

    private func restoreSnapshot(_ snap: SaveSnapshot) {
        do {
            // Preserve the current state as a separate snapshot before replacing it.
            let recovery = backupsDir.appending(path: "Before restore-" + UUID().uuidString)
            try SaveSnapshotStore.create(home: app.dataDir, destination: recovery)
            try SaveSnapshotStore.restore(snapshot: snap.url, home: app.dataDir)
            statusMessage = "Restored snapshot '\(snap.name)'"
            scanSandbox()
        } catch { statusMessage = "Restore error: \(error.localizedDescription)" }
    }

    private func deleteSnapshot(_ snap: SaveSnapshot) {
        do { try FileManager.default.removeItem(at: snap.url); loadSnapshots() }
        catch { statusMessage = "Delete error: \(error.localizedDescription)" }
    }
}

// MARK: - Unified File Editor Router

struct FileEditorSheet: View {
    let item: FileItem
    @State private var document: FileEditorSession

    init(item: FileItem) {
        self.item = item
        _document = State(initialValue: FileEditorSession(url: item.url))
    }

    var body: some View {
        NavigationStack {
            FileEditorDestinationView(item: item)
                .navigationBarTitleDisplayMode(.inline)
        }
        .id(document.resetID)
        .modifier(FileEditorControls())
        .environment(document)
        .onDisappear { document.saveAction = nil; document.requestClose = nil }
        .interactiveDismissDisabled(document.dirty)
    }
}

struct FileEditorDestinationView: View {
    let item: FileItem
    @State private var overrideMode: EditorMode?
    @Environment(FileEditorSession.self) private var document

    enum EditorMode: String, CaseIterable, Identifiable {
        case auto = "Smart Editor"
        case plist = "Plist"
        case sqlite = "SQLite"
        case json = "JSON"
        case ini = "Key-Value"
        case text = "Text"
        case binary = "Hex / Binary"
        var id: String { rawValue }
    }

    private var defaultKind: FileKind {
        FileKind(url: item.url, isDirectory: item.isDirectory)
    }

    var body: some View {
        Group {
            switch overrideMode ?? .auto {
            case .auto:
                switch defaultKind {
                case .folder:
                    DirectoryView(url: item.url, title: item.name, isRoot: false)
                case .image:
                    ImagePreviewView(url: item.url)
                case .plist:
                    PlistEditorView(url: item.url)
                case .sqlite:
                    SQLiteEditorView(url: item.url)
                case .json:
                    JSONEditorView(url: item.url)
                case .ini:
                    KeyValueEditorView(url: item.url)
                case .text:
                    TextEditorView(url: item.url)
                case .compressed, .binary:
                    BinaryEditorView(url: item.url)
                }
            case .plist:
                PlistEditorView(url: item.url)
            case .sqlite:
                SQLiteEditorView(url: item.url)
            case .json:
                JSONEditorView(url: item.url)
            case .ini:
                KeyValueEditorView(url: item.url)
            case .text:
                TextEditorView(url: item.url)
            case .binary:
                BinaryEditorView(url: item.url)
            }
        }
        .id(overrideMode ?? .auto)
        .safeAreaInset(edge: .top, spacing: 0) {
            if !item.isDirectory {
                HStack {
                    Menu {
                        Picker("Open As", selection: Binding(
                            get: { overrideMode ?? .auto },
                            set: { document.ready = false; overrideMode = ($0 == .auto) ? nil : $0 }
                        )) {
                            ForEach(EditorMode.allCases) { mode in
                                Text(mode.rawValue).tag(mode)
                            }
                        }
                    } label: {
                        Label("Open As", systemImage: "slider.horizontal.2.square")
                    }
                    .disabled(document.dirty)
                    Spacer()
                }
                .font(.caption).padding(.horizontal).padding(.vertical, 6).background(.bar)
            }
        }
    }
}

// MARK: - Directory Listing

struct DirectoryView: View {
    let url: URL
    let title: String
    let isRoot: Bool

    @State private var editingFile: FileItem?
    @State private var model: DirectoryModel
    @State private var importing = false
    @State private var prompt: Prompt?
    @State private var promptText = ""

    enum Prompt: Identifiable {
        case newFolder, newFile, rename(FileItem)
        var id: String {
            switch self {
            case .newFolder: "newFolder"
            case .newFile: "newFile"
            case .rename(let f): "rename:\(f.id)"
            }
        }
        var title: String {
            switch self {
            case .newFolder: "New Folder"
            case .newFile: "New File"
            case .rename: "Rename"
            }
        }
    }

    init(url: URL, title: String, isRoot: Bool) {
        self.url = url
        self.title = title
        self.isRoot = isRoot
        _model = State(initialValue: DirectoryModel(url: url))
    }

    var body: some View {
        List {
            if model.items.isEmpty {
                ContentUnavailableView("Empty Folder", systemImage: "folder",
                                       description: Text("Use the + menu to import or create files."))
            }
            ForEach(model.items) { item in
                Group {
                    if item.isDirectory {
                        NavigationLink(value: item) { FileRow(item: item) }
                    } else {
                        Button { editingFile = item } label: { FileRow(item: item) }
                            .foregroundStyle(.primary)
                    }
                }
                .contextMenu { itemMenu(item) }
                .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                    Button(role: .destructive) { model.delete(item) } label: {
                        Label("Delete", systemImage: "trash")
                    }
                    Button {
                        promptText = item.name
                        prompt = .rename(item)
                    } label: {
                        Label("Rename", systemImage: "pencil")
                    }
                    .tint(.orange)
                }
            }
        }
        .navigationTitle(title)
        .navigationDestination(for: FileItem.self) { item in
            if item.isDirectory {
                DirectoryView(url: item.url, title: item.name, isRoot: false)
            }
        }
        .sheet(item: $editingFile, onDismiss: { model.reload() }) { item in
            FileEditorSheet(item: item)
        }
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Menu {
                    Button { importing = true } label: {
                        Label("Import Files…", systemImage: "square.and.arrow.down")
                    }
                    Divider()
                    Button {
                        promptText = "untitled.txt"
                        prompt = .newFile
                    } label: {
                        Label("New File…", systemImage: "doc.badge.plus")
                    }
                    Button {
                        promptText = "New Folder"
                        prompt = .newFolder
                    } label: {
                        Label("New Folder…", systemImage: "folder.badge.plus")
                    }
                } label: {
                    Image(systemName: "plus")
                }

                Button { model.reload() } label: {
                    Image(systemName: "arrow.clockwise")
                }
            }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item], allowsMultipleSelection: true) { res in
            if case .success(let urls) = res { model.importFiles(urls) }
        }
        .alert(
            prompt?.title ?? "",
            isPresented: Binding(get: { prompt != nil }, set: { if !$0 { prompt = nil } }),
            presenting: prompt
        ) { p in
            TextField("Name", text: $promptText)
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
            Button("OK") {
                commitPrompt(p)
                prompt = nil
            }
            Button("Cancel", role: .cancel) { prompt = nil }
        }
        .alert("Error", isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })) {
            Button("OK", role: .cancel) { model.error = nil }
        } message: {
            Text(model.error ?? "")
        }
        .onAppear { model.reload() }
    }

    @ViewBuilder
    private func itemMenu(_ item: FileItem) -> some View {
        ShareLink(item: item.url) {
            Label("Export / Share…", systemImage: "square.and.arrow.up")
        }
        Button {
            promptText = item.name
            prompt = .rename(item)
        } label: {
            Label("Rename…", systemImage: "pencil")
        }
        Button { model.duplicate(item) } label: {
            Label("Duplicate", systemImage: "plus.square.on.square")
        }
        Divider()
        Button(role: .destructive) { model.delete(item) } label: {
            Label("Delete", systemImage: "trash")
        }
    }

    private func commitPrompt(_ p: Prompt) {
        switch p {
        case .newFolder: model.newFolder(named: promptText)
        case .newFile: model.newFile(named: promptText)
        case .rename(let item): model.rename(item, to: promptText)
        }
    }
}

private struct PromptInputHook: View {
    @Binding var text: String
    var body: some View { EmptyView() }
}

private struct FileRow: View {
    let item: FileItem
    var body: some View {
        let kind = FileKind(url: item.url, isDirectory: item.isDirectory)
        HStack(spacing: 12) {
            Image(systemName: kind.symbol)
                .font(.title3)
                .foregroundStyle(item.isDirectory ? Color.accentColor : Color.secondary)
                .frame(width: 26)
            VStack(alignment: .leading, spacing: 2) {
                Text(item.name).lineLimit(1)
                HStack(spacing: 8) {
                    if !item.isDirectory {
                        Text(kind.badgeLabel)
                            .font(.caption2.weight(.medium))
                            .foregroundStyle(.tint)
                        Text(ByteCountFormatter.string(fromByteCount: item.size, countStyle: .file))
                    }
                    if let m = item.modified {
                        Text(m, style: .relative)
                    }
                }
                .font(.caption)
                .foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Image Preview

private struct ImagePreviewView: View {
    let url: URL
    var body: some View {
        GeometryReader { geo in
            if let data = try? Data(contentsOf: url), let img = UIImage(data: data) {
                ScrollView([.horizontal, .vertical]) {
                    Image(uiImage: img)
                        .interpolation(.none)
                        .resizable()
                        .scaledToFit()
                        .frame(maxWidth: geo.size.width, maxHeight: geo.size.height)
                }
            } else {
                ContentUnavailableView("Cannot Load Image", systemImage: "photo")
            }
        }
        .navigationTitle(url.lastPathComponent)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) { ShareLink(item: url) }
        }
    }
}

// MARK: - Keychain & Save File Auto-Synchronization

enum KeychainSaveSync {
    static func syncFileToKeychainIfNeeded(fileURL: URL, text: String) {
        let filename = fileURL.lastPathComponent
        guard filename == "backup.txt" || text.contains("coins=") || text.contains("bux=") else { return }

        var cur = fileURL.deletingLastPathComponent()
        var keychainURL: URL?
        for _ in 0..<5 {
            let candidate = cur.appending(path: "Library/Keychains/hvm-keychain.plist")
            if FileManager.default.fileExists(atPath: candidate.path(percentEncoded: false)) {
                keychainURL = candidate
                break
            }
            cur = cur.deletingLastPathComponent()
        }
        guard let kURL = keychainURL,
              let plistData = try? Data(contentsOf: kURL),
              var plistArray = (try? PropertyListSerialization.propertyList(from: plistData, format: nil)) as? [[String: Any]] else {
            return
        }
        guard let textData = text.data(using: .utf8) else { return }
        var modified = false
        for i in 0..<plistArray.count {
            var item = plistArray[i]
            let svce = item["svce"] as? String ?? ""
            if svce == "com.eeenmachine.tinytowers.gameData" || svce.contains("gameData") || (item["v_Data"] as? Data).flatMap({ String(data: $0, encoding: .utf8) })?.contains("coins=") == true {
                item["v_Data"] = textData
                plistArray[i] = item
                modified = true
            }
        }
        if modified {
            if let outData = try? PropertyListSerialization.data(fromPropertyList: plistArray, format: .xml, options: 0) {
                try? outData.write(to: kURL, options: .atomic)
            }
        }
    }

    @MainActor
    static func syncKeychainToFileIfNeeded(keychainURL: URL, plistRoot: PlistNode) {
        guard keychainURL.lastPathComponent == "hvm-keychain.plist" else { return }
        guard case .array(let items) = plistRoot.value else { return }
        for item in items {
            guard case .dict(let entries) = item.value else { continue }
            var isTower = false
            var payloadData: Data?
            for entry in entries {
                if entry.key == "svce", case .string(let s) = entry.value, s.contains("gameData") {
                    isTower = true
                }
                if entry.key == "v_Data", case .data(let d) = entry.value {
                    payloadData = d
                }
            }
            if isTower, let data = payloadData, let s = String(data: data, encoding: .utf8), s.contains("coins=") {
                let dataDir = keychainURL.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
                let backupURL = dataDir.appending(path: "Documents/backup.txt")
                try? data.write(to: backupURL, options: .atomic)
            }
        }
    }
}

// MARK: - Text Editor

struct TextEditorView: View {
    let url: URL
    @State private var text = ""
    @State private var encoding: String.Encoding = .utf8
    @State private var byteOrderMark = Data()
    @State private var compression: SaveCompressionFormat = .none
    @Environment(FileEditorSession.self) private var document
    @State private var loaded = false
    @State private var error: String?

    var body: some View {
        VStack(spacing: 0) {
            if compression != .none {
                HStack {
                    Image(systemName: "archivebox.fill")
                    Text("Compressed (\(compression.rawValue)) — will re-compress on save")
                    Spacer()
                }
                .font(.caption)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(Color.orange.opacity(0.15))
            }
            TextEditor(text: Binding(get: { text }, set: { text = $0; document.markChanged() }))
                .modifier(FileEditorTextFocus())
                .accessibilityIdentifier("plain-text-editor")
                .font(.system(.footnote, design: .monospaced))
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
        }
        .navigationTitle(url.lastPathComponent)
        .safeAreaInset(edge: .top, spacing: 0) {
            HStack(spacing: 18) {
                ShareLink(item: url)
                Spacer()
            }
        }
        .modifier(FileEditorRegistration(save: save))
        .editorSaveBottomBar()
        .alert("Save Failed", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear { if !loaded { load(); loaded = true } }
    }

    private func load() {
        document.ready = false
        do {
            let (payload, fmt) = try SaveCompressionFormat.decode(document.store.load())
            compression = fmt
            let decoded = try SaveTextCodec.decode(payload)
            text = decoded.text
            encoding = decoded.encoding
            byteOrderMark = decoded.byteOrderMark
            document.dirty = false
            document.ready = true
        } catch { self.error = error.localizedDescription }
    }

    private func save() -> Bool {
        do {
            guard let raw = text.data(using: encoding, allowLossyConversion: false) else {
                throw CocoaError(.fileWriteInapplicableStringEncoding)
            }
            let out = try SaveCompressionFormat.encode(byteOrderMark + raw, format: compression)
            try document.commit(out)
            KeychainSaveSync.syncFileToKeychainIfNeeded(fileURL: url, text: text)

            return true
        } catch {
            document.saveError = error.localizedDescription
            return false
        }
    }
}

// MARK: - Key-Value / INI / CFG / Strings Editor

struct KeyValueEntry: Identifiable {
    let id = UUID()
    var section: String
    var key: String
    var value: String
    var comment: String?
    var sourceLine: Int? = nil
    var originalValue: String? = nil
}

struct KeyValueEditorView: View {
    let url: URL
    @State private var entries: [KeyValueEntry] = []
    @State private var originalLines: [String] = []
    @State private var originalEntryLines: Set<Int> = []
    @State private var searchText = ""
    @State private var rawMode = false
    @State private var rawText = ""
    @State private var encoding: String.Encoding = .utf8
    @State private var byteOrderMark = Data()
    @State private var compression: SaveCompressionFormat = .none
    @Environment(FileEditorSession.self) private var document
    @State private var loaded = false
    @State private var addingEntry = false
    @State private var newSection = ""
    @State private var newKey = ""
    @State private var newVal = ""
    @State private var error: String?

    private var filteredIndices: [Int] {
        entries.indices.filter { idx in
            let e = entries[idx]
            if e.key.isEmpty && e.comment != nil { return searchText.isEmpty }
            if searchText.isEmpty { return true }
            return e.key.localizedCaseInsensitiveContains(searchText) ||
                   e.value.localizedCaseInsensitiveContains(searchText) ||
                   e.section.localizedCaseInsensitiveContains(searchText)
        }
    }

    var body: some View {
        Group {
            if rawMode {
                TextEditor(text: Binding(get: { rawText }, set: { rawText = $0; document.markChanged() }))
                .modifier(FileEditorTextFocus())
                    .font(.system(.footnote, design: .monospaced))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
            } else {
                List {
                    ForEach(filteredIndices, id: \.self) { idx in
                        if !entries[idx].key.isEmpty {
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    if !entries[idx].section.isEmpty {
                                        Text("[\(entries[idx].section)]")
                                            .font(.caption2.monospaced())
                                            .foregroundStyle(.secondary)
                                    }
                                    Text(entries[idx].key)
                                        .font(.subheadline.monospaced().weight(.semibold))
                                }
                                TextField("Value", text: Binding(
                                    get: { entries[idx].value },
                                    set: { entries[idx].value = $0; document.markChanged() }
                                ))
                                .font(.system(.body, design: .monospaced))
                                .textFieldStyle(.roundedBorder)
                                .autocorrectionDisabled()
                                .textInputAutocapitalization(.never)
                            }
                            .padding(.vertical, 2)
                        }
                    }
                    .onDelete { offsets in
                        let toRemove = IndexSet(offsets.map { filteredIndices[$0] })
                        entries.remove(atOffsets: toRemove)
                        document.markChanged()
                    }
                }
                .searchable(text: $searchText, prompt: "Filter keys or values…")
            }
        }
        .navigationTitle(url.lastPathComponent)
        .safeAreaInset(edge: .top, spacing: 0) {
            HStack(spacing: 18) {
                Button {
                    if rawMode {
                        parseINI(rawText)
                    } else {
                        do { rawText = try serializeINI() } catch { self.error = error.localizedDescription; return }
                    }
                    rawMode.toggle()
                } label: {
                    Image(systemName: rawMode ? "list.bullet" : "doc.plaintext")
                }
                .disabled(url.pathExtension.lowercased() == "strings")
                if !rawMode {
                    Button {
                        newKey = ""
                        newVal = ""
                        addingEntry = true
                    } label: {
                        Image(systemName: "plus")
                    }
                }
                Spacer()
            }
        }
        .modifier(FileEditorRegistration(save: save))
        .editorSaveBottomBar()
        .alert("Add Config Key", isPresented: $addingEntry) {
            TextField("Section (optional)", text: $newSection)
            TextField("Key", text: $newKey)
            TextField("Value", text: $newVal)
            Button("Add") {
                guard !newKey.isEmpty else { return }
                entries.append(KeyValueEntry(section: newSection, key: newKey, value: newVal))
                document.markChanged()
            }
            Button("Cancel", role: .cancel) {}
        }
        .alert("Save Failed", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: { Text(error ?? "") }
        .onAppear { if !loaded { load(); loaded = true } }
    }

    private func load() {
        document.ready = false
        do {
            let (payload, fmt) = try SaveCompressionFormat.decode(document.store.load())
            compression = fmt
            let decoded = try SaveTextCodec.decode(payload)
            encoding = decoded.encoding
            byteOrderMark = decoded.byteOrderMark
            rawText = decoded.text
            parseINI(rawText)
            // .strings has escaping/comment rules beyond the INI parser.
            rawMode = true
            document.dirty = false
            document.ready = true
        } catch { self.error = error.localizedDescription }
    }

    private func parseINI(_ text: String) {
        originalLines = text.components(separatedBy: "\n")
        var parsed: [KeyValueEntry] = []
        var currentSection = ""
        for (index, line) in originalLines.enumerated() {
            let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
            if trimmed.hasPrefix("[") && trimmed.hasSuffix("]") {
                currentSection = String(trimmed.dropFirst().dropLast())
            } else if let field = SaveConfigLine(line) {
                parsed.append(KeyValueEntry(section: currentSection, key: field.key, value: field.value,
                    sourceLine: index, originalValue: field.value))
            }
        }
        entries = parsed
        originalEntryLines = Set(parsed.compactMap(\.sourceLine))
    }

    private func serializeINI() throws -> String {
        let existing = Dictionary(uniqueKeysWithValues: entries.compactMap { entry in
            entry.sourceLine.map { ($0, entry) }
        })
        var lines: [String] = []
        for (index, line) in originalLines.enumerated() {
            if !originalEntryLines.contains(index) { lines.append(line); continue }
            guard let entry = existing[index] else { continue }
            if entry.value == entry.originalValue { lines.append(line) }
            else if let field = SaveConfigLine(line) { lines.append(try field.replacingValue(entry.value)) }
        }
        var result = lines.joined(separator: "\n")
        let newline = rawText.contains("\r\n") ? "\r\n" : "\n"
        for entry in entries where entry.sourceLine == nil {
            let line = "\(entry.key)=\(entry.value)" + newline
            if entry.section.isEmpty {
                // Global values belong before the first section header.
                result = line + result
            } else {
                if !result.isEmpty && !result.hasSuffix("\n") { result += newline }
                result += "[\(entry.section)]" + newline + line
            }
        }
        return result
    }

    private func save() -> Bool {
        let outText: String
        do { outText = rawMode ? rawText : try serializeINI() } catch { document.saveError = error.localizedDescription; return false }
        do {
            guard let bytes = outText.data(using: encoding, allowLossyConversion: false) else {
                throw CocoaError(.fileWriteInapplicableStringEncoding)
            }
            try document.commit(SaveCompressionFormat.encode(byteOrderMark + bytes, format: compression))
            return true
        } catch {
            document.saveError = error.localizedDescription
            return false
        }
    }
}

// MARK: - Structured JSON Editor

struct JSONEditorView: View {
    let url: URL
    @State private var root: JSONNode?
    @State private var rawMode = false
    @State private var rawText = ""
    @State private var compression: SaveCompressionFormat = .none
    @Environment(FileEditorSession.self) private var document
    @State private var loaded = false
    @State private var error: String?

    var body: some View {
        Group {
            if rawMode {
                TextEditor(text: Binding(get: { rawText }, set: { rawText = $0; document.markChanged() }))
                .modifier(FileEditorTextFocus())
                    .font(.system(.footnote, design: .monospaced))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
            } else if let root {
                JSONContainerListView(node: root, isRoot: true, onMutate: { document.markChanged() })
            } else {
                ContentUnavailableView("Invalid JSON", systemImage: "exclamationmark.triangle")
            }
        }
        .navigationTitle(url.lastPathComponent)
        .safeAreaInset(edge: .top, spacing: 0) {
            HStack(spacing: 18) {
                Button {
                    toggleRawMode()
                } label: {
                    Image(systemName: rawMode ? "list.bullet.indent" : "curlybraces")
                }
                ShareLink(item: url)
                Spacer()
            }
        }
        .modifier(FileEditorRegistration(save: save))
        .editorSaveBottomBar()
        .alert("JSON Error", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear { if !loaded { load(); loaded = true } }
    }

    private func load() {
        document.ready = false
        do {
            let (payload, fmt) = try SaveCompressionFormat.decode(document.store.load())
            compression = fmt
            guard let text = String(data: payload, encoding: .utf8) else {
                throw CocoaError(.fileReadInapplicableStringEncoding)
            }
            rawText = text
            if let object = try? JSONSerialization.jsonObject(with: payload, options: [.fragmentsAllowed]) {
                root = JSONNode.fromAny(object)
                if !(object is [String: Any]) && !(object is [Any]) { rawMode = true }
            } else { rawMode = true }
            document.dirty = false
            document.ready = true
        } catch { self.error = error.localizedDescription }
    }

    private func toggleRawMode() {
        if rawMode {
            if let data = rawText.data(using: .utf8),
               let obj = try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed]) {
                root = JSONNode.fromAny(obj)
                rawMode = false
            } else {
                error = "Raw text is not valid JSON."
            }
        } else if let root {
            if let data = try? JSONSerialization.data(withJSONObject: root.toAny(), options: [.prettyPrinted, .sortedKeys, .fragmentsAllowed]),
               let s = String(data: data, encoding: .utf8) {
                rawText = s
            }
            rawMode = true
        }
    }

    private func save() -> Bool {
        do {
            let jsonData: Data
            if rawMode {
                guard let d = rawText.data(using: .utf8) else { return false }
                _ = try JSONSerialization.jsonObject(with: d, options: [.fragmentsAllowed])
                jsonData = d
            } else if let root {
                jsonData = try JSONSerialization.data(withJSONObject: root.toAny(), options: [.prettyPrinted, .sortedKeys, .fragmentsAllowed])
            } else {
                return false
            }
            let out = try SaveCompressionFormat.encode(jsonData, format: compression)
            try document.commit(out)
            return true
        } catch {
            document.saveError = error.localizedDescription
            return false
        }
    }
}

private struct JSONContainerListView: View {
    @Bindable var node: JSONNode
    let isRoot: Bool
    let onMutate: () -> Void
    @State private var searchText = ""
    @State private var addingKey = false
    @State private var newKeyName = ""

    private var children: [JSONNode] {
        switch node.value {
        case .object(let a), .array(let a):
            guard !searchText.isEmpty else { return a }
            return a.filter { $0.key.localizedCaseInsensitiveContains(searchText) }
        default: return []
        }
    }

    var body: some View {
        List {
            ForEach(children) { child in
                JSONRowView(node: child, onMutate: onMutate)
            }
            .onDelete { offsets in
                switch node.value {
                case .object(var a):
                    let ids = Set(offsets.map { children[$0].id })
                    a.removeAll { ids.contains($0.id) }
                    node.value = .object(a)
                    onMutate()
                case .array(var a):
                    let ids = Set(offsets.map { children[$0].id })
                    a.removeAll { ids.contains($0.id) }
                    for i in a.indices { a[i].key = "[\(i)]" }
                    node.value = .array(a)
                    onMutate()
                default: break
                }
            }

            Section {
                Button {
                    switch node.value {
                    case .object:
                        newKeyName = ""
                        addingKey = true
                    case .array(var a):
                        a.append(JSONNode(key: "[\(a.count)]", value: .number(NSNumber(value: 0))))
                        node.value = .array(a)
                        onMutate()
                    default: break
                    }
                } label: {
                    Label("Add Entry", systemImage: "plus.circle")
                }
            }
        }
        .searchable(text: $searchText, prompt: "Search JSON keys…")
        .toolbar { FileEditorToolbar() }
        .navigationTitle(isRoot ? "JSON Tree" : node.key)
        .alert("New JSON Key", isPresented: $addingKey) {
            TextField("Key", text: $newKeyName)
            Button("Add") {
                if case .object(var a) = node.value, !newKeyName.isEmpty {
                    a.append(JSONNode(key: newKeyName, value: .number(NSNumber(value: 0))))
                    node.value = .object(a)
                    onMutate()
                }
            }
            Button("Cancel", role: .cancel) {}
        }
    }
}

private struct JSONRowView: View {
    @Bindable var node: JSONNode
    let onMutate: () -> Void

    var body: some View {
        switch node.value {
        case .object, .array:
            NavigationLink {
                JSONContainerListView(node: node, isRoot: false, onMutate: onMutate)
            } label: {
                HStack {
                    Text(node.key).font(.system(.body, design: .monospaced))
                    Spacer()
                    Text(node.value.label).font(.caption).foregroundStyle(.secondary)
                }
            }
            .accessibilityIdentifier("json-container-\(node.key)")
        case .bool(let b):
            Toggle(isOn: Binding(
                get: { b },
                set: { node.value = .bool($0); onMutate() }
            )) {
                Text(node.key).font(.system(.body, design: .monospaced))
            }
        case .number(let number):
            HStack {
                Text(node.key).font(.system(.body, design: .monospaced))
                Spacer()
                TextField("0", text: Binding(
                    get: { number.stringValue },
                    set: { s in
                        if let object = try? JSONSerialization.jsonObject(with: Data(s.utf8), options: [.fragmentsAllowed]),
                           let value = object as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID() {
                            node.value = .number(value)
                            onMutate()
                        }
                    }
                ))
                .keyboardType(.numbersAndPunctuation)
                .multilineTextAlignment(.trailing)
                .font(.system(.body, design: .monospaced))
                .frame(maxWidth: 160)
            }
        case .string(let s):
            VStack(alignment: .leading, spacing: 2) {
                Text(node.key).font(.caption.monospaced()).foregroundStyle(.secondary)
                TextField("Value", text: Binding(
                    get: { s },
                    set: { node.value = .string($0); onMutate() }
                ))
                .font(.system(.body, design: .monospaced))
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
            }
        case .null:
            HStack {
                Text(node.key).font(.system(.body, design: .monospaced))
                Spacer()
                Text("null").font(.caption.monospaced()).foregroundStyle(.secondary)
            }
        }
    }
}

// MARK: - Interactive SQLite3 Database Editor

struct SQLiteColumnInfo: Identifiable {
    let cid: Int
    let name: String
    let type: String
    let isPrimaryKey: Bool
    var id: Int { cid }
}

struct SQLiteRowData: Identifiable {
    let rowid: Int64
    var cells: [String: SQLiteCellValue]
    var id: Int64 { rowid }
}

enum SQLiteCellValue: Equatable {
    case null
    case integer(Int64)
    case real(Double)
    case text(String)
    case blob(Data)

    var displayString: String {
        switch self {
        case .null: "NULL"
        case .integer(let i): String(i)
        case .real(let d): String(d)
        case .text(let s): s
        case .blob(let b): "<BLOB \(b.count)B>"
        }
    }
}

/// One editable database cell. Split out of `SQLiteEditorView.body` so the value color is a concrete `Color` instead of an overloaded `ShapeStyle` ternary.
private struct SQLiteCellRow: View {
    let column: SQLiteColumnInfo
    let value: SQLiteCellValue
    let edit: () -> Void

    var body: some View {
        Button(action: edit) {
            HStack {
                columnLabel
                Spacer()
                valueLabel
            }
        }
    }

    private var columnLabel: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(column.name)
                .font(.caption.monospaced().weight(.semibold))
                .foregroundStyle(.primary)
            if !column.type.isEmpty {
                Text(column.type)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var valueLabel: some View {
        let text = value.displayString
        let color: Color = value == .null ? .secondary : .accentColor
        return Text(text)
            .font(.system(.subheadline, design: .monospaced))
            .foregroundStyle(color)
            .lineLimit(2)
    }
}

struct SQLiteEditorView: View {
    let url: URL
    @Environment(FileEditorSession.self) private var document
    @State private var workspace: SQLiteEditStore?
    @State private var loaded = false
    @State private var error: String?
    @State private var tables: [String] = []
    @State private var selectedTable: String = ""
    @State private var columns: [SQLiteColumnInfo] = []
    @State private var rows: [SQLiteRowData] = []
    @State private var searchText = ""
    @State private var customSQL = ""
    @State private var showingSQLConsole = false
    @State private var sqlResultMessage: String?
    @State private var editingCell: EditingCellContext?
    @State private var editCellText = ""

    struct EditingCellContext: Identifiable {
        let id = UUID()
        let rowid: Int64
        let column: String
        let currentValue: SQLiteCellValue
    }

    private var filteredRows: [SQLiteRowData] {
        guard !searchText.isEmpty else { return rows }
        return rows.filter { row in
            row.cells.values.contains { $0.displayString.localizedCaseInsensitiveContains(searchText) }
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            if !tables.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(tables, id: \.self) { t in
                            Button {
                                selectedTable = t
                                loadTable(t)
                            } label: {
                                Text(t)
                                    .font(.subheadline.monospaced().weight(selectedTable == t ? .bold : .regular))
                                    .padding(.horizontal, 12)
                                    .padding(.vertical, 6)
                                    .background(selectedTable == t ? Color.accentColor : Color.secondary.opacity(0.15), in: Capsule())
                                    .foregroundStyle(selectedTable == t ? .white : .primary)
                            }
                        }
                    }
                    .padding(.horizontal)
                    .padding(.vertical, 8)
                }
                Divider()
            }

            if let msg = sqlResultMessage {
                HStack {
                    Text(msg).font(.caption.monospaced())
                    Spacer()
                    Button("Clear") { sqlResultMessage = nil }.font(.caption2)
                }
                .padding(.horizontal)
                .padding(.vertical, 6)
                .background(Color.secondary.opacity(0.12))
            }

            if tables.isEmpty {
                ContentUnavailableView("No Tables in Database", systemImage: "cylinder.split.1x2")
            } else {
                List {
                    ForEach(filteredRows) { row in
                        Section("Row #\(row.rowid)") {
                            ForEach(columns) { col in
                                let val = row.cells[col.name] ?? .null
                                SQLiteCellRow(column: col, value: val) {
                                    if case .blob(let bytes) = val {
                                        editCellText = bytes.map { String(format: "%02X", $0) }.joined(separator: " ")
                                    } else { editCellText = val.displayString == "NULL" ? "" : val.displayString }
                                    editingCell = EditingCellContext(rowid: row.rowid, column: col.name, currentValue: val)
                                }
                            }
                        }
                    }
                    .onDelete { offsets in
                        deleteRows(at: offsets)
                    }
                }
                .searchable(text: $searchText, prompt: "Search rows in \(selectedTable)…")
            }
        }
        .navigationTitle(url.lastPathComponent)
        .safeAreaInset(edge: .top, spacing: 0) {
            HStack(spacing: 18) {
                Button {
                    showingSQLConsole = true
                } label: {
                    Label("Run SQL", systemImage: "terminal")
                }
                Button {
                    loadTable(selectedTable)
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
            }
        }
        .modifier(FileEditorRegistration(save: save))
        .alert("Database Error", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: { Text(error ?? "") }
        .sheet(isPresented: $showingSQLConsole) {
            NavigationStack {
                Form {
                    Section("Custom SQL Statement") {
                        TextEditor(text: $customSQL)
                            .font(.system(.body, design: .monospaced))
                            .frame(minHeight: 120)
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                    }
                    Section("Quick Templates") {
                        Button("SELECT * FROM \"\(selectedTable)\" LIMIT 100;") {
                            customSQL = "SELECT * FROM \"\(selectedTable)\" LIMIT 100;"
                        }
                        if let firstCol = columns.first?.name {
                            Button("UPDATE \"\(selectedTable)\" SET \"\(firstCol)\" = 999999;") {
                                customSQL = "UPDATE \"\(selectedTable)\" SET \"\(firstCol)\" = 999999;"
                            }
                        }
                    }
                }
                .navigationTitle("SQLite Console")
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Close") { showingSQLConsole = false }
                    }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Execute") {
                            executeCustomSQL(customSQL)
                            showingSQLConsole = false
                        }
                    }
                }
            }
        }
        .sheet(item: $editingCell) { ctx in
            NavigationStack {
                Form {
                    Section("Column: \(ctx.column) (Row #\(ctx.rowid))") {
                        TextField("Value", text: $editCellText, axis: .vertical)
                            .font(.system(.body, design: .monospaced))
                            .autocorrectionDisabled()
                            .textInputAutocapitalization(.never)
                    }
                    Section {
                        Button("Set to NULL") {
                            if updateCellToNull(ctx: ctx) { editingCell = nil }
                        }
                    }
                }
                .navigationTitle("Edit Cell")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Cancel") { editingCell = nil }
                    }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Update") {
                            if updateCell(ctx: ctx, newText: editCellText) { editingCell = nil }
                        }
                    }
                }
            }
            .presentationDetents([.medium])
        }
        .onAppear {
            guard !loaded else { return }
            loaded = true
            document.ready = false
            do {
                workspace = try SQLiteEditStore(source: url)
                try SQLiteEditStore.requireCheckpointed(url)
                _ = try document.store.load()
                document.ready = true
                loadDatabase()
            } catch { self.error = error.localizedDescription }
        }
    }

    private func save() -> Bool {
        guard document.ready, let workspace else { return false }
        do {
            try SQLiteEditStore.requireCheckpointed(url)
            try document.commit(workspace.bytes())
            return true
        } catch {
            document.saveError = error.localizedDescription
            return false
        }
    }

    private func openDB() -> OpaquePointer? {
        guard let workspace else { return nil }
        var db: OpaquePointer?
        if sqlite3_open_v2(workspace.url.path, &db, SQLITE_OPEN_READWRITE, nil) == SQLITE_OK {
            // SQL is restricted to this working database; ATTACH and VACUUM INTO
            // must not bypass the editor's Save/Discard boundary.
            sqlite3_set_authorizer(db, { _, action, _, _, _, _ in
                (action == SQLITE_ATTACH || action == SQLITE_DETACH) ? SQLITE_DENY : SQLITE_OK
            }, nil)
            return db
        }
        error = SQLiteEditStore.failure(db, "Open working database").localizedDescription
        if let db { sqlite3_close(db) }
        return nil
    }

    private func loadDatabase() {
        guard let db = openDB() else { return }
        defer { sqlite3_close(db) }

        var stmt: OpaquePointer?
        var found: [String] = []
        let sql = "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;"
        if sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK {
            while sqlite3_step(stmt) == SQLITE_ROW {
                if let cStr = sqlite3_column_text(stmt, 0) {
                    found.append(String(cString: cStr))
                }
            }
            sqlite3_finalize(stmt)
        }
        tables = found
        if let first = found.first {
            selectedTable = first
            loadTable(first)
        }
    }

    private func loadTable(_ table: String) {
        guard !table.isEmpty, let db = openDB() else { return }
        defer { sqlite3_close(db) }

        let escapedTable = table.replacingOccurrences(of: "\"", with: "\"\"")
        var cols: [SQLiteColumnInfo] = []
        var pragmaStmt: OpaquePointer?
        if sqlite3_prepare_v2(db, "PRAGMA table_info(\"\(escapedTable)\");", -1, &pragmaStmt, nil) == SQLITE_OK {
            while sqlite3_step(pragmaStmt) == SQLITE_ROW {
                let cid = Int(sqlite3_column_int(pragmaStmt, 0))
                let name = sqlite3_column_text(pragmaStmt, 1).map { String(cString: $0) } ?? "col\(cid)"
                let type = sqlite3_column_text(pragmaStmt, 2).map { String(cString: $0) } ?? ""
                let pk = sqlite3_column_int(pragmaStmt, 5) != 0
                cols.append(SQLiteColumnInfo(cid: cid, name: name, type: type, isPrimaryKey: pk))
            }
            sqlite3_finalize(pragmaStmt)
        }
        columns = cols

        var loadedRows: [SQLiteRowData] = []
        var stmt: OpaquePointer?
        let query = "SELECT rowid, * FROM \"\(escapedTable)\" LIMIT 500;"
        if sqlite3_prepare_v2(db, query, -1, &stmt, nil) == SQLITE_OK {
            let colCount = sqlite3_column_count(stmt)
            while sqlite3_step(stmt) == SQLITE_ROW {
                let rid = sqlite3_column_int64(stmt, 0)
                var cells: [String: SQLiteCellValue] = [:]
                for i in 1..<colCount {
                    let cName = sqlite3_column_name(stmt, i).map { String(cString: $0) } ?? "col\(i)"
                    switch sqlite3_column_type(stmt, i) {
                    case SQLITE_INTEGER:
                        cells[cName] = .integer(sqlite3_column_int64(stmt, i))
                    case SQLITE_FLOAT:
                        cells[cName] = .real(sqlite3_column_double(stmt, i))
                    case SQLITE_TEXT:
                        let s = sqlite3_column_text(stmt, i).map { String(cString: $0) } ?? ""
                        cells[cName] = .text(s)
                    case SQLITE_BLOB:
                        let len = Int(sqlite3_column_bytes(stmt, i))
                        if len > 0, let ptr = sqlite3_column_blob(stmt, i) {
                            cells[cName] = .blob(Data(bytes: ptr, count: len))
                        } else {
                            cells[cName] = .blob(Data())
                        }
                    default:
                        cells[cName] = .null
                    }
                }
                loadedRows.append(SQLiteRowData(rowid: rid, cells: cells))
            }
            sqlite3_finalize(stmt)
        }
        rows = loadedRows
    }

    private func updateCell(ctx: EditingCellContext, newText: String) -> Bool {
        guard let db = openDB() else { return false }
        defer { sqlite3_close(db) }

        let escapedTable = selectedTable.replacingOccurrences(of: "\"", with: "\"\"")
        let escapedCol = ctx.column.replacingOccurrences(of: "\"", with: "\"\"")
        let sql = "UPDATE \"\(escapedTable)\" SET \"\(escapedCol)\" = ? WHERE rowid = ?;"
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK else {
            sqlResultMessage = String(cString: sqlite3_errmsg(db))
            return false
        }
        defer { sqlite3_finalize(stmt) }

        let SQLITE_TRANSIENT = unsafeBitCast(-1, to: sqlite3_destructor_type.self)
        switch ctx.currentValue {
        case .integer:
            if let v = Int64(newText.trimmingCharacters(in: .whitespaces)) {
                sqlite3_bind_int64(stmt, 1, v)
            } else {
                sqlite3_bind_text(stmt, 1, newText, -1, SQLITE_TRANSIENT)
            }
        case .real:
            if let v = Double(newText.trimmingCharacters(in: .whitespaces)) {
                sqlite3_bind_double(stmt, 1, v)
            } else {
                sqlite3_bind_text(stmt, 1, newText, -1, SQLITE_TRANSIENT)
            }
        case .blob:
            let hex = newText.filter { !$0.isWhitespace }
            guard hex.count % 2 == 0 else { error = "Enter complete hexadecimal byte pairs."; return false }
            var bytes = [UInt8]()
            var index = hex.startIndex
            while index != hex.endIndex {
                let end = hex.index(index, offsetBy: 2)
                guard let byte = UInt8(hex[index..<end], radix: 16) else {
                    error = "Enter hexadecimal bytes (00–FF)."; return false
                }
                bytes.append(byte)
                index = end
            }
            if bytes.isEmpty { sqlite3_bind_zeroblob(stmt, 1, 0) }
            else { _ = bytes.withUnsafeBytes { sqlite3_bind_blob(stmt, 1, $0.baseAddress, Int32($0.count), SQLITE_TRANSIENT) } }
        default:
            if let v = Int64(newText), ctx.currentValue == .null {
                sqlite3_bind_int64(stmt, 1, v)
            } else if let v = Double(newText), ctx.currentValue == .null {
                sqlite3_bind_double(stmt, 1, v)
            } else {
                sqlite3_bind_text(stmt, 1, newText, -1, SQLITE_TRANSIENT)
            }
        }
        sqlite3_bind_int64(stmt, 2, ctx.rowid)

        if sqlite3_step(stmt) == SQLITE_DONE {
            sqlResultMessage = "Updated \(ctx.column) on row #\(ctx.rowid)"
            document.markChanged()
            loadTable(selectedTable)
            return true
        } else {
            error = String(cString: sqlite3_errmsg(db))
            return false
        }
    }

    private func updateCellToNull(ctx: EditingCellContext) -> Bool {
        guard let db = openDB() else { return false }
        defer { sqlite3_close(db) }
        let escapedTable = selectedTable.replacingOccurrences(of: "\"", with: "\"\"")
        let escapedCol = ctx.column.replacingOccurrences(of: "\"", with: "\"\"")
        let sql = "UPDATE \"\(escapedTable)\" SET \"\(escapedCol)\" = NULL WHERE rowid = \(ctx.rowid);"
        guard sqlite3_exec(db, sql, nil, nil, nil) == SQLITE_OK else {
            error = String(cString: sqlite3_errmsg(db)); return false
        }
        document.markChanged()
        loadTable(selectedTable)
        return true
    }

    private func deleteRows(at offsets: IndexSet) {
        guard let db = openDB() else { return }
        defer { sqlite3_close(db) }
        let escapedTable = selectedTable.replacingOccurrences(of: "\"", with: "\"\"")
        guard sqlite3_exec(db, "BEGIN IMMEDIATE", nil, nil, nil) == SQLITE_OK else {
            error = String(cString: sqlite3_errmsg(db)); return
        }
        for idx in offsets {
            let rid = filteredRows[idx].rowid
            if sqlite3_exec(db, "DELETE FROM \"\(escapedTable)\" WHERE rowid = \(rid);", nil, nil, nil) != SQLITE_OK {
                error = String(cString: sqlite3_errmsg(db))
                sqlite3_exec(db, "ROLLBACK", nil, nil, nil)
                return
            }
        }
        guard sqlite3_exec(db, "COMMIT", nil, nil, nil) == SQLITE_OK else {
            error = String(cString: sqlite3_errmsg(db)); return
        }
        document.markChanged()
        loadTable(selectedTable)
    }

    private func executeCustomSQL(_ sql: String) {
        guard let db = openDB() else { return }
        defer { sqlite3_close(db) }
        var errMsg: UnsafeMutablePointer<CChar>?
        // Even a multi-statement command that fails can change the working copy.
        document.markChanged()
        if sqlite3_exec(db, sql, nil, nil, &errMsg) == SQLITE_OK {
            let changes = sqlite3_changes(db)
            sqlResultMessage = "SQL executed (\(changes) rows affected)"
            loadDatabase()
        } else if let errMsg {
            sqlResultMessage = String(cString: errMsg)
            sqlite3_free(errMsg)
        }
    }
}

// MARK: - Plist & NSKeyedArchiver Editor

/// Editable property-list tree node that preserves `CFKeyedArchiverUID` opaque references
/// so editing NSKeyedArchiver game saves never corrupts UID links.
struct PlistEditorView: View {
    let url: URL
    @State private var root: PlistNode?
    @State private var format: PropertyListSerialization.PropertyListFormat = .binary
    @State private var compression: SaveCompressionFormat = .none
    @State private var viewMode: ViewMode = .smart
    @Environment(FileEditorSession.self) private var document
    @State private var loaded = false
    @State private var error: String?

    enum ViewMode: String, CaseIterable {
        case smart = "Objects"
        case rawTree = "Raw Tree"
    }

    var body: some View {
        VStack(spacing: 0) {
            if let root, root.isKeyedArchiveRoot {
                Picker("View Mode", selection: $viewMode) {
                    Text("NSKeyedArchiver Objects").tag(ViewMode.smart)
                    Text("Raw Plist Tree").tag(ViewMode.rawTree)
                }
                .pickerStyle(.segmented)
                .padding(.horizontal)
                .padding(.vertical, 6)
            }

            if let root {
                if root.isKeyedArchiveRoot && viewMode == .smart {
                    KeyedArchiveObjectsView(root: root, onMutate: { document.markChanged() })
                } else {
                    PlistContainerView(node: root, isRoot: true, archiveRoot: root, onMutate: { document.markChanged() })
                }
            } else {
                ContentUnavailableView("Invalid Property List", systemImage: "exclamationmark.triangle",
                                       description: Text(error ?? ""))
            }
        }
        .navigationTitle(url.lastPathComponent)
        .safeAreaInset(edge: .top, spacing: 0) {
            HStack(spacing: 18) {
                Menu {
                    Picker("Format", selection: Binding(get: { format }, set: { format = $0; document.markChanged() })) {
                        Text("Binary Plist").tag(PropertyListSerialization.PropertyListFormat.binary)
                        Text("XML Plist").tag(PropertyListSerialization.PropertyListFormat.xml)
                    }
                    .disabled(root?.isKeyedArchiveRoot == true)
                } label: {
                    Image(systemName: "gearshape")
                }
                ShareLink(item: url)
                Spacer()
            }
        }
        .modifier(FileEditorRegistration(save: save))
        .editorSaveBottomBar()
        .alert("Save Failed", isPresented: Binding(get: { error != nil },
                                                    set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear { if !loaded { load(); loaded = true } }
    }

    private func load() {
        document.ready = false
        do {
            let (data, compressionFormat) = try SaveCompressionFormat.decode(document.store.load())
            compression = compressionFormat
            var fmt: PropertyListSerialization.PropertyListFormat = .binary
            let obj = try PropertyListSerialization.propertyList(from: data, options: [], format: &fmt)
            format = fmt
            root = PlistNode.fromAny(obj)
            document.dirty = false
            document.ready = true
        } catch { self.error = error.localizedDescription }
    }

    private func save() -> Bool {
        guard let root else { return false }
        do {
            let data = try PropertyListSerialization.data(fromPropertyList: root.toAny(), format: format, options: 0)
            let out = try SaveCompressionFormat.encode(data, format: compression)
            try document.commit(out)
            KeychainSaveSync.syncKeychainToFileIfNeeded(keychainURL: url, plistRoot: root)

            return true
        } catch {
            document.saveError = error.localizedDescription
            return false
        }
    }
}

/// Decodes and presents an `NSKeyedArchiver` `$objects` array so users can browse and edit archived objects directly.
private struct KeyedArchiveObjectsView: View {
    @Bindable var root: PlistNode
    let onMutate: () -> Void
    @State private var searchText = ""

    private var objects: [PlistNode] {
        root.keyedArchiveObjects ?? []
    }

    private func describeArchivedObject(_ node: PlistNode, allObjects: [PlistNode]) -> String {
        switch node.value {
        case .string(let s): return "\"\(s)\""
        case .integer(let i): return "\(i)"
        case .real(let r): return "\(r)"
        case .bool(let b): return b ? "true" : "false"
        case .dict(let entries):
            var className = "Dictionary"
            if let clsNode = entries.first(where: { $0.key == "$class" }),
               case .uid(let ref) = clsNode.value,
               let cid = PlistNode.uidIndex(ref),
               cid < allObjects.count,
               case .dict(let clsDict) = allObjects[cid].value,
               let cnameNode = clsDict.first(where: { $0.key == "$classname" }),
               case .string(let cname) = cnameNode.value {
                className = cname
            }
            let propKeys = entries.map(\.key).filter { !$0.hasPrefix("$") }
            return propKeys.isEmpty ? className : "\(className) { \(propKeys.prefix(4).joined(separator: ", ")) }"
        default:
            return node.value.typeLabel
        }
    }

    var body: some View {
        List {
            if case .dict(let rootEntries) = root.value,
               let topNode = rootEntries.first(where: { $0.key == "$top" }),
               case .dict(let topEntries) = topNode.value {
                Section("Archive Entry Points ($top)") {
                    ForEach(topEntries) { entry in
                        if case .uid(let ref) = entry.value,
                           let idx = PlistNode.uidIndex(ref),
                           idx < objects.count {
                            NavigationLink {
                                PlistContainerView(node: objects[idx], isRoot: false, archiveRoot: root, onMutate: onMutate)
                            } label: {
                                HStack {
                                    Text(entry.key).font(.system(.body, design: .monospaced).weight(.semibold))
                                    Spacer()
                                    Text("Object[\(idx)] — \(describeArchivedObject(objects[idx], allObjects: objects))")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                            }
                        } else {
                            PlistRowView(node: entry, isDictChild: true, archiveRoot: root, onMutate: onMutate)
                        }
                    }
                }
            }

            Section("Archived Objects ($objects: \(objects.count))") {
                ForEach(Array(objects.enumerated()), id: \.element.id) { idx, objNode in
                    let summary = describeArchivedObject(objNode, allObjects: objects)
                    if searchText.isEmpty ||
                        summary.localizedCaseInsensitiveContains(searchText) ||
                        "\(idx)".contains(searchText) ||
                        nodeMatchesSearch(objNode, query: searchText) {
                        switch objNode.value {
                        case .dict, .array:
                            NavigationLink {
                                PlistContainerView(node: objNode, isRoot: false, archiveRoot: root, onMutate: onMutate)
                            } label: {
                                HStack {
                                    Text("[\(idx)]")
                                        .font(.caption.monospaced().weight(.bold))
                                        .foregroundStyle(.tint)
                                        .frame(width: 44, alignment: .leading)
                                    Text(summary)
                                        .font(.system(.subheadline, design: .monospaced))
                                        .lineLimit(1)
                                }
                            }
                        default:
                            PlistRowView(node: objNode, isDictChild: false, archiveRoot: root, onMutate: onMutate)
                        }
                    }
                }
            }
        }
        .toolbar { FileEditorToolbar() }
        .searchable(text: $searchText, prompt: "Search classes, keys, or values…")
    }

    private func nodeMatchesSearch(_ n: PlistNode, query: String) -> Bool {
        if n.key.localizedCaseInsensitiveContains(query) { return true }
        switch n.value {
        case .string(let s): return s.localizedCaseInsensitiveContains(query)
        case .integer(let i): return String(i).contains(query)
        case .real(let r): return String(r).contains(query)
        case .dict(let d): return d.contains { nodeMatchesSearch($0, query: query) }
        case .array(let a): return a.contains { nodeMatchesSearch($0, query: query) }
        default: return false
        }
    }
}

private struct PlistContainerView: View {
    @Bindable var node: PlistNode
    let isRoot: Bool
    var archiveRoot: PlistNode?
    let onMutate: () -> Void

    @State private var addingKey = false
    @State private var newKeyName = ""
    @State private var newKindChoice = "String"
    @State private var searchText = ""

    private var children: [PlistNode] {
        let all: [PlistNode]
        switch node.value {
        case .dict(let c), .array(let c): all = c
        default: all = []
        }
        guard !searchText.isEmpty else { return all }
        return all.filter { $0.key.localizedCaseInsensitiveContains(searchText) }
    }

    private var isDict: Bool {
        if case .dict = node.value { return true }
        return false
    }

    var body: some View {
        List {
            ForEach(children) { child in
                PlistRowView(node: child, isDictChild: isDict, archiveRoot: archiveRoot, onMutate: onMutate)
            }
            .onDelete { idx in
                deleteChildren(at: idx)
            }

            Section {
                Button {
                    if isDict {
                        newKeyName = ""
                        addingKey = true
                    } else {
                        appendChild(key: "[\(children.count)]", kind: .string(""))
                    }
                } label: {
                    Label(isDict ? "Add Key…" : "Append Item", systemImage: "plus.circle")
                }
            }
        }
        .searchable(text: $searchText, prompt: "Filter keys…")
        .toolbar { FileEditorToolbar() }
        .navigationTitle(isRoot ? "Plist" : node.key)
        .alert("Add Key", isPresented: $addingKey) {
            TextField("Key name", text: $newKeyName)
            Button("Add") {
                let k = newKeyName.trimmingCharacters(in: .whitespaces)
                if !k.isEmpty { appendChild(key: k, kind: defaultKind(newKindChoice)) }
            }
            Button("Cancel", role: .cancel) {}
        }
    }

    private func defaultKind(_ name: String) -> PlistNode.Kind {
        switch name {
        case "Integer": .integer(0)
        case "Number": .real(0)
        case "Boolean": .bool(false)
        case "Dict": .dict([])
        case "Array": .array([])
        default: .string("")
        }
    }

    private func appendChild(key: String, kind: PlistNode.Kind) {
        let child = PlistNode(key: key, value: kind)
        switch node.value {
        case .dict(var c):
            c.append(child)
            node.value = .dict(c)
        case .array(var c):
            c.append(child)
            node.value = .array(c)
        default: break
        }
        onMutate()
    }

    private func deleteChildren(at offsets: IndexSet) {
        switch node.value {
        case .dict(var c):
            let ids = Set(offsets.map { children[$0].id })
            c.removeAll { ids.contains($0.id) }
            node.value = .dict(c)
        case .array(var c):
            let ids = Set(offsets.map { children[$0].id })
            c.removeAll { ids.contains($0.id) }
            for i in c.indices { c[i].key = "[\(i)]" }
            node.value = .array(c)
        default: break
        }
        onMutate()
    }
}

private struct PlistRowView: View {
    @Bindable var node: PlistNode
    let isDictChild: Bool
    var archiveRoot: PlistNode?
    let onMutate: () -> Void
    @State private var showingNestedPlist = false

    var body: some View {
        Group {
            switch node.value {
            case .dict, .array:
                NavigationLink {
                    PlistContainerView(node: node, isRoot: false, archiveRoot: archiveRoot, onMutate: onMutate)
                } label: {
                    HStack {
                        Text(node.key).font(.system(.body, design: .monospaced))
                        Spacer()
                        Text(node.value.typeLabel)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

            case .uid(let ref):
                if let idx = PlistNode.uidIndex(ref),
                   let objs = archiveRoot?.keyedArchiveObjects,
                   idx < objs.count {
                    NavigationLink {
                        PlistContainerView(node: objs[idx], isRoot: false, archiveRoot: archiveRoot, onMutate: onMutate)
                    } label: {
                        HStack {
                            Text(node.key).font(.system(.body, design: .monospaced))
                            Spacer()
                            Text("UID → Object[\(idx)]")
                                .font(.caption.monospaced())
                                .foregroundStyle(.tint)
                        }
                    }
                } else {
                    HStack {
                        Text(node.key).font(.system(.body, design: .monospaced))
                        Spacer()
                        Text(node.value.typeLabel).font(.caption).foregroundStyle(.secondary)
                    }
                }

            case .bool(let b):
                Toggle(isOn: Binding(
                    get: { b },
                    set: { node.value = .bool($0); onMutate() }
                )) {
                    Text(node.key).font(.system(.body, design: .monospaced))
                }

            case .integer(let n):
                HStack {
                    Text(node.key).font(.system(.body, design: .monospaced))
                    Spacer()
                    TextField("0", text: Binding(
                        get: { String(n) },
                        set: { if let v = Int64($0.trimmingCharacters(in: .whitespaces)) { node.value = .integer(v); onMutate() } }
                    ))
                    .keyboardType(.numbersAndPunctuation)
                    .multilineTextAlignment(.trailing)
                    .font(.system(.body, design: .monospaced))
                    .frame(maxWidth: 160)
                }

            case .real(let r):
                HStack {
                    Text(node.key).font(.system(.body, design: .monospaced))
                    Spacer()
                    TextField("0.0", text: Binding(
                        get: { String(r) },
                        set: { if let v = Double($0.trimmingCharacters(in: .whitespaces)) { node.value = .real(v); onMutate() } }
                    ))
                    .keyboardType(.numbersAndPunctuation)
                    .multilineTextAlignment(.trailing)
                    .font(.system(.body, design: .monospaced))
                    .frame(maxWidth: 160)
                }

            case .string(let s):
                VStack(alignment: .leading, spacing: 2) {
                    Text(node.key).font(.caption.monospaced()).foregroundStyle(.secondary)
                    TextField("Value", text: Binding(
                        get: { s },
                        set: { node.value = .string($0); onMutate() }
                    ))
                    .font(.system(.body, design: .monospaced))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                }

            case .date(let d):
                DatePicker(node.key, selection: Binding(
                    get: { d },
                    set: { node.value = .date($0); onMutate() }
                ))
                .font(.system(.body, design: .monospaced))

            case .data(let d):
                NavigationLink {
                    NestedDataInspectorView(key: node.key, data: d) { updatedData in
                        node.value = .data(updatedData)
                        onMutate()
                    }
                } label: {
                    HStack {
                        Text(node.key).font(.system(.body, design: .monospaced))
                        Spacer()
                        Text(describeDataPayload(d))
                            .font(.caption)
                            .foregroundStyle(.tint)
                    }
                }
            }
        }
        .contextMenu {
            Menu("Change Type") {
                Button("String") { convert(to: .string("")) }
                Button("Integer") { convert(to: .integer(0)) }
                Button("Number (Float)") { convert(to: .real(0)) }
                Button("Boolean") { convert(to: .bool(false)) }
                Button("Dict") { convert(to: .dict([])) }
                Button("Array") { convert(to: .array([])) }
            }
        }
    }

    private func describeDataPayload(_ d: Data) -> String {
        if d.starts(with: Array("bplist".utf8)) { return "Nested Binary Plist (\(d.count) B)" }
        if SaveCompressionFormat.detect(d) != .none { return "Compressed Data (\(d.count) B)" }
        if !d.isEmpty, !d.contains(0), String(data: d, encoding: .utf8) != nil { return "UTF-8 Data (\(d.count) B)" }
        return "Data (\(d.count) B)"
    }

    private func convert(to target: PlistNode.Kind) {
        switch (node.value, target) {
        case (.string(let s), .integer): node.value = .integer(Int64(s) ?? 0)
        case (.string(let s), .real): node.value = .real(Double(s) ?? 0)
        case (.string(let s), .bool): node.value = .bool((s as NSString).boolValue)
        case (.integer(let i), .string): node.value = .string(String(i))
        case (.integer(let i), .real): node.value = .real(Double(i))
        case (.integer(let i), .bool): node.value = .bool(i != 0)
        case (.real(let r), .string): node.value = .string(String(r))
        case (.real(let r), .integer): node.value = .integer(Int64(r))
        case (.bool(let b), .integer): node.value = .integer(b ? 1 : 0)
        case (.bool(let b), .string): node.value = .string(b ? "true" : "false")
        default: node.value = target
        }
        onMutate()
    }
}

/// Inspects and edits a `Data` blob embedded inside a Plist (supports nested binary/XML plists, UTF-8 text, or hex).
private struct NestedDataInspectorView: View {
    let key: String
    @State var data: Data
    let onUpdate: (Data) -> Void

    @State private var nestedPlist: PlistNode?
    @State private var nestedPlistFormat: PropertyListSerialization.PropertyListFormat = .binary
    @State private var textRepresentation: String = ""
    @State private var mode: Mode = .hex
    @State private var loaded = false
    @State private var compression: SaveCompressionFormat = .none
    @State private var error: String?

    enum Mode: String, CaseIterable {
        case plist = "Nested Plist"
        case text = "UTF-8 Text"
        case hex = "Hex Bytes"
    }

    var body: some View {
        VStack(spacing: 0) {
            Picker("Inspector Mode", selection: $mode) {
                if nestedPlist != nil {
                    Text("Nested Plist").tag(Mode.plist)
                }
                Text("UTF-8 Text").tag(Mode.text)
                Text("Hex").tag(Mode.hex)
            }
            .pickerStyle(.segmented)
            .padding()

            switch mode {
            case .plist:
                if let nestedPlist {
                    PlistContainerView(node: nestedPlist, isRoot: false, archiveRoot: nestedPlist) {
                        do {
                            let serialized = try PropertyListSerialization.data(
                            fromPropertyList: nestedPlist.toAny(),
                            format: nestedPlistFormat,
                            options: 0
                            )
                            apply(serialized)
                        } catch { self.error = error.localizedDescription }
                    }
                }
            case .text:
                TextEditor(text: Binding(get: { textRepresentation }, set: { value in
                    textRepresentation = value
                    let encoded = Data(value.utf8)
                    apply(encoded)
                }))
                    .modifier(FileEditorTextFocus())
                    .font(.system(.footnote, design: .monospaced))
            case .hex:
                ScrollView {
                    Text(data.map { String(format: "%02X", $0) }.joined(separator: " "))
                        .font(.system(.caption, design: .monospaced))
                        .padding()
                }
            }
        }
        .toolbar { FileEditorToolbar() }
        .navigationTitle(key)
        .onAppear {
            guard !loaded else { return }
            loaded = true
            do {
                let (payload, fmt) = try SaveCompressionFormat.decode(data)
                data = payload
                compression = fmt
            } catch { self.error = error.localizedDescription; return }
            var fmt: PropertyListSerialization.PropertyListFormat = .binary
            if let obj = try? PropertyListSerialization.propertyList(from: data, options: [], format: &fmt) {
                nestedPlistFormat = fmt
                nestedPlist = PlistNode.fromAny(obj)
                mode = .plist
            } else if let s = String(data: data, encoding: .utf8), !data.contains(0) {
                textRepresentation = s
                mode = .text
            } else {
                textRepresentation = String(decoding: data, as: UTF8.self)
                mode = .hex
            }
        }
        .alert("Nested Data Error", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: { Text(error ?? "") }
    }
    private func apply(_ payload: Data) {
        do {
            let encoded = try SaveCompressionFormat.encode(payload, format: compression)
            data = payload
            onUpdate(encoded)
        } catch { self.error = error.localizedDescription }
    }

}

// MARK: - Binary / Hex Editor with Value Inspector & Cheat Scanner

struct BinarySearchMatch: Identifiable {
    let offset: Int
    let preview: String
    var id: Int { offset }
}

struct BinaryEditorView: View {
    let url: URL
    @State private var data = Data()
    @State private var compression: SaveCompressionFormat = .none
    @State private var selectedOffset: Int = 0
    @Environment(FileEditorSession.self) private var document
    @State private var loaded = false
    @State private var error: String?

    // Value Search / Cheat Scanner
    @State private var showingScanner = false
    @State private var searchQuery = ""
    @State private var searchType: SearchValueType = .int32LE
    @State private var searchMatches: [BinarySearchMatch] = []
    @State private var replaceValueText = ""

    enum SearchValueType: String, CaseIterable, Identifiable {
        case int32LE = "Int32 (LE)"
        case uint32LE = "UInt32 (LE)"
        case int16LE = "Int16 (LE)"
        case int64LE = "Int64 (LE)"
        case float32LE = "Float32 (LE)"
        case float64LE = "Double (LE)"
        case ascii = "ASCII / UTF-8"
        case utf16LE = "UTF-16 (LE)"
        case hex = "Hex Bytes"
        var id: String { rawValue }
    }

    private let bytesPerRow = 16
    private let maxDisplayBytes = 131_072 // 128 KB window

    var body: some View {
        VStack(spacing: 0) {
            if compression != .none {
                HStack {
                    Image(systemName: "archivebox.fill")
                    Text("Decompressed \(compression.rawValue) (\(data.count) bytes) — will re-compress on save")
                    Spacer()
                }
                .font(.caption)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(Color.orange.opacity(0.15))
            }

            // Interactive Multi-Type Value Inspector Bar at `selectedOffset`
            if !data.isEmpty && selectedOffset < data.count {
                BinaryValueInspectorBar(data: $data, offset: $selectedOffset, totalCount: data.count) {
                    document.markChanged()
                }
                Divider()
            }

            // Hex Rows
            let shown = min(data.count, maxDisplayBytes)
            let rowCount = (shown + bytesPerRow - 1) / bytesPerRow
            List {
                ForEach(0..<rowCount, id: \.self) { row in
                    let base = row * bytesPerRow
                    let end = min(base + bytesPerRow, shown)
                    HexRow(
                        base: base,
                        slice: data[base..<end],
                        selectedOffset: $selectedOffset
                    ) { offset, byte in
                        data[offset] = byte
                        document.markChanged()
                    }
                    .listRowInsets(EdgeInsets(top: 2, leading: 10, bottom: 2, trailing: 10))
                }
            }
            .listStyle(.plain)
            .font(.system(.caption, design: .monospaced))
        }
        .navigationTitle(url.lastPathComponent)
        .safeAreaInset(edge: .top, spacing: 0) {
            HStack(spacing: 18) {
                Button {
                    showingScanner = true
                } label: {
                    Label("Value Scanner", systemImage: "magnifyingglass.circle")
                }
                ShareLink(item: url)
                Spacer()
            }
        }
        .modifier(FileEditorRegistration(save: save))
        .editorSaveBottomBar()
        .sheet(isPresented: $showingScanner) {
            NavigationStack {
                Form {
                    Section("Search Game Value (Coins, Gold, Stats, String)") {
                        Picker("Data Type", selection: $searchType) {
                            ForEach(SearchValueType.allCases) { t in
                                Text(t.rawValue).tag(t)
                            }
                        }
                        HStack {
                            TextField("Value to find (e.g. 500 or player name)", text: $searchQuery)
                                .font(.system(.body, design: .monospaced))
                                .autocorrectionDisabled()
                                .textInputAutocapitalization(.never)
                            Button("Scan") {
                                runBinarySearch()
                            }
                            .buttonStyle(.borderedProminent)
                        }
                    }

                    if !searchMatches.isEmpty {
                        Section("Batch Modify All \(searchMatches.count) Matches") {
                            HStack {
                                TextField("New value", text: $replaceValueText)
                                    .font(.system(.body, design: .monospaced))
                                Button("Replace All") {
                                    replaceAllMatches()
                                }
                                .buttonStyle(.bordered)
                            }
                        }

                        Section("Matches (\(searchMatches.count)) — Tap to Jump") {
                            ForEach(searchMatches) { m in
                                Button {
                                    selectedOffset = m.offset
                                    showingScanner = false
                                } label: {
                                    HStack {
                                        Text(String(format: "0x%06X (%d)", m.offset, m.offset))
                                            .font(.system(.subheadline, design: .monospaced).weight(.semibold))
                                        Spacer()
                                        Text(m.preview)
                                            .font(.system(.caption, design: .monospaced))
                                            .foregroundStyle(.secondary)
                                    }
                                }
                            }
                        }
                    }
                }
                .navigationTitle("Binary Cheat / Value Scanner")
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Close") { showingScanner = false }
                    }
                }
            }
        }
        .alert("Save Failed", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear {
            guard !loaded else { return }
            loaded = true
            do {
                let (payload, fmt) = try SaveCompressionFormat.decode(document.store.load())
                data = payload
                compression = fmt
                document.dirty = false
                document.ready = true
            } catch { self.error = error.localizedDescription }
        }
    }

    private func save() -> Bool {
        do {
            let out = try SaveCompressionFormat.encode(data, format: compression)
            try document.commit(out)
            return true
        } catch {
            document.saveError = error.localizedDescription
            return false
        }
    }

    private func encodePattern(query: String, type: SearchValueType) -> Data? {
        let q = query.trimmingCharacters(in: .whitespaces)
        switch type {
        case .int32LE:
            guard let v = Int32(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 4)
        case .uint32LE:
            guard let v = UInt32(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 4)
        case .int16LE:
            guard let v = Int16(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 2)
        case .int64LE:
            guard let v = Int64(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 8)
        case .float32LE:
            guard let v = Float(q) else { return nil }
            var bits = v.bitPattern.littleEndian
            return Data(bytes: &bits, count: 4)
        case .float64LE:
            guard let v = Double(q) else { return nil }
            var bits = v.bitPattern.littleEndian
            return Data(bytes: &bits, count: 8)
        case .ascii:
            return q.data(using: .utf8)
        case .utf16LE:
            return q.data(using: .utf16LittleEndian)
        case .hex:
            let clean = q.replacingOccurrences(of: " ", with: "")
            guard clean.count >= 2 && clean.count % 2 == 0 else { return nil }
            var bytes = Data()
            var idx = clean.startIndex
            while idx < clean.endIndex {
                let next = clean.index(idx, offsetBy: 2)
                guard let b = UInt8(clean[idx..<next], radix: 16) else { return nil }
                bytes.append(b)
                idx = next
            }
            return bytes
        }
    }

    private func runBinarySearch() {
        guard let needle = encodePattern(query: searchQuery, type: searchType), !needle.isEmpty else {
            searchMatches = []
            return
        }
        var found: [BinarySearchMatch] = []
        var searchRange = data.startIndex..<data.endIndex
        while let r = data.range(of: needle, options: [], in: searchRange) {
            let off = r.lowerBound
            let ctxEnd = min(data.endIndex, off + 8)
            let hexPreview = data[off..<ctxEnd].map { String(format: "%02X", $0) }.joined(separator: " ")
            found.append(BinarySearchMatch(offset: off, preview: hexPreview))
            if found.count >= 250 { break }
            searchRange = (r.lowerBound + 1)..<data.endIndex
        }
        searchMatches = found
    }

    private func replaceAllMatches() {
        guard let replacement = encodePattern(query: replaceValueText, type: searchType) else { return }
        for m in searchMatches {
            guard m.offset + replacement.count <= data.count else { continue }
            data.replaceSubrange(m.offset..<(m.offset + replacement.count), with: replacement)
        }
        document.markChanged()
        runBinarySearch()
    }
}

private struct BinaryValueInspectorBar: View {
    @Binding var data: Data
    @Binding var offset: Int
    let totalCount: Int
    let onMutate: () -> Void

    @State private var editingType: String?
    @State private var editValueString = ""

    private func readLE<T: FixedWidthInteger>(_ type: T.Type) -> T? {
        let size = MemoryLayout<T>.size
        guard offset >= 0, offset + size <= data.count else { return nil }
        var val: T = 0
        _ = withUnsafeMutableBytes(of: &val) { dst in
            data.copyBytes(to: dst, from: offset..<(offset + size))
        }
        return T(littleEndian: val)
    }

    private func writeLE<T: FixedWidthInteger>(_ val: T) {
        let size = MemoryLayout<T>.size
        guard offset >= 0, offset + size <= data.count else { return }
        var le = val.littleEndian
        let bytes = withUnsafeBytes(of: &le) { Data($0) }
        data.replaceSubrange(offset..<(offset + size), with: bytes)
        onMutate()
    }

    private var float32Val: Float? {
        guard let bits = readLE(UInt32.self) else { return nil }
        return Float(bitPattern: bits)
    }

    private var float64Val: Double? {
        guard let bits = readLE(UInt64.self) else { return nil }
        return Double(bitPattern: bits)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(String(format: "Offset: 0x%06X (%d)", offset, offset))
                    .font(.caption.monospaced().weight(.bold))
                Spacer()
                Stepper("", value: $offset, in: 0...max(0, totalCount - 1))
                    .labelsHidden()
                    .controlSize(.mini)
            }
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 10) {
                    if let v = readLE(UInt8.self) {
                        inspectorChip(label: "UInt8", value: "\(v)") {
                            if let nv = UInt8($0) { writeLE(nv) }
                        }
                    }
                    if let v = readLE(Int16.self) {
                        inspectorChip(label: "Int16", value: "\(v)") {
                            if let nv = Int16($0) { writeLE(nv) }
                        }
                    }
                    if let v = readLE(Int32.self) {
                        inspectorChip(label: "Int32", value: "\(v)") {
                            if let nv = Int32($0) { writeLE(nv) }
                        }
                    }
                    if let v = readLE(UInt32.self) {
                        inspectorChip(label: "UInt32", value: "\(v)") {
                            if let nv = UInt32($0) { writeLE(nv) }
                        }
                    }
                    if let v = readLE(Int64.self) {
                        inspectorChip(label: "Int64", value: "\(v)") {
                            if let nv = Int64($0) { writeLE(nv) }
                        }
                    }
                    if let f = float32Val {
                        inspectorChip(label: "Float32", value: String(format: "%.4g", f)) {
                            if let nv = Float($0) { writeLE(nv.bitPattern) }
                        }
                    }
                    if let d = float64Val {
                        inspectorChip(label: "Double", value: String(format: "%.6g", d)) {
                            if let nv = Double($0) { writeLE(nv.bitPattern) }
                        }
                    }
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color.secondary.opacity(0.08))
    }

    @ViewBuilder
    private func inspectorChip(label: String, value: String, commit: @escaping (String) -> Void) -> some View {
        Button {
            editValueString = value
            editingType = label
        } label: {
            VStack(alignment: .leading, spacing: 1) {
                Text(label).font(.system(size: 9, weight: .semibold, design: .monospaced)).foregroundStyle(.secondary)
                Text(value).font(.system(.caption, design: .monospaced).weight(.medium)).foregroundStyle(.primary)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(.background, in: RoundedRectangle(cornerRadius: 6))
        }
        .alert("Edit \(label) at 0x\(String(format: "%X", offset))",
               isPresented: Binding(get: { editingType == label }, set: { if !$0 { editingType = nil } })) {
            TextField(label, text: $editValueString)
                .keyboardType(.numbersAndPunctuation)
            Button("Write") { commit(editValueString); editingType = nil }
            Button("Cancel", role: .cancel) { editingType = nil }
        }
    }
}

private struct HexRow: View {
    let base: Int
    let slice: Data.SubSequence
    @Binding var selectedOffset: Int
    let onEdit: (Int, UInt8) -> Void

    @State private var editingOffset: Int?
    @State private var hexInput = ""

    var body: some View {
        HStack(spacing: 6) {
            Text(String(format: "%06X", base))
                .foregroundStyle(.secondary)
            HStack(spacing: 4) {
                ForEach(0..<16, id: \.self) { i in
                    let idx = slice.startIndex + i
                    if idx < slice.endIndex {
                        let b = slice[idx]
                        Text(String(format: "%02X", b))
                            .padding(.horizontal, 2)
                            .background(selectedOffset == idx ? Color.accentColor.opacity(0.3) : Color.clear,
                                        in: RoundedRectangle(cornerRadius: 3))
                            .onTapGesture {
                                selectedOffset = idx
                            }
                            .onLongPressGesture {
                                selectedOffset = idx
                                hexInput = String(format: "%02X", b)
                                editingOffset = idx
                            }
                    } else {
                        Text("  ")
                    }
                }
            }
            Spacer(minLength: 4)
            Text(asciiColumn)
                .foregroundStyle(.secondary)
        }
        .alert("Edit Byte", isPresented: Binding(get: { editingOffset != nil },
                                                 set: { if !$0 { editingOffset = nil } })) {
            TextField("Hex (00–FF)", text: $hexInput)
                .textInputAutocapitalization(.characters)
                .autocorrectionDisabled()
            Button("OK") {
                if let off = editingOffset,
                   let v = UInt8(hexInput.trimmingCharacters(in: .whitespaces), radix: 16) {
                    onEdit(off, v)
                }
                editingOffset = nil
            }
            Button("Cancel", role: .cancel) { editingOffset = nil }
        } message: {
            if let off = editingOffset {
                Text(String(format: "Offset 0x%06X", off))
            }
        }
    }

    private var asciiColumn: String {
        String(slice.map { (0x20...0x7E).contains($0) ? Character(UnicodeScalar($0)) : "." })
    }
}
