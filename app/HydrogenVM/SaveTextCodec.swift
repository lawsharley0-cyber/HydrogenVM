import Foundation

enum SaveTextCodec {
    struct Decoded {
        let text: String
        let encoding: String.Encoding
        let byteOrderMark: Data
    }

    static func decode(_ bytes: Data) throws -> Decoded {
        let formats: [(Data, String.Encoding)] = [
            (Data([0xff, 0xfe, 0, 0]), .utf32LittleEndian),
            (Data([0, 0, 0xfe, 0xff]), .utf32BigEndian),
            (Data([0xef, 0xbb, 0xbf]), .utf8),
            (Data([0xff, 0xfe]), .utf16LittleEndian),
            (Data([0xfe, 0xff]), .utf16BigEndian),
        ]
        for (bom, encoding) in formats where bytes.starts(with: bom) {
            guard let text = String(data: bytes.dropFirst(bom.count), encoding: encoding) else {
                throw CocoaError(.fileReadInapplicableStringEncoding)
            }
            return Decoded(text: text, encoding: encoding, byteOrderMark: bom)
        }
        for encoding: String.Encoding in [.utf8, .isoLatin1] {
            if let text = String(data: bytes, encoding: encoding) {
                return Decoded(text: text, encoding: encoding, byteOrderMark: Data())
            }
        }
        throw CocoaError(.fileReadInapplicableStringEncoding)
    }
}
