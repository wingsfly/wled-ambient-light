import Foundation
import ServiceManagement

/// 开机自启，替代 Python 版那份 launchd 代理（RunAtLoad + KeepAlive）。
///
/// 走 `SMAppService` 而不是自己往 `~/Library/LaunchAgents` 塞 plist：注册后
/// 由系统在登录时拉起，用户能在「系统设置 → 通用 → 登录项」里看见它、随手
/// 关掉。一个藏在 LaunchAgents 目录里的 plist 没有这个可见性。
///
/// 注意 SMAppService 记的是**当前这份 app 的位置**，所以必须先装到最终目录
/// 再注册；从 build/ 里直接注册，挪走之后登录项就指空了。
enum LoginItem {
    static var status: SMAppService.Status { SMAppService.mainApp.status }
    static var isEnabled: Bool { status == .enabled }
    /// macOS 有时会把新注册的登录项挂起，等用户在系统设置里点同意。
    static var needsApproval: Bool { status == .requiresApproval }

    /// 给没有 UI 的场合用（ssh 里 `defaults read com.hjma.lamp.sender
    /// launchAtLoginStatus`）—— 登录项的真实状态只有系统知道，BTM 那份
    /// 数据库要 sudo 才读得到。
    static var statusText: String {
        switch status {
        case .enabled:         return "enabled"
        case .requiresApproval: return "requiresApproval"
        case .notRegistered:   return "notRegistered"
        case .notFound:        return "notFound"
        @unknown default:      return "unknown(\(status.rawValue))"
        }
    }

    static func set(_ on: Bool) throws {
        if on {
            guard status != .enabled else { return }
            try SMAppService.mainApp.register()
        } else {
            guard status != .notRegistered else { return }
            try SMAppService.mainApp.unregister()
        }
    }
}
