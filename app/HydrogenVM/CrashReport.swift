import CryptoKit
import SwiftUI
import UIKit
import Darwin

@MainActor
struct CrashReport: Identifiable {
    let id = UUID()
    let json: String
    let file: URL
    let summary: String
    let signature: String
    let app: String

    static var directory: URL {
        URL.documentsDirectory.appending(path: "HydrogenVM/crashes", directoryHint: .isDirectory)
    }
    static var pending: URL { directory.appending(path: "pending.json") }
    static var signal: URL { directory.appending(path: "pending.signal.json") }
    /// The emulator's host-fault diagnostics (log tail, guest threads), written from its signal handler.
    static var hostFault: URL { directory.appending(path: "pending.hostfault.txt") }
    private static var journalID: UUID?

    /// Called only after the previous engine has released its arena. Persist
    /// identity before opening the signal files, and preserve an older crash
    /// before any O_TRUNC open (including batch restarts).
    static func beginSession(bundleID: String, version: String, renderer: String, phase: String) throws -> UUID {
        guard HVMEngine.liveGuestArenaCount() == 0 else {
            throw NSError(domain: "HydrogenVM.CrashJournal", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "The previous guest arena is still active."])
        }
        HVMCrashJournal.closeSignalFile()
        let evidence = [pending, signal, hostFault].contains {
            ((try? Data(contentsOf: $0))?.count ?? 0) > 0
        }
        if evidence && recover() == nil {
            throw NSError(domain: "HydrogenVM.CrashJournal", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "Could not preserve the previous crash journal."])
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let id = UUID()
        let fields: [String: Any] = ["bundleId": bundleID, "appVersion": version,
                                    "emulatorCommit": HVMSession.emulatorCommit,
                                    "renderer": renderer, "phase": phase,
                                    "journalId": id.uuidString,
                                    "host": ["pageSize": getpagesize()]]
        try JSONSerialization.data(withJSONObject: fields).write(to: pending, options: .atomic)
        journalID = id
        HVMCrashJournal.openSignalFile(signal.path)
        return id
    }

    static func endSession(_ id: UUID) {
        guard journalID == id else { return }
        clearPending()
    }

    static func save(_ fields: [String: Any], app: String) throws -> CrashReport {
        var fields = fields
        let signature = fields["signature"] as? String ?? String(Insecure.SHA1.hash(data: Data((fields["exceptionKind"] as? String ?? "unknown").utf8)).map { String(format: "%02x", $0) }.joined().prefix(12))
        fields["signature"] = signature
        fields["deviceModel"] = deviceModel
        fields["iOSVersion"] = UIDevice.current.systemVersion
        let data = try JSONSerialization.data(withJSONObject: fields, options: [.prettyPrinted, .sortedKeys])
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let date = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let name = app.map { $0.isLetter || $0.isNumber || $0 == "." ? $0 : "_" }
        let file = directory.appending(path: "\(date)-\(String(name))-\(UUID().uuidString).json")
        try data.write(to: file, options: .atomic)
        return CrashReport(json: String(decoding: data, as: UTF8.self), file: file,
                           summary: fields["summary"] as? String ?? "The previous game session ended unexpectedly. Share this report to help diagnose it.", signature: signature, app: app)
    }

    static var deviceModel: String {
        var system = utsname()
        uname(&system)
        return withUnsafeBytes(of: &system.machine) { bytes in
            String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
        }
    }

    static func recover() -> CrashReport? {
        let data = try? Data(contentsOf: pending)
        var fields = data.flatMap { try? JSONSerialization.jsonObject(with: $0) } as? [String: Any] ?? [:]
        let signalData = try? Data(contentsOf: signal)
        let fault = signalData.flatMap { try? JSONSerialization.jsonObject(with: $0) } as? [String: Any]
        let pendingRaw = data.map { String(decoding: $0, as: UTF8.self) } ?? ""
        let signalRaw = signalData.map { String(decoding: $0, as: UTF8.self) } ?? ""
        let hostText = (try? Data(contentsOf: hostFault)).map { String(decoding: $0, as: UTF8.self) } ?? ""
        // Older batch builds did not create pending.json. Recover their signal
        // evidence too, rather than erasing it at the next launch.
        guard !pendingRaw.isEmpty || !signalRaw.isEmpty || !hostText.isEmpty else { return nil }
        if fields.isEmpty && !pendingRaw.isEmpty { fields["pendingRaw"] = String(pendingRaw.prefix(200_000)) }
        fields["exceptionKind"] = "interrupted-host-session"
        fields["summary"] = "The previous game session ended unexpectedly. iOS may have stopped HydrogenVM; the exact cause is unknown."
        if let fault {
            fields.merge(fault) { _, new in new }
            fields["summary"] = "HydrogenVM encountered a host memory fault. Share this report to help diagnose it."
        }
        if fault == nil && !signalRaw.isEmpty { fields["signalRaw"] = String(signalRaw.prefix(200_000)) }
        if !hostText.isEmpty {
            // The bounded native report is < 200 KB even at registry capacity.
            // Keep its header/address as well as the log tail.
            fields["hostFaultDiagnostics"] = String(hostText.prefix(200_000))
        }
        guard let report = try? save(fields, app: fields["bundleId"] as? String ?? "unknown") else { return nil }
        clearPending()
        return report
    }

    static func clearPending() {
        journalID = nil
        HVMCrashJournal.closeSignalFile()
        try? FileManager.default.removeItem(at: pending)
        try? FileManager.default.removeItem(at: signal)
        try? FileManager.default.removeItem(at: hostFault)
    }

    var issueURL: URL {
        var url = URLComponents(string: "https://github.com/lawsharley0-cyber/HydrogenVM/issues/new")!
        url.queryItems = [URLQueryItem(name: "title", value: "[\(signature)] \(app)"),
                          URLQueryItem(name: "body", value: "\(summary)\n\nAttach the full JSON using Share.\n\n```json\n\(json.prefix(1000))\n``` (excerpt)")]
        return url.url!
    }
}

struct CrashReportView: View {
    let report: CrashReport
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 20) {
                Text(report.summary)
                Text("Report \(report.signature)").font(.caption.monospaced())
                Button("Copy report", systemImage: "doc.on.doc") { UIPasteboard.general.string = report.json }
                ShareLink(item: report.file) { Label("Share", systemImage: "square.and.arrow.up") }
                Link(destination: report.issueURL) { Label("Report on GitHub", systemImage: "link") }
                Spacer()
            }
            .padding()
            .navigationTitle("Game stopped")
            .toolbar { Button("Close") { dismiss() } }
        }
    }
}
