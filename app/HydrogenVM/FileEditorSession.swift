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
    let save: () -> Bool

    private func commit() -> Bool {
        let success = save()
        if success { UIImpactFeedbackGenerator(style: .medium).impactOccurred() }
        return success
    }

    func body(content: Content) -> some View {
        content
            .safeAreaInset(edge: .bottom, spacing: 0) {
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
                Toggle("Auto-save edits", isOn: $autoSave)
                    .font(.caption).padding(.horizontal).padding(.vertical, 4)
                    .background(.bar)
            }
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
            .task(id: document.editVersion) {
                guard autoSave, document.dirty, document.ready else { return }
                do { try await Task.sleep(for: .milliseconds(700)) } catch { return }
                guard !Task.isCancelled else { return }
                _ = commit()
            }
    }
}
