import SwiftUI

/// Clicky's ball: liquid glass with extra shadows.
///
/// It has no colour of its own. Everything you see in it is whatever is behind
/// it, blurred and tinted by the glass — which is why it looked cream with
/// orange and cyan marks over the Figma canvas in the `agent` frame. Put it
/// over a blue wallpaper and it goes blue.
///
/// Built from a real `NSVisualEffectView` backdrop rather than a painted
/// gradient, so it refracts the actual desktop underneath. On macOS 26 the
/// native `.glassEffect()` would replace the hand-rolled rim and specular
/// below; this targets macOS 13, so it is assembled by hand.
public struct ClickyOrb: View {
    public var size: CGFloat
    public var urgent: Bool
    public var breathing: Bool

    public init(size: CGFloat = 62, urgent: Bool = false, breathing: Bool = false) {
        self.size = size
        self.urgent = urgent
        self.breathing = breathing
    }

    public var body: some View {
        glass
            // Extra shadows, stacked: a tight contact shadow, a mid body
            // shadow, and a wide ambient one. Layering is what stops glass
            // reading as a flat translucent disc.
            .shadow(color: .black.opacity(0.30), radius: size * 0.06, y: size * 0.02)
            .shadow(color: .black.opacity(0.22), radius: size * 0.20, y: size * 0.09)
            .shadow(color: urgent ? Tok.you.opacity(breathing ? 0.75 : 0.30)
                                  : .black.opacity(0.16),
                    radius: urgent ? size * (breathing ? 0.55 : 0.28) : size * 0.44,
                    y: urgent ? 0 : size * 0.18)
            .scaleEffect(urgent && breathing ? 1.05 : 1.0)
            .animation(urgent ? Tok.alert : Tok.calm, value: breathing)
            .frame(width: size, height: size)
    }

    private var glass: some View {
        ZStack {
            // The refraction: real backdrop blur, not a painted fill.
            VisualEffect(material: .hudWindow, blending: .behindWindow)

            // Glass is never perfectly clear — a faint body tint, brighter at
            // the top where it catches the sky.
            LinearGradient(
                colors: [Color.white.opacity(0.28), Color.white.opacity(0.06)],
                startPoint: .top,
                endPoint: .bottom
            )

            // Light bouncing back up through the underside.
            RadialGradient(
                colors: [Color.white.opacity(0.22), .clear],
                center: UnitPoint(x: 0.5, y: 0.92),
                startRadius: 0,
                endRadius: size * 0.55
            )

            // Specular: a small hard catchlight, and a long soft one.
            Ellipse()
                .fill(Color.white.opacity(0.85))
                .frame(width: size * 0.20, height: size * 0.13)
                .rotationEffect(.degrees(-28))
                .offset(x: -size * 0.17, y: -size * 0.24)
                .blur(radius: size * 0.03)

            Ellipse()
                .fill(Color.white.opacity(0.30))
                .frame(width: size * 0.46, height: size * 0.20)
                .rotationEffect(.degrees(-24))
                .offset(x: -size * 0.06, y: -size * 0.30)
                .blur(radius: size * 0.09)
        }
        .clipShape(Circle())
        // The rim is the whole trick: bright where the light is, nearly gone
        // on the opposite side, with a thin inner line to give the glass edge
        // some thickness.
        .overlay(
            Circle().stroke(
                LinearGradient(
                    colors: [Color.white.opacity(0.95),
                             Color.white.opacity(0.22),
                             Color.white.opacity(0.55)],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                ),
                lineWidth: max(0.8, size * 0.018)
            )
        )
        .overlay(
            Circle()
                .stroke(Color.white.opacity(0.30), lineWidth: max(0.5, size * 0.008))
                .padding(max(1.2, size * 0.030))
                .blendMode(.plusLighter)
        )
    }
}
