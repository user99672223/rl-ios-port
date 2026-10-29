import SwiftUI

/// Pre-launch screen: JIT status, settings (spec 6.9), and the start button.
struct RootView: View {
    @ObservedObject var model: AppModel
    @State private var draft = PortConfig()

    var body: some View {
        NavigationStack {
            Form {
                Section("Status") {
                    statusRow
                    LabeledContent("Game data", value: model.gamePresent ? "found" : "missing (push with push_game.sh)")
                    if model.externalDisplayConnected {
                        LabeledContent("Display", value: "external")
                    }
                }
                Section("Rendering") {
                    VStack(alignment: .leading) {
                        Text("Render scale \(draft.renderScale, specifier: "%.2f")")
                        Slider(value: $draft.renderScale, in: 0.25...1.0, step: 0.01)
                    }
                    Picker("Drawable size", selection: $draft.drawableMode) {
                        Text("Auto").tag("auto")
                        Text("Native").tag("native")
                    }
                    Stepper("Frame cap \(draft.maxFPS)", value: $draft.maxFPS, in: 15...120, step: 15)
                    Toggle("Lower render scale when hot", isOn: $draft.thermalScaling)
                }
                Section("Textures") {
                    Picker("BC1 → ASTC block", selection: $draft.astcBC1Block) {
                        ForEach(PortConfig.blockSizes, id: \.self) { Text($0).tag($0) }
                    }
                    Picker("BC3 → ASTC block", selection: $draft.astcBC3Block) {
                        ForEach(PortConfig.blockSizes, id: \.self) { Text($0).tag($0) }
                    }
                }
                Section("Advanced") {
                    Picker("Log level", selection: $draft.logLevel) {
                        ForEach(PortConfig.logLevels, id: \.self) { Text($0).tag($0) }
                    }
                    Picker("TXM", selection: $draft.jitTXM) {
                        Text("Detect").tag("auto")
                        Text("Present").tag("true")
                        Text("Absent").tag("false")
                    }
                    Toggle("AOT only (unsupported)", isOn: $draft.allowAOTOnly).disabled(true)
                }
                Section {
                    Button("Save settings") { model.save(draft) }
                        .disabled(draft == model.config)
                    Button("Start Rocket League") {}
                        .disabled(true)  // enabled once the Wine runtime lands (STATUS.md, step 6)
                }
            }
            .navigationTitle("RLPort")
        }
        .onAppear { draft = model.config }
    }

    @ViewBuilder private var statusRow: some View {
        switch model.phase {
        case .starting:
            LabeledContent("JIT", value: "checking…")
        case .needsJIT(let why):
            VStack(alignment: .leading, spacing: 4) {
                Text("Launch through StikDebug").font(.headline)
                Text(why).font(.footnote).foregroundStyle(.secondary)
            }
        case .jitError(let why):
            VStack(alignment: .leading, spacing: 4) {
                Text("JIT unavailable").font(.headline).foregroundStyle(.red)
                Text(why).font(.footnote)
            }
        case .ready:
            LabeledContent("JIT", value: "ready")
        }
    }
}
