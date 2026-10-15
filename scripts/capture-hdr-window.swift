// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
// Capture exactly one explicitly selected window, never the whole desktop.
import Foundation
import AppKit
import ScreenCaptureKit
import CoreImage
import UniformTypeIdentifiers

@main struct CaptureHDRWindow {
    static func main() async {
        // ScreenCaptureKit's HDR screenshot path needs an initialized AppKit
        // connection even for a command-line helper launched as an app bundle.
        _ = NSApplication.shared
        do {
            var args = CommandLine.arguments
            if args.count >= 3 && args[1] == "--report" {
                guard freopen(args[2], "w", stdout) != nil, freopen(args[2], "a", stderr) != nil else {
                    throw NSError(domain: "cannot open capture report", code: 6)
                }
                args.removeSubrange(1...2)
            }
            if args.count == 2 && args[1] == "permission" {
                let allowed = CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess()
                print(allowed ? "Screen capture permission granted" : "Enable Scry HDR Capture in Screen & System Audio Recording, then reopen it")
                return
            }
            guard args.count >= 3, let pid = Int32(args[2]) else {
                throw NSError(domain: "usage: capture-hdr-window list PID | capture PID WINDOW_ID OUTPUT.scryhdr", code: 1)
            }
            // A window may move to another Space when the helper activates.
            // Ownership and the explicit window ID still restrict every capture.
            let content = try await SCShareableContent.excludingDesktopWindows(true, onScreenWindowsOnly: false)
            let own = content.windows.filter { $0.owningApplication?.processID == pid }
            if args[1] == "list" {
                for window in own { print("\(window.windowID)\t\(window.frame.width)x\(window.frame.height)\t\(window.title ?? "")") }
                return
            }
            guard args.count == 5, args[1] == "capture", let id = UInt32(args[3]),
                  let window = own.first(where: {$0.windowID == id}) else {
                throw NSError(domain: "selected window does not belong to the specified process", code: 2)
            }
            let filter = SCContentFilter(desktopIndependentWindow: window)
            let config = SCScreenshotConfiguration()
            config.dynamicRange = .bothSDRAndHDR
            config.displayIntent = .canonical
            config.showsCursor = false
            config.ignoreShadows = true
            config.includeChildWindows = false
            let result = try await SCScreenshotManager.captureScreenshot(contentFilter: filter, configuration: config)
            let hdr: CGImage
            if let image = result.hdrImage {
                hdr = image
            } else {
                // Some OpenGL windows return no image through the macOS 26
                // screenshot API. The stream-based API also supports native
                // half-float HDR; never substitute an 8-bit capture here.
                let stream = SCStreamConfiguration(preset: .captureHDRScreenshotCanonicalDisplay)
                stream.captureDynamicRange = .hdrCanonicalDisplay
                stream.pixelFormat = kCVPixelFormatType_64RGBAHalf
                stream.colorSpaceName = CGColorSpace.extendedLinearSRGB
                stream.showsCursor = false
                stream.ignoreShadowsSingleWindow = true
                stream.includeChildWindows = false
                stream.width = Int(filter.contentRect.width * CGFloat(filter.pointPixelScale))
                stream.height = Int(filter.contentRect.height * CGFloat(filter.pointPixelScale))
                hdr = try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: stream)
                guard hdr.bitsPerComponent >= 16 else {
                    throw NSError(domain: "capture did not retain HDR precision", code: 3)
                }
            }
            let color = CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!
            let context = CIContext(options: [.workingColorSpace: color, .workingFormat: CIFormat.RGBAh])
            let width = hdr.width, height = hdr.height
            let (rowBytes, rowOverflow) = width.multipliedReportingOverflow(by: 8)
            let (length, sizeOverflow) = rowBytes.multipliedReportingOverflow(by: height)
            guard !rowOverflow && !sizeOverflow && width <= Int(UInt32.max) && height <= Int(UInt32.max) else {
                throw NSError(domain: "HDR capture dimensions overflow", code: 4)
            }
            var pixels = Data(count: length)
            pixels.withUnsafeMutableBytes { buffer in
                context.render(CIImage(cgImage: hdr), toBitmap: buffer.baseAddress!, rowBytes: rowBytes,
                    bounds: CGRect(x:0,y:0,width:width,height:height), format: .RGBAh, colorSpace: color)
            }
            var data = Data("SCRYHDR1".utf8)
            for dimension in [width,height] { var value = UInt32(dimension).littleEndian; withUnsafeBytes(of:&value) { data.append(contentsOf:$0) } }
            data.append(pixels)
            try data.write(to: URL(fileURLWithPath:args[4]), options:.atomic)
            if let sdr = result.sdrImage {
                let output=URL(fileURLWithPath:args[4]).deletingPathExtension().appendingPathExtension("png")
                if let dest=CGImageDestinationCreateWithURL(output as CFURL, UTType.png.identifier as CFString, 1, nil) {
                    CGImageDestinationAddImage(dest,sdr,nil)
                    guard CGImageDestinationFinalize(dest) else { throw NSError(domain:"PNG fallback encoding failed",code:5) }
                }
            }
            print("Captured only window \(id): \(width)x\(height), source \(hdr.bitsPerComponent) bits/component, floating point \(hdr.bitmapInfo.contains(.floatComponents)), \(hdr.colorSpace?.name as String? ?? "unknown") -> linear RGBA binary16")
        } catch { fputs("HDR capture: \(error)\n",stderr); exit(1) }
    }
}
