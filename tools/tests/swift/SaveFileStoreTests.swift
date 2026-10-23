import Foundation
import SQLite3

@main
struct SaveFileStoreTests {
    static func expect(_ value: @autoclosure () throws -> Bool, _ message: String) throws {
        guard try value() else { throw NSError(domain: message, code: 1) }
    }

    static func rejects(_ message: String, _ operation: () throws -> Void) throws {
        do { try operation() } catch { return }
        throw NSError(domain: "Expected failure: \(message)", code: 1)
    }

    @MainActor static func main() throws {
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
        let config = SaveConfigLine(" coins :  12  ; earned coins\r")!
        try expect(config.replacingValue("99") == " coins :  99  ; earned coins\r", "config separators/comments/CRLF preserved")
        let quoted = SaveConfigLine("name=\"player\" # comment")!
        try expect(quoted.replacingValue("new player") == "name=\"new player\" # comment", "quoted config preserved")
        try rejects("quote injection") { _ = try quoted.replacingValue("bad\"value") }
        try rejects("newline in config value") { _ = try config.replacingValue("1\nother=2") }
        let json = try JSONSerialization.jsonObject(with: Data("42".utf8), options: [.fragmentsAllowed])
        try expect(JSONSerialization.data(withJSONObject: json, options: [.fragmentsAllowed]) == Data("42".utf8), "scalar JSON persists")
        let precise = try JSONSerialization.jsonObject(with: Data("{\"count\":9007199254740993,\"max\":18446744073709551615,\"name\":\"before\"}".utf8))
        let jsonTree = JSONNode.fromAny(precise)
        if case .object(let fields) = jsonTree.value {
            fields.first { $0.key == "name" }!.value = .string("after")
        }
        let jsonAfter = jsonTree.toAny() as! [String: Any]
        try expect((jsonAfter["count"] as! NSNumber).stringValue == "9007199254740993", "JSON integers above 2^53 stay exact")
        try expect((jsonAfter["max"] as! NSNumber).stringValue == "18446744073709551615", "JSON UInt64 stays exact")
        let archived = try NSKeyedArchiver.archivedData(withRootObject: ["score": 42], requiringSecureCoding: false)
        let archiveObject = try PropertyListSerialization.propertyList(from: archived, format: nil)
        let archiveTree = PlistNode.fromAny(archiveObject)
        let roundtrip = try PropertyListSerialization.data(fromPropertyList: archiveTree.toAny(), format: .binary, options: 0)
        let unarchiver = try NSKeyedUnarchiver(forReadingFrom: roundtrip)
        unarchiver.requiresSecureCoding = false
        let decodedArchive = unarchiver.decodeObject(forKey: NSKeyedArchiveRootObjectKey) as? [String: Int]
        unarchiver.finishDecoding()
        try expect(decodedArchive?["score"] == 42, "binary NSKeyedArchiver references survive the actual plist model")

        // A committed WAL row must survive the working-copy import, and changes
        // in that copy must not reach the source until the common Save step.
        let database = root.appendingPathComponent("save.sqlite")
        var db: OpaquePointer?
        try expect(sqlite3_open(database.path, &db) == SQLITE_OK, "create SQLite fixture")
        try expect(sqlite3_exec(db, "PRAGMA journal_mode=WAL; CREATE TABLE stats(coins INTEGER); INSERT INTO stats VALUES(10);", nil, nil, nil) == SQLITE_OK, "seed WAL")
        let workspace = try SQLiteEditStore(source: database)
        sqlite3_close(db)
        db = nil
        print("SQLite: source closed after WAL import")
        try SQLiteEditStore.requireCheckpointed(database)
        let databaseStore = SaveFileStore(url: database, backupRoot: root.appendingPathComponent("backups"))
        let beforeDatabase = try databaseStore.load()
        try expect(sqlite3_open(workspace.url.path, &db) == SQLITE_OK, "open working copy")
        var statement: OpaquePointer?
        try expect(sqlite3_prepare_v2(db, "SELECT coins FROM stats", -1, &statement, nil) == SQLITE_OK, "read copied WAL row")
        try expect(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 10, "copied value is ten")
        sqlite3_finalize(statement)
        try expect(sqlite3_exec(db, "UPDATE stats SET coins=99", nil, nil, nil) == SQLITE_OK, "edit working copy")
        sqlite3_close(db)
        db = nil
        print("SQLite: working copy closed before Save")
        try expect(Data(contentsOf: database) == beforeDatabase, "unsaved SQLite edits isolated")
        try databaseStore.save(workspace.bytes())
        try expect(sqlite3_open(database.path, &db) == SQLITE_OK, "reopen saved database")
        try expect(sqlite3_prepare_v2(db, "SELECT coins FROM stats", -1, &statement, nil) == SQLITE_OK, "read saved row")
        try expect(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 99, "saved SQLite value is ninety-nine")
        sqlite3_finalize(statement)
        sqlite3_close(db)
        db = nil
        let activeJournal = URL(fileURLWithPath: database.path + "-wal")
        try Data([1]).write(to: activeJournal)
        try rejects("active database journal") { try SQLiteEditStore.requireCheckpointed(database) }
        try rejects("hex mode cannot bypass WAL guard") { try databaseStore.save(beforeDatabase) }
        try Data().write(to: activeJournal)
        try SQLiteEditStore.requireCheckpointed(database)
        try fm.removeItem(at: activeJournal)

        // A missing snapshot root must fail during staging, before originals move.
        let home = root.appendingPathComponent("home")
        try fm.createDirectory(at: home.appendingPathComponent("Documents"), withIntermediateDirectories: true)
        try fm.createDirectory(at: home.appendingPathComponent("Library"), withIntermediateDirectories: true)
        let save = home.appendingPathComponent("Documents/progress.bin")
        try original.write(to: save)
        let snapshot = root.appendingPathComponent("snapshot")
        try SaveSnapshotStore.create(home: home, destination: snapshot)
        try updated.write(to: save)
        try SaveSnapshotStore.restore(snapshot: snapshot, home: home)
        try expect(Data(contentsOf: save) == original, "snapshot restores complete original")
        try fm.removeItem(at: snapshot.appendingPathComponent("Library"))
        try updated.write(to: save)
        try rejects("incomplete snapshot") { try SaveSnapshotStore.restore(snapshot: snapshot, home: home) }
        try expect(Data(contentsOf: save) == updated, "failed snapshot preserves live save")
        try expect(fm.contentsOfDirectory(atPath: home.path).sorted() == ["Documents", "Library"], "staging cleanup")
        try rejects("duplicate snapshot name") { try SaveSnapshotStore.create(home: home, destination: snapshot) }
        try expect(Data(contentsOf: snapshot.appendingPathComponent("Documents/progress.bin")) == original, "duplicate snapshot preserves existing backup")
        print("PASS: persistence, conflict/failure handling, backup retention, encodings/compression, SQLite WAL isolation/save/reopen, staged snapshot restore")
    }
}
