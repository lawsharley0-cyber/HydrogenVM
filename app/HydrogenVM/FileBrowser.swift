import SwiftUI
import UniformTypeIdentifiers

// Per-app file explorer: the app's sandbox (Documents, Library — saves and settings live
// here — and tmp) next to its bundle (the game's own files). Saves can be edited (plists
// in a structured editor, text files, raw bytes), and files can be imported, exported,
// renamed, duplicated and deleted.

// MARK: - Model

struct FileItem: Identifiable, Hashable {
    let url: URL
    let isDirectory: Bool
    let size: Int64
    let modified: Date?
    var id: URL { url }
    var name: String { url.lastPathComponent }
}

enum FileKind {
    case folder, image, plist, text, binary

    static let textExtensions: Set<String> = [
        "txt", "json", "xml", "ini", "cfg", "conf", "lua", "js", "csv", "yml", "yaml", "log", "html", "htm", "css",
        "strings", "md", "dat.txt", "properties", "sh", "lst", "tsv", "fnt", "atlas", "plist.txt", "glsl", "vsh", "fsh",
    ]
    static let imageExtensions: Set<String> = ["png", "jpg", "jpeg", "gif", "bmp", "tif", "tiff"]

    init(url: URL, isDirectory: Bool) {
        if isDirectory { self = .folder; return }
        let ext = url.pathExtension.lowercased()
        if Self.imageExtensions.contains(ext) { self = .image }
        else if ext == "plist" || FileKind.looksLikePlist(url) { self = .plist }
        else if Self.textExtensions.contains(ext) || FileKind.looksLikeText(url) { self = .text }
        else { self = .binary }
    }

    var symbol: String {
        switch self {
        case .folder: "folder.fill"
        case .image: "photo"
        case .plist: "list.bullet.rectangle"
        case .text: "doc.text"
        case .binary: "doc"
        }
    }

    private static func head(_ url: URL, _ n: Int) -> Data {
        guard let h = try? FileHandle(forReadingFrom: url) else { return Data() }
        defer { try? h.close() }
        return (try? h.read(upToCount: n)) ?? Data()
    }
    static func looksLikePlist(_ url: URL) -> Bool {
        let d = head(url, 64)
        if d.starts(with: Array("bplist".utf8)) { return true }
        guard let s = String(data: d, encoding: .utf8) else { return false }
        return s.contains("<plist") || s.contains("<!DOCTYPE plist")
    }
    static func looksLikeText(_ url: URL) -> Bool {
        let d = head(url, 4096)
        if d.isEmpty { return true }
        if d.contains(0) { return false }
        return String(data: d, encoding: .utf8) != nil
    }
}

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

    /// Copies picked files in, replacing files of the same name (the usual intent when
    /// restoring a save).
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

@MainActor
enum FileFormat {
    static let size = ByteCountFormatter()
    static func subtitle(_ item: FileItem) -> String {
        var parts: [String] = []
        if !item.isDirectory { parts.append(size.string(fromByteCount: item.size)) }
        if let m = item.modified { parts.append(m.formatted(date: .abbreviated, time: .shortened)) }
        return parts.joined(separator: " · ")
    }
}

// MARK: - Browser

/// Entry point: a sheet with its own navigation stack.
struct FileBrowserView: View {
    let root: URL
    let title: String
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            DirectoryView(url: root, title: title, root: root)
                .navigationDestination(for: FileItem.self) { item in
                    if item.isDirectory {
                        DirectoryView(url: item.url, title: item.name, root: root)
                    } else {
                        FileDetailView(url: item.url, root: root)
                    }
                }
                .toolbar {
                    ToolbarItem(placement: .topBarLeading) { Button("Done") { dismiss() } }
                }
        }
    }
}

struct DirectoryView: View {
    let title: String
    let root: URL
    @State private var model: DirectoryModel
    @State private var renaming: FileItem?
    @State private var newName = ""
    @State private var deleting: FileItem?
    @State private var creatingFolder = false
    @State private var creatingFile = false
    @State private var importing = false
    @State private var filter = ""

