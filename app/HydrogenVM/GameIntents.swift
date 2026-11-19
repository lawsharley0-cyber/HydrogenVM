import AppIntents
import Foundation
import UIKit

/// Games as Shortcuts actions: "Play Game" with the game as its parameter, offered in the
/// Shortcuts app, Spotlight and Siri ("Play Dead Space in HydrogenVM"). A shortcut made from it
/// goes on the Home Screen like any other shortcut, and tapping it opens HydrogenVM straight
/// into the game, through the same route as a `hydrogenvm://run/<id>` link.

/// One installed, runnable game.
struct GameEntity: AppEntity {
    static var typeDisplayRepresentation: TypeDisplayRepresentation { TypeDisplayRepresentation(name: "Game") }
    static var defaultQuery: GameQuery { GameQuery() }

    let id: String
    let title: String
    let platform: String
    let iconPath: String?

    var displayRepresentation: DisplayRepresentation {
        let image: DisplayRepresentation.Image
        if let iconPath, let png = GameCatalog.iconPNG(iconPath) {
            image = DisplayRepresentation.Image(data: png)
        } else {
            image = DisplayRepresentation.Image(systemName: "gamecontroller.fill")
        }
        return DisplayRepresentation(title: "\(title)", subtitle: "\(platform)", image: image)
    }
}

/// The installed games, read from the library folder (the same place `LibraryStore` reads).
enum GameCatalog {
    static func all() -> [GameEntity] {
        let fm = FileManager.default
        let apps = URL.documentsDirectory.appending(path: "Apps", directoryHint: .isDirectory)
        var found: [GameEntity] = []
        for dir in (try? fm.contentsOfDirectory(at: apps, includingPropertiesForKeys: nil)) ?? [] {
            for item in (try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? []
            where item.pathExtension == "app" {
                guard let info = HVMEngine.appInfo(atPath: item.path(percentEncoded: false)), info.runnable,
                      GameLink.isGameID(info.bundleIdentifier) else { continue }
                let title = info.displayName.isEmpty ? info.name : info.displayName
                found.append(GameEntity(id: info.bundleIdentifier, title: title,
                                        platform: info.isAndroid ? "Android" : "iPhone", iconPath: info.iconPath))
            }
        }
        return found.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
    }

    /// The game's icon as PNG (old icons are often Xcode-crushed; the engine decodes those).
    static func iconPNG(_ path: String) -> Data? {
        if let image = UIImage(contentsOfFile: path) { return image.pngData() }
        if let cg = HVMEngine.decodeImage(atPath: path) { return UIImage(cgImage: cg).pngData() }
        return nil
    }
}

struct GameQuery: EntityStringQuery {
    func entities(for identifiers: [GameEntity.ID]) async throws -> [GameEntity] {
        GameCatalog.all().filter { identifiers.contains($0.id) }
    }

    func entities(matching string: String) async throws -> [GameEntity] {
        GameCatalog.all().filter { $0.title.localizedCaseInsensitiveContains(string) }
    }

    func suggestedEntities() async throws -> [GameEntity] { GameCatalog.all() }
}

/// "Play <game>": opens HydrogenVM and starts the game.
struct PlayGameIntent: AppIntent {
    static var title: LocalizedStringResource { "Play Game" }
    static var description: IntentDescription { IntentDescription("Opens a game in HydrogenVM.") }
    static var openAppWhenRun: Bool { true }

    @Parameter(title: "Game")
    var game: GameEntity

    init() {}
    init(game: GameEntity) { self.game = game }

    static var parameterSummary: some ParameterSummary { Summary("Play \(\.$game)") }

    @MainActor
    func perform() async throws -> some IntentResult {
        if let url = GameLink.url(forGameID: game.id) { LaunchRouter.shared.open(url) }
        return .result()
    }
}

/// The ready-made shortcuts: one "Play <game>" per installed game, under HydrogenVM in the
/// Shortcuts app and in Spotlight, and the Siri phrases.
struct HydrogenShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: PlayGameIntent(),
                    phrases: ["Play \(\.$game) in \(.applicationName)",
                              "Open \(\.$game) in \(.applicationName)",
                              "Start \(\.$game) in \(.applicationName)"],
                    shortTitle: "Play Game",
                    systemImageName: "gamecontroller.fill")
    }

    static var shortcutTileColor: ShortcutTileColor { .blue }
}
