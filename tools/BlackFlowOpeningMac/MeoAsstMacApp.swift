import SwiftUI

@main
struct MeoAsstMacApp: App {
    @NSApplicationDelegateAdaptor private var appDelegate: AppDelegate
    @StateObject private var appViewModel = MAAViewModel()

    var body: some Scene {
        WindowGroup("MAA 开拓者") {
            ContentView().environmentObject(appViewModel)
        }
        .commands {
            CommandGroup(replacing: .newItem) { OpenLogFileView() }
        }
        Settings {
            TabView {
                ConnectionSettingsView()
                    .tabItem { Label("连接设置", systemImage: "rectangle.connected.to.line.below") }
                GameSettingsView()
                    .tabItem { Label("游戏设置", systemImage: "gamecontroller") }
                SystemSettingsView()
                    .tabItem { Label("系统设置", systemImage: "wrench.adjustable") }
            }
            .environmentObject(appViewModel)
            .frame(maxWidth: 400, minHeight: 260)
        }
    }
}

private class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillFinishLaunching(_ notification: Notification) {
        NSWindow.allowsAutomaticWindowTabbing = false
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
