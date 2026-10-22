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
        let d = head(url, 4)
        guard d.count >= 2 else { return false }
        let b0 = d[d.startIndex], b1 = d[d.startIndex + 1]
        if b0 == 0x1F && b1 == 0x8B { return true } // gzip
        if b0 == 0x78 && (b1 == 0x01 || b1 == 0x5E || b1 == 0x9C || b1 == 0xDA) { return true } // zlib
        return false
    }

    static func looksLikeText(_ url: URL) -> Bool {
        let d = head(url, 4096)
        if d.isEmpty { return true }
        if d.contains(0) { return false }
        return String(data: d, encoding: .utf8) != nil
    }
}

// MARK: - Compression Helpers (Zlib / Gzip)

enum SaveCompressionFormat: String {
    case none = "Uncompressed"
    case zlib = "Zlib"
    case gzip = "Gzip"

    static func detect(_ data: Data) -> SaveCompressionFormat {
        guard data.count >= 2 else { return .none }
        let b0 = data[data.startIndex], b1 = data[data.startIndex + 1]
        if b0 == 0x1F && b1 == 0x8B { return .gzip }
        if b0 == 0x78 && (b1 == 0x01 || b1 == 0x5E || b1 == 0x9C || b1 == 0xDA) { return .zlib }
        return .none
    }

    static func decompress(_ data: Data) -> (Data, SaveCompressionFormat)? {
        let fmt = detect(data)
        guard fmt != .none, !data.isEmpty else { return nil }
        var stream = z_stream()
        let windowBits: Int32 = (fmt == .gzip) ? (MAX_WBITS + 16) : MAX_WBITS
        guard inflateInit2_(&stream, windowBits, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            return nil
        }
        defer { inflateEnd(&stream) }

        var out = Data(capacity: data.count * 3)
        let chunkSize = 32768
        var chunk = [UInt8](repeating: 0, count: chunkSize)

        let status: Int32 = data.withUnsafeBytes { rawBuf in
            guard let base = rawBuf.bindMemory(to: Bytef.self).baseAddress else { return Z_DATA_ERROR }
            stream.next_in = UnsafeMutablePointer<Bytef>(mutating: base)
            stream.avail_in = uInt(data.count)
            var ret: Int32 = Z_OK
            repeat {
                ret = chunk.withUnsafeMutableBufferPointer { outBuf in
                    stream.next_out = outBuf.baseAddress
                    stream.avail_out = uInt(chunkSize)
                    return inflate(&stream, Z_NO_FLUSH)
                }
                if ret == Z_OK || ret == Z_STREAM_END {
                    let have = chunkSize - Int(stream.avail_out)
                    if have > 0 { out.append(chunk, count: have) }
                } else {
                    break
                }
            } while ret != Z_STREAM_END
            return ret
        }
        guard status == Z_STREAM_END else { return nil }
        return (out, fmt)
    }

