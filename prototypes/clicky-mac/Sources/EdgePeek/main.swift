import AppKit
import SwiftUI
import ClickyKit

// Direction D — Edge Peek.
//
// No map, no panel, no zoom. Agents sit just past the bezel and bleed light
// back in at the edge nearest them. Keep pushing the trackpad toward an edge
// and you travel to that agent; let go early and it springs back — the same
// rubber-band contract as Safari's back-swipe, so the gesture is already
// learned before you arrive.

/// How far you have to push before you commit to travelling.
private let commitDistance: CGFloat = 150

/// Named Bezel rather than Edge on purpose: SwiftUI already has an `Edge`, and
/// shadowing it breaks any `.transition(.move(edge:))` added to this file later.
enum Bezel: CaseIterable {
    case left, right, top, bottom

    /// Unit vector pointing out of that bezel.
    var vector: CGSize {
        switch self {
        case .left:   return CGSize(width: -1, height: 0)
        case .right:  return CGSize(width: 1, height: 0)
        case .top:    return CGSize(width: 0, height: -1)
        case .bottom: return CGSize(width: 0, height: 1)
        }
    }

    static func nearest(for spot: CGPoint) -> Bezel {
        if abs(spot.x) >= abs(spot.y) {
            return spot.x >= 0 ? .right : .left
        }
        return spot.y >= 0 ? .bottom : .top
    }
}

final class PeekModel: ObservableObject {
    /// Live finger travel, before commit. Springs back if you let go.
    @Published var push: CGSize = .zero
    /// nil = you are home. Stored as an id, not a copy: the fleet mutates as
    /// agents work, and a snapshot would freeze the room you are standing in.
    @Published var visitingID: Int?

    var visiting: Agent? {
        guard let id = visitingID else { return nil }
        return fleet.agents.first { $0.id == id }
    }
    @Published var breathing = false

    let fleet = Fleet()
    private let gestures = GestureMonitor()
    private let wheelEnd = WheelEndDetector()

    func start() {
        gestures.onScroll = { [weak self] s in
            guard let self else { return }

            // Momentum after a commit would keep nudging the camera forever.
            if s.isMomentum { return }
            if s.began { self.push = .zero }

            let gain: CGFloat = s.precise ? 1.0 : 6.0
            // Natural scrolling: fingers left means "take me rightward".
            self.push.width -= s.dx * gain
            self.push.height -= s.dy * gain

            if s.ended {
                self.wheelEnd.cancel()
                self.release()
            } else if !s.precise {
                // A scroll wheel never sends phases, so synthesise the end.
                self.wheelEnd.bump()
            }
        }

        wheelEnd.onEnd = { [weak self] in self?.release() }

        gestures.onKeyDown = { [weak self] e in
            guard let self else { return false }
            if e.keyCode == 53 { self.goHome(); return true }   // esc
            switch e.keyCode {
            case 124: self.step(.right);  return true           // →
            case 123: self.step(.left);   return true           // ←
            case 125: self.step(.bottom); return true           // ↓
            case 126: self.step(.top);    return true           // ↑
            default: return false
            }
        }

        gestures.start()

        withAnimation(.easeInOut(duration: 2.4).repeatForever(autoreverses: true)) {
            breathing = true
        }
    }

    /// Arrow keys: travel when home, come home when visiting. Previously they
    /// silently did nothing once you had arrived somewhere.
    private func step(_ bezel: Bezel) {
        if visitingID == nil { travel(to: bezel) } else { goHome() }
    }

    /// Fingers lifted: either commit to the bezel you were pushing toward, or
    /// rubber-band home.
    private func release() {
        let far = max(abs(push.width), abs(push.height))
        guard far >= commitDistance else {
            withAnimation(Tok.calm) { push = .zero }
            return
        }

        if visitingID != nil { goHome(); return }

        let bezel: Bezel = abs(push.width) >= abs(push.height)
            ? (push.width > 0 ? .right : .left)
            : (push.height > 0 ? .bottom : .top)
        travel(to: bezel)
    }

    func travel(to bezel: Bezel) {
        guard let target = agent(on: bezel) else {
            // Nothing out that way. Bounce rather than silently ignoring —
            // the absence of an agent is information too.
            withAnimation(Tok.calm) { push = .zero }
            return
        }
        withAnimation(Tok.travel) {
            visitingID = target.id
            push = .zero
        }
    }

    func goHome() {
        withAnimation(Tok.travel) {
            visitingID = nil
            push = .zero
        }
    }

    func agent(on bezel: Bezel) -> Agent? {
        fleet.agents.first { Bezel.nearest(for: $0.spot) == bezel }
    }

    /// 0...1 — how close the current push is to committing.
    func pressure(on bezel: Bezel) -> CGFloat {
        let along: CGFloat
        switch bezel {
        case .left:   along = -push.width
        case .right:  along = push.width
        case .top:    along = -push.height
        case .bottom: along = push.height
        }
        return clamp(along / commitDistance, 0, 1)
    }
}

// MARK: - Views

struct BezelGlow: View {
    var bezel: Bezel
    var agent: Agent
    var pressure: CGFloat
    var breathing: Bool

    private var urgent: Bool { agent.status == .needsYou }

