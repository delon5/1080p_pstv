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

func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> NSColor {
    NSColor(red: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: a)
}
func label(_ s: String, _ size: CGFloat, _ color: NSColor, weight: NSFont.Weight = .bold) -> NSAttributedString {
    NSAttributedString(string: s, attributes: [.font: NSFont.systemFont(ofSize: size, weight: weight), .foregroundColor: color])
}
func centered(_ a: NSAttributedString, in r: CGRect, dy: CGFloat = 0) {
    let sz = a.size()
    a.draw(at: CGPoint(x: r.midX - sz.width / 2, y: r.midY - sz.height / 2 + dy))
}
func rounded(_ r: CGRect, _ radius: CGFloat) -> NSBezierPath { NSBezierPath(roundedRect: r, xRadius: radius, yRadius: radius) }

let navy   = rgb(16, 21, 32)
let navy2  = rgb(28, 36, 54)
let steel  = rgb(52, 66, 96)
let accent = rgb(94, 170, 255)
let accent2 = rgb(52, 120, 200)
let white  = NSColor.white
let dim    = rgb(170, 180, 200)
let mint   = rgb(120, 220, 160)

// A stylised widescreen TV: bezel, panel, stand.  `scale` = width of the bezel.
func drawTV(x: CGFloat, y: CGFloat, w: CGFloat, panelText: String?, textSize: CGFloat, showStand: Bool) {
    let h = w * 9 / 16
    let bezel = CGRect(x: x, y: y, width: w, height: h)
    steel.setFill(); rounded(bezel, w * 0.035).fill()
    let inset = w * 0.03
    let panel = bezel.insetBy(dx: inset, dy: inset)
    navy.setFill(); rounded(panel, w * 0.02).fill()
    // scan lines
    rgb(255, 255, 255, 0.045).setFill()
    var ly = panel.minY + inset
    while ly < panel.maxY - inset { CGRect(x: panel.minX + inset, y: ly, width: panel.width - 2 * inset, height: 1).fill(); ly += 4 }
    // accent light band across the panel
    accent2.withAlphaComponent(0.22).setFill()
    let band = NSBezierPath()
    band.move(to: CGPoint(x: panel.minX, y: panel.minY + panel.height * 0.35))
    band.line(to: CGPoint(x: panel.maxX, y: panel.minY + panel.height * 0.62))
    band.line(to: CGPoint(x: panel.maxX, y: panel.minY + panel.height * 0.78))
    band.line(to: CGPoint(x: panel.minX, y: panel.minY + panel.height * 0.51))
    band.close(); band.fill()
    if let t = panelText {
        centered(label(t, textSize, white, weight: .heavy), in: panel, dy: textSize * 0.10)
    }
    // power LED
    mint.setFill()
    NSBezierPath(ovalIn: CGRect(x: bezel.maxX - inset * 1.6, y: bezel.minY + inset * 0.45, width: inset * 0.5, height: inset * 0.5)).fill()
    if showStand {
        steel.setFill()
        CGRect(x: bezel.midX - w * 0.05, y: bezel.minY - h * 0.12, width: w * 0.10, height: h * 0.12).fill()
        rounded(CGRect(x: bezel.midX - w * 0.22, y: bezel.minY - h * 0.16, width: w * 0.44, height: h * 0.05), 3).fill()
    }
}

// icon0.png 128x128
let icon = render(128, 128) { r in
    NSColor.clear.setFill(); r.fill()
    navy2.setFill(); rounded(r.insetBy(dx: 2, dy: 2), 24).fill()
    // soft diagonal highlight
    rgb(255, 255, 255, 0.05).setFill()
    let hl = NSBezierPath(); hl.move(to: CGPoint(x: 2, y: 60)); hl.line(to: CGPoint(x: 126, y: 110)); hl.line(to: CGPoint(x: 126, y: 126)); hl.line(to: CGPoint(x: 2, y: 126)); hl.close(); hl.fill()
    drawTV(x: 16, y: 44, w: 96, panelText: "1080p", textSize: 24, showStand: true)
    centered(label("CONFIG", 12, accent, weight: .heavy), in: CGRect(x: 0, y: 8, width: 128, height: 20))
}
try! icon.write(to: URL(fileURLWithPath: outDir + "/icon0.png"))