    static func compress(_ data: Data, format: SaveCompressionFormat) -> Data? {
        guard format != .none else { return data }
        var stream = z_stream()
        let windowBits: Int32 = (format == .gzip) ? (MAX_WBITS + 16) : MAX_WBITS
        guard deflateInit2_(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY,
                            ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            return nil
        }
        defer { deflateEnd(&stream) }

        var out = Data()
        let chunkSize = 32768
        var chunk = [UInt8](repeating: 0, count: chunkSize)

        let status: Int32 = data.withUnsafeBytes { rawBuf in
            let base = rawBuf.bindMemory(to: Bytef.self).baseAddress
            stream.next_in = UnsafeMutablePointer<Bytef>(mutating: base)
            stream.avail_in = uInt(data.count)
            var ret: Int32 = Z_OK
            repeat {
                ret = chunk.withUnsafeMutableBufferPointer { outBuf in
                    stream.next_out = outBuf.baseAddress
                    stream.avail_out = uInt(chunkSize)
                    return deflate(&stream, Z_FINISH)
                }
                if ret == Z_OK || ret == Z_STREAM_END {
                    let have = chunkSize - Int(stream.avail_out)
                    if have > 0 { out.append(chunk, count: have) }
                } else {
                    break
                }
            } while ret != Z_STREAM_END
            return ret
        }
        return status == Z_STREAM_END ? out : nil
    }
}

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
        guard !clean.isEmpty, clean != item.name, !clean.contains("/") else { return }
        run { try FileManager.default.moveItem(at: item.url, to: url.appending(path: clean)) }
    }

    func duplicate(_ item: FileItem) {
        run { try FileManager.default.copyItem(at: item.url, to: freeName(for: item.name, suffix: " copy")) }
    }

    func newFolder(named name: String) {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty else { return }
        run { try FileManager.default.createDirectory(at: freeName(for: clean, suffix: ""), withIntermediateDirectories: false) }
    }

    func newFile(named name: String) {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty else { return }
        run { try Data().write(to: freeName(for: clean, suffix: "")) }
    }

    func importFiles(_ urls: [URL]) {
        run {
            for src in urls {
                let scoped = src.startAccessingSecurityScopedResource()
                defer { if scoped { src.stopAccessingSecurityScopedResource() } }
                let dst = url.appending(path: src.lastPathComponent)
                if FileManager.default.fileExists(atPath: dst.path(percentEncoded: false)) {
                    try FileManager.default.removeItem(at: dst)
                }
                try FileManager.default.copyItem(at: src, to: dst)
            }
        }
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
                    description: Text("Launch \(app.name) first so it creates its save files, preferences, or databases in Documents and Library.")
                )
            } else {
                ForEach(DiscoveredSaveFile.Category.allCases, id: \.rawValue) { cat in
                    let group = filteredFiles.filter { $0.category == cat }
                    if !group.isEmpty {
                        Section(cat.rawValue) {
                            ForEach(group) { entry in
                                NavigationLink(value: entry.item) {
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
        .navigationTitle("\(app.name) — Save Editor")
        .searchable(text: $searchText, prompt: "Search saves, plists, databases…")
        .navigationDestination(for: FileItem.self) { item in
            FileEditorDestinationView(item: item)
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

        for r in roots {
            let dirURL = app.dataDir.appending(path: r)
            guard let enumerator = fm.enumerator(
                at: dirURL,
                includingPropertiesForKeys: [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey],
                options: [.skipsHiddenFiles]
            ) else { continue }

            for case let fileURL as URL in enumerator {
                let vals = try? fileURL.resourceValues(forKeys: [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey])
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
        guard let urls = try? fm.contentsOfDirectory(at: backupsDir, includingPropertiesForKeys: [.creationDateKey, .isDirectoryKey]) else {
            snapshots = []
            return
        }
        snapshots = urls.compactMap { u -> SaveSnapshot? in
            let v = try? u.resourceValues(forKeys: [.creationDateKey, .isDirectoryKey])
            guard v?.isDirectory == true else { return nil }
            return SaveSnapshot(url: u, name: u.lastPathComponent, created: v?.creationDate ?? Date())
        }.sorted { $0.created > $1.created }
    }

    private func createSnapshot(label: String) {
        let fm = FileManager.default
        let cleanLabel = label.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "/", with: "-")
        let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let folderName = cleanLabel.isEmpty ? stamp : "\(cleanLabel) (\(stamp))"
        let dest = backupsDir.appending(path: folderName)
        do {
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            for sub in ["Documents", "Library"] {
                let src = app.dataDir.appending(path: sub)
                if fm.fileExists(atPath: src.path(percentEncoded: false)) {
                    try fm.copyItem(at: src, to: dest.appending(path: sub))
                }
            }
            statusMessage = "Created snapshot '\(folderName)'"
            loadSnapshots()
        } catch {
            statusMessage = "Snapshot error: \(error.localizedDescription)"
        }
    }

    private func restoreSnapshot(_ snap: SaveSnapshot) {
        let fm = FileManager.default
        do {
            for sub in ["Documents", "Library"] {
                let src = snap.url.appending(path: sub)
                let dst = app.dataDir.appending(path: sub)
                if fm.fileExists(atPath: src.path(percentEncoded: false)) {
                    if fm.fileExists(atPath: dst.path(percentEncoded: false)) {
                        try fm.removeItem(at: dst)
                    }
                    try fm.copyItem(at: src, to: dst)
                }
            }
            statusMessage = "Restored snapshot '\(snap.name)'"
            scanSandbox()
        } catch {
            statusMessage = "Restore error: \(error.localizedDescription)"
        }
    }

    private func deleteSnapshot(_ snap: SaveSnapshot) {
        try? FileManager.default.removeItem(at: snap.url)
        loadSnapshots()
    }
}

// MARK: - Unified File Editor Router

struct FileEditorDestinationView: View {
    let item: FileItem
    @State private var overrideMode: EditorMode?

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
        .toolbar {
            if !item.isDirectory {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Picker("Open As", selection: Binding(
                            get: { overrideMode ?? .auto },
                            set: { overrideMode = ($0 == .auto) ? nil : $0 }
                        )) {
                            ForEach(EditorMode.allCases) { mode in
                                Text(mode.rawValue).tag(mode)
                            }
                        }
                    } label: {
                        Image(systemName: "slider.horizontal.2.square")
                    }
                }
            }
        }
    }
}

// MARK: - Directory Listing

struct DirectoryView: View {
    let url: URL
    let title: String
    let isRoot: Bool

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
                NavigationLink(value: item) {
                    FileRow(item: item)
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
            } else {
                FileEditorDestinationView(item: item)
            }
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

// MARK: - Reliable Save Infrastructure (shared by every editor)

/// Writes edited save data and proves it landed: makes read-only files writable,
/// writes atomically, then re-reads the file and compares bytes.
enum SafeFileWriter {
    struct VerifyError: LocalizedError {
        var errorDescription: String? { "The file was written but reading it back did not match. The game or another process may be modifying it." }
    }

    static func write(_ data: Data, to url: URL) throws {
        let fm = FileManager.default
        let path = url.path(percentEncoded: false)
        if fm.fileExists(atPath: path), !fm.isWritableFile(atPath: path) {
            try? fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: path)
        }
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            // Atomic replace can fail in sandboxed/locked directories; fall back to in-place write.
            try data.write(to: url, options: [])
        }
        guard let back = try? Data(contentsOf: url), back == data else { throw VerifyError() }
    }
}

/// Always-visible save bar pinned above the keyboard, plus Cmd+S, autosave when the
/// editor is closed or the app leaves the foreground, and a visible saved/unsaved state.
struct SaveBarModifier: ViewModifier {
    @Binding var dirty: Bool
    let save: () -> Void
    @Environment(\.scenePhase) private var scenePhase
    @State private var lastSaved: Date?

    func body(content: Content) -> some View {
        content
            .safeAreaInset(edge: .bottom, spacing: 0) { bar }
            .toolbar {
                // Second Save button riding directly on top of the keyboard.
                ToolbarItemGroup(placement: .keyboard) {
                    Button("Save") { save() }
                        .fontWeight(.semibold)
                        .disabled(!dirty)
                    Spacer()
                    Button("Done") {
                        UIApplication.shared.sendAction(#selector(UIResponder.resignFirstResponder),
                                                        to: nil, from: nil, for: nil)
                    }
                }
            }
            .onChange(of: dirty) { old, new in
                if old && !new { lastSaved = Date() }
            }
            .onChange(of: scenePhase) { _, phase in
                if phase != .active && dirty { save() }
            }
            .onDisappear {
                if dirty { save() }
            }
    }

    private var bar: some View {
        HStack(spacing: 10) {
            Image(systemName: dirty ? "circle.fill" : "checkmark.circle.fill")
                .foregroundStyle(dirty ? Color.orange : Color.green)
            VStack(alignment: .leading, spacing: 1) {
                Text(dirty ? "Unsaved changes" : "All changes saved")
                    .font(.footnote.weight(.semibold))
                if !dirty, let lastSaved {
                    Text("Saved \(lastSaved.formatted(date: .omitted, time: .standard))")
                        .font(.caption2).foregroundStyle(.secondary)
                } else if dirty {
                    Text("Saves automatically when you close this file")
                        .font(.caption2).foregroundStyle(.secondary)
                }
            }
            Spacer()
            Button {
                save()
            } label: {
                Label("Save", systemImage: "square.and.arrow.down")
                    .font(.subheadline.weight(.semibold))
            }
            .buttonStyle(.borderedProminent)
            .disabled(!dirty)
            .keyboardShortcut("s", modifiers: .command)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.bar)
        .overlay(alignment: .top) { Divider() }
    }
}

extension View {
    func saveBar(dirty: Binding<Bool>, save: @escaping () -> Void) -> some View {
        modifier(SaveBarModifier(dirty: dirty, save: save))
    }
}

// MARK: - Text Editor (with transparent Zlib/Gzip support)

struct TextEditorView: View {
    let url: URL
    @State private var text = ""
    @State private var encoding: String.Encoding = .utf8
    @State private var compression: SaveCompressionFormat = .none
    @State private var dirty = false
    @State private var baseline = ""
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
            TextEditor(text: $text)
                .font(.system(.footnote, design: .monospaced))
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .onChange(of: text) { _, new in dirty = (new != baseline) }
        }
        .navigationTitle(url.lastPathComponent)
        .saveBar(dirty: $dirty) { save() }
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                ShareLink(item: url)
            }
        }
        .alert("Save Failed", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear { load() }
    }

    private func load() {
        guard let rawData = try? Data(contentsOf: url) else { return }
        let payload: Data
        if let (decomp, fmt) = SaveCompressionFormat.decompress(rawData) {
            payload = decomp
            compression = fmt
        } else {
            payload = rawData
            compression = .none
        }
        for enc: String.Encoding in [.utf8, .utf16, .isoLatin1, .ascii] {
            if let s = String(data: payload, encoding: enc) {
                text = s
                baseline = s
                encoding = enc
                dirty = false
                return
            }
        }
    }

    private func save() {
        do {
            guard let raw = text.data(using: encoding) ?? text.data(using: .utf8) else {
                throw CocoaError(.fileWriteInapplicableStringEncoding)
            }
            let out = (compression == .none) ? raw : (SaveCompressionFormat.compress(raw, format: compression) ?? raw)
            try SafeFileWriter.write(out, to: url)
            KeychainSaveSync.syncFileToKeychainIfNeeded(fileURL: url, text: text)
            baseline = text
            dirty = false
        } catch {
            self.error = error.localizedDescription
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
}

struct KeyValueEditorView: View {
    let url: URL
    @State private var entries: [KeyValueEntry] = []
    @State private var searchText = ""
    @State private var rawMode = false
    @State private var rawText = ""
    @State private var dirty = false
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
                TextEditor(text: $rawText)
                    .font(.system(.footnote, design: .monospaced))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .onChange(of: rawText) { _, _ in dirty = true }
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
                                    set: { entries[idx].value = $0; dirty = true }
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
                        dirty = true
                    }
                }
                .searchable(text: $searchText, prompt: "Filter keys or values…")
            }
        }
        .navigationTitle(url.lastPathComponent)
        .saveBar(dirty: $dirty) { save() }
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Button {
                    if rawMode {
                        parseINI(rawText)
                    } else {
                        rawText = serializeINI()
                    }
                    rawMode.toggle()
                } label: {
                    Image(systemName: rawMode ? "list.bullet" : "doc.plaintext")
                }
                if !rawMode {
                    Button {
                        newKey = ""
                        newVal = ""
                        addingEntry = true
                    } label: {
                        Image(systemName: "plus")
                    }
                }
            }
        }
        .alert("Add Config Key", isPresented: $addingEntry) {
            TextField("Section (optional)", text: $newSection)
            TextField("Key", text: $newKey)
            TextField("Value", text: $newVal)
            Button("Add") {
                guard !newKey.isEmpty else { return }
                entries.append(KeyValueEntry(section: newSection, key: newKey, value: newVal))
                dirty = true
            }
            Button("Cancel", role: .cancel) {}
        }
        .onAppear { load() }
    }

    private func load() {
        guard let d = try? Data(contentsOf: url),
              let s = String(data: d, encoding: .utf8) ?? String(data: d, encoding: .isoLatin1) else { return }
        rawText = s
        parseINI(s)
        dirty = false
    }

    private func parseINI(_ text: String) {
        var parsed: [KeyValueEntry] = []
        var currentSection = ""
        for line in text.components(separatedBy: .newlines) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty || trimmed.hasPrefix("#") || trimmed.hasPrefix(";") || trimmed.hasPrefix("//") {
                parsed.append(KeyValueEntry(section: currentSection, key: "", value: "", comment: line))
                continue
            }
            if trimmed.hasPrefix("[") && trimmed.hasSuffix("]") {
                currentSection = String(trimmed.dropFirst().dropLast())
                continue
            }
            if let eq = line.firstIndex(of: "=") ?? line.firstIndex(of: ":") {
                let k = String(line[..<eq]).trimmingCharacters(in: .whitespacesAndNewlines)
                    .trimmingCharacters(in: CharacterSet(charactersIn: "\""))
                var v = String(line[line.index(after: eq)...]).trimmingCharacters(in: .whitespacesAndNewlines)
                if v.hasSuffix(";") { v.removeLast() }
                v = v.trimmingCharacters(in: CharacterSet(charactersIn: "\""))
                parsed.append(KeyValueEntry(section: currentSection, key: k, value: v))
            } else {
                parsed.append(KeyValueEntry(section: currentSection, key: "", value: "", comment: line))
            }
        }
        entries = parsed
    }

    private func serializeINI() -> String {
        var lines: [String] = []
        var lastSection = ""
        let isStrings = url.pathExtension.lowercased() == "strings"
        for e in entries {
            if e.key.isEmpty, let c = e.comment {
                lines.append(c)
                continue
            }
            if e.section != lastSection && !e.section.isEmpty {
                lines.append("[\(e.section)]")
                lastSection = e.section
            }
            if isStrings {
                lines.append("\"\(e.key)\" = \"\(e.value)\";")
            } else {
                lines.append("\(e.key)=\(e.value)")
            }
        }
        return lines.joined(separator: "\n")
    }

    private func save() {
        let outText = rawMode ? rawText : serializeINI()
        do {
            try SafeFileWriter.write(outText.data(using: .utf8) ?? Data(), to: url)
            dirty = false
        } catch {
            self.error = error.localizedDescription
        }
    }
}

