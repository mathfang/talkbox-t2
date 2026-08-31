import AppKit
import SwiftUI
import ClickyKit

// Direction A — Zoom-Out Canvas.
//
// Your display is one tile on a larger desktop. Pinch out and the agents are
// simply there. At scale 1 there is no Clicky chrome at all: the point of the
// direction is that nothing was ever summoned.

private let displaySize = CGSize(width: 1040, height: 650)
private let canvasSize = CGSize(width: displaySize.width * 3.4,
                                height: displaySize.height * 3.2)
private let minScale: CGFloat = 0.32
private let maxScale: CGFloat = 1.0
/// The scale at which every agent is comfortably on screen.
private let overviewScale: CGFloat = 0.40

final class CanvasModel: ObservableObject {
    @Published var scale: CGFloat = maxScale
    @Published var pan: CGSize = .zero

    let fleet = Fleet()
    private let gestures = GestureMonitor()
    /// Where the pan was when the pinch started, so the camera can return to
    /// centre continuously instead of snapping when the fingers lift.
    private var panAnchor: CGSize = .zero

    var zoomedOut: Bool { scale < 0.92 }

    func start() {
        gestures.onMagnify = { [weak self] mag, phase in
            guard let self else { return }
            if phase == .began { self.panAnchor = self.pan }
            // Direct, unanimated: a pinch should track the fingers exactly.
            self.scale = clamp(self.scale * (1 + mag), minScale, maxScale)

            // Ease the pan home as the scale approaches 1, so arriving back at
            // your own display is one continuous motion.
            let t = clamp((self.scale - overviewScale) / (maxScale - overviewScale), 0, 1)
            self.pan = CGSize(width: self.panAnchor.width * (1 - t),
                              height: self.panAnchor.height * (1 - t))

            if phase == .ended || phase == .cancelled { self.settle() }
        }

        gestures.onScroll = { [weak self] s in
            guard let self, self.zoomedOut, !s.isMomentum else { return }
            let gain: CGFloat = s.precise ? 1.0 : 6.0
            self.pan.width += s.dx * gain
            self.pan.height += s.dy * gain
            self.clampPan()
        }

        // Two-finger double tap — the macOS "smart zoom" gesture.
        gestures.onSmartMagnify = { [weak self] in
            self?.toggle()
        }

        gestures.onKeyDown = { [weak self] e in
            guard let self else { return false }
            switch e.charactersIgnoringModifiers {
            case "-", "_":
                withAnimation(Tok.travel) { self.scale = overviewScale; self.clampPan() }
                return true
            case "=", "+":
                withAnimation(Tok.travel) { self.scale = maxScale; self.pan = .zero }
                return true
            case " ":
                self.toggle()
                return true
            default:
                if e.keyCode == 53 {   // esc
                    withAnimation(Tok.travel) { self.scale = maxScale; self.pan = .zero }
                    return true
                }
                return false
            }
        }

        gestures.start()
    }

    func toggle() {
        withAnimation(Tok.arrive) {
            if zoomedOut {
                scale = maxScale
                pan = .zero
            } else {
                scale = overviewScale
                clampPan()
            }
        }
    }

    /// Snap home if the pinch ended near 1 — otherwise you get stuck at 0.97
    /// with a hairline of agent showing, which reads as a bug.
    private func settle() {
        if scale > 0.92 {
            withAnimation(Tok.calm) { scale = maxScale; pan = .zero }
        }
    }

    func flyTo(_ agent: Agent) {
        withAnimation(Tok.travel) {
            scale = 0.78
            pan = CGSize(width: -agent.spot.x * displaySize.width * 0.78,
                         height: -agent.spot.y * displaySize.height * 0.78)
        }
    }

    /// Far enough to reach any agent, never far enough to lose your own
    /// display off the side of the window.
    private func clampPan() {
        let limit = CGSize(width: displaySize.width * scale * 1.5,
                           height: displaySize.height * scale * 1.5)
        pan.width = clamp(pan.width, -limit.width, limit.width)
        pan.height = clamp(pan.height, -limit.height, limit.height)
    }
}

struct ZoomCanvasView: View {
    @StateObject private var m = CanvasModel()

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            canvas
                .frame(width: canvasSize.width, height: canvasSize.height)
                .scaleEffect(m.scale)
                .offset(m.pan)

