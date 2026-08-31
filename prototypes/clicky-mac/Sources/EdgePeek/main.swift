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

enum Edge {
    case left, right, top, bottom

    /// Unit vector pointing out of that edge.
    var vector: CGSize {
        switch self {
        case .left:   return CGSize(width: -1, height: 0)
        case .right:  return CGSize(width: 1, height: 0)
        case .top:    return CGSize(width: 0, height: -1)
        case .bottom: return CGSize(width: 0, height: 1)
        }
    }

    static func nearest(for spot: CGPoint) -> Edge {
        if abs(spot.x) >= abs(spot.y) {
            return spot.x >= 0 ? .right : .left
        }
        return spot.y >= 0 ? .bottom : .top
    }
}

final class PeekModel: ObservableObject {
    /// Live finger travel, before commit. Springs back if you let go.
    @Published var push: CGSize = .zero
    /// nil = you are home.
    @Published var visiting: Agent?
    @Published var breathing = false

    let fleet = Fleet()
    private let gestures = GestureMonitor()

    func start() {
        gestures.onScroll = { [weak self] dx, dy, phase, precise in
            guard let self else { return }
            let gain: CGFloat = precise ? 1.0 : 6.0

            // Natural scrolling: fingers left means "take me rightward".
            self.push.width -= dx * gain
            self.push.height -= dy * gain

            if phase == .ended || phase == .cancelled {
                self.release()
            }
        }

        gestures.onKeyDown = { [weak self] e in
            guard let self else { return false }
            if e.keyCode == 53 {                       // esc
                self.goHome()
                return true
            }
            guard self.visiting == nil else { return false }
            switch e.keyCode {
            case 124: self.travel(to: .right); return true   // →
            case 123: self.travel(to: .left);  return true   // ←
            case 125: self.travel(to: .bottom); return true  // ↓
            case 126: self.travel(to: .top);   return true   // ↑
            default: return false
            }
        }

        gestures.start()

        withAnimation(.easeInOut(duration: 2.4).repeatForever(autoreverses: true)) {
            breathing = true
        }
    }

    /// Fingers lifted: either commit to the edge you were pushing toward, or
    /// rubber-band home.
    private func release() {
        let far = max(abs(push.width), abs(push.height))
        guard far >= commitDistance else {
            withAnimation(Tok.calm) { push = .zero }
            return
        }

        if visiting != nil {
            goHome()
            return
        }

        let edge: Edge = abs(push.width) >= abs(push.height)
            ? (push.width > 0 ? .right : .left)
            : (push.height > 0 ? .bottom : .top)
        travel(to: edge)
    }

    func travel(to edge: Edge) {
        guard let target = agent(on: edge) else {
            // Nothing out that way. Bounce rather than silently ignoring —
            // the absence of an agent is information too.
            withAnimation(Tok.calm) { push = .zero }
            return
        }
        withAnimation(Tok.travel) {
            visiting = target
            push = .zero
        }
    }

    func goHome() {
        withAnimation(Tok.travel) {
            visiting = nil
            push = .zero
        }
    }

    func agent(on edge: Edge) -> Agent? {
        fleet.agents.first { Edge.nearest(for: $0.spot) == edge }
    }

    /// 0...1 — how close the current push is to committing.
    func pressure(on edge: Edge) -> CGFloat {
        let along: CGFloat
        switch edge {
        case .left:   along = -push.width
        case .right:  along = push.width
        case .top:    along = -push.height
        case .bottom: along = push.height
        }
        return clamp(along / commitDistance, 0, 1)
    }
}

// MARK: - Views

struct EdgeGlow: View {
    var edge: Edge
    var agent: Agent
    var pressure: CGFloat
    var breathing: Bool

    private var thickness: CGFloat {
        let base: CGFloat = agent.status == .needsYou ? 30 : 18
        let breath: CGFloat = breathing && agent.status != .idle ? 8 : 0
        return base + breath + pressure * 90
    }

    private var tint: Color { agent.status.tint }

    var body: some View {
        GeometryReader { geo in
            let horizontal = edge == .left || edge == .right
            LinearGradient(
                colors: [tint.opacity(agent.status == .idle ? 0.30 : 0.85), tint.opacity(0)],
                startPoint: startPoint,
                endPoint: endPoint
            )
            .frame(width: horizontal ? thickness : geo.size.width,
                   height: horizontal ? geo.size.height : thickness)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: alignment)
            .overlay(alignment: alignment) {
                Rectangle()
                    .fill(tint)
                    .frame(width: horizontal ? 2 : nil, height: horizontal ? nil : 2)
                    .opacity(0.5 + pressure * 0.5)
            }
        }
        .allowsHitTesting(false)
    }

    private var alignment: Alignment {
        switch edge {
        case .left: return .leading
        case .right: return .trailing
        case .top: return .top
        case .bottom: return .bottom
        }
    }
    private var startPoint: UnitPoint {
        switch edge {
        case .left: return .leading
        case .right: return .trailing
        case .top: return .top
        case .bottom: return .bottom
        }
    }
    private var endPoint: UnitPoint {
        switch edge {
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
                        .overlay(edgeGlows)
                        .position(x: size.width / 2, y: size.height / 2)

                    ForEach(m.fleet.agents) { a in
                        let e = Edge.nearest(for: a.spot)
                        AgentRoom(agent: a)
                            .frame(width: size.width, height: size.height)
                            .position(
                                x: size.width / 2 + e.vector.width * (size.width + 40),
                                y: size.height / 2 + e.vector.height * (size.height + 40)
                            )
                    }
                }
                .offset(x: -contentOffset(size).width, y: -contentOffset(size).height)

                footer
            }
            .clipped()
        }
        .ignoresSafeArea()
        .onAppear { m.start() }
    }

    /// Where the camera is: committed travel plus live finger drag.
    private func contentOffset(_ size: CGSize) -> CGSize {
        var base = CGSize.zero
        if let v = m.visiting {
            let e = Edge.nearest(for: v.spot)
            base = CGSize(width: e.vector.width * (size.width + 40),
                          height: e.vector.height * (size.height + 40))
        }
        return CGSize(width: base.width + m.push.width,
                      height: base.height + m.push.height)
    }

    private var edgeGlows: some View {
        ZStack {
            ForEach(m.fleet.agents) { a in
                let e = Edge.nearest(for: a.spot)
                EdgeGlow(edge: e, agent: a,
                         pressure: m.pressure(on: e),
                         breathing: m.breathing)
            }
        }
        .opacity(m.visiting == nil ? 1 : 0)
        .animation(Tok.calm, value: m.visiting)
    }

    private var footer: some View {
        VStack {
            Spacer()
            Text(m.visiting == nil
                 ? "push past an edge to go to an agent"
                 : "push back, or esc, to come home")
                .font(Tok.mono(10))
                .foregroundStyle(Color.white.opacity(0.35))
                .padding(.bottom, 16)
        }
        .allowsHitTesting(false)
    }
}

runClicky(appName: "Edge Peek") {
    makeMainWindow(title: "Edge Peek",
                   size: CGSize(width: 1280, height: 820),
                   root: EdgePeekView())
}
