import SwiftUI
import UIKit

/// The "Gloss" look: iPhone OS 6, toned down. Glossy blue-grey bars, a quiet linen
/// ground, white grouped cards and one glossy action color. The gloss stays on bars and
/// buttons; body text is plain system type so long lists stay easy to read.
enum Gloss {
    static let barTop = UIColor(red: 0.561, green: 0.643, blue: 0.741, alpha: 1)      // #8fa4bd
    static let barMid = UIColor(red: 0.427, green: 0.518, blue: 0.635, alpha: 1)      // #6d84a2
    static let barLow = UIColor(red: 0.388, green: 0.482, blue: 0.604, alpha: 1)      // #637b9a
    static let barBottom = UIColor(red: 0.337, green: 0.435, blue: 0.561, alpha: 1)   // #566f8f
    static let barEdge = UIColor(red: 0.247, green: 0.333, blue: 0.455, alpha: 1)     // #3f5574

    static let linen = Color(red: 0.902, green: 0.910, blue: 0.925)                   // #e6e8ec
    static let card = Color.white
    static let cardEdge = Color(red: 0.722, green: 0.745, blue: 0.784)                // #b8bec8
    static let rowRule = Color(red: 0.882, green: 0.894, blue: 0.914)                 // #e1e4e9
    static let ink = Color(red: 0.169, green: 0.192, blue: 0.220)                     // #2b3138
    static let inkSoft = Color(red: 0.373, green: 0.420, blue: 0.478)                 // #5f6b7a
    static let sectionInk = Color(red: 0.298, green: 0.345, blue: 0.408)              // #4c5868
    static let link = Color(red: 0.184, green: 0.373, blue: 0.620)                    // #2f5f9e
    static let ok = Color(red: 0.184, green: 0.478, blue: 0.243)                      // #2f7a3e
    static let warn = Color(red: 0.604, green: 0.353, blue: 0.0)                      // #9a5a00
    static let bad = Color(red: 0.690, green: 0.200, blue: 0.170)

    /// Bars and the tab bar, through UIKit appearance so native navigation keeps working
    /// (back swipe, large-title collapse, accessibility sizes).
    @MainActor static func installAppearance() {
        let bar = UINavigationBarAppearance()
        bar.configureWithOpaqueBackground()
        bar.backgroundImage = verticalGradient([barTop, barMid, barLow, barBottom], stops: [0, 0.5, 0.5, 1])
        bar.shadowColor = barEdge
        let titleShadow = NSShadow()
        titleShadow.shadowColor = UIColor.black.withAlphaComponent(0.4)
        titleShadow.shadowOffset = CGSize(width: 0, height: -1)
        bar.titleTextAttributes = [.foregroundColor: UIColor.white, .shadow: titleShadow,
                                   .font: UIFont.systemFont(ofSize: 18, weight: .bold)]
        bar.largeTitleTextAttributes = [.foregroundColor: UIColor.white, .shadow: titleShadow]
        let item = UIBarButtonItemAppearance()
        item.normal.titleTextAttributes = [.foregroundColor: UIColor.white, .shadow: titleShadow]
        bar.buttonAppearance = item
        bar.backButtonAppearance = item
        UINavigationBar.appearance().standardAppearance = bar
        UINavigationBar.appearance().scrollEdgeAppearance = bar
        UINavigationBar.appearance().compactAppearance = bar
        UINavigationBar.appearance().tintColor = .white

        let tab = UITabBarAppearance()
        tab.configureWithOpaqueBackground()
        tab.backgroundImage = verticalGradient([UIColor(white: 0.235, alpha: 1), UIColor(white: 0.122, alpha: 1)], stops: [0, 1])
        tab.shadowColor = UIColor(white: 0.07, alpha: 1)
        let selected = UIColor(red: 0.612, green: 0.769, blue: 1.0, alpha: 1)              // #9cc4ff
        let normal = UIColor(white: 0.72, alpha: 1)
        for layout in [tab.stackedLayoutAppearance, tab.inlineLayoutAppearance, tab.compactInlineLayoutAppearance] {
            layout.selected.iconColor = selected
            layout.selected.titleTextAttributes = [.foregroundColor: UIColor.white]
            layout.normal.iconColor = normal
            layout.normal.titleTextAttributes = [.foregroundColor: normal]
        }
        UITabBar.appearance().standardAppearance = tab
        UITabBar.appearance().scrollEdgeAppearance = tab

        UISegmentedControl.appearance().selectedSegmentTintColor = barMid
        UISegmentedControl.appearance().setTitleTextAttributes([.foregroundColor: UIColor.white], for: .selected)
    }

