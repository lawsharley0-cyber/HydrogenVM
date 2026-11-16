import Foundation

@main
struct PlayerProblemReportTests {
    static func main() throws {
        let crash = """
        {"signature":"1234abcd5678", "registers":[1,2,3], "pc":4096,
         "account":{"email":"owner@example.invalid","token":"do-not-export"},
         "report":"/Users/private/build/file.cpp account secret",
         "backtrace":["0x1234 _malloc /Users/private/file.cpp"],
         "host":{"account":"private-user"}}
        """
        let report = PlayerProblemReport(bundleID: "org.test.game", gameVersion: "1.2.3",
                                         device: "iPhone18,2", osVersion: "27.3", build: "0.1 (7) deadbeef",
                                         log: ["[hvm:error] Crash: bad memory access at 0x1234 _malloc",
                                               "[hvm] Loaded /Users/private/MyGame.app: armv7",
                                               "account=Alice token=do-not-export",
                                               "Crash: username Alice at 0x55",
                                               "HTTP https://example.invalid/?token=secret",
                                               "Hello from player Alice",
                                               "Stall: waiting in _pthread_mutex_lock at 0x5678"],
                                         crashJSON: crash)
        let fields = try JSONSerialization.jsonObject(with: Data(report.text.utf8)) as! [String: Any]
        let metadata = fields["metadata"] as! [String: String]
        precondition(metadata["gameBundleID"] == "org.test.game")
        precondition(metadata["gameVersion"] == "1.2.3")
        precondition(metadata["device"] == "iPhone18,2" && metadata["iOSVersion"] == "27.3")
        precondition(metadata["hydrogenBuild"] == "0.1 (7) deadbeef")
        precondition(report.text.contains("0x1234") && report.text.contains("_malloc"))
        precondition(report.text.contains("_pthread_mutex_lock"))
        for privateValue in ["Alice", "do-not-export", "owner@example.invalid", "/Users/private", "example.invalid", "private-user"] {
            precondition(!report.text.contains(privateValue), privateValue)
            precondition(!report.issueURL.absoluteString.contains(privateValue), privateValue)
        }
        let safeCrash = fields["crash"] as! [String: Any]
        precondition(safeCrash["account"] == nil && safeCrash["report"] == nil && safeCrash["host"] == nil)
        precondition((safeCrash["registers"] as! [NSNumber]).count == 3)
        precondition((safeCrash["pc"] as! NSNumber).intValue == 4096)
        precondition(safeCrash["signature"] as? String == "1234abcd5678")
        let query = URLComponents(url: report.issueURL, resolvingAgainstBaseURL: false)!.queryItems!
        let body = query.first { $0.name == "body" }!.value!
        precondition(body.contains("org.test.game") && !body.contains("0x1234"))
        precondition(report.issueURL.host == "github.com" && report.issueURL.path.hasSuffix("/issues/new"))
        let noCrash = PlayerProblemReport(bundleID: "org.test.game", gameVersion: "1", device: "iPhone", osVersion: "27", build: "x", log: [], crashJSON: nil)
        let noCrashFields = try JSONSerialization.jsonObject(with: Data(noCrash.text.utf8)) as! [String: Any]
        precondition(noCrashFields["crash"] == nil)
        precondition(PlayerProblemReport.machineCrash("bad /Users/private value")["available"] as? Bool == true)
        let limited = PlayerProblemReport.diagnosticLog(Array(repeating: "Crash: 0x1234", count: 5000))
        precondition(limited.count == 4001)
        precondition(PlayerProblemReport.diagnosticLog(["Crash: 'arbitrary-value' /outside/path/0x1234"])[0] == "Crash: ")
        print("Player problem report metadata, privacy, bounded logs and issue draft tests passed")
    }
}
