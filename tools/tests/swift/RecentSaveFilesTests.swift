import Foundation

/// Recent files opened for app A must never appear in app B's save editor.
@main
struct RecentSaveFilesTests {
    static var checks = 0

    static func check(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        precondition(condition(), message)
    }

    static func names(_ files: RecentSaveFiles, in container: URL) -> [String] {
        files.visible(in: container).map(\.lastPathComponent)
    }

    static func main() throws {
        let birds = URL(fileURLWithPath: "/Games/AngryBirds", isDirectory: true)
        let sandbox = URL(fileURLWithPath: "/Games/TheSandbox", isDirectory: true)
        let clash = URL(fileURLWithPath: "/Games/Clash", isDirectory: true)
        let birdsScore = birds.appending(path: "Documents/highscore.plist")
        let birdsSettings = birds.appending(path: "Library/Preferences/settings.plist")
        let sandboxPlist = sandbox.appending(path: "Library/Preferences/com.pixowl.TheSandbox.plist")
        let clashGuest = clash.appending(path: "Documents/ClashGuestOffline.plist")
        let birdsBundle = "com.rovio.angrybirds"
        let sandboxBundle = "com.pixowl.TheSandbox"

        // The old editor stored every game in one list. Filtering that list for
        // Angry Birds must drop The Sandbox and Clash.
        let shared = RecentSaveFiles(paths: [birdsScore.path, sandboxPlist.path, clashGuest.path])
        check(names(shared, in: birds) == ["highscore.plist"], "app A hides other games in a shared list")
        check(names(shared, in: sandbox) == ["com.pixowl.TheSandbox.plist"], "app B hides other games in a shared list")
        check(!names(shared, in: birds).contains("ClashGuestOffline.plist"), "Clash file is not an Angry Birds recent")
        check(!names(shared, in: sandbox).contains("highscore.plist"), "Angry Birds file is not a Sandbox recent")

        // A sibling directory that shares a string prefix is a different app.
        let lookalike = URL(fileURLWithPath: "/Games/AngryBirds2/Documents/highscore.plist")
        let prefixed = RecentSaveFiles(paths: [birdsScore.path, lookalike.path])
        check(names(prefixed, in: birds) == ["highscore.plist"], "AngryBirds2 is not inside AngryBirds")

        var birdsOnly = RecentSaveFiles()
        birdsOnly.record(birdsScore, container: birds)
        birdsOnly.record(sandboxPlist, container: birds)
        birdsOnly.record(URL(fileURLWithPath: "/Games/AngryBirds/../Clash/Documents/ClashGuestOffline.plist"), container: birds)
        check(birdsOnly.paths == [RecentSaveFiles.canonicalPath(birdsScore)], "recording refuses another app and a .. escape")

        var sandboxOnly = RecentSaveFiles()
        sandboxOnly.record(sandboxPlist, container: sandbox)
        let union = RecentSaveFiles(paths: birdsOnly.paths + sandboxOnly.paths)
        check(names(union, in: sandbox) == ["com.pixowl.TheSandbox.plist"], "app B never lists app A's recorded files")
        check(names(union, in: birds) == ["highscore.plist"], "app A never lists app B's recorded files")

        var order = RecentSaveFiles()
        order.record(birdsScore, container: birds)
        order.record(birdsSettings, container: birds)
        order.record(birdsScore, container: birds)
        check(order.paths == [RecentSaveFiles.canonicalPath(birdsScore), RecentSaveFiles.canonicalPath(birdsSettings)], "newest open is first and duplicates collapse")
        for index in 0..<12 {
            order.record(birds.appending(path: "Documents/slot\(index).bin"), container: birds)
        }
        check(order.paths.count == RecentSaveFiles.capacity, "recent list keeps ten files")
        check(order.paths.first == RecentSaveFiles.canonicalPath(birds.appending(path: "Documents/slot11.bin")), "newest file stays first")

        let suite = "recent-save-files-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let legacy = try JSONEncoder().encode([birdsScore, sandboxPlist, clashGuest])
        defaults.set(legacy, forKey: RecentSaveFiles.legacyKey)

        let loadedBirds = RecentSaveFiles.load(bundleID: birdsBundle, container: birds, defaults: defaults)
        check(names(loadedBirds, in: birds) == ["highscore.plist"], "legacy list migrates only this app")
        check(defaults.data(forKey: RecentSaveFiles.storageKey(bundleID: sandboxBundle)) == nil,
              "migrating app A does not create app B's list")

        let loadedSandbox = RecentSaveFiles.load(bundleID: sandboxBundle, container: sandbox, defaults: defaults)
        check(names(loadedSandbox, in: sandbox) == ["com.pixowl.TheSandbox.plist"], "app B migrates only its own legacy paths")
        check(!names(loadedSandbox, in: sandbox).contains("highscore.plist"), "migrated app B has no app A file")

        var edited = loadedBirds
        edited.record(birdsSettings, container: birds)
        edited.save(bundleID: birdsBundle, defaults: defaults)
        defaults.set(try JSONEncoder().encode([birdsScore, sandboxPlist, clashGuest, birdsSettings]),
                     forKey: RecentSaveFiles.legacyKey)
        let birdsAgain = RecentSaveFiles.load(bundleID: birdsBundle, container: birds, defaults: defaults)
        check(names(birdsAgain, in: birds) == ["settings.plist", "highscore.plist"],
              "a per-app list ignores later writes to the old global list")
        let sandboxAgain = RecentSaveFiles.load(bundleID: sandboxBundle, container: sandbox, defaults: defaults)
        check(names(sandboxAgain, in: sandbox) == ["com.pixowl.TheSandbox.plist"],
              "app B still does not see files opened in app A")

        let poisoned = RecentSaveFiles(paths: [birdsScore.path, sandboxPlist.path, clashGuest.path])
        poisoned.save(bundleID: birdsBundle, defaults: defaults)
        let cleaned = RecentSaveFiles.load(bundleID: birdsBundle, container: birds, defaults: defaults)
        check(cleaned.paths == [RecentSaveFiles.canonicalPath(birdsScore)], "a poisoned per-app blob drops other games on load")

        let keyed = Data(#"[{"relative":"file:///Games/AngryBirds/Documents/highscore.plist"},{"relative":"file:///Games/TheSandbox/Library/Preferences/com.pixowl.TheSandbox.plist"}]"#.utf8)
        check(RecentSaveFiles(paths: RecentSaveFiles.paths(fromLegacyData: keyed)).visible(in: birds).map(\.lastPathComponent) == ["highscore.plist"],
              "historical keyed URL list is scoped")

        defaults.set(Data("not-json".utf8), forKey: RecentSaveFiles.legacyKey)
        let emptyBundle = "com.example.empty"
        let empty = RecentSaveFiles.load(bundleID: emptyBundle, container: birds, defaults: defaults)
        check(empty.paths.isEmpty, "corrupt legacy data does not invent files")

        print("PASS: recent files of app A never appear for app B (\(checks) checks)")
    }
}