    init(url: URL, title: String, root: URL) {
        self.title = title
        self.root = root
        _model = State(initialValue: DirectoryModel(url: url))
    }

    private var shown: [FileItem] {
        filter.isEmpty ? model.items : model.items.filter { $0.name.localizedCaseInsensitiveContains(filter) }
    }

    var body: some View {
        List {
            if model.url == root {
                Section {
                    Label("Saves and settings are usually in Documents or Library/Preferences. Close the game before editing its files.",
                          systemImage: "info.circle")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            if shown.isEmpty {
                Text(filter.isEmpty ? "Empty folder" : "No matches").foregroundStyle(.secondary)
            }
            ForEach(shown) { item in
                NavigationLink(value: item) { row(item) }
                    .contextMenu { menu(item) }
                    .swipeActions(edge: .trailing) {
                        Button("Delete", systemImage: "trash", role: .destructive) { deleting = item }
                        Button("Rename", systemImage: "pencil") { newName = item.name; renaming = item }.tint(.orange)
                    }
            }
        }
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $filter, placement: .navigationBarDrawer(displayMode: .automatic), prompt: "Filter")
        .refreshable { model.reload() }
        .onAppear { model.reload() }
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu("Add", systemImage: "plus") {
                    Button("Import Files…", systemImage: "square.and.arrow.down") { importing = true }
                    Button("New Folder", systemImage: "folder.badge.plus") { newName = "New Folder"; creatingFolder = true }
                    Button("New Text File", systemImage: "doc.badge.plus") { newName = "notes.txt"; creatingFile = true }
                }
            }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { model.importFiles(urls) }
        }
        .alert("Rename", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $newName).textInputAutocapitalization(.never).autocorrectionDisabled()
            Button("Rename") { if let r = renaming { model.rename(r, to: newName) }; renaming = nil }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
        .alert("New Folder", isPresented: $creatingFolder) {
            TextField("Name", text: $newName)
            Button("Create") { model.newFolder(named: newName) }
            Button("Cancel", role: .cancel) {}
        }
        .alert("New Text File", isPresented: $creatingFile) {
            TextField("Name", text: $newName).textInputAutocapitalization(.never).autocorrectionDisabled()
            Button("Create") { model.newFile(named: newName) }
            Button("Cancel", role: .cancel) {}
        }
        .confirmationDialog("Delete \(deleting?.name ?? "")?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible) {
            Button("Delete", role: .destructive) { if let d = deleting { model.delete(d) }; deleting = nil }
        } message: {
            Text("This can't be undone.")
        }
        .alert("Couldn't do that", isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })) {
            Button("OK") { model.error = nil }
        } message: { Text(model.error ?? "") }
    }

    private func row(_ item: FileItem) -> some View {
        let kind = FileKind(url: item.url, isDirectory: item.isDirectory)
        return HStack(spacing: 12) {
            Image(systemName: kind.symbol)
                .foregroundStyle(item.isDirectory ? Color.accentColor : .secondary)
                .frame(width: 26)
            VStack(alignment: .leading, spacing: 2) {
                Text(item.name).lineLimit(1).truncationMode(.middle)
                Text(FileFormat.subtitle(item)).font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private func menu(_ item: FileItem) -> some View {
        if !item.isDirectory { ShareLink(item: item.url) { Label("Share / Export", systemImage: "square.and.arrow.up") } }
        Button("Rename", systemImage: "pencil") { newName = item.name; renaming = item }
        Button("Duplicate", systemImage: "plus.square.on.square") { model.duplicate(item) }
        Button("Copy Path", systemImage: "doc.on.clipboard") {
            UIPasteboard.general.string = String(item.url.path(percentEncoded: false).dropFirst(root.path(percentEncoded: false).count))
        }
        Button("Delete", systemImage: "trash", role: .destructive) { deleting = item }
    }
}

// MARK: - File detail

struct FileDetailView: View {
    let url: URL
    let root: URL
    @State private var mode: Mode

    enum Mode: String, CaseIterable, Identifiable {
        case image = "Image", plist = "Plist", text = "Text", hex = "Hex"
        var id: String { rawValue }
    }

    init(url: URL, root: URL) {
        self.url = url
        self.root = root
        switch FileKind(url: url, isDirectory: false) {
        case .image: _mode = State(initialValue: .image)
        case .plist: _mode = State(initialValue: .plist)
        case .text: _mode = State(initialValue: .text)
        default: _mode = State(initialValue: .hex)
        }
    }

    private var modes: [Mode] {
        switch FileKind(url: url, isDirectory: false) {
        case .image: [.image, .hex]
        case .plist: [.plist, .text, .hex]
        default: [.text, .hex]
        }
    }

    var body: some View {
        Group {
            switch mode {
            case .image: ImageFileView(url: url)
            case .plist: PlistFileView(url: url)
            case .text: TextFileView(url: url)
            case .hex: HexFileView(url: url)
            }
        }
        .navigationTitle(url.lastPathComponent)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .principal) {
                Picker("View as", selection: $mode) { ForEach(modes) { Text($0.rawValue).tag($0) } }
                    .pickerStyle(.segmented)
                    .frame(maxWidth: 260)
            }
            ToolbarItem(placement: .topBarTrailing) {
                ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }
            }
        }
    }
}