// MARK: - Structured JSON Editor

@MainActor
@Observable
final class JSONNode: Identifiable {
    let id = UUID()
    var key: String
    var value: Kind

    enum Kind {
        case string(String)
        case number(Double, isInteger: Bool)
        case bool(Bool)
        case null
        case array([JSONNode])
        case object([JSONNode])

        var label: String {
            switch self {
            case .string: "String"
            case .number(_, let isInt): isInt ? "Integer" : "Number"
            case .bool: "Boolean"
            case .null: "Null"
            case .array(let a): "Array (\(a.count))"
            case .object(let o): "Object (\(o.count))"
            }
        }
    }

    init(key: String, value: Kind) {
        self.key = key
        self.value = value
    }

    static func fromAny(_ any: Any, key: String = "root") -> JSONNode {
        if let dict = any as? [String: Any] {
            let children = dict.keys.sorted().map { k in fromAny(dict[k]!, key: k) }
            return JSONNode(key: key, value: .object(children))
        } else if let arr = any as? [Any] {
            let children = arr.enumerated().map { i, v in fromAny(v, key: "[\(i)]") }
            return JSONNode(key: key, value: .array(children))
        } else if let s = any as? String {
            return JSONNode(key: key, value: .string(s))
        } else if let n = any as? NSNumber {
            if CFGetTypeID(n) == CFBooleanGetTypeID() {
                return JSONNode(key: key, value: .bool(n.boolValue))
            }
            let d = n.doubleValue
            let isInt = floor(d) == d && abs(d) < Double(Int64.max)
            return JSONNode(key: key, value: .number(d, isInteger: isInt))
        } else {
            return JSONNode(key: key, value: .null)
        }
    }

