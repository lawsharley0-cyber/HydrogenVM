import Foundation
import XCTest
@testable import HydrogenVM

final class CrashReportTests: XCTestCase {
    @MainActor
    private func withFreshJournal(_ body: () throws -> Void) throws {
        let fm = FileManager.default
        let dir = CrashReport.directory
        let backup = dir.deletingLastPathComponent().appendingPathComponent(UUID().uuidString)
        let existed = fm.fileExists(atPath: dir.path)
        if existed { try fm.moveItem(at: dir, to: backup) }
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        defer {
            try? fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: dir.path)
            try? fm.removeItem(at: dir)
            if existed { try? fm.moveItem(at: backup, to: dir) }
        }
        try body()
    }

    @MainActor
    func testMalformedPendingAndSignalArePreserved() throws {
        try withFreshJournal {
            let pending = Data([0x7b, 0xff, 0x01])
            let signal = Data("{clipped".utf8)
            try pending.write(to: CrashReport.pending)
            try signal.write(to: CrashReport.signal)
            try Data("header\naddress=0x1234\nfooter".utf8).write(to: CrashReport.hostFault)
            let report = try XCTUnwrap(CrashReport.recover())
            let json = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(report.json.utf8)) as? [String: Any])
            XCTAssertEqual(json["pendingRaw"] as? String, String(decoding: pending, as: UTF8.self))
            XCTAssertEqual(json["signalRaw"] as? String, String(decoding: signal, as: UTF8.self))
            XCTAssertEqual(json["hostFaultDiagnostics"] as? String, "header\naddress=0x1234\nfooter")
            XCTAssertTrue(FileManager.default.fileExists(atPath: report.file.path))
            XCTAssertFalse(FileManager.default.fileExists(atPath: CrashReport.pending.path))
            XCTAssertFalse(FileManager.default.fileExists(atPath: CrashReport.signal.path))
        }
    }

    @MainActor
    func testSignalWithoutPendingMetadataIsRecovered() throws {
        try withFreshJournal {
            try Data("{\"exceptionKind\":\"host-SIGSEGV\"}".utf8).write(to: CrashReport.signal)
            let report = try XCTUnwrap(CrashReport.recover())
            let json = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(report.json.utf8)) as? [String: Any])
            XCTAssertEqual(json["exceptionKind"] as? String, "host-SIGSEGV")
            XCTAssertFalse(FileManager.default.fileExists(atPath: CrashReport.signal.path))
        }
    }

    @MainActor
    func testFailedRealFilesystemSaveRetainsAllJournalBytes() throws {
        try withFreshJournal {
            // A valid JSON bundle ID can exceed NAME_MAX in save's generated
            // filename. Creation fails with ENAMETOOLONG while pending files
            // remain readable and deletable. Retention must depend on the
            // successful-save guard, which already exists on main.
            let pending = try JSONSerialization.data(withJSONObject: ["bundleId": String(repeating: "x", count: 300)])
            let signal = Data("{\"exceptionKind\":\"host-SIGBUS\"}".utf8)
            let host = Data("private test host-fault text".utf8)
            try pending.write(to: CrashReport.pending)
            try signal.write(to: CrashReport.signal)
            try host.write(to: CrashReport.hostFault)
            XCTAssertNil(CrashReport.recover())
            XCTAssertEqual(try Data(contentsOf: CrashReport.pending), pending)
            XCTAssertEqual(try Data(contentsOf: CrashReport.signal), signal)
            XCTAssertEqual(try Data(contentsOf: CrashReport.hostFault), host)
            try JSONSerialization.data(withJSONObject: ["bundleId": "test.journal"]).write(to: CrashReport.pending)
            XCTAssertNotNil(CrashReport.recover())
        }
    }
}
