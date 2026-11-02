import Foundation

/// Recently opened save files for one guest app.
///
/// The save editor used to keep a single `saveEditor.recentFiles` list. Opening
/// Angry Birds then showed The Sandbox and Clash paths. Storage is now keyed by
/// bundle id, and any path outside that app's container is dropped — including
/// paths left in the old shared list.
struct RecentSaveFiles: Equatable, Sendable, Codable {
    static let legacyKey = "saveEditor.recentFiles"
    static let capacity = 10

    var paths: [String]

    init(paths: [String] = []) {
        self.paths = Self.limiting(Self.normalized(paths))
    }

    static func storageKey(bundleID: String) -> String {
        "saveEditor.recentFiles.\(bundleID)"
    }

    /// `file` is inside `container` only when it is a strict descendant.
    /// `/Apps/Birds` does not contain `/Apps/Birds2/...` or `/Apps/Birds/../Other/...`.
    static func contains(_ path: String, container root: String) -> Bool {
        guard root.count > 1, path.hasPrefix("/") else { return false }
        let prefix = root.hasSuffix("/") ? root : root + "/"
        return path.hasPrefix(prefix)
    }

    func visible(in container: URL) -> [URL] {
        let root = Self.canonicalPath(container)
        return paths.compactMap { path in
            guard Self.contains(path, container: root) else { return nil }
            return URL(fileURLWithPath: path)
        }
    }

    /// Records `file` for this app. A file outside `container` is ignored.
    mutating func record(_ file: URL, container: URL) {
        let path = Self.canonicalPath(file)
        guard Self.contains(path, container: Self.canonicalPath(container)) else { return }
        paths.removeAll { $0 == path }
        paths.insert(path, at: 0)
        if paths.count > Self.capacity {
            paths.removeLast(paths.count - Self.capacity)
        }
    }

    func save(bundleID: String, defaults: UserDefaults) {
        guard !bundleID.isEmpty, let data = try? JSONEncoder().encode(self) else { return }
        defaults.set(data, forKey: Self.storageKey(bundleID: bundleID))
    }

    /// Loads this app's list. A missing per-app list is seeded from the old
    /// global list, keeping only paths inside `container`. An existing per-app
    /// list is never widened by that global list.
    static func load(bundleID: String, container: URL, defaults: UserDefaults = .standard) -> RecentSaveFiles {
        let key = storageKey(bundleID: bundleID)
        if let data = defaults.data(forKey: key) {
            let decoded = (try? JSONDecoder().decode(RecentSaveFiles.self, from: data)) ?? RecentSaveFiles()
            return adopt(decoded.paths, bundleID: bundleID, container: container, defaults: defaults, replacingExisting: true)
        }
        guard !bundleID.isEmpty, let legacy = defaults.data(forKey: legacyKey) else {
            return RecentSaveFiles()
        }
        return adopt(paths(fromLegacyData: legacy), bundleID: bundleID, container: container, defaults: defaults, replacingExisting: false)
    }

    private static func adopt(_ raw: [String], bundleID: String, container: URL, defaults: UserDefaults, replacingExisting: Bool) -> RecentSaveFiles {
        let scoped = RecentSaveFiles(paths: RecentSaveFiles(paths: raw).visible(in: container).map { canonicalPath($0) })
        guard !bundleID.isEmpty else { return scoped }
        if replacingExisting {
            if RecentSaveFiles(paths: raw).paths != scoped.paths {
                scoped.save(bundleID: bundleID, defaults: defaults)
            }
        } else if !scoped.paths.isEmpty {
            scoped.save(bundleID: bundleID, defaults: defaults)
        }
        return scoped
    }

    static func paths(fromLegacyData data: Data) -> [String] {
        let decoder = JSONDecoder()
        if let urls = try? decoder.decode([URL].self, from: data) {
            return urls.map { canonicalPath($0) }
        }
        if let strings = try? decoder.decode([String].self, from: data) {
            return normalized(strings)
        }
        guard let rows = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [] }
        return rows.compactMap { row in
            guard let relative = row["relative"] as? String else { return nil }
            if let url = URL(string: relative), url.isFileURL || relative.hasPrefix("/") {
                return canonicalPath(url.isFileURL ? url : URL(fileURLWithPath: relative))
            }
            return canonicalPath(URL(fileURLWithPath: relative))
        }
    }

    static func canonicalPath(_ url: URL) -> String {
        var path = url.standardizedFileURL.path
        while path.count > 1 && path.hasSuffix("/") { path.removeLast() }
        return path
    }

    private static func normalized(_ paths: [String]) -> [String] {
        var seen = Set<String>()
        var out: [String] = []
        for raw in paths {
            guard raw.hasPrefix("/") else { continue }
            let path = canonicalPath(URL(fileURLWithPath: raw))
            guard path.count > 1, seen.insert(path).inserted else { continue }
            out.append(path)
        }
        return out
    }

    private static func limiting(_ paths: [String]) -> [String] {
        Array(paths.prefix(capacity))
    }
}
