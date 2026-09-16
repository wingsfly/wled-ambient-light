// 生成应用图标。macOS 出 .icns，Android 出自适应图标的两层 PNG。
//
//   xcrun swift make_icon.swift ../Resources                    # macOS
//   xcrun swift make_icon.swift --android <res 目录>            # Android
//
// 两端共用这一份绘制代码 —— 图标要一眼认得出是同一个 App，照着重画一遍
// 迟早画歪。
//
// 画的是这盏灯本身：两根发光的主柱立在底座环上，与固件几何里的
// 「底座环 12 + 底柱 6 + 主柱 30」对应。没用现成的 SF Symbol —— 灯泡图标
// 谁都在用，认不出是哪个 App。
import AppKit
import CoreGraphics

let outDir = CommandLine.arguments.count > 1
    ? URL(fileURLWithPath: CommandLine.arguments[1])
    : URL(fileURLWithPath: ".")

enum Layer { case full, background, foreground }

func newContext(_ S: CGFloat) -> CGContext? {
    CGContext(data: nil, width: Int(S), height: Int(S), bitsPerComponent: 8,
              bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
}

func draw(size S: CGFloat, layer: Layer = .full) -> CGImage? {
    guard let ctx = newContext(S) else { return nil }
    let space = CGColorSpaceCreateDeviceRGB()

    // Android 自适应图标的前景内容只能占中间 66%（72dp / 108dp），外围会被
    // 启动器裁成圆形、方形或水滴形。这套图案本身就只占画布 63%（内容高
    // 650/1024），已经在安全区内，所以几乎不用再缩 —— 0.95 只是留一点余量。
    // 先前设成 0.62 缩过了头，图标在手机上会显得又小又空。
    let inset: CGFloat = layer == .foreground ? 0.95 : 1.0
    let u = S / 1024.0 * inset
    let cx = S / 2

    if layer != .foreground {
        // 背景：macOS 要圆角方（系统不裁），Android 给整块（系统自己裁形状）
        if layer == .full {
            let r = S * 0.2237                 // Apple 的圆角比例
            ctx.addPath(CGPath(roundedRect: CGRect(x: 0, y: 0, width: S, height: S),
                               cornerWidth: r, cornerHeight: r, transform: nil))
            ctx.clip()
        }
        if let grad = CGGradient(colorsSpace: space,
                                 colors: [CGColor(red: 0.09, green: 0.10, blue: 0.16, alpha: 1),
                                          CGColor(red: 0.03, green: 0.03, blue: 0.06, alpha: 1)] as CFArray,
                                 locations: [0, 1]) {
            ctx.drawLinearGradient(grad, start: CGPoint(x: 0, y: S), end: CGPoint(x: 0, y: 0), options: [])
        }
        if layer == .background { return ctx.makeImage() }
    }

    // 两根主柱。左暖右冷 —— 一眼能看出是两路独立的灯
    let tubeW = 120 * u, tubeH = 560 * u
    // 前景层要垂直居中。内容从底座环下沿（baseY - 90u）到柱顶（baseY + 560u），
    // 共 650u，中心落在 baseY + 235u —— 令它等于画布中线。
    let baseY = layer == .foreground ? S / 2 - 235 * u : 260 * u
    let cols: [(CGFloat, CGColor, CGColor)] = [
        (cx - 150 * u, CGColor(red: 1.00, green: 0.62, blue: 0.24, alpha: 1),
                       CGColor(red: 1.00, green: 0.35, blue: 0.42, alpha: 1)),
        (cx + 30 * u,  CGColor(red: 0.36, green: 0.78, blue: 1.00, alpha: 1),
                       CGColor(red: 0.45, green: 0.40, blue: 1.00, alpha: 1)),
    ]
    for (x, top, bottom) in cols {
        let rect = CGRect(x: x, y: baseY, width: tubeW, height: tubeH)
        let path = CGPath(roundedRect: rect, cornerWidth: tubeW / 2, cornerHeight: tubeW / 2, transform: nil)
        ctx.saveGState()
        ctx.setShadow(offset: .zero, blur: 90 * u, color: top.copy(alpha: 0.85))
        ctx.addPath(path); ctx.setFillColor(top); ctx.fillPath()
        ctx.restoreGState()
        ctx.saveGState()
        ctx.addPath(path); ctx.clip()
        if let g = CGGradient(colorsSpace: space, colors: [top, bottom] as CFArray, locations: [0, 1]) {
            ctx.drawLinearGradient(g, start: CGPoint(x: 0, y: baseY + tubeH),
                                   end: CGPoint(x: 0, y: baseY), options: [])
        }
        ctx.restoreGState()
    }

    // 底座环：一道横向的光带
    let ringY = layer == .foreground ? baseY - 90 * u : 170 * u
    let ringRect = CGRect(x: cx - 250 * u, y: ringY, width: 500 * u, height: 66 * u)
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: 60 * u, color: CGColor(red: 1, green: 0.8, blue: 0.5, alpha: 0.6))
    ctx.addPath(CGPath(roundedRect: ringRect, cornerWidth: 33 * u, cornerHeight: 33 * u, transform: nil))
    ctx.setFillColor(CGColor(red: 0.95, green: 0.85, blue: 0.70, alpha: 0.92))
    ctx.fillPath()
    ctx.restoreGState()

    return ctx.makeImage()
}