// bg.png 840x500: LiveArea background
let bgImg = render(840, 500) { r in
    navy.setFill(); r.fill()
    NSGradient(starting: rgb(26, 34, 52), ending: navy)!.draw(in: r, angle: -90)
    // faint grid
    rgb(255, 255, 255, 0.035).setFill()
    var gx: CGFloat = 0
    while gx < 840 { CGRect(x: gx, y: 0, width: 1, height: 500).fill(); gx += 40 }
    var gy: CGFloat = 0
    while gy < 500 { CGRect(x: 0, y: gy, width: 840, height: 1).fill(); gy += 40 }
    // big TV on the right
    drawTV(x: 470, y: 150, w: 320, panelText: "1080p", textSize: 76, showStand: true)
    centered(label("30 Hz", 22, dim, weight: .semibold), in: CGRect(x: 470, y: 96, width: 320, height: 30))
    // title block on the left
    accent.setFill(); CGRect(x: 60, y: 318, width: 120, height: 6).fill()
    label("pstv1080p", 62, white, weight: .heavy).draw(at: CGPoint(x: 58, y: 338))
    label("Configurator", 38, accent, weight: .semibold).draw(at: CGPoint(x: 60, y: 262))
    label("Per-game frame pacing overrides", 21, dim, weight: .regular).draw(at: CGPoint(x: 60, y: 210))
    label("and 1080p plugin options", 21, dim, weight: .regular).draw(at: CGPoint(x: 60, y: 182))
    // legend chips
    let chips: [(String, NSColor)] = [("frameskip", mint), ("nowait", rgb(255, 190, 90)), ("off", rgb(255, 110, 110)), ("inject", accent), ("force", rgb(220, 140, 255))]
    var cx: CGFloat = 60
    for (t, c) in chips {
        let a = label(t, 15, c, weight: .semibold)
        let w = a.size().width + 22
        rgb(255, 255, 255, 0.07).setFill(); rounded(CGRect(x: cx, y: 118, width: w, height: 30), 15).fill()
        c.withAlphaComponent(0.6).setStroke(); let p = rounded(CGRect(x: cx + 0.5, y: 118.5, width: w - 1, height: 29), 14.5); p.lineWidth = 1; p.stroke()
        a.draw(at: CGPoint(x: cx + 11, y: 124))
        cx += w + 10
    }
    label("for PlayStation TV", 15, dim, weight: .regular).draw(at: CGPoint(x: 60, y: 70))
}
try! bgImg.write(to: URL(fileURLWithPath: outDir + "/bg.png"))

// startup.png 280x158: the gate button
let startup = render(280, 158) { r in
    NSColor.clear.setFill(); r.fill()
    navy2.setFill(); rounded(r.insetBy(dx: 2, dy: 2), 16).fill()
    accent2.withAlphaComponent(0.35).setFill(); rounded(CGRect(x: 2, y: 2, width: 276, height: 60), 16).fill()
    navy2.setFill(); CGRect(x: 2, y: 40, width: 276, height: 22).fill()
    drawTV(x: 22, y: 60, w: 96, panelText: "1080p", textSize: 22, showStand: false)
    label("Configure", 30, white, weight: .bold).draw(at: CGPoint(x: 132, y: 92))
    label("game overrides", 16, dim, weight: .regular).draw(at: CGPoint(x: 134, y: 68))
    centered(label("press to start", 14, accent, weight: .semibold), in: CGRect(x: 0, y: 18, width: 280, height: 20))
}
try! startup.write(to: URL(fileURLWithPath: outDir + "/startup.png"))
print("assets written to \(outDir)")
