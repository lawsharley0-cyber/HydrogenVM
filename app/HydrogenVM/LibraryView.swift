import SwiftUI
import UniformTypeIdentifiers

/// Imported .ipa files live in Documents/Library. Launching them needs the
/// Mach-O loader and HLE frameworks (roadmap steps 3–5), so for now the
/// library only manages files.
@Observable
@MainActor
final class LibraryStore {
    private(set) var apps: [URL] = []
    var lastError: String?

    static var libraryDir: URL {
        URL.documentsDirectory.appending(path: "Library", directoryHint: .isDirectory)
    }

    init() { reload() }

    func reload() {
        let fm = FileManager.default
        try? fm.createDirectory(at: Self.libraryDir, withIntermediateDirectories: true)
        let items = (try? fm.contentsOfDirectory(at: Self.libraryDir, includingPropertiesForKeys: nil)) ?? []
        apps = items.filter { $0.pathExtension.lowercased() == "ipa" }.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    func importIPA(from url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let dest = Self.libraryDir.appending(path: url.lastPathComponent)
        do {
            if FileManager.default.fileExists(atPath: dest.path()) { try FileManager.default.removeItem(at: dest) }
            try FileManager.default.copyItem(at: url, to: dest)
            reload()
        } catch {
            lastError = error.localizedDescription
        }
    }

    func delete(_ url: URL) {
        try? FileManager.default.removeItem(at: url)
        reload()
    }
}

struct LibraryView: View {
    @State private var store = LibraryStore()
    @State private var importing = false

    var body: some View {
        NavigationStack {
            Group {
                if store.apps.isEmpty {
                    ContentUnavailableView("No apps yet",
                                           systemImage: "shippingbox",
                                           description: Text("Import a 32-bit .ipa (iOS 10 or earlier) to add it here."))
                } else {
                    List {
                        ForEach(store.apps, id: \.self) { url in
                            Label(url.deletingPathExtension().lastPathComponent, systemImage: "app.dashed")
                        }
                        .onDelete { idx in idx.map { store.apps[$0] }.forEach(store.delete) }
                    }
                }
            }
            .navigationTitle("HydrogenVM")
            .toolbar {
                Button("Import", systemImage: "plus") { importing = true }
            }
            .fileImporter(isPresented: $importing,
                          allowedContentTypes: [UTType(filenameExtension: "ipa") ?? .zip],
                          allowsMultipleSelection: true) { result in
                if case .success(let urls) = result { urls.forEach(store.importIPA) }
            }
            .alert("Import failed", isPresented: .constant(store.lastError != nil)) {
                Button("OK") { store.lastError = nil }
            } message: { Text(store.lastError ?? "") }
        }
    }
}
