import SwiftUI
import PhotosUI
import ImageIO
import UniformTypeIdentifiers

struct GamePhotoRequest: Identifiable { let id: UInt64 }

/// The system picker grants access only to the photo the player selects.
struct GamePhotoPicker: UIViewControllerRepresentable {
    let completion: (Data?) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(completion: completion) }
    func makeUIViewController(context: Context) -> PHPickerViewController {
        var config = PHPickerConfiguration()
        config.filter = .images
        config.selectionLimit = 1
        let picker = PHPickerViewController(configuration: config)
        picker.delegate = context.coordinator
        return picker
    }
    func updateUIViewController(_ controller: PHPickerViewController, context: Context) {}

    final class Coordinator: NSObject, PHPickerViewControllerDelegate {
        let completion: (Data?) -> Void
        private var finished = false
        private var loading = false
        init(completion: @escaping (Data?) -> Void) { self.completion = completion }

        func picker(_ picker: PHPickerViewController, didFinishPicking results: [PHPickerResult]) {
            guard !finished else { return }
            guard let result = results.first else { finished = true; completion(nil); return }
            guard !loading else { return }
            loading = true
            let provider = result.itemProvider
            let type = provider.registeredTypeIdentifiers.first { UTType($0)?.conforms(to: .image) == true } ?? UTType.image.identifier
            provider.loadFileRepresentation(forTypeIdentifier: type) { [weak self] url, _ in
                // Decode a bounded, orientation-correct thumbnail, not an entire
                // high-resolution camera image. No location metadata is copied.
                var png: Data?
                if let url, let source = CGImageSourceCreateWithURL(url as CFURL, nil),
                   let image = CGImageSourceCreateThumbnailAtIndex(source, 0, [
                       kCGImageSourceCreateThumbnailFromImageAlways: true,
                       kCGImageSourceCreateThumbnailWithTransform: true,
                       kCGImageSourceThumbnailMaxPixelSize: 1024
                   ] as CFDictionary) {
                    png = UIImage(cgImage: image).pngData()
                }
                let result = png
                DispatchQueue.main.async { [weak self] in
                    guard let self, !self.finished else { return }
                    self.finished = true
                    self.completion(result)
                }
            }
        }
    }
}
