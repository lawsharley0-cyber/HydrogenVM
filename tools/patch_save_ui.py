import re, sys
p = "app/HydrogenVM/FileBrowser.swift"
s = open(p, encoding="utf-8").read()

helper = '''// MARK: - Reliable Save Infrastructure (shared by every editor)

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
    @Environment(\\.scenePhase) private var scenePhase
    @State private var lastSaved: Date?

    func body(content: Content) -> some View {
        content
            .safeAreaInset(edge: .bottom, spacing: 0) { bar }
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
                    Text("Saved \\(lastSaved.formatted(date: .omitted, time: .standard))")
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

'''
marker = "// MARK: - Text Editor (with transparent Zlib/Gzip support)"
assert marker in s
s = s.replace(marker, helper + marker, 1)

# safe writes
s = s.replace("try out.write(to: url, options: .atomic)", "try SafeFileWriter.write(out, to: url)")
s = s.replace("try (outText.data(using: .utf8) ?? Data()).write(to: url, options: .atomic)",
              "try SafeFileWriter.write(outText.data(using: .utf8) ?? Data(), to: url)")

# remove toolbar Save buttons
n_before = s.count('Button("Save") { save() }.disabled(!dirty)')
s = re.sub(r'\n[ \t]*Button\("Save"\) \{ save\(\) \}\.disabled\(!dirty\)', '', s)

# attach save bar to each editor, right after its navigationTitle
def attach(s, struct_name):
    i = s.index("struct %s: View" % struct_name)
    j = s.index(".navigationTitle(url.lastPathComponent)", i)
    k = j + len(".navigationTitle(url.lastPathComponent)")
    return s[:k] + "\n        .saveBar(dirty: $dirty) { save() }" + s[k:]
for name in ["TextEditorView", "KeyValueEditorView", "JSONEditorView", "PlistEditorView", "BinaryEditorView"]:
    s = attach(s, name)

# text editor: don't flag load as an edit
s = s.replace('''    @State private var dirty = false
    @State private var error: String?

    var body: some View {
        VStack(spacing: 0) {
            if compression != .none {
                HStack {
                    Image(systemName: "archivebox.fill")
                    Text("Compressed (\\(compression.rawValue)) — will re-compress on save")''',
'''    @State private var dirty = false
    @State private var baseline = ""
    @State private var error: String?

    var body: some View {
        VStack(spacing: 0) {
            if compression != .none {
                HStack {
                    Image(systemName: "archivebox.fill")
                    Text("Compressed (\\(compression.rawValue)) — will re-compress on save")''', 1)
s = s.replace(".onChange(of: text) { _, _ in dirty = true }", ".onChange(of: text) { _, new in dirty = (new != baseline) }", 1)
s = s.replace('''                text = s
                encoding = enc
                dirty = false''', '''                text = s
                baseline = s
                encoding = enc
                dirty = false''', 1)
s = s.replace('''            KeychainSaveSync.syncFileToKeychainIfNeeded(fileURL: url, text: text)
            dirty = false''', '''            KeychainSaveSync.syncFileToKeychainIfNeeded(fileURL: url, text: text)
            baseline = text
            dirty = false''', 1)
open(p, "w", encoding="utf-8").write(s)
print("removed toolbar save buttons:", n_before)
