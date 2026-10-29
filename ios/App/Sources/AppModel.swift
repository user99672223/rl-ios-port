import Foundation
import os
import UIKit

/// Startup state shared by the phone and external-display scenes. All heavy work runs on a
/// background queue so the launch watchdog never sees a blocked main thread (spec 5, 6.6).
final class AppModel: ObservableObject {
    static let shared = AppModel()

    enum Phase: Equatable {
        case starting
        case needsJIT(String)       // "Launch through StikDebug"
        case jitError(String)
        case ready                  // JIT reserved; game can be started
    }

    @Published var phase: Phase = .starting
    @Published var config: PortConfig
    @Published var externalDisplayConnected = false
    @Published var gamePresent = false

    private let work = DispatchQueue(label: "rlport.startup", qos: .userInitiated)

    private init() {
        config = ConfigStore.load()
        Log.shared.setLevel(config.logLevel)
    }

    func start() {
        work.async { self.startup() }
    }

    func save(_ c: PortConfig) {
        config = c
        Log.shared.setLevel(c.logLevel)
        work.async { ConfigStore.save(c) }
    }

    private func startup() {
        let info = Bundle.main.infoDictionary ?? [:]
        logW("startup: \(info["CFBundleIdentifier"] ?? "?") \(info["CFBundleShortVersionString"] ?? "?") (\(info["CFBundleVersion"] ?? "?")), iOS \(UIDevice.current.systemVersion)")
        logW("startup: available memory \(availableMemoryMB()) MB, thermal state \(ProcessInfo.processInfo.thermalState.rawValue)")
        let game = FileManager.default.fileExists(atPath: Paths.game.appendingPathComponent("Binaries/Win64/RocketLeague.exe").path)
        logW("startup: game data \(game ? "present" : "missing") at \(Paths.game.path)")

        let probe = rl_simulator_jit_probe()
        if probe != -100 { logW("startup: simulator split-mapping probe returned \(probe) (expect 42)") }

        var reason = [CChar](repeating: 0, count: 256)
        // 64 MB: FEX's code buffers come out of this one region (D-010: it cannot grow later).
        let st = jit_init(jit_platform_backend(), 64 << 20, config.txmOverride, &reason, reason.count)
        let why = String(cString: reason)
        logW("startup: jit_init -> \(String(cString: jit_status_str(st)))\(why.isEmpty ? "" : ": " + why)")

        writeStartupRecord(game: game, jit: st, reason: why, probe: probe)
        Log.shared.flush()

        DispatchQueue.main.async {
            self.gamePresent = game
            switch st {
            case JIT_STATUS_READY: self.phase = .ready
            case JIT_STATUS_NOT_DEBUGGED: self.phase = .needsJIT(why)
            default: self.phase = .jitError(why)
            }
        }
    }

    /// Machine-readable summary for CI smoke tests and for the owner's first device run.
    private func writeStartupRecord(game: Bool, jit: jit_status, reason: String, probe: Int32) {
        let rec: [String: Any] = [
            "config_path": Paths.config.path,
            "config_exists": FileManager.default.fileExists(atPath: Paths.config.path),
            "game_present": game,
            "jit_status": String(cString: jit_status_str(jit)),
            "jit_reason": reason,
            "jit_region_bytes": jit_region_size(),
            "simulator_jit_probe": probe,
            "available_memory_mb": availableMemoryMB(),
        ]
        if let data = try? JSONSerialization.data(withJSONObject: rec, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: Paths.logs.appendingPathComponent("startup.json"))
        }
    }

    private func availableMemoryMB() -> Int {
        // os_proc_available_memory() (<os/proc.h>, iOS 13+): the budget left before jetsam.
        Int(os_proc_available_memory() / (1 << 20))
    }
}
