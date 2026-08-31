import AppKit
import SwiftUI
import ClickyKit

// The ball at the notch.
//
// At rest it is a small object tucked against the notch, carrying only the
// fleet's pulse. Swipe over it (or click it, or press ⌥Space) and it grows into
// a minimap of where every agent is working relative to your display.
//
// Runs as an .accessory app: no Dock icon, no menu bar, never steals focus.
// Quit it from the terminal it was launched from (ctrl-C).
//
// HONEST LIMIT: macOS does not expose Mission Control / Exposé state to a
// normal app. Hooking the real Exposé gesture needs private API or an
// Accessibility-trusted event tap. So the triggers are hover, swipe, click and
// ⌥Space. Hover, swipe and click are watched globally and work whatever app is
// focused; only the hotkey needs focus, because a global *keyboard* tap is the
// one kind that would require Accessibility.

final class NotchModel: ObservableObject {
    enum Mode: Equatable { case idle, peek, open }

    @Published var mode: Mode = .idle
    @Published var breathing = false

    let fleet = Fleet()
    weak var window: NSWindow?
    var anchor: CGRect = .zero
    var hasRealNotch = false

    private let gestures = GestureMonitor()
    private var openCharge: CGFloat = 0
    private var closeCharge: CGFloat = 0
    private var hoverWork: DispatchWorkItem?
    private var screenObserver: Any?

    /// Tight to the notch. A generous zone here sits exactly where the cursor
    /// passes on its way to the menu bar, and the panel flickers all day.
    private var hotZone: CGRect { anchor.insetBy(dx: -24, dy: -6) }

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
            let inside = self.hotZone.contains(p)

            if inside, self.mode == .idle {
                // A short dwell so brushing past the notch does nothing.
                self.hoverWork?.cancel()
                let w = DispatchWorkItem { [weak self] in
                    guard let self, self.mode == .idle,
                          self.hotZone.contains(NSEvent.mouseLocation) else { return }
                    self.set(.peek)
                }
                self.hoverWork = w
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.18, execute: w)
            } else if !inside {
                self.hoverWork?.cancel()
                if self.mode == .peek { self.set(.idle) }
            }
        }

        gestures.onScroll = { [weak self] s in
            guard let self, !s.isMomentum else { return }

            if self.mode == .open {
                // Swiping away closes it. Without this the panel is a dead end
                // for anyone who has clicked back into their real work.
                self.closeCharge += abs(s.dy)
                if self.closeCharge > 90 { self.closeCharge = 0; self.set(.idle) }
                if s.ended { self.closeCharge = 0 }
                return
            }

            guard self.hotZone.contains(NSEvent.mouseLocation) else { return }
            self.openCharge += abs(s.dy)
            if self.openCharge > 110 { self.openCharge = 0; self.set(.open) }
            if s.ended { self.openCharge = 0 }
        }

        // Clicking anywhere outside the panel dismisses it, like any popover.
        gestures.onMouseDown = { [weak self] p in
            guard let self, self.mode == .open else { return }
            if let frame = self.window?.frame, frame.contains(p) { return }
            self.set(.idle)
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

        gestures.start(watchMouseGlobally: true,
                       watchScrollGlobally: true,
                       watchClicksGlobally: true)

        // Docking or undocking a display moves the notch out from under us.
        screenObserver = NotificationCenter.default.addObserver(
            forName: NSApplication.didChangeScreenParametersNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            guard let self, let screen = NSScreen.main ?? NSScreen.screens.first else { return }
            let (rect, real) = Notch.anchor(on: screen)
            self.anchor = rect
            self.hasRealNotch = real
            self.layout(for: self.mode)
        }

        withAnimation(.easeInOut(duration: 2.6).repeatForever(autoreverses: true)) {
            breathing = true
        }
    }

    func set(_ next: Mode) {
        guard next != mode else { return }
        // Opening is the earned moment. Closing and the hover step stay calm.
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
        Circle()
            .fill(
                RadialGradient(
                    colors: urgent
                        ? [Tok.you, Tok.you.opacity(0.45)]
                        : [Tok.agent.opacity(0.9), Tok.agent.opacity(0.28)],
                    center: .init(x: 0.35, y: 0.3),
                    startRadius: 1,
                    endRadius: 22
                )
            )
            .overlay(Circle().stroke(Color.white.opacity(0.22), lineWidth: 0.5))
            // An agent waiting on you is the one thing allowed to be loud.
            .shadow(color: (urgent ? Tok.you : Tok.agent)
                        .opacity(m.breathing ? (urgent ? 1.0 : 0.6) : 0.25),
                    radius: m.breathing ? (urgent ? 20 : 9) : 4)
            .scaleEffect(urgent && m.breathing ? 1.18 : 1.0)
            .animation(urgent ? Tok.alert : Tok.calm, value: m.breathing)
            .frame(width: 20, height: 20)
    }
}

// MARK: - The minimap

struct Minimap: View {
    @ObservedObject var m: NotchModel

    var body: some View {
        GeometryReader { geo in
            let box = geo.size
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

    private var urgent: Bool { agent.status == .needsYou }

    var body: some View {
        VStack(spacing: 5) {
            ZStack {
                if urgent {
                    Circle()
                        .stroke(Tok.you.opacity(breathing ? 0.0 : 0.75), lineWidth: 2)
                        .frame(width: breathing ? 34 : 12, height: breathing ? 34 : 12)
                }
                Circle()
                    .fill(agent.status.tint)
                    .frame(width: urgent ? 13 : 7, height: urgent ? 13 : 7)
                    .shadow(color: agent.status.tint.opacity(breathing ? 0.95 : 0.4),
                            radius: agent.status == .idle ? 0 : (urgent ? 14 : 6))
            }
            .frame(width: 34, height: 34)
            .animation(urgent ? Tok.alert : Tok.calm, value: breathing)

            Text(agent.app)
                .font(Tok.mono(8))
                .foregroundStyle(urgent ? Tok.label : Tok.labelDim)
        }
    }
}

// MARK: - Root

struct NotchView: View {
    @ObservedObject var m: NotchModel

    var body: some View {
        VStack(spacing: 0) {
            if m.mode == .open {
                panel
            } else {
                collapsed
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
        .background(Capsule().fill(Color.black.opacity(m.mode == .peek ? 0.55 : 0.0)))
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
                Text("swipe away, click out, or esc")
                    .font(Tok.mono(9))
                    .foregroundStyle(Color.white.opacity(Tok.hintIdle))
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
                .padding(.horizontal, 14)
                .padding(.vertical, 11)
                .background(Tok.you.opacity(0.18))
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

runClicky(appName: "Notch Ball", policy: .accessory) {
    guard let screen = NSScreen.main ?? NSScreen.screens.first else {
        FileHandle.standardError.write(
            Data("Notch Ball needs at least one attached display.\n".utf8))
        exit(1)
    }
    let (rect, real) = Notch.anchor(on: screen)
    model.anchor = rect
    model.hasRealNotch = real

    let size = model.size(for: .idle)
    let w = makeNotchWindow(size: size, root: NotchView(m: model))
    model.window = w
    w.setFrameOrigin(NSPoint(x: rect.midX - size.width / 2, y: rect.maxY - size.height))
    return w
}
