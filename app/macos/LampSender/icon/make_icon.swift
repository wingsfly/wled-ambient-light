// 生成 AppIcon.icns。
//
//   xcrun swift make_icon.swift ../Resources
//
// 画的是这盏灯本身：两根发光的主柱立在底座环上，与固件几何里的
// 「底座环 12 + 底柱 6 + 主柱 30」对应。没用现成的 SF Symbol —— 灯泡图标
// 谁都在用，认不出是哪个 App。
import AppKit
import CoreGraphics

let outDir = CommandLine.arguments.count > 1
    ? URL(fileURLWithPath: CommandLine.arguments[1])
    : URL(fileURLWithPath: ".")

func draw(size S: CGFloat) -> CGImage? {
    guard let ctx = CGContext(data: nil, width: Int(S), height: Int(S), bitsPerComponent: 8,
                              bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
    let u = S / 1024.0                     // 所有尺寸按 1024 的比例算

    // 背景：深色圆角方，macOS 图标的标准形状
    let r = S * 0.2237                     // Apple 的圆角比例
    let bg = CGPath(roundedRect: CGRect(x: 0, y: 0, width: S, height: S),
                    cornerWidth: r, cornerHeight: r, transform: nil)
    ctx.addPath(bg); ctx.clip()
    let space = CGColorSpaceCreateDeviceRGB()
    if let grad = CGGradient(colorsSpace: space,
                             colors: [CGColor(red: 0.09, green: 0.10, blue: 0.16, alpha: 1),
                                      CGColor(red: 0.03, green: 0.03, blue: 0.06, alpha: 1)] as CFArray,
                             locations: [0, 1]) {
        ctx.drawLinearGradient(grad, start: CGPoint(x: 0, y: S), end: CGPoint(x: 0, y: 0), options: [])
    }

    // 两根主柱。左暖右冷 —— 一眼能看出是两路独立的灯
    let tubeW = 120 * u, tubeH = 560 * u
    let baseY = 260 * u
    let cols: [(CGFloat, CGColor, CGColor)] = [
        (S * 0.5 - 150 * u, CGColor(red: 1.00, green: 0.62, blue: 0.24, alpha: 1),
                            CGColor(red: 1.00, green: 0.35, blue: 0.42, alpha: 1)),
        (S * 0.5 + 30 * u,  CGColor(red: 0.36, green: 0.78, blue: 1.00, alpha: 1),
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
    let ringRect = CGRect(x: S * 0.5 - 250 * u, y: 170 * u, width: 500 * u, height: 66 * u)
    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: 60 * u, color: CGColor(red: 1, green: 0.8, blue: 0.5, alpha: 0.6))
    ctx.addPath(CGPath(roundedRect: ringRect, cornerWidth: 33 * u, cornerHeight: 33 * u, transform: nil))
    ctx.setFillColor(CGColor(red: 0.95, green: 0.85, blue: 0.70, alpha: 0.92))
    ctx.fillPath()
    ctx.restoreGState()

    return ctx.makeImage()
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