// MARK: Image

struct ImageFileView: View {
    let url: URL
    var body: some View {
        if let img = AppIcon.load(url.path(percentEncoded: false)) {
            ScrollView([.horizontal, .vertical]) {
                VStack(spacing: 10) {
                    Image(uiImage: img).resizable().interpolation(.none).scaledToFit()
                        .frame(maxWidth: max(img.size.width, 1) * 2)
                        .background(Image(systemName: "checkerboard.rectangle").resizable().opacity(0.05))
                    Text("\(Int(img.size.width)) × \(Int(img.size.height))").font(.caption).foregroundStyle(.secondary)
                }
                .padding()
            }
        } else {
            ContentUnavailableView("Can't show this image", systemImage: "photo.badge.exclamationmark",
                                   description: Text("It may be a compressed texture; try the Hex view."))
        }
    }
}

// MARK: Text

struct TextFileView: View {
    let url: URL
    @State private var text = ""
    @State private var original = ""
    @State private var encoding: String.Encoding = .utf8
    @State private var tooBig = false
    @State private var error: String?

    var body: some View {
        Group {
            if tooBig {
                ContentUnavailableView("Too large to edit as text", systemImage: "doc.text",
                                       description: Text("Files over 4 MB open in the Hex view."))
            } else {
                TextEditor(text: $text)
                    .font(.system(.footnote, design: .monospaced))
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .scrollContentBackground(.hidden)
                    .padding(.horizontal, 6)
            }
        }
        .toolbar {
            ToolbarItemGroup(placement: .bottomBar) {
                Button("Revert") { text = original }.disabled(text == original)
                Spacer()
                Text(text == original ? encodingName : "Edited").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Save") { save() }.bold().disabled(text == original)
            }
        }
        .onAppear(perform: load)
        .alert("Couldn't save", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK") { error = nil }
        } message: { Text(error ?? "") }
    }

    private var encodingName: String {
        switch encoding {
        case .utf8: "UTF-8"
        case .utf16: "UTF-16"
        default: "Latin-1"
        }
    }

    private func load() {
        guard let data = try? Data(contentsOf: url) else { return }
        if data.count > 4 << 20 { tooBig = true; return }
        if let s = String(data: data, encoding: .utf8) { text = s; encoding = .utf8 }
        else if data.starts(with: [0xFF, 0xFE]) || data.starts(with: [0xFE, 0xFF]), let s = String(data: data, encoding: .utf16) {
            text = s; encoding = .utf16
        } else { text = String(data: data, encoding: .isoLatin1) ?? ""; encoding = .isoLatin1 }
        original = text
    }

    private func save() {
        guard let data = text.data(using: encoding) else { error = "The text can't be written in \(encodingName)."; return }
        do { try data.write(to: url, options: .atomic); original = text }
        catch { self.error = error.localizedDescription }
    }
}

