import Foundation
import SQLite3

/// SQLite changes stay in a private database until the common Save action.
/// The backup API includes committed WAL pages; copying only the main file does not.
final class SQLiteEditStore {
    let url: URL
    private let directory: URL

    init(source: URL) throws {
        directory = FileManager.default.temporaryDirectory.appendingPathComponent("hvm-sqlite-\(UUID().uuidString)")
        url = directory.appendingPathComponent("working.sqlite")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        var sourceDB: OpaquePointer?
        var destinationDB: OpaquePointer?
        var success = false
        defer {
            if let sourceDB { sqlite3_close(sourceDB) }
            if let destinationDB { sqlite3_close(destinationDB) }
            if !success { try? FileManager.default.removeItem(at: directory) }
        }
        guard sqlite3_open_v2(source.path, &sourceDB, SQLITE_OPEN_READWRITE, nil) == SQLITE_OK,
              let sourceDB else { throw Self.failure(sourceDB, "Open database") }
        guard sqlite3_wal_checkpoint_v2(sourceDB, nil, SQLITE_CHECKPOINT_TRUNCATE, nil, nil) == SQLITE_OK else {
            throw Self.failure(sourceDB, "Close the game before editing its database")
        }
        guard sqlite3_open(url.path, &destinationDB) == SQLITE_OK, let destinationDB else {
            throw Self.failure(destinationDB, "Create working database")
        }
        guard let backup = sqlite3_backup_init(destinationDB, "main", sourceDB, "main") else {
            throw Self.failure(destinationDB, "Copy database")
        }
        let result = sqlite3_backup_step(backup, -1)
        let finish = sqlite3_backup_finish(backup)
        guard result == SQLITE_DONE, finish == SQLITE_OK else {
            throw Self.failure(destinationDB, "Copy database")
        }
        guard sqlite3_exec(destinationDB, "PRAGMA journal_mode=DELETE", nil, nil, nil) == SQLITE_OK else {
            throw Self.failure(destinationDB, "Prepare working database")
        }
        success = true
    }

    deinit { try? FileManager.default.removeItem(at: directory) }

    static func failure(_ db: OpaquePointer?, _ operation: String) -> NSError {
        let detail = db.map { String(cString: sqlite3_errmsg($0)) } ?? "Unable to open database."
        return NSError(domain: "HydrogenVM.SQLiteEditor", code: 1,
                       userInfo: [NSLocalizedDescriptionKey: "\(operation): \(detail)"])
    }

    static func requireClosed(_ source: URL) throws {
        let fm = FileManager.default
        for suffix in ["-wal", "-shm", "-journal"] where fm.fileExists(atPath: source.path + suffix) {
            throw NSError(domain: "HydrogenVM.SQLiteEditor", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "The database \(source.lastPathComponent) has a journal file (\(suffix)). Close the game and reopen the editor before saving."])
        }
    }

    func bytes() throws -> Data {
        try Self.requireClosed(url)
        return try Data(contentsOf: url)
    }
}
