// Rasterise Inter Tight glyphs for tools/aa_extend.py (macOS CoreText): one JSON line per character with its
// advance (px), ink box (left bearing, top above baseline, width, height) and 8-bit alpha rows.
// swift tools/aa_raster.swift <font.ttf> <px> <weight> <chars>
import CoreGraphics
import CoreText
import Foundation

let a = CommandLine.arguments
let url = URL(fileURLWithPath: a[1]) as CFURL
let px = CGFloat(Double(a[2])!), wght = Double(a[3])!
let descs = CTFontManagerCreateFontDescriptorsFromURL(url) as! [CTFontDescriptor]
let wghtTag = 0x7767_6874
let attrs = [kCTFontVariationAttribute: [wghtTag: wght]] as CFDictionary
let desc = CTFontDescriptorCreateCopyWithAttributes(descs[0], attrs)
let font = CTFontCreateWithFontDescriptor(desc, px, nil)
let W = Int(px * 3), H = Int(px * 3), ox = Int(px), oy = Int(px)  // pen origin, baseline (from the bottom)
for ch in a[4].unicodeScalars {
    var u = [UniChar](String(ch).utf16), g = [CGGlyph](repeating: 0, count: u.count)
    CTFontGetGlyphsForCharacters(font, &u, &g, u.count)
    var adv = CGSize.zero
    CTFontGetAdvancesForGlyphs(font, .horizontal, &g, &adv, 1)
    let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W,
                        space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    ctx.setFillColor(gray: 0, alpha: 1); ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
    ctx.setShouldAntialias(true); ctx.setAllowsFontSmoothing(false); ctx.setShouldSmoothFonts(false)
    ctx.setFillColor(gray: 1, alpha: 1)
    var pos = CGPoint(x: ox, y: oy)
    CTFontDrawGlyphs(font, &g, &pos, 1, ctx)
    let p = ctx.data!.bindMemory(to: UInt8.self, capacity: W * H)
    var x0 = W, x1 = -1, y0 = H, y1 = -1  // rows from the top
    for y in 0..<H { for x in 0..<W where p[y * W + x] > 8 { x0 = min(x0, x); x1 = max(x1, x); y0 = min(y0, y); y1 = max(y1, y) } }
    var rows: [[Int]] = []
    if x1 >= 0 { for y in y0...y1 { rows.append((x0...x1).map { Int(p[y * W + $0]) }) } }
    let baseTop = H - oy  // the baseline's row from the top
    let o: [String: Any] = ["ch": Int(ch.value), "adv": Double(adv.width), "bx": x1 >= 0 ? x0 - ox : 0,
                            "top": x1 >= 0 ? baseTop - y0 : 0, "rows": rows]
    print(String(data: try! JSONSerialization.data(withJSONObject: o), encoding: .utf8)!)
}