    /// A 1-pt-wide gradient strip; UIKit stretches it across the bar.
    static func verticalGradient(_ colors: [UIColor], stops: [CGFloat]) -> UIImage {
        let size = CGSize(width: 1, height: 64)
        return UIGraphicsImageRenderer(size: size).image { ctx in
            let cg = colors.map(\.cgColor) as CFArray
            guard let g = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: cg, locations: stops) else { return }
            ctx.cgContext.drawLinearGradient(g, start: .zero, end: CGPoint(x: 0, y: size.height), options: [])
        }.resizableImage(withCapInsets: .zero, resizingMode: .stretch)
    }
}

/// The quiet linen ground: two faint hairline weaves over a light grey.
struct LinenBackground: View {
    var body: some View {
        Canvas { ctx, size in
            ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Gloss.linen))
            var y: CGFloat = 0
            while y < size.height {
                ctx.fill(Path(CGRect(x: 0, y: y, width: size.width, height: 1)), with: .color(.white.opacity(0.35)))
                y += 3
            }
            var x: CGFloat = 0
            while x < size.width {
                ctx.fill(Path(CGRect(x: x, y: 0, width: 1, height: size.height)), with: .color(.black.opacity(0.025)))
                x += 3
            }
        }
        .ignoresSafeArea()
        .accessibilityHidden(true)
    }
}

/// Glossy pill buttons: a light top half over a darker bottom half, an inner highlight and
/// an embossed label. `.green` is the one "go" action on a screen (Play).
struct GlossButtonStyle: ButtonStyle {
    enum Kind { case green, blue, plain }
    var kind: Kind = .blue
    var height: CGFloat = 50

    private var colors: (top: Color, mid1: Color, mid2: Color, bottom: Color, edge: Color, text: Color) {
        switch kind {
        case .green:
            return (Color(red: 0.471, green: 0.776, blue: 0.427), Color(red: 0.247, green: 0.604, blue: 0.227),
                    Color(red: 0.204, green: 0.545, blue: 0.188), Color(red: 0.184, green: 0.498, blue: 0.173),
                    Color(red: 0.173, green: 0.420, blue: 0.184), .white)
        case .blue:
            return (Color(red: 0.498, green: 0.588, blue: 0.702), Color(red: 0.420, green: 0.518, blue: 0.643),
                    Color(red: 0.373, green: 0.471, blue: 0.600), Color(red: 0.353, green: 0.451, blue: 0.576),
                    Color(red: 0.235, green: 0.318, blue: 0.439), .white)
        case .plain:
            return (Color(white: 0.99), Color(white: 0.95), Color(white: 0.91), Color(white: 0.87),
                    Gloss.cardEdge, Gloss.ink)
        }
    }

    func makeBody(configuration: Configuration) -> some View {
        let c = colors
        return configuration.label
            .font(.system(size: kind == .green ? 18 : 15, weight: .bold))
            .foregroundStyle(c.text)
            .shadow(color: kind == .plain ? .white : .black.opacity(0.35), radius: 0, x: 0, y: kind == .plain ? 1 : -1)
            .frame(maxWidth: .infinity, minHeight: height)
            .background {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(LinearGradient(stops: [.init(color: c.top, location: 0), .init(color: c.mid1, location: 0.5),
                                                 .init(color: c.mid2, location: 0.5), .init(color: c.bottom, location: 1)],
                                         startPoint: .top, endPoint: .bottom))
                    .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).strokeBorder(c.edge, lineWidth: 1))
                    .overlay(alignment: .top) {
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .strokeBorder(Color.white.opacity(0.45), lineWidth: 1)
                            .mask(LinearGradient(colors: [.white, .clear], startPoint: .top, endPoint: .center))
                    }
                    .shadow(color: .black.opacity(0.25), radius: 1, x: 0, y: 1)
            }
            .opacity(configuration.isPressed ? 0.82 : 1)
            .contentShape(.rect)
    }
}