// MARK: Property lists (most saves and settings)

/// One entry of a property list, editable in place.
@MainActor
@Observable
final class PlistNode: Identifiable {
    enum Value {
        case dict([PlistNode]), array([PlistNode])
        case string(String), integer(Int), real(Double), bool(Bool), date(Date), data(Data)
    }
    let id = UUID()
    var key: String
    var value: Value

    init(key: String, value: Value) { self.key = key; self.value = value }

    convenience init(key: String, any: Any) {
        switch any {
        case let d as [String: Any]:
            self.init(key: key, value: .dict(d.keys.sorted { $0.localizedStandardCompare($1) == .orderedAscending }
                .map { PlistNode(key: $0, any: d[$0]!) }))
        case let a as [Any]:
            self.init(key: key, value: .array(a.enumerated().map { PlistNode(key: "\($0.offset)", any: $0.element) }))
        case let s as String: self.init(key: key, value: .string(s))
        case let n as NSNumber:
            if CFGetTypeID(n) == CFBooleanGetTypeID() { self.init(key: key, value: .bool(n.boolValue)) }
            else if CFNumberIsFloatType(n) { self.init(key: key, value: .real(n.doubleValue)) }
            else { self.init(key: key, value: .integer(n.intValue)) }
        case let d as Date: self.init(key: key, value: .date(d))
        case let d as Data: self.init(key: key, value: .data(d))
        default: self.init(key: key, value: .string(String(describing: any)))
        }
    }

    var any: Any {
        switch value {
        case .dict(let c): Dictionary(c.map { ($0.key, $0.any) }, uniquingKeysWith: { a, _ in a })
        case .array(let c): c.map(\.any)
        case .string(let s): s
        case .integer(let i): i
        case .real(let d): d
        case .bool(let b): b
        case .date(let d): d
        case .data(let d): d
        }
    }

    var typeName: String {
        switch value {
        case .dict(let c): "Dictionary (\(c.count))"
        case .array(let c): "Array (\(c.count))"
        case .string: "String"
        case .integer: "Number"
        case .real: "Decimal"
        case .bool: "Boolean"
        case .date: "Date"
        case .data(let d): "Data (\(d.count) bytes)"
        }
    }

    var isContainer: Bool {
        switch value { case .dict, .array: true; default: false }
    }
}

@MainActor
@Observable
final class PlistDocument {
    let url: URL
    var root: PlistNode?
    var format: PropertyListSerialization.PropertyListFormat = .binary
    var dirty = false
    var error: String?

    init(url: URL) { self.url = url }

    func load() {
        do {
            let data = try Data(contentsOf: url)
            var fmt = PropertyListSerialization.PropertyListFormat.binary
            let obj = try PropertyListSerialization.propertyList(from: data, options: [], format: &fmt)
            format = fmt
            root = PlistNode(key: url.lastPathComponent, any: obj)
            dirty = false
        } catch { self.error = "Not a readable property list: \(error.localizedDescription)" }
    }

    func save() {
        guard let root else { return }
        do {
            let out: PropertyListSerialization.PropertyListFormat = format == .xml ? .xml : .binary
            let data = try PropertyListSerialization.data(fromPropertyList: root.any, format: out, options: 0)
            try data.write(to: url, options: .atomic)
            dirty = false
        } catch { self.error = error.localizedDescription }
    }
}

struct PlistFileView: View {
    let url: URL
    @State private var doc: PlistDocument

    init(url: URL) {
        self.url = url
        _doc = State(initialValue: PlistDocument(url: url))
    }

    var body: some View {
        Group {
            if let root = doc.root {
                PlistContainerView(node: root, doc: doc)
            } else {
                ContentUnavailableView("Loading…", systemImage: "list.bullet.rectangle")
            }
        }
        .toolbar {
            ToolbarItemGroup(placement: .bottomBar) {
                Button("Revert") { doc.load() }.disabled(!doc.dirty)
                Spacer()
                Text(doc.dirty ? "Edited" : (doc.format == .xml ? "XML plist" : "Binary plist")).font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Save") { doc.save() }.bold().disabled(!doc.dirty)
            }
        }
        .onAppear { if doc.root == nil { doc.load() } }
        .alert("Property list", isPresented: Binding(get: { doc.error != nil }, set: { if !$0 { doc.error = nil } })) {
            Button("OK") { doc.error = nil }
        } message: { Text(doc.error ?? "") }
    }
}

