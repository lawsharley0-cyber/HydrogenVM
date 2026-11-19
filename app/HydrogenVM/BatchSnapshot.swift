import Foundation

/// The file operations the last-full-run snapshot needs, so tests can make one fail.
protocol SnapshotFiles {
    func exists(_ url: URL) -> Bool
    func makeDirectory(_ url: URL) throws
    func write(_ data: Data, to url: URL) throws
    func copy(_ from: URL, to: URL) throws
    func move(_ from: URL, to: URL) throws
    func remove(_ url: URL)
}

struct DiskSnapshotFiles: SnapshotFiles {
    func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path(percentEncoded: false)) }
    func makeDirectory(_ url: URL) throws { try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true) }
    func write(_ data: Data, to url: URL) throws { try data.write(to: url) }
    func copy(_ from: URL, to: URL) throws { try FileManager.default.copyItem(at: from, to: to) }
    func move(_ from: URL, to: URL) throws { try FileManager.default.moveItem(at: from, to: to) }
    func remove(_ url: URL) { try? FileManager.default.removeItem(at: url) }
}

/// Keeps the last full Test All Games run as one folder, `<reports>/last_full_run/`, holding
/// report.json, report.md and thumbnails/ (copies of the pictures the report names; its
/// thumbnailPath entries are "thumbnails/<name>", so they resolve inside the folder). A retest
/// overwrites thumbnails/<game>_<mode>.png, so the pictures are copied, not referenced.
enum BatchSnapshot {
    enum Outcome: Equatable {
        /// latest_batch_report is not a full run with results; nothing to keep.
        case nothingToKeep
        /// last_full_run/ now holds the latest full run.
        case kept
        /// The copy failed; last_full_run/ (or, if a swap could not finish, last_full_run.old/,
        /// which the next call restores first) still holds the previous snapshot.
        case failed
    }

    static func folder(in reports: URL) -> URL { reports.appending(path: "last_full_run", directoryHint: .isDirectory) }

    /// Before a new run replaces latest_batch_report: stage the report, its markdown and every
    /// picture it names in last_full_run.staging/, then publish by renames only when all of it
    /// copied. A named picture that is missing fails the snapshot (a kept report never points at
    /// a picture it does not have). Partial and empty reports are never kept.
    static func keepLastFullRun(reports: URL, thumbnails: URL, files: SnapshotFiles = DiskSnapshotFiles()) -> Outcome {
        let final = folder(in: reports)
        let old = reports.appending(path: "last_full_run.old", directoryHint: .isDirectory)
        let staging = reports.appending(path: "last_full_run.staging", directoryHint: .isDirectory)
        // A swap that could not finish left the previous snapshot aside: put it back first.
        if !files.exists(final), files.exists(old) {
            do { try files.move(old, to: final) } catch { return .failed }
        }
        guard let data = try? Data(contentsOf: reports.appending(path: "latest_batch_report.json")),
              BatchSelection.isFullRunWithResults(data) else { return .nothingToKeep }
        files.remove(staging)
        do {
            let stagedThumbs = staging.appending(path: "thumbnails", directoryHint: .isDirectory)
            try files.makeDirectory(stagedThumbs)
            try files.write(data, to: staging.appending(path: "report.json"))
            let md = reports.appending(path: "latest_batch_report.md")
            if files.exists(md) { try files.copy(md, to: staging.appending(path: "report.md")) }
            for name in BatchSelection.thumbnailFiles(BatchSelection.thumbnailPaths(inReport: data)) {
                // copy throws for a missing source, which fails the snapshot.
                try files.copy(thumbnails.appending(path: name), to: stagedThumbs.appending(path: name))
            }
        } catch {
            files.remove(staging)
            return .failed
        }
        // Swap: the previous snapshot moves aside, the new one takes its name, the aside copy is
        // removed. If the new one cannot take the name, the previous one goes back; if even
        // that fails it stays in last_full_run.old/, which the next call restores.
        files.remove(old)
        let hadPrevious = files.exists(final)
        do {
            if hadPrevious { try files.move(final, to: old) }
        } catch {
            files.remove(staging)
            return .failed
        }
        do {
            try files.move(staging, to: final)
        } catch {
            if hadPrevious { try? files.move(old, to: final) }
            files.remove(staging)
            return .failed
        }
        files.remove(old)
        return .kept
    }
}
