import Foundation
import CoreFoundation
import Observation

@MainActor
@Observable
final class PlistNode: Identifiable {
    let id = UUID()
    var key: String
    var value: Kind

    enum Kind {
        case string(String)
        case integer(Int64)
        case real(Double)
        case bool(Bool)
        case date(Date)
        case data(Data)
        case uid(Any) // Preserves CFKeyedArchiverUID opaque reference
        case array([PlistNode])
        case dict([PlistNode])

        var typeLabel: String {
            switch self {
            case .string: "String"
            case .integer: "Integer"
            case .real: "Number"
            case .bool: "Boolean"
            case .date: "Date"
            case .data(let d): "Data (\(d.count) B)"
            case .uid(let ref): "UID (\(PlistNode.uidIndex(ref) ?? 0))"
            case .array(let a): "Array (\(a.count))"
            case .dict(let d): "Dict (\(d.count))"
            }
        }
    }

    init(key: String, value: Kind) {
        self.key = key
        self.value = value
    }

    nonisolated static func isKeyedArchiverUID(_ any: Any) -> Bool {
        let desc = String(describing: any)
        return desc.contains("CFKeyedArchiverUID")
    }

    nonisolated static func uidIndex(_ any: Any) -> Int? {
        if let num = any as? NSNumber { return num.intValue }
        if let i = any as? Int { return i }
        let desc = String(describing: any)
        // Format: "<CFKeyedArchiverUID ...>{value = 42}"
        guard let range = desc.range(of: "value = ") else { return nil }
        let tail = desc[range.upperBound...]
        let digits = tail.prefix { $0.isNumber }
        return Int(digits)
    }

    static func fromAny(_ any: Any, key: String = "Root") -> PlistNode {
        if let dict = any as? [String: Any] {
            if dict.count == 1, let uidVal = dict["CF$UID"] as? NSNumber {
                return PlistNode(key: key, value: .uid(uidVal.intValue))
            }
            let children = dict.keys.sorted().map { k in fromAny(dict[k]!, key: k) }
            return PlistNode(key: key, value: .dict(children))
        } else if let arr = any as? [Any] {
            let children = arr.enumerated().map { i, v in fromAny(v, key: "[\(i)]") }
            return PlistNode(key: key, value: .array(children))
        } else if let s = any as? String {
            return PlistNode(key: key, value: .string(s))
        } else if let d = any as? Date {
            return PlistNode(key: key, value: .date(d))
        } else if let d = any as? Data {
            return PlistNode(key: key, value: .data(d))
        } else if let n = any as? NSNumber {
            if CFGetTypeID(n) == CFBooleanGetTypeID() {
                return PlistNode(key: key, value: .bool(n.boolValue))
            }
            let t = String(cString: n.objCType)
            if t == "f" || t == "d" {
                return PlistNode(key: key, value: .real(n.doubleValue))
            }
            return PlistNode(key: key, value: .integer(n.int64Value))
        } else if isKeyedArchiverUID(any) {
            return PlistNode(key: key, value: .uid(any))
        } else {
            return PlistNode(key: key, value: .string(String(describing: any)))
        }
    }

    func toAny() -> Any {
        switch value {
        case .string(let s): return s
        case .integer(let i): return NSNumber(value: i)
        case .real(let r): return NSNumber(value: r)
        case .bool(let b): return NSNumber(value: b)
        case .date(let d): return d
        case .data(let d): return d
        case .uid(let ref):
            // Binary UID objects carry a distinct plist type. Turning them into
            // CF$UID dictionaries corrupts NSKeyedArchiver references on save.
            if PlistNode.isKeyedArchiverUID(ref) { return ref }
            if let idx = PlistNode.uidIndex(ref) {
                return ["CF$UID": NSNumber(value: idx)]
            }
            return ref
        case .array(let a): return a.map { $0.toAny() }
        case .dict(let d):
            var out: [String: Any] = [:]
            for c in d { out[c.key] = c.toAny() }
            return out
        }
    }

    var isKeyedArchiveRoot: Bool {
        guard case .dict(let d) = value else { return false }
        return d.contains(where: { $0.key == "$archiver" }) && d.contains(where: { $0.key == "$objects" })
    }

    var keyedArchiveObjects: [PlistNode]? {
        guard case .dict(let d) = value,
              let objsNode = d.first(where: { $0.key == "$objects" }),
              case .array(let arr) = objsNode.value else { return nil }
        return arr
    }
}
