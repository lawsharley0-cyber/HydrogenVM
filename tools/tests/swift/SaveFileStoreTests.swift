import Foundation

@main
struct SaveFileStoreTests {
    static func expect(_ value: @autoclosure () throws -> Bool, _ message: String) throws {
        guard try value() else { throw NSError(domain: message, code: 1) }
    }

    static func rejects(_ message: String, _ operation: () throws -> Void) throws {
        do { try operation() } catch { return }
        throw NSError(domain: "Expected failure: \(message)", code: 1)
    }

    static func main() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let file = root.appendingPathComponent("save.bin")
        let original = Data([0, 1, 0xff, 0x80])
        let updated = Data([4, 5, 6])
        try original.write(to: file)
        let store = SaveFileStore(url: file, backupRoot: root.appendingPathComponent("backups"))
        try rejects("save without load") { try store.save(updated) }
        try expect(store.load() == original, "load exact bytes")
        try store.save(updated)
        try expect(Data(contentsOf: file) == updated, "save exact bytes")
        let firstBackup = try fm.contentsOfDirectory(at: store.backupDirectory, includingPropertiesForKeys: nil)[0]
        try expect(Data(contentsOf: firstBackup) == original, "backup original bytes")
        let reopened = SaveFileStore(url: file, backupRoot: root.appendingPathComponent("backups"))
        try expect(reopened.load() == updated, "fresh editor sees saved bytes")
        try original.write(to: file)
        try rejects("external modification") { try store.save(Data([8])) }
        try expect(Data(contentsOf: file) == original, "conflict preserves external bytes")
        try expect(store.revision == updated, "failed save preserves loaded revision")
        _ = try store.load()
        let blockedRoot = root.appendingPathComponent("blocked")
        try Data([1]).write(to: blockedRoot)
        let blocked = SaveFileStore(url: file, backupRoot: blockedRoot)
        _ = try blocked.load()
        try rejects("backup failure") { try blocked.save(updated) }
        try expect(Data(contentsOf: file) == original, "backup failure leaves target intact")
        try store.save(Data())
        try expect(Data(contentsOf: file).isEmpty, "empty saves persist")
        for value in 0..<15 { try store.save(Data([UInt8(value)])) }
        try expect(fm.contentsOfDirectory(at: store.backupDirectory, includingPropertiesForKeys: nil).count == 10,
                   "retain ten originals")
        let otherFile = root.appendingPathComponent("other.bin")
        try Data([1]).write(to: otherFile)
        let other = SaveFileStore(url: otherFile, backupRoot: root.appendingPathComponent("backups"))
        try expect(other.backupDirectory != store.backupDirectory, "per-file backup isolation")
        let link = root.appendingPathComponent("link.bin")
        try fm.createSymbolicLink(at: link, withDestinationURL: file)
        let linked = SaveFileStore(url: link, backupRoot: root.appendingPathComponent("backups"))
        _ = try linked.load()
        try linked.save(updated)
        try expect(Data(contentsOf: file) == updated, "symlink saves target")
        try expect(fm.destinationOfSymbolicLink(atPath: link.path) == file.path, "symlink preserved")
        try fm.removeItem(at: file)
        try rejects("deleted target") { try linked.save(original) }
        try expect(!fm.fileExists(atPath: file.path), "deleted target is not recreated")

        for format: SaveCompressionFormat in [.none, .zlib, .gzip] {
            for payload in [Data(), original, Data(repeating: 0x61, count: 100_000)] {
                let encoded = try SaveCompressionFormat.encode(payload, format: format)
                let (decoded, detected) = try SaveCompressionFormat.decode(encoded)
                try expect(decoded == payload && detected == format, "compression round trip")
            }
        }
        try rejects("truncated gzip") { _ = try SaveCompressionFormat.decode(Data([0x1f, 0x8b, 0])) }
        try rejects("truncated zlib") { _ = try SaveCompressionFormat.decode(Data([0x78, 0x9c, 0])) }
        for encoding: String.Encoding in [.utf8, .utf16LittleEndian, .utf16BigEndian, .utf32LittleEndian, .utf32BigEndian] {
            let text = "score=42\r\nname=café\r\n"
            let bom: Data
            switch encoding {
            case .utf8: bom = Data([0xef, 0xbb, 0xbf])
            case .utf16LittleEndian: bom = Data([0xff, 0xfe])
            case .utf16BigEndian: bom = Data([0xfe, 0xff])
            case .utf32LittleEndian: bom = Data([0xff, 0xfe, 0, 0])
            default: bom = Data([0, 0, 0xfe, 0xff])
            }
            let source = bom + text.data(using: encoding)!
            let decoded = try SaveTextCodec.decode(source)
            try expect(decoded.text == text, "text decode")
            try expect(decoded.byteOrderMark + decoded.text.data(using: decoded.encoding)! == source, "encoding and BOM preserved")
        }
        let latin = Data([0x63, 0x61, 0x66, 0xe9])
        let decoded = try SaveTextCodec.decode(latin)
        try expect(decoded.text.data(using: decoded.encoding) == latin, "Latin-1 preserved")
        let json = try JSONSerialization.jsonObject(with: Data("42".utf8), options: [.fragmentsAllowed])
        try expect(JSONSerialization.data(withJSONObject: json, options: [.fragmentsAllowed]) == Data("42".utf8), "scalar JSON persists")
        print("PASS: save/reopen, backups/retention, conflicts, failed backup, empty files, symlinks, deleted targets, compression and encodings")
    }
}