/// The entries of a dictionary or array.
struct PlistContainerView: View {
    @Bindable var node: PlistNode
    let doc: PlistDocument
    @State private var adding = false
    @State private var newKey = ""
    @State private var newType = "String"

    private var children: [PlistNode] {
        switch node.value { case .dict(let c), .array(let c): c; default: [] }
    }
    private var isDict: Bool { if case .dict = node.value { true } else { false } }

    var body: some View {
        List {
            ForEach(children) { child in
                PlistRow(node: child, doc: doc, showKey: isDict)
            }
            .onDelete { idx in
                var c = children
                c.remove(atOffsets: idx)
                set(c)
            }
        }
        .navigationTitle(node.key)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button("Add", systemImage: "plus") { newKey = isDict ? "newKey" : ""; adding = true }
            }
        }
        .alert(isDict ? "Add Entry" : "Add Item", isPresented: $adding) {
            if isDict { TextField("Key", text: $newKey).textInputAutocapitalization(.never).autocorrectionDisabled() }
            ForEach(["String", "Number", "Boolean", "Dictionary", "Array"], id: \.self) { t in
                Button(t) { add(type: t) }
            }
            Button("Cancel", role: .cancel) {}
        }
    }

    private func set(_ c: [PlistNode]) {
        if isDict { node.value = .dict(c) } else {
            for (i, n) in c.enumerated() { n.key = "\(i)" }
            node.value = .array(c)
        }
        doc.dirty = true
    }

    private func add(type: String) {
        let value: PlistNode.Value = switch type {
        case "Number": .integer(0)
        case "Boolean": .bool(false)
        case "Dictionary": .dict([])
        case "Array": .array([])
        default: .string("")
        }
        var c = children
        let key = isDict ? (newKey.isEmpty ? "newKey" : newKey) : "\(c.count)"
        if isDict, c.contains(where: { $0.key == key }) { doc.error = "There's already a key named \(key)."; return }
        c.append(PlistNode(key: key, value: value))
        set(c)
    }
}

struct PlistRow: View {
    @Bindable var node: PlistNode
    let doc: PlistDocument
    let showKey: Bool

    var body: some View {
        if node.isContainer {
            NavigationLink {
                PlistContainerView(node: node, doc: doc)
            } label: {
                LabeledContent(showKey ? node.key : "Item \(node.key)", value: node.typeName)
            }
        } else {
            VStack(alignment: .leading, spacing: 4) {
                Text(showKey ? node.key : "Item \(node.key)").font(.caption).foregroundStyle(.secondary)
                editor
            }
        }
    }

    @ViewBuilder private var editor: some View {
        switch node.value {
        case .string(let s):
            TextField("", text: Binding(get: { s }, set: { node.value = .string($0); doc.dirty = true }), axis: .vertical)
                .textInputAutocapitalization(.never).autocorrectionDisabled()
        case .integer(let i):
            TextField("", value: Binding(get: { i }, set: { node.value = .integer($0); doc.dirty = true }), format: .number.grouping(.never))
                .keyboardType(.numbersAndPunctuation).font(.body.monospacedDigit())
        case .real(let d):
            TextField("", value: Binding(get: { d }, set: { node.value = .real($0); doc.dirty = true }), format: .number.grouping(.never))
                .keyboardType(.numbersAndPunctuation).font(.body.monospacedDigit())
        case .bool(let b):
            Toggle(b ? "YES" : "NO", isOn: Binding(get: { b }, set: { node.value = .bool($0); doc.dirty = true }))
        case .date(let d):
            DatePicker("", selection: Binding(get: { d }, set: { node.value = .date($0); doc.dirty = true })).labelsHidden()
        case .data(let d):
            Text(d.prefix(48).map { String(format: "%02x", $0) }.joined(separator: " ") + (d.count > 48 ? " …" : ""))
                .font(.caption.monospaced()).foregroundStyle(.secondary).lineLimit(3)
        case .dict, .array:
            EmptyView()
        }
    }
}

