import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var viewModel: MAAViewModel
    @State private var initialized = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("开拓者开局零件采集")
                .font(.title.bold())
            Text("开拓者分队 · 开局弹窗与第一层背包 · recovery-v4")
                .foregroundStyle(.secondary)
            Text("先在设置中连接 PlayCover 或模拟器，再进入黑流树海探索入口。请已解锁并激活所需的开拓者分队等级。开始后会放弃当前探索，反复采集开局零件和第一层背包。")
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 12) {
                Button("开始采集") {
                    Task {
                        do {
                            try await viewModel.miniGame(name: "MiniGame@BlackFlow@Begin")
                        } catch {
                            viewModel.logError("采集启动失败：\(error.localizedDescription)")
                        }
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(!initialized || viewModel.status != .idle)
                Button("停止") {
                    Task {
                        do { try await viewModel.stop() }
                        catch { viewModel.logError("停止失败：\(error.localizedDescription)") }
                    }
                }
                .disabled(viewModel.status != .busy)
                Button("打开采集目录") {
                    let url = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
                        .appendingPathComponent("MAAPioneer/debug/roguelike/black_flow/opening_cycles", isDirectory: true)
                    do {
                        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
                        NSWorkspace.shared.open(url)
                    } catch {
                        viewModel.logError("打开目录失败：\(error.localizedDescription)")
                    }
                }
                Spacer()
                Text(viewModel.status == .idle ? "就绪" : viewModel.status == .busy ? "采集中" : "连接／初始化中")
                    .foregroundStyle(.secondary)
            }
            Divider()
            LogView()
        }
        .padding(24)
        .frame(minWidth: 760, minHeight: 540)
        .task {
            do {
                try await viewModel.initialize()
                initialized = true
                viewModel.logInfo("开拓者专项已就绪；使用随包固定资源，采集目录独立保存。")
            } catch {
                viewModel.logError("初始化失败：\(error.localizedDescription)")
            }
        }
    }
}
