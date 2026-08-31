import AppKit
import SwiftUI
import ClickyKit

// The ball at the notch.
//
// At rest it is a small object tucked against the notch, carrying only the
// fleet's pulse. Swipe down over it (or press ⌥Space) and it grows into a
// minimap of where every agent is working relative to your display.
//
// HONEST LIMIT: macOS does not expose Mission Control / Exposé state to a
// normal app. Hooking the real Exposé gesture needs private API or an
// Accessibility-trusted event tap. So the triggers here are: hover the notch,
// swipe over it, click it, or ⌥Space (⌥Space only while this app is focused —
// keyboard monitors are the one kind that would need Accessibility). Hover and
// swipe work globally. Everything else about the interaction is real.

final class NotchModel: ObservableObject {
    enum Mode: Equatable { case idle, peek, open }

    @Published var mode: Mode = .idle
    @Published var breathing = false

    let fleet = Fleet()
    weak var window: NSWindow?
    var anchor: CGRect = .zero
    var hasRealNotch = false

    private let gestures = GestureMonitor()
    private var swipeCharge: CGFloat = 0

    /// Window size per mode. The window is resized rather than kept large and
    /// transparent, so a collapsed ball never eats clicks meant for your desktop.
    func size(for mode: Mode) -> CGSize {
        switch mode {
        case .idle: return CGSize(width: max(anchor.width, 160) + 80, height: anchor.height + 26)
        case .peek: return CGSize(width: max(anchor.width, 160) + 190, height: anchor.height + 44)
        case .open: return CGSize(width: 560, height: 392)
        }
    }

    func start() {
        gestures.onMouseMoved = { [weak self] p in
            guard let self else { return }
            let hot = self.anchor.insetBy(dx: -70, dy: -10)
            let inside = hot.contains(p)
            if inside, self.mode == .idle { self.set(.peek) }
            else if !inside, self.mode == .peek { self.set(.idle) }
        }

        gestures.onScroll = { [weak self] _, dy, phase, _ in
            guard let self, self.mode != .open else { return }
            guard self.anchor.insetBy(dx: -70, dy: -10).contains(NSEvent.mouseLocation) else { return }
            self.swipeCharge += abs(dy)
            if self.swipeCharge > 40 { self.swipeCharge = 0; self.set(.open) }
            if phase == .ended { self.swipeCharge = 0 }
        }

        gestures.onKeyDown = { [weak self] e in
            guard let self else { return false }
            if e.keyCode == 53, self.mode == .open { self.set(.idle); return true }
            if e.charactersIgnoringModifiers == " ", e.modifierFlags.contains(.option) {
                self.set(self.mode == .open ? .idle : .open)
                return true
            }
            return false
        }

        gestures.start(watchMouseGlobally: true, watchScrollGlobally: true)

        withAnimation(.easeInOut(duration: 2.6).repeatForever(autoreverses: true)) {
            breathing = true
        }
    }

    func set(_ next: Mode) {
        guard next != mode else { return }
        // Opening is the earned moment, so it gets the springy curve; closing
        // and the small hover step stay calm.
        withAnimation(next == .open ? Tok.arrive : Tok.calm) { mode = next }
        layout(for: next)
    }

    func toggleOpen() { set(mode == .open ? .idle : .open) }

    /// Keep the window's top edge pinned to the notch, whatever the size.
    func layout(for mode: Mode) {
        guard let w = window else { return }
        let s = size(for: mode)
        let origin = CGPoint(x: anchor.midX - s.width / 2, y: anchor.maxY - s.height)
        w.setFrame(NSRect(origin: origin, size: s), display: true, animate: false)
    }
}

// MARK: - The ball

struct Ball: View {
    @ObservedObject var m: NotchModel

    private var urgent: Bool { m.fleet.needsYou > 0 }

    var body: some View {
        ZStack {
            Circle()
                .fill(
                    RadialGradient(
                        colors: urgent
                            ? [Tok.you.opacity(0.95), Tok.you.opacity(0.35)]
                            : [Tok.agent.opacity(0.9), Tok.agent.opacity(0.28)],
                        center: .init(x: 0.35, y: 0.3),
                        startRadius: 1,
                        endRadius: 22
                    )
                )
                .overlay(Circle().stroke(Color.white.opacity(0.22), lineWidth: 0.5))
                .shadow(color: (urgent ? Tok.you : Tok.agent).opacity(m.breathing ? 0.75 : 0.3),
                        radius: m.breathing ? 11 : 5)
        }
        .frame(width: 20, height: 20)
    }
}

// MARK: - The minimap

struct Minimap: View {
    @ObservedObject var m: NotchModel

