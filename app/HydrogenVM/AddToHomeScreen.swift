import SwiftUI
import UIKit

/// "Add to Home Screen" for one game, through Shortcuts.
///
/// Every runnable game is a "Play Game" action (GameIntents.swift). The player makes a
/// shortcut from it in the Shortcuts app and adds that to the Home Screen; tapping the icon
/// opens HydrogenVM straight into the game. iOS lets no app place a Home Screen icon by
/// itself, so the Shortcuts steps are the player's part. The game's icon can be saved to
/// Photos first, to use as the shortcut's Home Screen picture.
struct AddToHomeScreenSheet: View {
    let app: InstalledApp
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    @State private var iconImage: UIImage?
    @State private var copiedName = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 0) {
                    VStack(spacing: 10) {
                        GlossIcon(path: app.info.iconPath, title: app.title, size: 84)
                        Text(app.title)
                            .font(.system(size: 19, weight: .bold))
                            .foregroundStyle(Gloss.ink)
                            .shadow(color: Gloss.emboss, radius: 0, x: 0, y: 1)
                            .multilineTextAlignment(.center)
                        Text("A Home Screen shortcut that opens straight into the game")
                            .font(.system(size: 13))
                            .foregroundStyle(Gloss.inkSoft)
                            .multilineTextAlignment(.center)
                    }
                    .padding(.top, 24)
                    .padding(.horizontal, 24)
                    .padding(.bottom, 16)

                    if !app.info.runnable {
                        GlossCard {
                            Label("This game can't run yet, so Shortcuts doesn't offer it.", systemImage: "exclamationmark.triangle")
                                .font(.system(size: 14))
                                .foregroundStyle(Gloss.warn)
                                .padding(14)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .padding(.bottom, 12)
                    }

                    Button {
                        if let url = URL(string: "shortcuts://create-shortcut") { openURL(url) }
                    } label: {
                        Label("Open Shortcuts", systemImage: "square.2.layers.3d.top.filled")
                    }
                    .buttonStyle(GlossButtonStyle(kind: .blue))
                    .padding(.horizontal, 16)
                    .accessibilityIdentifier("add-home-screen-open-shortcuts")

                    GlossSectionLabel("In Shortcuts")
                    GlossCard {
                        StepRow(number: 1, text: "Tap **Add Action**, search for **Play Game** and pick it under HydrogenVM.")
                        GlossRule()
                        StepRow(number: 2, text: "Tap **Game** in the action and choose **\(app.title)**.")
                        GlossRule()
                        StepRow(number: 3, text: "Tap the shortcut's name at the top, then **Add to Home Screen**.")
                        GlossRule()
                        StepRow(number: 4, text: "Tap the icon to use the game's picture (saved below), name it, and tap **Add**.")
                    }

                    GlossSectionLabel("The game's icon")
                    GlossCard {
                        if let iconImage {
                            let picture = Image(uiImage: iconImage)
                            ShareLink(item: picture, preview: SharePreview(app.title, image: picture)) {
                                actionRow("Save icon to Photos", systemImage: "photo.badge.arrow.down")
                            }
                            .accessibilityIdentifier("add-home-screen-save-icon")
                        } else {
                            Text("This game has no icon HydrogenVM can read; Shortcuts will use its own.")
                                .font(.system(size: 13)).foregroundStyle(Gloss.inkSoft)
                                .padding(14).frame(maxWidth: .infinity, alignment: .leading)
                        }
                        GlossRule()
                        Button {
                            UIPasteboard.general.string = app.title
                            copiedName = true
                        } label: {
                            actionRow(copiedName ? "Name copied" : "Copy the game's name",
                                      systemImage: copiedName ? "checkmark" : "doc.on.doc")
                        }
                        .buttonStyle(.plain)
                    }
                    Text("Share → Save Image puts the icon in Photos. In step 4, choose Photo and pick it.")
                        .font(.system(size: 12)).foregroundStyle(Gloss.inkSoft)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 24).padding(.top, 6)

                    GlossSectionLabel("Also")
                    GlossCard {
                        HStack(alignment: .top, spacing: 12) {
                            Image(systemName: "mic.fill").foregroundStyle(Gloss.link).frame(width: 24)
                            Text("Ask Siri: “Play \(app.title) in HydrogenVM”. The game also shows up in Spotlight search.")
                                .font(.system(size: 14)).foregroundStyle(Gloss.ink)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        .padding(14)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .padding(.bottom, 28)
                }
            }
            .background(LinenBackground())
            .navigationTitle("Add to Home Screen")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .task {
                if iconImage == nil, let path = app.info.iconPath { iconImage = AppIcon.load(path) }
                HydrogenShortcuts.updateAppShortcutParameters()
            }
        }
    }

    private func actionRow(_ title: String, systemImage: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: systemImage).font(.system(size: 16, weight: .semibold)).foregroundStyle(Gloss.link).frame(width: 24)
            Text(title).font(.system(size: 15, weight: .medium)).foregroundStyle(Gloss.ink)
            Spacer()
        }
        .padding(.horizontal, 14)
        .frame(minHeight: 48)
        .contentShape(.rect)
    }
}

/// One numbered step.
private struct StepRow: View {
    let number: Int
    let text: AttributedString

    /// `text` is Markdown (**bold** for the names to tap).
    init(number: Int, text: String) {
        self.number = number
        self.text = (try? AttributedString(markdown: text)) ?? AttributedString(text)
    }

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Text("\(number)")
                .font(.system(size: 13, weight: .bold))
                .foregroundStyle(.white)
                .frame(width: 24, height: 24)
                .background(Circle().fill(Color(uiColor: Gloss.barMid)))
            Text(text)
                .font(.system(size: 14))
                .foregroundStyle(Gloss.ink)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 11)
    }
}
