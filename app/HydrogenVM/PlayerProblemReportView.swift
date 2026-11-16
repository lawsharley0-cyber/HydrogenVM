import SwiftUI
import UIKit

struct PlayerProblemReportView: View {
    let report: PlayerProblemReport
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    @State private var copied = false

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 16) {
                Text("Review the report before sharing. Account details, paths and free-form game messages are omitted. Nothing is sent automatically.")
                    .font(.callout)
                ScrollView {
                    Text(report.text).font(.caption.monospaced()).textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                Button(copied ? "Report copied" : "Copy report", systemImage: "doc.on.doc") {
                    UIPasteboard.general.string = report.text
                    copied = true
                }.accessibilityIdentifier("player-report-copy")
                ShareLink(item: report.text) { Label("Share report", systemImage: "square.and.arrow.up") }
                    .accessibilityIdentifier("player-report-share")
                Button("Open GitHub draft", systemImage: "link") { openURL(report.issueURL) }
                    .accessibilityIdentifier("player-report-github")
                Text("GitHub opens an editable issue. Attach or paste the report there when you choose to submit it.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding()
            .navigationTitle("Report a problem")
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
    }
}
