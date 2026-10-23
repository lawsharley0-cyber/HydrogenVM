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
    @ObservationIgnored var requestClose: (() -> Void)?

    func saveChanges() -> Bool {
        guard ready, let saveAction else { return false }
        let success = saveAction()
        if success {
            UIImpactFeedbackGenerator(style: .medium).impactOccurred()
            saved = true
        }
        return success
    }

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

/// Sticky bottom save bar modifier attached via `.safeAreaInset(edge: .bottom)`.
struct EditorSaveBottomBarModifier: ViewModifier {
    @Environment(FileEditorSession.self) private var document
    @State private var showingRevert = false
    @State private var showingBackups = false
    @State private var showSavedBanner = false

    func body(content: Content) -> some View {
        content
            .safeAreaInset(edge: .bottom) {
                if document.ready || document.dirty {
                    VStack(spacing: 6) {
                        if showSavedBanner {
                            HStack(spacing: 6) {
                                Image(systemName: "checkmark.circle.fill")
                                    .foregroundStyle(.green)
                                Text("Saved ✓")
                                    .font(.caption.weight(.bold))
                                    .foregroundStyle(.green)
                            }
                            .padding(.vertical, 4)
                            .padding(.horizontal, 12)
                            .background(Color.green.opacity(0.15), in: Capsule())
                            .transition(.move(edge: .bottom).combined(with: .opacity))
                        }

                        HStack(spacing: 12) {
                            VStack(alignment: .leading, spacing: 2) {
                                HStack(spacing: 4) {
                                    Image(systemName: document.dirty ? "pencil.circle.fill" : "checkmark.circle.fill")
                                        .foregroundStyle(document.dirty ? .orange : .green)
                                    Text(document.dirty ? "Unsaved Changes" : "All Changes Saved")
                                        .font(.caption.weight(.semibold))
                                        .foregroundStyle(document.dirty ? .orange : .secondary)
                                }
                                HStack(spacing: 10) {
                                    Button("Revert", role: .destructive) {
                                        showingRevert = true
                                    }
                                    .font(.caption2)
                                    .disabled(!document.dirty)

                                    Button {
                                        showingBackups = true
                                    } label: {
                                        Label("Revisions", systemImage: "clock.arrow.circlepath")
                                    }
                                    .font(.caption2)
                                    .disabled(document.dirty || !document.ready)
                                }
                            }

                            Spacer()

                            Button {
                                if document.saveChanges() {
                                    withAnimation(.spring(duration: 0.3)) {
                                        showSavedBanner = true
                                    }
                                }
                            } label: {
                                Label("Save Changes", systemImage: "square.and.arrow.down.fill")
                                    .fontWeight(.semibold)
                            }
                            .buttonStyle(.borderedProminent)
                            .accessibilityIdentifier("save-changes")
                            .disabled(!document.dirty || !document.ready)
                        }
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(.ultraThinMaterial)
                    .overlay(Divider(), alignment: .top)
                    .confirmationDialog("Discard edits and reload from disk?", isPresented: $showingRevert, titleVisibility: .visible) {
                        Button("Discard Changes", role: .destructive) { document.revert() }
                        Button("Cancel", role: .cancel) {}
                    }
                    .sheet(isPresented: $showingBackups) { FileBackupHistoryView(document: document) }
                    .onChange(of: document.saved) { _, isSaved in
                        if isSaved {
                            withAnimation(.spring(duration: 0.3)) {
                                showSavedBanner = true
                            }
                            Task {
                                try? await Task.sleep(for: .seconds(2))
                                withAnimation {
                                    showSavedBanner = false
                                }
                            }
                        }
                    }
                }
            }
    }
}

extension View {
    func editorSaveBottomBar() -> some View {
        modifier(EditorSaveBottomBarModifier())
    }
}

/// Shared by every buffered editor sheet.
struct FileEditorControls: ViewModifier {
    @Environment(FileEditorSession.self) private var document
    @Environment(\.dismiss) private var dismiss
    @AppStorage("fileEditor.autoSave") private var autoSave = false
    @State private var showingClose = false

    private func commit() -> Bool { document.saveChanges() }

    func body(content: Content) -> some View {
        content
            .onAppear {
                document.requestClose = {
                    if document.dirty { showingClose = true } else { dismiss() }
                }
            }
            .interactiveDismissDisabled(document.dirty)
            .confirmationDialog("Unsaved Changes", isPresented: $showingClose, titleVisibility: .visible) {
                Button("Save & Close") { if commit() { dismiss() } }
                Button("Discard Changes", role: .destructive) { document.revert(); dismiss() }
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
            .toolbar { FileEditorToolbar() }
            .onAppear { document.saveAction = save }
            .onChange(of: document.editVersion) { _, _ in document.saveAction = save }
    }
}

/// Navigation bars and keyboard accessories belong to the visible destination.
@MainActor
struct FileEditorToolbar: ToolbarContent {
    @Environment(FileEditorSession.self) private var document

    var body: some ToolbarContent {
        ToolbarItem(id: "file-editor-close", placement: .topBarLeading) {
            Button("Close") { document.requestClose?() }
                .accessibilityIdentifier("editor-close")
        }
        ToolbarItem(id: "file-editor-save", placement: .topBarTrailing) {
            Button("Save") { _ = document.saveChanges() }.bold()
                .disabled(!document.dirty || !document.ready)
                .accessibilityIdentifier("navigation-save")
        }
        ToolbarItemGroup(placement: .keyboard) {
            Button("Done") {
                UIApplication.shared.sendAction(#selector(UIResponder.resignFirstResponder), to: nil, from: nil, for: nil)
            }
            Spacer()
            Button("Save") { _ = document.saveChanges() }.bold()
                .disabled(!document.dirty || !document.ready)
                .accessibilityIdentifier("keyboard-save")
        }
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

/// Explicit focus also makes taps in the blank part of a text editor begin
/// editing, rather than depending on the text layout's current hit point.
struct FileEditorTextFocus: ViewModifier {
    @FocusState private var focused: Bool

    func body(content: Content) -> some View {
        content
            .focused($focused)
            .simultaneousGesture(TapGesture().onEnded { focused = true })
    }
}
