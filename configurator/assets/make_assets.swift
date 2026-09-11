// Renders the LiveArea assets for the pstv1080p Configurator with AppKit.
// Usage: swift make_assets.swift <output dir>   (macOS only; the PNGs are committed)
import AppKit

let outDir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."

func render(_ w: Int, _ h: Int, _ draw: (CGRect) -> Void) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                               colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    draw(CGRect(x: 0, y: 0, width: w, height: h))
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

func label(_ s: String, _ size: CGFloat, _ color: NSColor, weight: NSFont.Weight = .bold) -> NSAttributedString {
    NSAttributedString(string: s, attributes: [
        .font: NSFont.systemFont(ofSize: size, weight: weight),
        .foregroundColor: color,
    ])
}

func centered(_ a: NSAttributedString, in r: CGRect, dy: CGFloat = 0) {
    let sz = a.size()
    a.draw(at: CGPoint(x: r.midX - sz.width / 2, y: r.midY - sz.height / 2 + dy))
}

let bg = NSColor(red: 0.09, green: 0.10, blue: 0.13, alpha: 1)
let accent = NSColor(red: 0.47, green: 0.74, blue: 1.0, alpha: 1)
let white = NSColor.white
let dim = NSColor(white: 0.7, alpha: 1)

// icon0.png 128x128: rounded dark tile with "1080p" and a thin accent bar
let icon = render(128, 128) { r in
    NSColor.clear.setFill(); r.fill()
    let path = NSBezierPath(roundedRect: r.insetBy(dx: 2, dy: 2), xRadius: 22, yRadius: 22)
    bg.setFill(); path.fill()
    accent.setFill(); NSBezierPath(roundedRect: CGRect(x: 24, y: 34, width: 80, height: 5), xRadius: 2, yRadius: 2).fill()
    centered(label("1080p", 34, white), in: r, dy: 12)
    centered(label("config", 17, dim, weight: .semibold), in: r, dy: -30)
}
try! icon.write(to: URL(fileURLWithPath: outDir + "/icon0.png"))

// bg.png 840x500: LiveArea background
let bgImg = render(840, 500) { r in
    bg.setFill(); r.fill()
    let grad = NSGradient(starting: NSColor(red: 0.13, green: 0.16, blue: 0.23, alpha: 1), ending: bg)!
    grad.draw(in: r, angle: -90)
    accent.setFill(); CGRect(x: 60, y: 300, width: 180, height: 6).fill()
    label("pstv1080p", 64, white).draw(at: CGPoint(x: 60, y: 320))
    label("Configurator", 40, accent, weight: .semibold).draw(at: CGPoint(x: 60, y: 240))
    label("Per-game frame-pacing overrides and global options", 22, dim, weight: .regular).draw(at: CGPoint(x: 60, y: 190))
    label("for the PS TV 1080p (30 Hz) plugin", 22, dim, weight: .regular).draw(at: CGPoint(x: 60, y: 160))
}
try! bgImg.write(to: URL(fileURLWithPath: outDir + "/bg.png"))

// startup.png 280x158: the gate button
let startup = render(280, 158) { r in
    NSColor.clear.setFill(); r.fill()
    let path = NSBezierPath(roundedRect: r.insetBy(dx: 2, dy: 2), xRadius: 14, yRadius: 14)
    NSColor(red: 0.18, green: 0.30, blue: 0.47, alpha: 1).setFill(); path.fill()
    centered(label("Configure", 34, white), in: r, dy: 12)
    centered(label("1080p overrides", 18, dim, weight: .semibold), in: r, dy: -22)
}
try! startup.write(to: URL(fileURLWithPath: outDir + "/startup.png"))
print("assets written to \(outDir)")
