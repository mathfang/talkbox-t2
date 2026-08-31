import SwiftUI

/// A generic app window. Stand-in scenery so the zoom and the travel have
/// something to act on — not a recreation of any real app's interface.
public struct MacWindow: View {
    public var title: String
    public var accent: Color
    public var lines: [CGFloat]
    public var dimmed: Bool

    public init(title: String, accent: Color = Tok.agentDim,
                lines: [CGFloat] = [1.0, 0.78, 0.9, 0.56], dimmed: Bool = false) {
        self.title = title
        self.accent = accent
        self.lines = lines
        self.dimmed = dimmed
    }

    public var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 6) {
                ForEach(0..<3, id: \.self) { i in
                    Circle()
                        .fill(Color.white.opacity(0.22))
                        .frame(width: 7, height: 7)
                        .accessibilityHidden(true)
                        .opacity(i == 0 ? 1 : 0.8)
                }
                Text(title)
                    .font(Tok.ui(10, .medium))
                    .foregroundStyle(Tok.labelDim)
                    .padding(.leading, 4)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 9)
            .frame(height: 24)
            .background(Color.white.opacity(0.06))

            VStack(alignment: .leading, spacing: 6) {
                ForEach(Array(lines.enumerated()), id: \.offset) { _, w in
                    Capsule()
                        .fill(Color.white.opacity(0.14))
                        .frame(height: 5)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .scaleEffect(x: w, y: 1, anchor: .leading)
                }
                Spacer(minLength: 0)
            }
            .padding(11)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .background(Tok.chrome)
        }
        .clipShape(RoundedRectangle(cornerRadius: Tok.tileRadius, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: Tok.tileRadius, style: .continuous)
                .stroke(accent.opacity(dimmed ? 0.30 : 0.75), lineWidth: Tok.hairline)
        )
        .shadow(color: .black.opacity(0.45), radius: 14, y: 6)
        .opacity(dimmed ? 0.55 : 1)
    }
}

/// One agent, off-screen, working. Carries its own status ring so the state
/// reads at a glance rather than from a label.
public struct AgentTile: View {
    public var agent: Agent
    public var compact: Bool

    public init(agent: Agent, compact: Bool = false) {
        self.agent = agent
        self.compact = compact
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 7) {
            MacWindow(title: agent.app,
                      accent: agent.status.tint,
                      lines: [0.9, 0.6, 0.75],
                      dimmed: agent.status == .idle)
                .frame(height: compact ? 84 : 122)

            HStack(spacing: 6) {
                Circle()
                    .fill(agent.status.tint)
                    .frame(width: 6, height: 6)
                    .shadow(color: agent.status.tint.opacity(0.9), radius: 5)
                Text(agent.task)
                    .font(Tok.mono(9))
                    .foregroundStyle(Tok.labelDim)
                    .lineLimit(1)
            }

            if let line = agent.line, !compact {
                SpeechBubble(text: line)
            }
        }
        .frame(width: compact ? 150 : 210, alignment: .leading)
    }
}

/// What an agent says. In the shipped product this is voice only — it is drawn
/// here so the prototype can be evaluated silently.
public struct SpeechBubble: View {
    public var text: String
    public init(text: String) { self.text = text }

    public var body: some View {
        Text(text)
            .font(Tok.ui(11))
            .foregroundStyle(Tok.label)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 11)
            .padding(.vertical, 8)
            .background(
                RoundedRectangle(cornerRadius: 11, style: .continuous)
                    .fill(Tok.you.opacity(0.92))
            )
            .shadow(color: .black.opacity(0.3), radius: 8, y: 3)
    }
}

/// The rectangle standing in for your physical display.
public struct DisplayMock: View {
    public var showChrome: Bool
    public init(showChrome: Bool = true) { self.showChrome = showChrome }

    public var body: some View {
        ZStack {
            Wallpaper()

            if showChrome {
                VStack(alignment: .leading, spacing: 0) {
                    HStack(spacing: 14) {
                        ForEach(["Finder", "Xcode", "Docs"], id: \.self) { t in
                            Text(t)
                                .font(Tok.ui(11, .medium))
                                .foregroundStyle(Color.white.opacity(0.75))
                        }
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 18)
                    .frame(height: 26)
                    .background(.ultraThinMaterial)

                    Spacer(minLength: 0)

                    HStack(spacing: 24) {
                        MacWindow(title: "Doc", accent: .white.opacity(0.25),
                                  lines: [1.0, 0.8, 0.92, 0.6, 0.74])
                            .frame(width: 300, height: 190)
                        MacWindow(title: "Notes", accent: .white.opacity(0.25),
                                  lines: [0.7, 0.9, 0.5])
                            .frame(width: 220, height: 150)
                    }
                    .padding(34)

                    Spacer(minLength: 0)
                }
            }
        }
    }
}
