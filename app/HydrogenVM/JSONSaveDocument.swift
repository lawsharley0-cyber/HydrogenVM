import Foundation
import CoreFoundation
import Observation

@MainActor
@Observable
final class JSONNode: Identifiable {
    let id = UUID()
    var key: String
    var value: Kind

    enum Kind {
        case string(String)
        case number(NSNumber)
        case bool(Bool)
        case null
        case array([JSONNode])
        case object([JSONNode])

        var label: String {
            switch self {
            case .string: "String"
            case .number: "Number"
            case .bool: "Boolean"
            case .null: "Null"
            case .array(let a): "Array (\(a.count))"
            case .object(let o): "Object (\(o.count))"
            }
        }
    }

    init(key: String, value: Kind) {
        self.key = key
        self.value = value
    }

    static func fromAny(_ any: Any, key: String = "root") -> JSONNode {
        if let dict = any as? [String: Any] {
            let children = dict.keys.sorted().map { k in fromAny(dict[k]!, key: k) }
            return JSONNode(key: key, value: .object(children))
        } else if let arr = any as? [Any] {
            let children = arr.enumerated().map { i, v in fromAny(v, key: "[\(i)]") }
            return JSONNode(key: key, value: .array(children))
        } else if let s = any as? String {
            return JSONNode(key: key, value: .string(s))
        } else if let n = any as? NSNumber {
            if CFGetTypeID(n) == CFBooleanGetTypeID() {
                return JSONNode(key: key, value: .bool(n.boolValue))
            }
            return JSONNode(key: key, value: .number(n))
        } else {
            return JSONNode(key: key, value: .null)
        }
    }

    func toAny() -> Any {
        switch value {
        case .string(let s): return s
        case .number(let number): return number
        case .bool(let b): return b
        case .null: return NSNull()
        case .array(let a): return a.map { $0.toAny() }
        case .object(let o):
            var dict: [String: Any] = [:]
            for child in o { dict[child.key] = child.toAny() }
            return dict
        }
    }
}

