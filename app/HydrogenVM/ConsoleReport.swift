import Foundation

/// Picks the lines of a game session's console that point at a problem, for a
/// GitHub issue: errors and warnings, missing functions and methods, crashes,
/// and GPU status. Repeats are folded into one line with a count (addresses are
/// ignored when comparing), the worst problems come first, and the result is
/// cut to fit in an issue URL.
struct ConsoleReport {
    struct Problem {
        let text: String
        var count: Int
        let rank: Int       // 0 stop/crash, 1 error, 2 warning, 3 other notable
        let order: Int
    }

    private(set) var problems: [Problem] = []
    private(set) var tail: [String] = []

    /// The line that best names what went wrong.
    var headline: String? {
        guard let best = problems.min(by: { ($0.rank, $0.order) < ($1.rank, $1.order) }), best.rank <= 2 else { return nil }
        var t = best.text
        for prefix in ["[hvm:error] ", "[hvm:warn] ", "[hvm] "] where t.hasPrefix(prefix) { t.removeFirst(prefix.count) }
        if t.hasPrefix("Stopped: ") { t.removeFirst("Stopped: ".count) }
        return String(t.prefix(100))
    }

    init(lines: [(String, HVMLogLevel)], tailCount: Int = 12) {
        var index: [String: Int] = [:]
        for (order, (raw, level)) in lines.enumerated() {
            let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !text.isEmpty, let rank = Self.rank(text, level) else { continue }
            let key = Self.normalized(text)
            if let i = index[key] {
                problems[i].count += 1
            } else {
                index[key] = problems.count
                problems.append(Problem(text: String(text.prefix(600)), count: 1, rank: rank, order: order))
            }
        }
        tail = lines.suffix(tailCount).map { String($0.0.prefix(300)) }
    }

    /// Markdown for the issue body, at most `limit` characters.
    func markdown(limit: Int) -> String {
        var out = ""
        let sorted = problems.sorted { ($0.rank, $0.order) < ($1.rank, $1.order) }
        if sorted.isEmpty {
            out += "No errors or warnings in the console.\n"
        } else {
            out += "### Problems (\(sorted.count))\n```\n"
            let tailBlock = "```\n### Last lines\n```\n" + tail.joined(separator: "\n") + "\n```\n"
            let budget = max(400, limit - out.count - tailBlock.count - 80)
            var used = 0, shown = 0
            for p in sorted {
                let line = (p.count > 1 ? "\(p.count)× " : "") + p.text + "\n"
                if used + line.count > budget { break }
                out += line
                used += line.count
                shown += 1
            }
            if shown < sorted.count { out += "… \(sorted.count - shown) more (Copy in the console has the full log)\n" }
            out += tailBlock
            return out
        }
        out += "### Last lines\n```\n" + tail.joined(separator: "\n") + "\n```\n"
        return String(out.prefix(limit))
    }

    // MARK: - Classification

    private static let stopWords = ["stopped:", "crash", "fault", "sigsegv", "sigbus", "uncaught", "terminat", "abort", "assert"]
    private static let notableWords = ["missing", "not implemented", "aren't implemented", "unimplemented", "unrecognized selector",
                                       "unsupported", "failed", "failure", "cannot", "can't", "could not", "invalid", "gl error",
                                       "gpu:", "exception", "error", "timed out", "watchdog", "out of memory"]
    // Routine output that matches a word above but isn't a problem.
    private static let ignored = ["(localytics", "error uploading", "no error"]

    static func rank(_ text: String, _ level: HVMLogLevel) -> Int? {
        let t = text.lowercased()
        if ignored.contains(where: { t.contains($0) }) { return nil }
        if stopWords.contains(where: { t.contains($0) }) { return 0 }
        switch level {
        // The emulator's own errors and warnings. Game output (NSLog goes to
        // stderr) is judged by its words, like stdout.
        case .error: return 1
        case .warning: return 2
        default: break
        }
        return notableWords.contains(where: { t.contains($0) }) ? 3 : nil
    }

    /// Lines that differ only in addresses or counters count as the same problem.
    static func normalized(_ text: String) -> String {
        var s = text.replacingOccurrences(of: "0x[0-9A-Fa-f]+", with: "0x…", options: .regularExpression)
        s = s.replacingOccurrences(of: "\\b\\d{3,}\\b", with: "#", options: .regularExpression)
        return s
    }
}
