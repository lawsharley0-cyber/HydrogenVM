import SwiftUI

@MainActor
@Observable
final class FileEditorSession {
    let store: SaveFileStore
    var dirty = false
    var ready = false
    var editVersion = 0
    var resetID = UUID()
    var saved = false
    var saveError: String?
    @ObservationIgnored var saveAction: (() -> Bool)?

    init(url: URL) { store = SaveFileStore(url: url) }

    func markChanged() {
        dirty = true
        saved = false
        editVersion += 1
    }

    func commit(_ bytes: Data) throws {
        try store.save(bytes)
        dirty = false
        saved = true
    }

    func revert() {
        dirty = false
        ready = false
        saved = false
        resetID = UUID()
    }
}

/// Shared by every buffered editor, including when a child tree is visible.
struct FileEditorControls: ViewModifier {
    @Environment(FileEditorSession.self) private var document
    @Environment(\.dismiss) private var dismiss
    @AppStorage("fileEditor.autoSave") private var autoSave = false
    @State private var showingClose = false
    @State private var showingRevert = false
    @State private var showingBackups = false
    let save: () -> Bool

    private func commit() -> Bool {
        guard document.ready else { return false }
        let success = save()
        if success { UIImpactFeedbackGenerator(style: .medium).impactOccurred() }
        return success
    }

    func body(content: Content) -> some View {
        content
            .safeAreaInset(edge: .bottom, spacing: 0) {
                if document.ready || document.dirty {
                VStack(spacing: 8) {
                    HStack {
                        Label(document.dirty ? "Unsaved Changes" : (document.saved ? "Saved ✓" : "No Changes"),
                              systemImage: document.dirty ? "pencil.circle" : "checkmark.circle")
                            .font(.caption)
                        Spacer()
                        Button("Revert", role: .destructive) { showingRevert = true }
                            .disabled(!document.dirty)
                    }
                    Button { _ = commit() } label: {
                        Label("Save Changes", systemImage: "square.and.arrow.down.fill")
                            .frame(maxWidth: .infinity, minHeight: 34)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!document.dirty || !document.ready)
                }
                .padding(12)
                .background(.bar)
                }
            }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Close") {
                        if document.dirty { showingClose = true } else { dismiss() }
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Save") { _ = commit() }.bold().disabled(!document.dirty || !document.ready)
                }
                ToolbarItemGroup(placement: .keyboard) {
                    Button("Done") {
                        UIApplication.shared.sendAction(#selector(UIResponder.resignFirstResponder),
                                                        to: nil, from: nil, for: nil)
                    }
                    Spacer()
                    Button("Save") { _ = commit() }.bold().disabled(!document.dirty || !document.ready)
                }
            }
            .safeAreaInset(edge: .top, spacing: 0) {
                if document.ready {
                HStack {
                    Toggle("Auto-save edits", isOn: $autoSave)
                    Button("Backups") { showingBackups = true }
                        .disabled(document.dirty || !document.ready)
                }
                .font(.caption).padding(.horizontal).padding(.vertical, 4).background(.bar)
                }
            }
            .sheet(isPresented: $showingBackups) { FileBackupHistoryView(document: document) }
            .interactiveDismissDisabled(document.dirty)
            .confirmationDialog("Unsaved Changes", isPresented: $showingClose, titleVisibility: .visible) {
                Button("Save & Close") { if commit() { dismiss() } }
                Button("Discard Changes", role: .destructive) { document.revert(); dismiss() }
                Button("Cancel", role: .cancel) {}
            }
            .confirmationDialog("Discard edits and reload from disk?", isPresented: $showingRevert,
                                titleVisibility: .visible) {
                Button("Discard Changes", role: .destructive) { document.revert() }
                Button("Cancel", role: .cancel) {}
            }
            .alert("Save Failed", isPresented: Binding(get: { document.saveError != nil },
                set: { if !$0 { document.saveError = nil } })) {
                Button("OK", role: .cancel) { document.saveError = nil }
            } message: { Text(document.saveError ?? "") }
            .task(id: document.editVersion) {
                guard autoSave, document.dirty, document.ready else { return }
                do { try await Task.sleep(for: .milliseconds(700)) } catch { return }
                guard !Task.isCancelled else { return }
                _ = commit()
            }
    }
}

/// The sheet owns the controls so they remain visible throughout nested
/// navigation. Update the serializer whenever a user mutation changes state.
struct FileEditorRegistration: ViewModifier {
    @Environment(FileEditorSession.self) private var document
    let save: () -> Bool

    func body(content: Content) -> some View {
        content
            .onAppear { document.saveAction = save }
            .onChange(of: document.editVersion) { _, _ in document.saveAction = save }
    }
}

private struct FileBackupHistoryView: View {
    let document: FileEditorSession
    @Environment(\.dismiss) private var dismiss
    @State private var backups: [URL] = []
    @State private var selected: URL?
    @State private var error: String?

    var body: some View {
        NavigationStack {
            List {
                if backups.isEmpty { Text("No previous revisions yet.") }
                ForEach(backups, id: \.self) { backup in
                    HStack {
                        let date = (try? backup.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast
                        Text(date.formatted(date: .abbreviated, time: .standard)).font(.caption)
                        Spacer()
                        ShareLink(item: backup)
                        Button("Restore") { selected = backup }.buttonStyle(.bordered)
                    }
                }
            }
            .navigationTitle("Previous File Revisions")
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } } }
            .onAppear {
                do { backups = try document.store.backups() } catch { self.error = error.localizedDescription }
            }
            .confirmationDialog("Restore this file revision?", isPresented: Binding(
                get: { selected != nil }, set: { if !$0 { selected = nil } }), titleVisibility: .visible) {
                Button("Restore Revision") {
                    guard let selected else { return }
                    do {
                        try SQLiteEditStore.requireCheckpointed(document.store.url)
                        try document.store.save(Data(contentsOf: selected))
                        document.revert()
                        dismiss()
                    } catch { self.error = error.localizedDescription }
                }
                Button("Cancel", role: .cancel) { selected = nil }
            }
            .alert("Restore Failed", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
                Button("OK", role: .cancel) { error = nil }
            } message: { Text(error ?? "") }
        }
    }
}