    /// An agent waiting on you gets real area, not a slightly fatter hairline.
    /// This is the one loud thing on screen.
    private var thickness: CGFloat {
        let base: CGFloat = urgent ? 96 : (agent.status == .idle ? 12 : 20)
        let breath: CGFloat = breathing ? (urgent ? 34 : 6) : 0
        return base + breath + pressure * 110
    }

    private var strength: Double {
        switch agent.status {
        case .needsYou: return breathing ? 0.95 : 0.62
        case .working:  return 0.60
        case .idle:     return 0.26
        }
    }

    var body: some View {
        GeometryReader { geo in
            let horizontal = bezel == .left || bezel == .right
            LinearGradient(
                colors: [agent.status.tint.opacity(strength), agent.status.tint.opacity(0)],
                startPoint: startPoint,
                endPoint: endPoint
            )
            .frame(width: horizontal ? thickness : geo.size.width,
                   height: horizontal ? geo.size.height : thickness)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: alignment)
            .overlay(alignment: alignment) {
                Rectangle()
                    .fill(agent.status.tint)
                    .frame(width: horizontal ? (urgent ? 4 : 2) : nil,
                           height: horizontal ? nil : (urgent ? 4 : 2))
                    .opacity(0.5 + pressure * 0.5)
            }
        }
        .animation(urgent ? Tok.alert : Tok.calm, value: breathing)
        .allowsHitTesting(false)
    }

    private var alignment: Alignment {
        switch bezel {
        case .left: return .leading
        case .right: return .trailing
        case .top: return .top
        case .bottom: return .bottom
        }
    }
    private var startPoint: UnitPoint {
        switch bezel {
        case .left: return .leading
        case .right: return .trailing
        case .top: return .top
        case .bottom: return .bottom
        }
    }
    private var endPoint: UnitPoint {
        switch bezel {
        case .left: return .trailing
        case .right: return .leading
        case .top: return .bottom
        case .bottom: return .top
        }
    }
}

struct AgentRoom: View {
    var agent: Agent

    var body: some View {
        ZStack {
            Color.black
            VStack(spacing: 18) {
                MacWindow(title: agent.app,
                          accent: agent.status.tint,
                          lines: [1.0, 0.82, 0.94, 0.6, 0.7])
                    .frame(width: 560, height: 340)

                HStack(spacing: 8) {
                    Circle().fill(agent.status.tint).frame(width: 7, height: 7)
                    Text("\(agent.app) — \(agent.task)")
                        .font(Tok.mono(11))
                        .foregroundStyle(Tok.labelDim)
                }

                if let line = agent.line {
                    SpeechBubble(text: line).frame(maxWidth: 380)
                }
            }
        }
    }
}

struct EdgePeekView: View {
    @StateObject private var m = PeekModel()

    var body: some View {
        GeometryReader { geo in
            let size = geo.size
            ZStack {
                Color.black.ignoresSafeArea()

                ZStack {
                    DisplayMock()
                        .frame(width: size.width, height: size.height)
                        .overlay(bezelGlows)
                        .position(x: size.width / 2, y: size.height / 2)

                    ForEach(m.fleet.agents) { a in
                        let b = Bezel.nearest(for: a.spot)
                        AgentRoom(agent: a)
                            .frame(width: size.width, height: size.height)
                            .position(
                                x: size.width / 2 + b.vector.width * (size.width + 40),
                                y: size.height / 2 + b.vector.height * (size.height + 40)
                            )
                    }
                }
                .offset(x: -contentOffset(size).width, y: -contentOffset(size).height)

                footer
            }
            .clipped()
        }
        .ignoresSafeArea()
        .onAppear {
            m.start()
            m.fleet.startLiving()
        }
    }

    /// Where the camera is: committed travel plus live finger drag.
    private func contentOffset(_ size: CGSize) -> CGSize {
        var base = CGSize.zero
        if let v = m.visiting {
            let b = Bezel.nearest(for: v.spot)
            base = CGSize(width: b.vector.width * (size.width + 40),
                          height: b.vector.height * (size.height + 40))
        }
        return CGSize(width: base.width + m.push.width,
                      height: base.height + m.push.height)
    }

    private var bezelGlows: some View {
        ZStack {
            ForEach(m.fleet.agents) { a in
                let b = Bezel.nearest(for: a.spot)
                BezelGlow(bezel: b, agent: a,
                          pressure: m.pressure(on: b),
                          breathing: m.breathing)
            }
        }
        .opacity(m.visitingID == nil ? 1 : 0)
        .animation(Tok.calm, value: m.visitingID)
    }

    private var footer: some View {
        VStack {
            Spacer()
            Text(m.visitingID == nil
                 ? "two-finger push toward an edge — keep pushing to go there"
                 : "push back, or press esc, to come home")
                .font(Tok.mono(11))
                .foregroundStyle(Color.white.opacity(m.visitingID == nil ? Tok.hintIdle : Tok.hintActive))
                .padding(.bottom, 18)
        }
        .allowsHitTesting(false)
    }
}

runClicky(appName: "Edge Peek") {
    makeMainWindow(title: "Edge Peek",
                   size: CGSize(width: 1280, height: 820),
                   root: EdgePeekView())
}
