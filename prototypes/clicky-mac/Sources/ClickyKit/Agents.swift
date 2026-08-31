import SwiftUI

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

    public init(agents: [Agent] = Mock.agents) {
        self.agents = agents
    }

    public var needsYou: Int {
        agents.filter { $0.status == .needsYou }.count
    }

    public var working: Int {
        agents.filter { $0.status == .working }.count
    }
}
