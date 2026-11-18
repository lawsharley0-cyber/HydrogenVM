import Foundation
import Security
import SwiftUI
import UIKit

/// One-tap problem reports: a GitHub issue in the project's repository with the whole report,
/// not an excerpt. The issue body holds the summary, device and build; the full report JSON and
/// the log follow as comments (GitHub caps one body at 65,536 characters). Reports that can't be
/// sent (no network, GitHub down) wait in a queue and go out on the next launch or send.
///
/// It needs a GitHub token the player enters once on the Diagnostics tab: a fine-grained token
/// for this repository only, with Issues: Read and write. It is kept in the Keychain on this
/// device and only ever sent to api.github.com.
struct ProblemReport: Codable, Sendable, Identifiable {
    var id = UUID()
    var title: String
    var summary: String
    /// Named attachments, in order: "report.json", "log.txt", ...
    var parts: [Part]
    var created = Date()

    struct Part: Codable, Sendable {
        var name: String
        var text: String
        var language: String = ""
    }
}

enum ProblemReporter {
    static let repository = "lawsharley0-cyber/HydrogenVM"
    private static let keychainService = "com.lfull.HydrogenVM.github-reports"
    private static let maxChunk = 60_000
    private static let maxComments = 15

    // MARK: Token (Keychain)

