import Foundation

/// `Documents/config/port.toml` (spec 6.9). Flat `key = value` lines only, which is all the
/// file uses; unknown keys are kept so hand edits survive a save.
struct PortConfig: Equatable {
    var renderScale = 0.67
    var drawableMode = "auto"
    var maxFPS = 60
    var thermalScaling = true
    var allowAOTOnly = false          // unsupported since D-007; kept so the key round-trips
    var astcBC1Block = "6x6"
    var astcBC3Block = "4x4"
    var logLevel = "warn"
    var jitTXM = "auto"               // "auto" | "true" | "false" (D-010)
    var extra: [(String, String)] = []

    static func == (a: PortConfig, b: PortConfig) -> Bool { a.serialize() == b.serialize() }

    static let blockSizes = ["4x4", "5x4", "5x5", "6x5", "6x6", "8x5", "8x6", "8x8"]
    static let logLevels = ["error", "warn", "info", "debug"]

    func serialize() -> String {
        var lines = [
            "render_scale = \(renderScale)",
            "drawable_mode = \"\(drawableMode)\"",
            "max_fps = \(maxFPS)",
            "thermal_scaling = \(thermalScaling)",
            "allow_aot_only = \(allowAOTOnly)",
            "astc_bc1_block = \"\(astcBC1Block)\"",
            "astc_bc3_block = \"\(astcBC3Block)\"",
            "log_level = \"\(logLevel)\"",
            "jit_txm = \"\(jitTXM)\"",
        ]
        lines += extra.map { "\($0.0) = \($0.1)" }
        return lines.joined(separator: "\n") + "\n"
    }

    static func parse(_ text: String) -> PortConfig {
        var c = PortConfig()
        for raw in text.split(separator: "\n", omittingEmptySubsequences: true) {
            let line = raw.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false)[0]
            let parts = line.split(separator: "=", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
            guard parts.count == 2, !parts[0].isEmpty else { continue }
            let key = parts[0], value = parts[1]
            let str = value.hasPrefix("\"") && value.hasSuffix("\"") && value.count >= 2
                ? String(value.dropFirst().dropLast()) : value
            switch key {
            case "render_scale": c.renderScale = min(max(Double(value) ?? c.renderScale, 0.25), 1.0)
            case "drawable_mode": c.drawableMode = str
            case "max_fps": c.maxFPS = min(max(Int(value) ?? c.maxFPS, 15), 120)
            case "thermal_scaling": c.thermalScaling = value == "true"
            case "allow_aot_only": c.allowAOTOnly = value == "true"
            case "astc_bc1_block": c.astcBC1Block = blockSizes.contains(str) ? str : c.astcBC1Block
            case "astc_bc3_block": c.astcBC3Block = blockSizes.contains(str) ? str : c.astcBC3Block
            case "log_level": c.logLevel = logLevels.contains(str) ? str : c.logLevel
            case "jit_txm": c.jitTXM = ["auto", "true", "false"].contains(str) ? str : c.jitTXM
            default: c.extra.append((key, value))
            }
        }
        return c
    }

    /// -1 detect, 0 absent, 1 present (jit_init's txm_override).
    var txmOverride: Int32 { jitTXM == "true" ? 1 : jitTXM == "false" ? 0 : -1 }
}

enum Paths {
    static let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    static let config = documents.appendingPathComponent("config/port.toml")
    static let logs = documents.appendingPathComponent("logs")
    static let game = documents.appendingPathComponent("game")
    static let prefix = documents.appendingPathComponent("prefix")
}

enum ConfigStore {
    /// Reads the config, writing defaults on first run.
    static func load() -> PortConfig {
        if let text = try? String(contentsOf: Paths.config, encoding: .utf8) {
            return PortConfig.parse(text)
        }
        let c = PortConfig()
        save(c)
        return c
    }

    static func save(_ c: PortConfig) {
        try? FileManager.default.createDirectory(at: Paths.config.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? c.serialize().write(to: Paths.config, atomically: true, encoding: .utf8)
    }
}
