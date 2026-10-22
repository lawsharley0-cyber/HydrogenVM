import Foundation
import CryptoKit

/// One loaded disk revision. All editor formats commit through this store.
final class SaveFileStore {
    enum Failure: LocalizedError {
        case notLoaded, changedOnDisk, verificationFailed

        var errorDescription: String? {
            switch self {
            case .notLoaded: "Open the file successfully before saving."
            case .changedOnDisk: "This file changed on disk. Your edits are still open. Copy any text you need, then close and reopen the file before trying again."
            case .verificationFailed: "The saved bytes could not be verified. Your edits are still open."
            }
        }
    }

    let url: URL
    private let backupRoot: URL
    private(set) var revision: Data?

    init(url: URL, backupRoot: URL? = nil) {
        self.url = url.resolvingSymlinksInPath().standardizedFileURL
        self.backupRoot = backupRoot ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("HydrogenVM/FileEditBackups", isDirectory: true)
    }

    func load() throws -> Data {
        let bytes = try Data(contentsOf: url)
        revision = bytes
        return bytes
    }

    var backupDirectory: URL {
        let key = SHA256.hash(data: Data(url.path.utf8)).map { String(format: "%02x", $0) }.joined()
        return backupRoot.appendingPathComponent(key, isDirectory: true)
    }

    func backups() throws -> [URL] {
        let fm = FileManager.default
        guard fm.fileExists(atPath: backupDirectory.path) else { return [] }
        return try fm.contentsOfDirectory(at: backupDirectory, includingPropertiesForKeys: [.creationDateKey])
            .filter { $0.pathExtension == "bak" }.sorted {
                ((try? $0.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast) >
                ((try? $1.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast)
            }
    }

    /// Refuse stale writes and preserve the exact original, including compression.
    /// Editors are only available while the guest is stopped; this check also
    /// catches external changes made while a sheet is open.
    func save(_ bytes: Data) throws {
        guard let revision else { throw Failure.notLoaded }
        guard try Data(contentsOf: url) == revision else { throw Failure.changedOnDisk }
        if bytes == revision { return }
        let fm = FileManager.default
        let directory = backupDirectory
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let backup = directory.appendingPathComponent("\(UUID().uuidString).bak")
        try revision.write(to: backup, options: .atomic)
        // Recheck after backup creation, before replacing the destination.
        guard try Data(contentsOf: url) == revision else { throw Failure.changedOnDisk }
        try bytes.write(to: url, options: .atomic)
        guard try Data(contentsOf: url) == bytes else { throw Failure.verificationFailed }
        self.revision = bytes
        // Backup retention is best effort only after a verified commit.
        let backups = (try? fm.contentsOfDirectory(at: directory,
            includingPropertiesForKeys: [.creationDateKey])) ?? []
        let ordered = backups.filter { $0.pathExtension == "bak" }.sorted {
            ((try? $0.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast) >
            ((try? $1.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast)
        }
        for old in ordered.dropFirst(10) { try? fm.removeItem(at: old) }
    }
}