// MARK: Hex

struct HexFileView: View {
    let url: URL
    @State private var data = Data()
    @State private var total: Int64 = 0
    @State private var patching = false
    @State private var offsetText = ""
    @State private var bytesText = ""
    @State private var error: String?
    private static let limit = 1 << 20   // show the first 1 MB

    var body: some View {
        ScrollView([.vertical, .horizontal]) {
            LazyVStack(alignment: .leading, spacing: 0) {
                if total > Int64(data.count) {
                    Text("Showing the first \(ByteCountFormatter.string(fromByteCount: Int64(data.count), countStyle: .file)) of \(ByteCountFormatter.string(fromByteCount: total, countStyle: .file))")
                        .font(.caption).foregroundStyle(.secondary).padding(.bottom, 6)
                }
                ForEach(0..<((data.count + 15) / 16), id: \.self) { line in
                    Text(row(line)).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                }
            }
            .padding(8)
        }
        .toolbar {
            ToolbarItemGroup(placement: .bottomBar) {
                Text("\(total) bytes").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Patch Bytes", systemImage: "pencil") { offsetText = ""; bytesText = ""; patching = true }
            }
        }
        .onAppear(perform: load)
        .alert("Patch Bytes", isPresented: $patching) {
            TextField("Offset (hex, e.g. 1A0)", text: $offsetText).textInputAutocapitalization(.never).autocorrectionDisabled()
            TextField("New bytes (hex, e.g. FF 00 7F)", text: $bytesText).textInputAutocapitalization(.never).autocorrectionDisabled()
            Button("Write") { patch() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Overwrites bytes at the offset. The file keeps its size unless you write past the end.")
        }
        .alert("Couldn't patch", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK") { error = nil }
        } message: { Text(error ?? "") }
    }

    private func load() {
        total = Int64((try? FileManager.default.attributesOfItem(atPath: url.path(percentEncoded: false))[.size] as? NSNumber)?.int64Value ?? 0)
        guard let h = try? FileHandle(forReadingFrom: url) else { return }
        defer { try? h.close() }
        data = (try? h.read(upToCount: Self.limit)) ?? Data()
    }

    private func row(_ line: Int) -> String {
        let start = line * 16
        let end = min(start + 16, data.count)
        let bytes = data[data.startIndex + start ..< data.startIndex + end]
        var hex = bytes.map { String(format: "%02x", $0) }.joined(separator: " ")
        hex += String(repeating: " ", count: max(0, 47 - hex.count))
        let ascii = String(bytes.map { (0x20...0x7e).contains($0) ? Character(UnicodeScalar($0)) : "." })
        return String(format: "%08x  ", start) + hex + "  " + ascii
    }

    private func patch() {
        let off = UInt64(offsetText.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "0x", with: ""), radix: 16)
        let cleaned = bytesText.replacingOccurrences(of: " ", with: "").replacingOccurrences(of: "0x", with: "")
        guard let off, !cleaned.isEmpty, cleaned.count % 2 == 0 else { error = "Enter a hex offset and an even number of hex digits."; return }
        var bytes: [UInt8] = []
        var i = cleaned.startIndex
        while i < cleaned.endIndex {
            let j = cleaned.index(i, offsetBy: 2)
            guard let b = UInt8(cleaned[i..<j], radix: 16) else { error = "\(cleaned[i..<j]) isn't a hex byte."; return }
            bytes.append(b)
            i = j
        }
        do {
            let h = try FileHandle(forWritingTo: url)
            defer { try? h.close() }
            try h.seek(toOffset: off)
            try h.write(contentsOf: Data(bytes))
            load()
        } catch { self.error = error.localizedDescription }
    }
}