    static var token: String? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: keychainService,
                                kSecReturnData as String: true,
                                kSecMatchLimit as String: kSecMatchLimitOne]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
        let t = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        return t.isEmpty ? nil : t
    }

    static var hasToken: Bool { token != nil }

    static func setToken(_ value: String?) {
        let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                   kSecAttrService as String: keychainService]
        SecItemDelete(base as CFDictionary)
        guard let value, !value.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        var add = base
        add[kSecValueData as String] = Data(value.trimmingCharacters(in: .whitespacesAndNewlines).utf8)
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(add as CFDictionary, nil)
    }

    // MARK: Building reports

    /// The device and build lines every report starts with.
    @MainActor static var deviceLine: String {
        "**Device:** \(CrashReport.deviceModel), iOS \(UIDevice.current.systemVersion) · " +
        "**Emulator:** `\(HVMSession.emulatorCommit.prefix(12))`"
    }

    /// Text split into pieces GitHub accepts, each at most `maxChunk` characters, on line ends
    /// where possible.
    static func chunks(_ text: String, size: Int = maxChunk) -> [String] {
        var out: [String] = []
        var rest = Substring(text)
        while !rest.isEmpty {
            if rest.count <= size { out.append(String(rest)); break }
            let limit = rest.index(rest.startIndex, offsetBy: size)
            let cut = rest[..<limit].lastIndex(of: "\n").map { rest.index(after: $0) } ?? limit
            out.append(String(rest[..<cut]))
            rest = rest[cut...]
        }
        return out
    }

    /// The issue body and comment texts for a report, within GitHub's limits.
    static func render(_ report: ProblemReport) -> (body: String, comments: [String]) {
        var body = report.summary
        body += "\n\n_Sent from HydrogenVM's one-tap report. The full report follows in the comments._\n"
        var comments: [String] = []
        for part in report.parts where !part.text.isEmpty {
            let fence = part.text.contains("```") ? "~~~~" : "```"
            let pieces = chunks(part.text, size: maxChunk - 200)
            for (i, piece) in pieces.enumerated() {
                let label = pieces.count > 1 ? "\(part.name) (\(i + 1) of \(pieces.count))" : part.name
                comments.append("**\(label)**\n\n\(fence)\(part.language)\n\(piece)\n\(fence)")
            }
        }
        if comments.count > maxComments {
            let dropped = comments.count - maxComments
            comments = Array(comments.prefix(maxComments))
            comments.append("_\(dropped) more part(s) were left out to stay within GitHub's limits; the full report is saved on the device._")
        }
        return (body, comments)
    }

    // MARK: Sending

    enum SendError: LocalizedError {
        case noToken, http(Int, String), badResponse
        var errorDescription: String? {
            switch self {
            case .noToken: return "Add a GitHub token on the Diagnostics tab to send reports."
            case .http(let code, let text): return "GitHub answered \(code): \(text)"
            case .badResponse: return "GitHub sent an answer HydrogenVM couldn't read."
            }
        }
    }

    /// Sends a report and returns the new issue's web address. On a network failure the report
    /// is queued (and the error still thrown), so it goes out later.
    static func send(_ report: ProblemReport) async throws -> URL {
        guard let token else { throw SendError.noToken }
        do {
            let url = try await post(report, token: token)
            removeFromQueue(report.id)
            return url
        } catch let error as SendError {
            throw error
        } catch {
            enqueue(report)
            throw error
        }
    }

    private static func post(_ report: ProblemReport, token: String) async throws -> URL {
        let (body, comments) = render(report)
        let issue = try await request("POST", "repos/\(repository)/issues", token: token,
                                      json: ["title": String(report.title.prefix(250)), "body": body])
        guard let number = issue["number"] as? Int, let html = (issue["html_url"] as? String).flatMap(URL.init(string:)) else {
            throw SendError.badResponse
        }
        for comment in comments {
            _ = try await request("POST", "repos/\(repository)/issues/\(number)/comments", token: token, json: ["body": comment])
        }
        return html
    }

    private static func request(_ method: String, _ path: String, token: String, json: [String: Any]) async throws -> [String: Any] {
        var req = URLRequest(url: URL(string: "https://api.github.com/\(path)")!)
        req.httpMethod = method
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try JSONSerialization.data(withJSONObject: json)
        req.timeoutInterval = 30
        let (data, response) = try await URLSession.shared.data(for: req)
        guard let http = response as? HTTPURLResponse else { throw SendError.badResponse }
        let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
        guard (200..<300).contains(http.statusCode) else {
            throw SendError.http(http.statusCode, object["message"] as? String ?? HTTPURLResponse.localizedString(forStatusCode: http.statusCode))
        }
        return object
    }

    /// Checks the token: it can read this repository's issues.
    static func verifyToken(_ candidate: String) async -> String? {
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(repository)")!)
        req.setValue("Bearer \(candidate)", forHTTPHeaderField: "Authorization")
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.timeoutInterval = 20
        guard let (data, response) = try? await URLSession.shared.data(for: req), let http = response as? HTTPURLResponse else {
            return "Couldn't reach GitHub. Check the connection and try again."
        }
        if http.statusCode == 200 {
            let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
            let issues = object?["has_issues"] as? Bool ?? true
            return issues ? nil : "Issues are turned off for the repository."
        }
        return http.statusCode == 401 ? "GitHub doesn't accept this token." :
               "This token can't see \(repository) (GitHub answered \(http.statusCode))."
    }

    // MARK: Queue

    static var queueDirectory: URL {
        URL.documentsDirectory.appending(path: "HydrogenVM/report-queue", directoryHint: .isDirectory)
    }

    static func enqueue(_ report: ProblemReport) {
        try? FileManager.default.createDirectory(at: queueDirectory, withIntermediateDirectories: true)
        if let data = try? JSONEncoder().encode(report) {
            try? data.write(to: queueDirectory.appending(path: "\(report.id.uuidString).json"), options: .atomic)
        }
    }

    static func removeFromQueue(_ id: UUID) {
        try? FileManager.default.removeItem(at: queueDirectory.appending(path: "\(id.uuidString).json"))
    }

    static var queued: [ProblemReport] {
        let files = (try? FileManager.default.contentsOfDirectory(at: queueDirectory, includingPropertiesForKeys: nil)) ?? []
        return files.compactMap { try? JSONDecoder().decode(ProblemReport.self, from: Data(contentsOf: $0)) }
            .sorted { $0.created < $1.created }
    }

    /// Sends what is waiting; returns how many went out.
    @discardableResult
    static func flushQueue() async -> Int {
        guard let token else { return 0 }
        var sent = 0
        for report in queued {
            guard (try? await post(report, token: token)) != nil else { break }
            removeFromQueue(report.id)
            sent += 1
        }
        return sent
    }
}

