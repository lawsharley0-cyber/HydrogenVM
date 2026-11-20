import SwiftUI

/// The library as an iPhone OS 6 Home screen: glossy icons in rows on the linen, a dot for
/// whether each game can run here, and a glass shelf of the games played last (the old dock).

/// The games played most recently, newest first, kept as bundle ids one per line in
/// `@AppStorage(RecentlyPlayed.key)` so the shelf redraws when a game starts.
enum RecentlyPlayed {
    static let key = "library.recentlyPlayed"
    static let limit = 8

    /// `raw` with `id` moved to the front (added if new), at most `limit` entries.
    static func noted(_ id: String, in raw: String) -> String {
        var ids = raw.split(separator: "\n").map(String.init).filter { $0 != id }
        ids.insert(id, at: 0)
        return ids.prefix(limit).joined(separator: "\n")
    }

    /// The installed games among the recent ids, newest first, at most `max`.
    static func apps(_ raw: String, from apps: [InstalledApp], max: Int = 4) -> [InstalledApp] {
        let ids = raw.split(separator: "\n").map(String.init)
        let found = ids.compactMap { id in apps.first { $0.info.bundleIdentifier == id } }
        return Array(found.prefix(max))
    }
}

/// One Home-screen icon: the game's icon, a status dot on its corner and its name under it.
struct LibraryTile: View {
    let app: InstalledApp
    var iconSize: CGFloat = 60
    var showsStatus = true

    var body: some View {
        let status = AppRow.status(app.info)
        VStack(spacing: 6) {
            GlossIcon(path: app.info.iconPath, title: app.title, size: iconSize)
                .overlay(alignment: .bottomTrailing) {
                    if showsStatus {
                        Circle()
                            .fill(status.color)
                            .frame(width: 12, height: 12)
                            .overlay(Circle().strokeBorder(.white, lineWidth: 2))
                            .shadow(color: .black.opacity(0.3), radius: 0.5, x: 0, y: 0.5)
                            .offset(x: 3, y: 3)
                    }
                }
            Text(app.title)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(Gloss.ink)
                .shadow(color: Gloss.emboss, radius: 0, x: 0, y: 1)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: iconSize + 20)
        }
        .frame(minWidth: 76, minHeight: iconSize + 26)
        .contentShape(.rect)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(app.title), \(status.text)")
        .accessibilityAddTraits(.isButton)
    }
}

/// What the corner dots mean, under the grid.
struct LibraryLegend: View {
    var body: some View {
        HStack(spacing: 16) {
            entry("Ready to play", Gloss.ok)
            entry("Can't run here", Gloss.warn)
        }
        .frame(maxWidth: .infinity)
    }

    private func entry(_ text: String, _ color: Color) -> some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 9, height: 9)
                .overlay(Circle().strokeBorder(.white, lineWidth: 1.5))
            Text(text).font(.system(size: 11, weight: .medium)).foregroundStyle(Gloss.sectionInk)
                .shadow(color: Gloss.emboss, radius: 0, x: 0, y: 1)
        }
    }
}

/// The glass shelf of recently played games along the bottom, like the old dock.
/// Tapping an icon plays that game.
struct RecentShelf: View {
    let apps: [InstalledApp]
    let play: (InstalledApp) -> Void
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let dark = scheme == .dark
        HStack(alignment: .bottom, spacing: 0) {
            ForEach(apps) { app in
                Button { play(app) } label: {
                    LibraryTile(app: app, iconSize: 54, showsStatus: false)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Play \(app.title)")
                .frame(maxWidth: .infinity)
            }
        }
        .padding(.horizontal, 6)
        .padding(.top, 10)
        .padding(.bottom, 8)
        .background(alignment: .bottom) {
            // The shelf: a frosted slab, lighter at the top edge, with a bright rim.
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(.ultraThinMaterial)
                .overlay(
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .fill(LinearGradient(colors: dark
                                             ? [.white.opacity(0.10), .white.opacity(0.03), .black.opacity(0.25)]
                                             : [.white.opacity(0.55), .white.opacity(0.18), Color(white: 0.62).opacity(0.35)],
                                             startPoint: .top, endPoint: .bottom))
                )
                .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(.white.opacity(dark ? 0.18 : 0.8), lineWidth: 1))
                .shadow(color: .black.opacity(dark ? 0.5 : 0.22), radius: 4, x: 0, y: 2)
                .frame(height: 62)
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Recently played")
    }
}

/// The iPhone OS 6 switch: a capsule that reads ON (blue) or OFF (grey), with a round knob.
struct GlossSwitchStyle: ToggleStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 12) {
            configuration.label
            Spacer(minLength: 8)
            GlossSwitch(isOn: configuration.isOn) { configuration.isOn.toggle() }
        }
        .contentShape(.rect)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isToggle)
        .accessibilityValue(configuration.isOn ? "On" : "Off")
        .accessibilityAction { configuration.isOn.toggle() }
    }
}

private struct GlossSwitch: View {
    let isOn: Bool
    let toggle: () -> Void
    @Environment(\.colorScheme) private var scheme
    @Environment(\.isEnabled) private var isEnabled

    private var track: LinearGradient {
        if isOn {
            return LinearGradient(colors: [Color(red: 0.122, green: 0.384, blue: 0.788), Color(red: 0.298, green: 0.545, blue: 0.918)],
                                  startPoint: .top, endPoint: .bottom)
        }
        return scheme == .dark
            ? LinearGradient(colors: [Color(white: 0.22), Color(white: 0.33)], startPoint: .top, endPoint: .bottom)
            : LinearGradient(colors: [Color(white: 0.89), Color(white: 0.98)], startPoint: .top, endPoint: .bottom)
    }

    var body: some View {
        Button {
            withAnimation(.easeOut(duration: 0.15)) { toggle() }
        } label: {
            ZStack(alignment: isOn ? .trailing : .leading) {
                Capsule()
                    .fill(track)
                    .overlay(Capsule().strokeBorder(.black.opacity(0.3), lineWidth: 1))
                    .overlay(alignment: isOn ? .leading : .trailing) {
                        Text(isOn ? "ON" : "OFF")
                            .font(.system(size: 14, weight: .heavy))
                            .foregroundStyle(isOn ? Color.white : (scheme == .dark ? Color(white: 0.75) : Color(white: 0.42)))
                            .shadow(color: isOn ? .black.opacity(0.3) : Gloss.emboss, radius: 0, x: 0, y: isOn ? -1 : 1)
                            .padding(.horizontal, isOn ? 12 : 9)
                    }
                Circle()
                    .fill(LinearGradient(colors: [.white, Color(white: 0.89)], startPoint: .top, endPoint: .bottom))
                    .overlay(Circle().strokeBorder(.black.opacity(0.28), lineWidth: 1))
                    .shadow(color: .black.opacity(0.3), radius: 1, x: 0, y: 1)
                    .frame(width: 29, height: 29)
            }
            .frame(width: 79, height: 29)
            .opacity(isEnabled ? 1 : 0.5)
        }
        .buttonStyle(.plain)
    }
}

/// A short status in a tinted capsule ("Ready", "Encrypted", ...).
struct GlossStatusPill: View {
    let text: String
    let color: Color
    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 7, height: 7)
            Text(text).font(.system(size: 11, weight: .bold)).foregroundStyle(color)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .background(color.opacity(0.12), in: .capsule)
        .overlay(Capsule().strokeBorder(color.opacity(0.4), lineWidth: 1))
    }
}
