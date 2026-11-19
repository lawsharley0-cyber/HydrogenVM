import SwiftUI
import UniformTypeIdentifiers
import UIKit

struct GameDocumentPicker: UIViewControllerRepresentable {
    let types: [String]
    let completion: (String?) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(completion: completion) }
    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let allowed = types.compactMap { UTType($0) }
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: allowed.isEmpty ? [.item] : allowed, asCopy: true)
        picker.allowsMultipleSelection = false
        picker.delegate = context.coordinator
        return picker
    }
    func updateUIViewController(_ controller: UIDocumentPickerViewController, context: Context) {}

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        let completion: (String?) -> Void
        private var finished = false
        init(completion: @escaping (String?) -> Void) { self.completion = completion }
        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            finish(nil)
        }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            guard let url = urls.first else { finish(nil); return }
            let access = url.startAccessingSecurityScopedResource()
            defer { if access { url.stopAccessingSecurityScopedResource() } }
            do {
                let values = try url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
                guard values.isRegularFile == true, let count = values.fileSize, count > 0, count <= 16 * 1024 * 1024 else {
                    finish(nil)
                    return
                }
                // Keep the selected copy alive until the guest imports it on its run loop.
                let folder = FileManager.default.temporaryDirectory.appendingPathComponent("GuestDocuments", isDirectory: true)
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                let target = folder.appendingPathComponent(UUID().uuidString).appendingPathExtension(url.pathExtension)
                try FileManager.default.copyItem(at: url, to: target)
                finish(target.path)
            } catch {
                finish(nil)
            }
        }
        private func finish(_ path: String?) {
            guard !finished else { return }
            finished = true
            completion(path)
        }
    }
}