/// A "Send report" button with its state: sending, sent (with the issue link), queued or failed.
/// Without a token it says where to add one and offers the old share route.
struct SendReportButton: View {
    let make: @MainActor () -> ProblemReport
    var fallback: URL? = nil
    @State private var state: Phase = .idle
    @Environment(\.openURL) private var openURL

    enum Phase: Equatable { case idle, sending, sent(URL), failed(String) }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            switch state {
            case .idle, .failed:
                Button {
                    send()
                } label: {
                    Label("Send report", systemImage: "paperplane.fill")
                }
                .buttonStyle(GlossButtonStyle(kind: .blue))
                .accessibilityIdentifier("send-problem-report")
            case .sending:
                HStack(spacing: 8) { ProgressView(); Text("Sending…") }
            case .sent(let url):
                Button { openURL(url) } label: { Label("Sent. Open the issue", systemImage: "checkmark.circle.fill") }
                    .buttonStyle(GlossButtonStyle(kind: .green))
            }
            if case .failed(let why) = state {
                Text(why).font(.footnote).foregroundStyle(Gloss.bad)
                if !ProblemReporter.hasToken, let fallback {
                    Link("Open GitHub instead (excerpt only)", destination: fallback).font(.footnote)
                }
            }
        }
    }

    private func send() {
        let report = make()
        guard ProblemReporter.hasToken else {
            state = .failed(ProblemReporter.SendError.noToken.localizedDescription)
            return
        }
        state = .sending
        Task {
            do {
                state = .sent(try await ProblemReporter.send(report))
            } catch {
                let queued = !(error is ProblemReporter.SendError)
                state = .failed(queued ? "Couldn't send now (\(error.localizedDescription)). It's saved and will go out automatically later."
                                       : error.localizedDescription)
            }
        }
    }
}

/// The Diagnostics tab's settings for reports: the token, a check, and the queue.
struct ProblemReportSettings: View {
    @State private var draft = ""
    @State private var hasToken = ProblemReporter.hasToken
    @State private var status: String?
    @State private var checking = false
    @State private var queuedCount = ProblemReporter.queued.count

    var body: some View {
        Section {
            if hasToken {
                LabeledContent("GitHub token", value: "Saved on this iPhone")
                Button("Remove token", role: .destructive) {
                    ProblemReporter.setToken(nil)
                    hasToken = false
                    status = nil
                }
            } else {
                SecureField("GitHub token", text: $draft)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button(checking ? "Checking…" : "Save token") { save() }
                    .disabled(checking || draft.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            if let status { Text(status).font(.footnote).foregroundStyle(Gloss.inkSoft) }
            if queuedCount > 0 {
                Button("Send \(queuedCount) waiting report\(queuedCount == 1 ? "" : "s")") {
                    Task {
                        let sent = await ProblemReporter.flushQueue()
                        queuedCount = ProblemReporter.queued.count
                        status = sent > 0 ? "Sent \(sent)." : "Still couldn't send. Try again when online."
                    }
                }
            }
        } header: {
            Text("Problem reports")
        } footer: {
            Text("With a token, \"Send report\" files a GitHub issue in \(ProblemReporter.repository) with the full report and log, in one tap. Use a fine-grained token for that repository only, with Issues: Read and write. It stays in this iPhone's Keychain.")
        }
    }

    private func save() {
        let candidate = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        checking = true
        Task {
            if let problem = await ProblemReporter.verifyToken(candidate) {
                status = problem
            } else {
                ProblemReporter.setToken(candidate)
                hasToken = true
                draft = ""
                status = "Saved. Reports now send in one tap."
                let sent = await ProblemReporter.flushQueue()
                if sent > 0 { status = "Saved, and sent \(sent) waiting report\(sent == 1 ? "" : "s")." }
                queuedCount = ProblemReporter.queued.count
            }
            checking = false
        }
    }
}
