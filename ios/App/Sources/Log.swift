import Foundation

/// `Documents/logs/app.log`. Every line is flushed immediately: the first device run must leave
/// a readable trail even if the process is killed.
final class Log {
    enum Level: Int { case error = 0, warn, info, debug }

    static let shared = Log()
    private let queue = DispatchQueue(label: "rlport.log")
    private var handle: FileHandle?
    var level: Level = .warn

    private init() {
        try? FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
        let url = Paths.logs.appendingPathComponent("app.log")
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil)
        }
        handle = try? FileHandle(forWritingTo: url)
        handle?.seekToEndOfFile()
    }

    func setLevel(_ name: String) {
        level = ["error": .error, "warn": .warn, "info": .info, "debug": .debug][name] ?? .warn
    }

    func write(_ lvl: Level, _ msg: String) {
        // Errors and warnings always; info/debug by configured level. Startup facts use .warn
        // so the default config still records them.
        guard lvl.rawValue <= level.rawValue else { return }
        let line = "\(ISO8601DateFormatter().string(from: Date())) [\(lvl)] \(msg)\n"
        queue.async { [weak self] in
            self?.handle?.write(line.data(using: .utf8)!)
            try? self?.handle?.synchronize()
        }
        NSLog("%@", msg)
    }

    func flush() { queue.sync {} }
}

func logW(_ m: String) { Log.shared.write(.warn, m) }
func logI(_ m: String) { Log.shared.write(.info, m) }
func logE(_ m: String) { Log.shared.write(.error, m) }