    var body: some View {
        GeometryReader { geo in
            let box = geo.size
            // Your display, drawn small and centred; agents sit around it in
            // the same canvas coordinates every other prototype uses.
            let dw = box.width * 0.30
            let dh = dw * 0.62

            ZStack {
                RoundedRectangle(cornerRadius: 4, style: .continuous)
                    .fill(Tok.you.opacity(0.14))
                    .overlay(
                        RoundedRectangle(cornerRadius: 4, style: .continuous)
                            .stroke(Tok.you, lineWidth: 1.5)
                    )
                    .frame(width: dw, height: dh)
                    .position(x: box.width / 2, y: box.height / 2)

                ForEach(m.fleet.agents) { a in
                    AgentDot(agent: a, breathing: m.breathing)
                        .position(x: box.width / 2 + a.spot.x * dw * 0.92,
                                  y: box.height / 2 + a.spot.y * dh * 1.05)
                }
            }
        }
    }
}

struct AgentDot: View {
    var agent: Agent
    var breathing: Bool

    var body: some View {
        VStack(spacing: 4) {
            Circle()
                .fill(agent.status.tint)
                .frame(width: agent.status == .needsYou ? 10 : 7,
                       height: agent.status == .needsYou ? 10 : 7)
                .shadow(color: agent.status.tint.opacity(breathing ? 0.9 : 0.4),
                        radius: agent.status == .idle ? 0 : 6)
            Text(agent.app)
                .font(Tok.mono(8))
                .foregroundStyle(Tok.labelDim)
        }
    }
}

// MARK: - Root

struct NotchView: View {
    @ObservedObject var m: NotchModel

    var body: some View {
        VStack(spacing: 0) {
            switch m.mode {
            case .idle:
                collapsed
            case .peek:
                collapsed
            case .open:
                panel
            }
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .onAppear { m.start() }
    }

    /// Hugs the notch: a bar the same height as the notch with the ball beside
    /// it, so on a notched Mac it reads as part of the hardware.
    private var collapsed: some View {
        HStack(spacing: 9) {
            Ball(m: m)
            if m.mode == .peek {
                Text(m.fleet.needsYou > 0
                     ? "\(m.fleet.needsYou) needs you"
                     : "\(m.fleet.working) working")
                    .font(Tok.mono(10))
                    .foregroundStyle(Tok.label)
                    .transition(.opacity.combined(with: .move(edge: .leading)))
            }
        }
        .padding(.horizontal, 12)
        .frame(height: max(m.anchor.height, 30))
        .background(
            Capsule().fill(Color.black.opacity(m.mode == .peek ? 0.55 : 0.0))
        )
        .contentShape(Capsule())
        .onTapGesture { m.toggleOpen() }
        .padding(.top, m.hasRealNotch ? 0 : 2)
    }

    private var panel: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Ball(m: m)
                Text("agents")
                    .font(Tok.ui(12, .semibold))
                    .foregroundStyle(Tok.label)
                Spacer()
                Text("swipe up or esc")
                    .font(Tok.mono(9))
                    .foregroundStyle(Tok.labelDim)
            }
            .padding(.horizontal, 16)
            .padding(.top, 14)
            .padding(.bottom, 10)

            Minimap(m: m)
                .frame(height: 210)
                .padding(.horizontal, 14)

            if let stuck = m.fleet.agents.first(where: { $0.status == .needsYou }),
               let line = stuck.line {
                HStack(spacing: 9) {
                    Circle().fill(Tok.you).frame(width: 7, height: 7)
                    Text(line)
                        .font(Tok.ui(11))
                        .foregroundStyle(Tok.label)
                        .lineLimit(2)
                    Spacer(minLength: 0)
                }
                .padding(.horizontal, 16)
                .padding(.top, 12)
                .padding(.bottom, 14)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(VisualEffect(material: .hudWindow))
        .clipShape(RoundedRectangle(cornerRadius: Tok.panelRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: Tok.panelRadius, style: .continuous)
                .stroke(Color.white.opacity(0.12), lineWidth: 0.5)
        )
        .shadow(color: .black.opacity(0.5), radius: 26, y: 12)
        .transition(.scale(scale: 0.94, anchor: .top).combined(with: .opacity))
    }
}

// MARK: - Boot

let model = NotchModel()

runClicky(appName: "Notch Ball") {
    let screen = NSScreen.main ?? NSScreen.screens[0]
    let (rect, real) = Notch.anchor(on: screen)
    model.anchor = rect
    model.hasRealNotch = real

    let size = model.size(for: .idle)
    let w = makeNotchWindow(size: size, root: NotchView(m: model))
    model.window = w
    w.setFrameOrigin(NSPoint(x: rect.midX - size.width / 2, y: rect.maxY - size.height))
    return w
}