/// A section label over a card, embossed against the linen.
struct GlossSectionLabel: View {
    let text: String
    init(_ text: String) { self.text = text }
    var body: some View {
        Text(text)
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(Gloss.sectionInk)
            .shadow(color: .white, radius: 0, x: 0, y: 1)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 20)
            .padding(.top, 14)
            .padding(.bottom, 6)
            .accessibilityAddTraits(.isHeader)
    }
}

/// A white grouped card: rounded, hairline border, rows separated by thin rules.
struct GlossCard<Content: View>: View {
    @ViewBuilder var content: Content
    var body: some View {
        VStack(spacing: 0) { content }
            .background(Gloss.card, in: .rect(cornerRadius: 10, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).strokeBorder(Gloss.cardEdge, lineWidth: 1))
            .padding(.horizontal, 12)
    }
}

/// A rule between card rows.
struct GlossRule: View {
    var body: some View { Rectangle().fill(Gloss.rowRule).frame(height: 1) }
}

/// An app icon with the rounded-tile frame; a glossy initials tile when the game has none.
struct GlossIcon: View {
    let path: String?
    let title: String
    var size: CGFloat = 48

    private var initials: String {
        let words = title.split(whereSeparator: { !$0.isLetter && !$0.isNumber })
        let letters = words.prefix(2).compactMap(\.first)
        return letters.isEmpty ? "?" : String(letters).uppercased()
    }

    /// A stable tint per title, from a short list of muted hues.
    private var tint: (Color, Color) {
        let hues: [(Color, Color)] = [
            (Color(red: 0.357, green: 0.561, blue: 0.839), Color(red: 0.173, green: 0.369, blue: 0.659)),
            (Color(red: 0.886, green: 0.404, blue: 0.357), Color(red: 0.659, green: 0.208, blue: 0.169)),
            (Color(red: 0.420, green: 0.659, blue: 0.396), Color(red: 0.227, green: 0.443, blue: 0.212)),
            (Color(red: 0.541, green: 0.416, blue: 0.353), Color(red: 0.302, green: 0.204, blue: 0.157)),
            (Color(red: 0.580, green: 0.478, blue: 0.780), Color(red: 0.365, green: 0.275, blue: 0.569)),
        ]
        let sum = title.unicodeScalars.reduce(0) { ($0 &* 31 &+ Int($1.value)) & 0x7fffffff }
        return hues[sum % hues.count]
    }

    var body: some View {
        let radius = size * 0.23
        Group {
            if let path, let image = AppIcon.load(path) {
                Image(uiImage: image).resizable().scaledToFill()
            } else {
                ZStack {
                    LinearGradient(colors: [tint.0, tint.1], startPoint: .top, endPoint: .bottom)
                    Text(initials)
                        .font(.system(size: size * 0.36, weight: .bold))
                        .foregroundStyle(.white)
                        .shadow(color: .black.opacity(0.3), radius: 0, x: 0, y: -1)
                }
            }
        }
        .frame(width: size, height: size)
        .clipShape(.rect(cornerRadius: radius, style: .continuous))
        .overlay(alignment: .top) {
            // The old icon gloss: a soft highlight over the top half.
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .fill(LinearGradient(colors: [.white.opacity(0.28), .white.opacity(0.04)], startPoint: .top, endPoint: .center))
                .frame(height: size / 2)
                .clipShape(.rect(topLeadingRadius: radius, topTrailingRadius: radius))
                .allowsHitTesting(false)
        }
        .shadow(color: .black.opacity(0.35), radius: 1.5, x: 0, y: 1)
        .accessibilityHidden(true)
    }
}