    func toAny() -> Any {
        switch value {
        case .string(let s): return s
        case .number(let d, let isInt): return isInt ? NSNumber(value: Int64(d)) : NSNumber(value: d)
        case .bool(let b): return b
        case .null: return NSNull()
        case .array(let a): return a.map { $0.toAny() }
        case .object(let o):
            var dict: [String: Any] = [:]
            for child in o { dict[child.key] = child.toAny() }
            return dict
        }
    }
}

struct JSONEditorView: View {
    let url: URL
    @State private var root: JSONNode?
    @State private var rawMode = false
    @State private var rawText = ""
    @State private var compression: SaveCompressionFormat = .none
    @State private var dirty = false
    @State private var error: String?

    var body: some View {
        Group {
            if rawMode {
                TextEditor(text: $rawText)
                    .font(.system(.footnote, design: .monospaced))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .onChange(of: rawText) { _, _ in dirty = true }
            } else if let root {
                JSONContainerListView(node: root, isRoot: true, onMutate: { dirty = true })
            } else {
                ContentUnavailableView("Invalid JSON", systemImage: "exclamationmark.triangle")
            }
        }
        .navigationTitle(url.lastPathComponent)
        .saveBar(dirty: $dirty) { save() }
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Button {
                    toggleRawMode()
                } label: {
                    Image(systemName: rawMode ? "list.bullet.indent" : "curlybraces")
                }
                ShareLink(item: url)
            }
        }
        .alert("JSON Error", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear { load() }
    }

    private func load() {
        guard let rawData = try? Data(contentsOf: url) else { return }
        let payload: Data
        if let (decomp, fmt) = SaveCompressionFormat.decompress(rawData) {
            payload = decomp
            compression = fmt
        } else {
            payload = rawData
            compression = .none
        }
        rawText = String(data: payload, encoding: .utf8) ?? ""
        if let obj = try? JSONSerialization.jsonObject(with: payload, options: [.fragmentsAllowed]) {
            root = JSONNode.fromAny(obj)
        }
        dirty = false
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
            if let data = try? JSONSerialization.data(withJSONObject: root.toAny(), options: [.prettyPrinted, .sortedKeys]),
               let s = String(data: data, encoding: .utf8) {
                rawText = s
            }
            rawMode = true
        }
    }

    private func save() {
        do {
            let jsonData: Data
            if rawMode {
                guard let d = rawText.data(using: .utf8) else { return }
                _ = try JSONSerialization.jsonObject(with: d, options: [.fragmentsAllowed])
                jsonData = d
            } else if let root {
                jsonData = try JSONSerialization.data(withJSONObject: root.toAny(), options: [.prettyPrinted, .sortedKeys])
            } else {
                return
            }
            let out = (compression == .none) ? jsonData : (SaveCompressionFormat.compress(jsonData, format: compression) ?? jsonData)
            try SafeFileWriter.write(out, to: url)
            dirty = false
        } catch {
            self.error = error.localizedDescription
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
                    a.remove(atOffsets: offsets)
                    node.value = .object(a)
                    onMutate()
                case .array(var a):
                    a.remove(atOffsets: offsets)
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
                        a.append(JSONNode(key: "[\(a.count)]", value: .number(0, isInteger: true)))
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
        .navigationTitle(isRoot ? "JSON Tree" : node.key)
        .alert("New JSON Key", isPresented: $addingKey) {
            TextField("Key", text: $newKeyName)
            Button("Add") {
                if case .object(var a) = node.value, !newKeyName.isEmpty {
                    a.append(JSONNode(key: newKeyName, value: .number(0, isInteger: true)))
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
        case .bool(let b):
            Toggle(isOn: Binding(
                get: { b },
                set: { node.value = .bool($0); onMutate() }
            )) {
                Text(node.key).font(.system(.body, design: .monospaced))
            }
        case .number(let d, let isInt):
            HStack {
                Text(node.key).font(.system(.body, design: .monospaced))
                Spacer()
                TextField("0", text: Binding(
                    get: { isInt ? String(Int64(d)) : String(d) },
                    set: { s in
                        if let i = Int64(s) {
                            node.value = .number(Double(i), isInteger: true)
                            onMutate()
                        } else if let v = Double(s) {
                            node.value = .number(v, isInteger: false)
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

struct SQLiteEditorView: View {
    let url: URL
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
                                Button {
                                    editCellText = val.displayString == "NULL" ? "" : val.displayString
                                    editingCell = EditingCellContext(rowid: row.rowid, column: col.name, currentValue: val)
                                } label: {
                                    HStack {
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(col.name)
                                                .font(.caption.monospaced().weight(.semibold))
                                                .foregroundStyle(.primary)
                                            if !col.type.isEmpty {
                                                Text(col.type)
                                                    .font(.caption2)
                                                    .foregroundStyle(.secondary)
                                            }
                                        }
                                        Spacer()
                                        Text(val.displayString)
                                            .font(.system(.subheadline, design: .monospaced))
                                            .foregroundStyle(val == .null ? .secondary : .accentColor)
                                            .lineLimit(2)
                                    }
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
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
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
                            updateCellToNull(ctx: ctx)
                            editingCell = nil
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
                            updateCell(ctx: ctx, newText: editCellText)
                            editingCell = nil
                        }
                    }
                }
            }
            .presentationDetents([.medium])
        }
        .onAppear { loadDatabase() }
    }

    private func openDB() -> OpaquePointer? {
        var db: OpaquePointer?
        if sqlite3_open_v2(url.path(percentEncoded: false), &db, SQLITE_OPEN_READWRITE, nil) == SQLITE_OK {
            return db
        }
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

    private func updateCell(ctx: EditingCellContext, newText: String) {
        guard let db = openDB() else { return }
        defer { sqlite3_close(db) }

        let escapedTable = selectedTable.replacingOccurrences(of: "\"", with: "\"\"")
        let escapedCol = ctx.column.replacingOccurrences(of: "\"", with: "\"\"")
        let sql = "UPDATE \"\(escapedTable)\" SET \"\(escapedCol)\" = ? WHERE rowid = ?;"
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK else {
            sqlResultMessage = String(cString: sqlite3_errmsg(db))
            return
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
            loadTable(selectedTable)
        } else {
            sqlResultMessage = String(cString: sqlite3_errmsg(db))
        }
    }

    private func updateCellToNull(ctx: EditingCellContext) {
        guard let db = openDB() else { return }
        defer { sqlite3_close(db) }
        let escapedTable = selectedTable.replacingOccurrences(of: "\"", with: "\"\"")
        let escapedCol = ctx.column.replacingOccurrences(of: "\"", with: "\"\"")
        let sql = "UPDATE \"\(escapedTable)\" SET \"\(escapedCol)\" = NULL WHERE rowid = \(ctx.rowid);"
        sqlite3_exec(db, sql, nil, nil, nil)
        loadTable(selectedTable)
    }

    private func deleteRows(at offsets: IndexSet) {
        guard let db = openDB() else { return }
        defer { sqlite3_close(db) }
        let escapedTable = selectedTable.replacingOccurrences(of: "\"", with: "\"\"")
        for idx in offsets {
            let rid = filteredRows[idx].rowid
            sqlite3_exec(db, "DELETE FROM \"\(escapedTable)\" WHERE rowid = \(rid);", nil, nil, nil)
        }
        loadTable(selectedTable)
    }

    private func executeCustomSQL(_ sql: String) {
        guard let db = openDB() else { return }
        defer { sqlite3_close(db) }
        var errMsg: UnsafeMutablePointer<CChar>?
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
@MainActor
@Observable
final class PlistNode: Identifiable {
    let id = UUID()
    var key: String
    var value: Kind

    enum Kind {
        case string(String)
        case integer(Int64)
        case real(Double)
        case bool(Bool)
        case date(Date)
        case data(Data)
        case uid(Any) // Preserves CFKeyedArchiverUID opaque reference
        case array([PlistNode])
        case dict([PlistNode])

        var typeLabel: String {
            switch self {
            case .string: "String"
            case .integer: "Integer"
            case .real: "Number"
            case .bool: "Boolean"
            case .date: "Date"
            case .data(let d): "Data (\(d.count) B)"
            case .uid(let ref): "UID (\(PlistNode.uidIndex(ref) ?? 0))"
            case .array(let a): "Array (\(a.count))"
            case .dict(let d): "Dict (\(d.count))"
            }
        }
    }

    init(key: String, value: Kind) {
        self.key = key
        self.value = value
    }

    static func isKeyedArchiverUID(_ any: Any) -> Bool {
        let desc = String(describing: any)
        return desc.contains("CFKeyedArchiverUID")
    }

    static func uidIndex(_ any: Any) -> Int? {
        if let num = any as? NSNumber { return num.intValue }
        if let i = any as? Int { return i }
        let desc = String(describing: any)
        // Format: "<CFKeyedArchiverUID ...>{value = 42}"
        guard let range = desc.range(of: "value = ") else { return nil }
        let tail = desc[range.upperBound...]
        let digits = tail.prefix { $0.isNumber }
        return Int(digits)
    }

    static func fromAny(_ any: Any, key: String = "Root") -> PlistNode {
        if let dict = any as? [String: Any] {
            if dict.count == 1, let uidVal = dict["CF$UID"] as? NSNumber {
                return PlistNode(key: key, value: .uid(uidVal.intValue))
            }
            let children = dict.keys.sorted().map { k in fromAny(dict[k]!, key: k) }
            return PlistNode(key: key, value: .dict(children))
        } else if let arr = any as? [Any] {
            let children = arr.enumerated().map { i, v in fromAny(v, key: "[\(i)]") }
            return PlistNode(key: key, value: .array(children))
        } else if let s = any as? String {
            return PlistNode(key: key, value: .string(s))
        } else if let d = any as? Date {
            return PlistNode(key: key, value: .date(d))
        } else if let d = any as? Data {
            return PlistNode(key: key, value: .data(d))
        } else if let n = any as? NSNumber {
            if CFGetTypeID(n) == CFBooleanGetTypeID() {
                return PlistNode(key: key, value: .bool(n.boolValue))
            }
            let t = String(cString: n.objCType)
            if t == "f" || t == "d" {
                return PlistNode(key: key, value: .real(n.doubleValue))
            }
            return PlistNode(key: key, value: .integer(n.int64Value))
        } else if isKeyedArchiverUID(any) {
            return PlistNode(key: key, value: .uid(any))
        } else {
            return PlistNode(key: key, value: .string(String(describing: any)))
        }
    }

    func toAny() -> Any {
        switch value {
        case .string(let s): return s
        case .integer(let i): return NSNumber(value: i)
        case .real(let r): return NSNumber(value: r)
        case .bool(let b): return NSNumber(value: b)
        case .date(let d): return d
        case .data(let d): return d
        case .uid(let ref):
            if let idx = PlistNode.uidIndex(ref) {
                return ["CF$UID": NSNumber(value: idx)]
            }
            return ref
        case .array(let a): return a.map { $0.toAny() }
        case .dict(let d):
            var out: [String: Any] = [:]
            for c in d { out[c.key] = c.toAny() }
            return out
        }
    }

    var isKeyedArchiveRoot: Bool {
        guard case .dict(let d) = value else { return false }
        return d.contains(where: { $0.key == "$archiver" }) && d.contains(where: { $0.key == "$objects" })
    }

    var keyedArchiveObjects: [PlistNode]? {
        guard case .dict(let d) = value,
              let objsNode = d.first(where: { $0.key == "$objects" }),
              case .array(let arr) = objsNode.value else { return nil }
        return arr
    }
}

struct PlistEditorView: View {
    let url: URL
    @State private var root: PlistNode?
    @State private var format: PropertyListSerialization.PropertyListFormat = .binary
    @State private var compression: SaveCompressionFormat = .none
    @State private var viewMode: ViewMode = .smart
    @State private var dirty = false
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
                    KeyedArchiveObjectsView(root: root, onMutate: { dirty = true })
                } else {
                    PlistContainerView(node: root, isRoot: true, archiveRoot: root, onMutate: { dirty = true })
                }
            } else {
                ContentUnavailableView("Invalid Property List", systemImage: "exclamationmark.triangle",
                                       description: Text(error ?? ""))
            }
        }
        .navigationTitle(url.lastPathComponent)
        .saveBar(dirty: $dirty) { save() }
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Menu {
                    Picker("Format", selection: $format) {
                        Text("Binary Plist").tag(PropertyListSerialization.PropertyListFormat.binary)
                        Text("XML Plist").tag(PropertyListSerialization.PropertyListFormat.xml)
                    }
                    .onChange(of: format) { _, _ in dirty = true }
                } label: {
                    Image(systemName: "gearshape")
                }
                ShareLink(item: url)
            }
        }
        .alert("Save Failed", isPresented: Binding(get: { error != nil && root != nil },
                                                    set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear { load() }
    }

    private func load() {
        do {
            let rawData = try Data(contentsOf: url)
            let data: Data
            if let (decomp, fmt) = SaveCompressionFormat.decompress(rawData) {
                data = decomp
                compression = fmt
            } else {
                data = rawData
                compression = .none
            }
            var fmt: PropertyListSerialization.PropertyListFormat = .binary
            let obj = try PropertyListSerialization.propertyList(from: data, options: [], format: &fmt)
            format = fmt
            root = PlistNode.fromAny(obj)
            dirty = false
        } catch {
            self.error = error.localizedDescription
        }
    }

    private func save() {
        guard let root else { return }
        do {
            let data = try PropertyListSerialization.data(fromPropertyList: root.toAny(), format: format, options: 0)
            let out = (compression == .none) ? data : (SaveCompressionFormat.compress(data, format: compression) ?? data)
            try SafeFileWriter.write(out, to: url)
            KeychainSaveSync.syncKeychainToFileIfNeeded(keychainURL: url, plistRoot: root)
            dirty = false
        } catch {
            self.error = error.localizedDescription
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
            c.remove(atOffsets: offsets)
            node.value = .dict(c)
        case .array(var c):
            c.remove(atOffsets: offsets)
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
                        if let serialized = try? PropertyListSerialization.data(
                            fromPropertyList: nestedPlist.toAny(),
                            format: nestedPlistFormat,
                            options: 0
                        ) {
                            data = serialized
                            onUpdate(serialized)
                        }
                    }
                }
            case .text:
                TextEditor(text: $textRepresentation)
                    .font(.system(.footnote, design: .monospaced))
                    .onChange(of: textRepresentation) { _, newValue in
                        if let encoded = newValue.data(using: .utf8) {
                            data = encoded
                            onUpdate(encoded)
                        }
                    }
            case .hex:
                ScrollView {
                    Text(data.map { String(format: "%02X", $0) }.joined(separator: " "))
                        .font(.system(.caption, design: .monospaced))
                        .padding()
                }
            }
        }
        .navigationTitle(key)
        .onAppear {
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
    @State private var dirty = false
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
                    dirty = true
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
                        dirty = true
                    }
                    .listRowInsets(EdgeInsets(top: 2, leading: 10, bottom: 2, trailing: 10))
                }
            }
            .listStyle(.plain)
            .font(.system(.caption, design: .monospaced))
        }
        .navigationTitle(url.lastPathComponent)
        .saveBar(dirty: $dirty) { save() }
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Button {
                    showingScanner = true
                } label: {
                    Label("Value Scanner", systemImage: "magnifyingglass.circle")
                }
                ShareLink(item: url)
            }
        }
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
            guard let raw = try? Data(contentsOf: url) else { return }
            if let (decomp, fmt) = SaveCompressionFormat.decompress(raw) {
                data = decomp
                compression = fmt
            } else {
                data = raw
                compression = .none
            }
            dirty = false
        }
    }

    private func save() {
        do {
            let out = (compression == .none) ? data : (SaveCompressionFormat.compress(data, format: compression) ?? data)
            try SafeFileWriter.write(out, to: url)
            dirty = false
        } catch {
            self.error = error.localizedDescription
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
        dirty = true
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
