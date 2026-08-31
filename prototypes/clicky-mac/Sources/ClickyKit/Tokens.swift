import SwiftUI
import AppKit

/// One place for every value the three prototypes share.
/// Orange is you. Sky is an agent. That rule holds everywhere.
public enum Tok {

    // MARK: Colour

    public static let you = Color(red: 0.851, green: 0.400, blue: 0.184)     // #D9662F
    public static let agent = Color(red: 0.067, green: 0.514, blue: 0.675)   // #1183AC
    public static let agentDim = Color(red: 0.067, green: 0.514, blue: 0.675).opacity(0.45)

    /// Chrome that sits on top of the simulated desktop.
    public static let chrome = Color(white: 0.13)
    public static let chromeLine = Color(white: 1.0).opacity(0.10)
    public static let label = Color(white: 1.0).opacity(0.92)
    public static let labelDim = Color(white: 1.0).opacity(0.45)

    // MARK: Motion
    //
    // Subtle by default; the one expressive spring is reserved for an
    // earned moment (a zoom out, a panel opening). Nothing linear —
    // linear reads mechanical.

    public static let calm = Animation.spring(response: 0.42, dampingFraction: 0.86)
    public static let travel = Animation.spring(response: 0.52, dampingFraction: 0.82)
    /// Slightly under-damped. Used only where something *arrives*.
    public static let arrive = Animation.spring(response: 0.50, dampingFraction: 0.72)

    // MARK: Metric

    public static let panelRadius: CGFloat = 14
    public static let tileRadius: CGFloat = 10
    public static let hairline: CGFloat = 1

    // MARK: Type
    //
    // System font on purpose: SF is what makes a window read as native.

    public static func mono(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .system(size: size, weight: weight, design: .monospaced)
    }
    public static func ui(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .system(size: size, weight: weight)
    }
}

public func clamp(_ v: CGFloat, _ lo: CGFloat, _ hi: CGFloat) -> CGFloat {
    min(max(v, lo), hi)
}

// MARK: - Native blur

/// NSVisualEffectView. The single biggest tell between "native" and "web page
/// in a window", so every floating surface in these prototypes uses it.
public struct VisualEffect: NSViewRepresentable {
    public var material: NSVisualEffectView.Material
    public var blending: NSVisualEffectView.BlendingMode

    public init(material: NSVisualEffectView.Material = .hudWindow,
                blending: NSVisualEffectView.BlendingMode = .behindWindow) {
        self.material = material
        self.blending = blending
    }

    public func makeNSView(context: Context) -> NSVisualEffectView {
        let v = NSVisualEffectView()
        v.material = material
        v.blendingMode = blending
        v.state = .active
        return v
    }

    public func updateNSView(_ v: NSVisualEffectView, context: Context) {
        v.material = material
        v.blendingMode = blending
    }
}

// MARK: - Simulated desktop wallpaper

/// A stand-in wallpaper. Deliberately generic — not an Apple asset.
public struct Wallpaper: View {
    public init() {}
    public var body: some View {
        LinearGradient(
            colors: [
                Color(red: 0.16, green: 0.13, blue: 0.42),
                Color(red: 0.25, green: 0.18, blue: 0.68),
                Color(red: 0.13, green: 0.11, blue: 0.30)
            ],
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
        .overlay(
            RadialGradient(
                colors: [Color.white.opacity(0.16), .clear],
                center: .init(x: 0.32, y: 0.22),
                startRadius: 2,
                endRadius: 620
            )
        )
    }
}
