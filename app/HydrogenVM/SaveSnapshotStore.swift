import Foundation

enum SaveSnapshotStore {
    private static let roots = ["Documents", "Library"]

    static func create(home: URL, destination: URL) throws {
        let fm = FileManager.default
        let parent = destination.deletingLastPathComponent()
        try fm.createDirectory(at: parent, withIntermediateDirectories: true)
        let stage = parent.appendingPathComponent(".snapshot-\(UUID().uuidString)")
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        for name in roots {
            let source = home.appendingPathComponent(name)
            let target = stage.appendingPathComponent(name)
            if fm.fileExists(atPath: source.path) { try fm.copyItem(at: source, to: target) }
            else { try fm.createDirectory(at: target, withIntermediateDirectories: false) }
        }
        try fm.moveItem(at: stage, to: destination)
    }

    /// Prepare every replacement first. Old folders remain available until all
    /// replacements succeed, and are moved back if installation fails.
    static func restore(snapshot: URL, home: URL) throws {
        let fm = FileManager.default
        let stage = home.appendingPathComponent(".restore-\(UUID().uuidString)")
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        var preserveRecovery = false
        defer { if !preserveRecovery { try? fm.removeItem(at: stage) } }
        for name in roots {
            let source = snapshot.appendingPathComponent(name)
            let values = try source.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard values.isDirectory == true, values.isSymbolicLink != true else { throw CocoaError(.fileReadCorruptFile) }
            try fm.copyItem(at: source, to: stage.appendingPathComponent("new-" + name))
        }
        var movedOld: [String] = []
        var installed: [String] = []
        do {
            for name in roots {
                let target = home.appendingPathComponent(name)
                if fm.fileExists(atPath: target.path) {
                    try fm.moveItem(at: target, to: stage.appendingPathComponent("old-" + name))
                    movedOld.append(name)
                }
                try fm.moveItem(at: stage.appendingPathComponent("new-" + name), to: target)
                installed.append(name)
            }
        } catch {
            let installError = error
            do {
                for name in installed.reversed() { try fm.removeItem(at: home.appendingPathComponent(name)) }
                for name in movedOld.reversed() {
                    try fm.moveItem(at: stage.appendingPathComponent("old-" + name), to: home.appendingPathComponent(name))
                }
            } catch {
                preserveRecovery = true
                throw NSError(domain: "HydrogenVM.Snapshot", code: 1,
                    userInfo: [NSLocalizedDescriptionKey: "Restore failed: \(installError.localizedDescription). Recovery files remain at \(stage.path): \(error.localizedDescription)"])
            }
            throw installError
        }
    }
    /// Export the whole snapshot folder as a zip file.
    static func exportZip(snapshot: URL, destination: URL) throws {
        let coordinator = NSFileCoordinator()
        var coordinateError: NSError?
        var zipError: Error?

        coordinator.coordinate(readingItemAt: snapshot, options: .forUploading, error: &coordinateError) { url in
            do {
                if FileManager.default.fileExists(atPath: destination.path) {
                    try FileManager.default.removeItem(at: destination)
                }
                try FileManager.default.copyItem(at: url, to: destination)
            } catch {
                zipError = error
            }
        }

        if let error = zipError ?? coordinateError {
            throw error
        }
    }
}
