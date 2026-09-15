import Foundation
import Darwin

/// LAMP1 + WLED V2 的 UDP 出口，带两条自愈。
///
/// 只由工作线程调用，内部因此不加锁。
final class UDPSender: @unchecked Sendable {
    private(set) var targetIP: String?
    private(set) var localIP: String?
    private var fd: Int32 = -1
    private var spec: String
    private var lastResolve = Date.distantPast
    private var lastProbe = Date.distantPast

    /// 逗号分隔的候选（mDNS 名或 IP），取第一个能解析的。换网之后 mDNS 可能
    /// 解析不到而固定 IP 能通，或者反过来 —— 两个都列上就不用改配置。
    init(targetSpec: String) {
        spec = targetSpec
        retarget()
    }
    deinit { closeSocket() }

    func updateTarget(_ newSpec: String) {
        guard newSpec != spec else { return }
        spec = newSpec
        lastResolve = .distantPast
        retarget()
    }

    private func closeSocket() { if fd >= 0 { close(fd); fd = -1 } }

    private func retarget() {
        targetIP = Self.resolve(spec)
        rebuildSocket()
        lastResolve = Date()
    }

    /// 出口接口很关键：组播要显式指定从哪块网卡出去，否则默认路由可能
    /// 挑到 Tailscale 之类的隧道口，板子什么都收不到。
    private func rebuildSocket() {
        closeSocket()
        fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { localIP = nil; return }
        var ttl: UInt8 = 1
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, socklen_t(MemoryLayout<UInt8>.size))
        localIP = targetIP.flatMap { Self.egressIP(toward: $0) }
        if let lip = localIP, var a = Self.inAddr(lip) {
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, socklen_t(MemoryLayout<in_addr>.size))
        }
    }

    func send(_ ptr: UnsafeRawPointer, _ len: Int, port: UInt16) {
        guard fd >= 0 else { return }
        sendTo("239.0.0.1", port, ptr, len)          // 组播：同网段的板子都能收
        if let t = targetIP { sendTo(t, port, ptr, len) }   // 单播：跨网段/组播被过滤时的保底
    }

    private func sendTo(_ ip: String, _ port: UInt16, _ ptr: UnsafeRawPointer, _ len: Int) {
        guard let addr = Self.inAddr(ip) else { return }
        var sa = sockaddr_in()
        sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        sa.sin_family = sa_family_t(AF_INET)
        sa.sin_port = port.bigEndian
        sa.sin_addr = addr
        _ = withUnsafePointer(to: &sa) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { sp in
                sendto(fd, ptr, len, 0, sp, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
    }

    /// 工作线程定期调用。两条自愈：
    ///  · 每 2 秒探测出口 IP —— 网络路径变了（Wi-Fi 切换、隧道起落）之后老
    ///    socket 会「活着但断路」：照常 sendto、板子颗粒无收，不重建不恢复。
    ///  · 每 60 秒重解析目标名，防板子换 IP。
    func tick() {
        let now = Date()
        if now.timeIntervalSince(lastProbe) >= 2 {
            lastProbe = now
            if let t = targetIP, let cur = Self.egressIP(toward: t), cur != localIP {
                rebuildSocket()
            }
        }
        if now.timeIntervalSince(lastResolve) >= 60 {
            lastResolve = now
            if let nip = Self.resolve(spec), nip != targetIP {
                targetIP = nip
                rebuildSocket()
            }
        }
    }

    // ── 纯函数工具 ──────────────────────────────────────────

    static func inAddr(_ ip: String) -> in_addr? {
        var a = in_addr()
        return inet_pton(AF_INET, ip, &a) == 1 ? a : nil
    }

    static func resolve(_ spec: String) -> String? {
        for cand in spec.split(separator: ",").map({ $0.trimmingCharacters(in: .whitespaces) })
        where !cand.isEmpty {
            if inAddr(cand) != nil { return cand }
            var hints = addrinfo(ai_flags: 0, ai_family: AF_INET, ai_socktype: SOCK_DGRAM,
                                 ai_protocol: 0, ai_addrlen: 0, ai_canonname: nil,
                                 ai_addr: nil, ai_next: nil)
            var res: UnsafeMutablePointer<addrinfo>?
            guard getaddrinfo(cand, nil, &hints, &res) == 0, let head = res else { continue }
            defer { freeaddrinfo(head) }
            guard let sa = head.pointee.ai_addr else { continue }
            let ip = sa.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { p -> String in
                var addr = p.pointee.sin_addr
                var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                inet_ntop(AF_INET, &addr, &buf, socklen_t(INET_ADDRSTRLEN))
                return String(cString: buf)
            }
            if !ip.isEmpty { return ip }
        }
        return nil
    }

    /// 内核会替我们挑路由：往目标 connect 一个不发包的 UDP socket，
    /// 再问它本地端绑到了哪个地址。
    static func egressIP(toward ip: String) -> String? {
        guard let a = inAddr(ip) else { return nil }
        let s = socket(AF_INET, SOCK_DGRAM, 0)
        guard s >= 0 else { return nil }
        defer { close(s) }
        var sa = sockaddr_in()
        sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        sa.sin_family = sa_family_t(AF_INET)
        sa.sin_port = UInt16(11988).bigEndian
        sa.sin_addr = a
        let ok = withUnsafePointer(to: &sa) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { sp in
                connect(s, sp, socklen_t(MemoryLayout<sockaddr_in>.size)) == 0
            }
        }
        guard ok else { return nil }
        var local = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let got = withUnsafeMutablePointer(to: &local) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { sp in
                getsockname(s, sp, &len) == 0
            }
        }
        guard got else { return nil }
        var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        inet_ntop(AF_INET, &local.sin_addr, &buf, socklen_t(INET_ADDRSTRLEN))
        return String(cString: buf)
    }
}
