import SwiftUI
import Foundation

public enum AgentStatus: Equatable {
    case working
    case needsYou
    case idle

    public var tint: Color {
        switch self {
        case .working:  return Tok.agent
        case .needsYou: return Tok.you
        case .idle:     return Tok.agentDim
        }
    }

    public var word: String {
        switch self {
        case .working:  return "working"
        case .needsYou: return "needs you"
        case .idle:     return "idle"
        }
    }
}

public struct Agent: Identifiable, Equatable {
    public let id: Int
    public let app: String
    public let task: String
    public var status: AgentStatus
    /// Position in canvas space. (0,0) is the centre of your real display;
    /// units are display-widths, so (1.2, 0) sits just off the right bezel.
    public var spot: CGPoint
    /// Only ever set when status == .needsYou. Spoken aloud in the real
    /// product — shown here so the prototype is legible without audio.
    public var line: String?

    // Liveness. A static tile makes the whole direction look dead, so each
    // agent carries enough state to visibly be working.

    /// 0...1, how far through its current task it is.
    public var progress: Double = 0.2
    /// Its cursor, normalised inside its own window.
    public var cursor: CGPoint = CGPoint(x: 0.3, y: 0.45)
    /// Bumps when the agent writes another line.
    public var written: Int = 2

    public init(id: Int, app: String, task: String, status: AgentStatus,
                spot: CGPoint, line: String? = nil) {
        self.id = id
        self.app = app
        self.task = task
        self.status = status
        self.spot = spot
        self.line = line
    }
}

public enum Mock {
    /// One agent per compass direction, deliberately. Edge-based navigation
    /// buckets by dominant axis, so two agents sharing a dominant axis would
    /// make one of them unreachable and stack their rooms on top of each other.
    public static let agents: [Agent] = [
        Agent(id: 1, app: "Xcode", task: "fixing the build",
              status: .working, spot: CGPoint(x: 1.30, y: -0.28)),
        Agent(id: 2, app: "Gmail", task: "drafting the reply",
              status: .needsYou, spot: CGPoint(x: 0.34, y: 1.20),
              line: "stuck — gmail isn't connected. can you connect it?"),
        Agent(id: 3, app: "Sheets", task: "reconciling the numbers",
              status: .working, spot: CGPoint(x: -1.28, y: 0.30)),
        Agent(id: 4, app: "Figma", task: "waiting on you",
              status: .idle, spot: CGPoint(x: -0.30, y: -1.18)),
    ]
}

/// Shared, observable so all three prototypes tell the same story.
public final class Fleet: ObservableObject {
    @Published public var agents: [Agent]

    private var timer: Timer?
    private var beat = 0

    public init(agents: [Agent] = Mock.agents) {
        self.agents = agents
    }

    /// Agents that never move read as a screenshot. This drives cursors,
    /// progress and the occasional status change so the thing is worth
    /// looking at while you evaluate the interaction.
    public func startLiving() {
        guard timer == nil else { return }
        let t = Timer.scheduledTimer(withTimeInterval: 0.85, repeats: true) { [weak self] _ in
            self?.step()
        }
        RunLoop.main.add(t, forMode: .common)   // keeps ticking during gestures
        timer = t
    }

    public func stopLiving() {
        timer?.invalidate()
        timer = nil
    }

    private func step() {
        beat += 1
        for i in agents.indices {
            guard agents[i].status == .working else { continue }

            agents[i].progress += Double.random(in: 0.03...0.10)
            if agents[i].progress >= 1 {
                agents[i].progress = 0
                agents[i].written = 2 + (agents[i].written % 4)
            }

            // Move in short hops, the way a cursor actually travels.
            let c = agents[i].cursor
            agents[i].cursor = CGPoint(
                x: clampD(c.x + Double.random(in: -0.22...0.22), 0.08, 0.92),
                y: clampD(c.y + Double.random(in: -0.18...0.18), 0.12, 0.88)
            )
        }

        // Every so often somebody gets stuck, and somebody else picks up.
        if beat % 14 == 0 {
            if let idle = agents.firstIndex(where: { $0.status == .idle }) {
                agents[idle].status = .working
            }
        }
    }

    private func clampD(_ v: Double, _ lo: Double, _ hi: Double) -> Double {
        min(max(v, lo), hi)
    }

    public var needsYou: Int {
        agents.filter { $0.status == .needsYou }.count
    }

    public var working: Int {
        agents.filter { $0.status == .working }.count
    }
}