            edgeHints
            overlay
        }
        .ignoresSafeArea()
        .onAppear {
            m.start()
            m.fleet.startLiving()
        }
    }

    private var canvas: some View {
        ZStack {
            // Everything is placed relative to the centre of the canvas, which
            // is the centre of your real display.
            ForEach(m.fleet.agents) { a in
                AgentTile(agent: a)
                    .opacity(m.zoomedOut ? 1 : 0)
                    .animation(Tok.calm, value: m.zoomedOut)
                    .position(x: canvasSize.width / 2 + a.spot.x * displaySize.width,
                              y: canvasSize.height / 2 + a.spot.y * displaySize.height)
                    .allowsHitTesting(m.zoomedOut)
                    .onTapGesture { m.flyTo(a) }
            }

            DisplayMock()
                .frame(width: displaySize.width, height: displaySize.height)
                .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
                .overlay(
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(Tok.you.opacity(m.zoomedOut ? 0.9 : 0), lineWidth: 2)
                )
                .shadow(color: .black.opacity(m.zoomedOut ? 0.6 : 0), radius: 40, y: 18)
                .position(x: canvasSize.width / 2, y: canvasSize.height / 2)
        }
    }

    /// At 1:1 the agents are off-canvas and therefore invisible, which makes
    /// the app look empty and the gesture undiscoverable. These are the only
    /// thing that says "there is something out there" — light leaking in from
    /// the direction each agent is working, brightest for one that needs you.
    private var edgeHints: some View {
        ZStack {
            ForEach(m.fleet.agents) { a in
                hintBar(a)
            }
        }
        .opacity(m.zoomedOut ? 0 : 1)
        .animation(Tok.calm, value: m.zoomedOut)
        .allowsHitTesting(false)
        .ignoresSafeArea()
    }

    private func hintBar(_ a: Agent) -> some View {
        let horizontal = abs(a.spot.x) >= abs(a.spot.y)
        let urgent = a.status == .needsYou
        let align: Alignment = horizontal ? (a.spot.x >= 0 ? .trailing : .leading)
                                          : (a.spot.y >= 0 ? .bottom : .top)
        let from: UnitPoint = horizontal ? (a.spot.x >= 0 ? .trailing : .leading)
                                         : (a.spot.y >= 0 ? .bottom : .top)
        let to: UnitPoint = horizontal ? (a.spot.x >= 0 ? .leading : .trailing)
                                       : (a.spot.y >= 0 ? .top : .bottom)
        let tint = a.status.tint
        let thickness: CGFloat = urgent ? 54 : (a.status == .idle ? 10 : 20)

        return LinearGradient(
            colors: [tint.opacity(urgent ? 0.8 : 0.42), tint.opacity(0)],
            startPoint: from,
            endPoint: to
        )
        .frame(width: horizontal ? thickness : nil,
               height: horizontal ? nil : thickness)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: align)
    }

    /// Deliberately almost nothing at scale 1 — ambient, not summoned.
    private var overlay: some View {
        VStack {
            Spacer()
            HStack {
                Text(m.zoomedOut
                     ? "spread fingers, or press esc, to come back"
                     : "\(m.fleet.agents.count) agents working past the edges  ·  squeeze two fingers to step back  ·  −")
                    .font(Tok.mono(11))
                    .foregroundStyle(Color.white.opacity(m.zoomedOut ? Tok.hintActive : Tok.hintIdle))
                Spacer()
                if m.fleet.needsYou > 0 && !m.zoomedOut {
                    HStack(spacing: 6) {
                        Circle().fill(Tok.you).frame(width: 6, height: 6)
                        Text("\(m.fleet.needsYou) needs you")
                            .font(Tok.mono(11))
                            .foregroundStyle(Color.white.opacity(Tok.hintActive))
                    }
                }
            }
            .padding(.horizontal, 22)
            .padding(.bottom, 16)
        }
        .animation(Tok.calm, value: m.zoomedOut)
    }
}

runClicky(appName: "Zoom Canvas") {
    makeMainWindow(title: "Zoom Canvas",
                   size: CGSize(width: 1280, height: 820),
                   root: ZoomCanvasView())
}