// ── Android：自适应图标的两层 PNG ──────────────────────────
//
// 108dp 的画布，前景内容只能占中间 72dp（66%）—— 外围留给启动器裁形状。
// minSdk 26 起自适应图标是标配，所以不再出传统的单层 mipmap。
if CommandLine.arguments.count > 2, CommandLine.arguments[1] == "--android" {
    let res = URL(fileURLWithPath: CommandLine.arguments[2])
    let densities: [(String, CGFloat)] = [
        ("mdpi", 108), ("hdpi", 162), ("xhdpi", 216), ("xxhdpi", 324), ("xxxhdpi", 432),
    ]
    for (name, px) in densities {
        let dir = res.appendingPathComponent("mipmap-\(name)")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        for (file, layer) in [("ic_launcher_foreground", Layer.foreground),
                              ("ic_launcher_background", Layer.background)] {
            guard let img = draw(size: px, layer: layer) else { continue }
            let rep = NSBitmapImageRep(cgImage: img)
            rep.size = NSSize(width: px, height: px)
            guard let png = rep.representation(using: .png, properties: [:]) else { continue }
            try? png.write(to: dir.appendingPathComponent("\(file).png"))
        }
        // 单层后备：anydpi-v26 在 minSdk 26 下总会生效，但 Play Store 与
        // 少数启动器仍按传统 mipmap 取图，缺了会拿到系统默认图标。
        let legacy = px * 48 / 108
        if let img = draw(size: legacy, layer: .full) {
            let rep = NSBitmapImageRep(cgImage: img)
            rep.size = NSSize(width: legacy, height: legacy)
            if let png = rep.representation(using: .png, properties: [:]) {
                try? png.write(to: dir.appendingPathComponent("ic_launcher.png"))
                try? png.write(to: dir.appendingPathComponent("ic_launcher_round.png"))
            }
        }
    }
    print("已生成 Android 自适应图标两层，\(densities.count) 档密度 → \(res.path)")
    exit(0)
}

// .iconset 要这几档；@2x 就是双倍像素同一张图
let specs: [(String, CGFloat)] = [
    ("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32), ("icon_32x32@2x", 64),
    ("icon_128x128", 128), ("icon_128x128@2x", 256), ("icon_256x256", 256),
    ("icon_256x256@2x", 512), ("icon_512x512", 512), ("icon_512x512@2x", 1024),
]
let iconset = outDir.appendingPathComponent("AppIcon.iconset")
try? FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for (name, size) in specs {
    guard let img = draw(size: size) else { continue }
    let rep = NSBitmapImageRep(cgImage: img)
    rep.size = NSSize(width: size, height: size)
    guard let png = rep.representation(using: .png, properties: [:]) else { continue }
    try png.write(to: iconset.appendingPathComponent("\(name).png"))
}
print("已生成 \(iconset.path)")
