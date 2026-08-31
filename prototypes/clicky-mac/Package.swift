// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "ClickyPrototypes",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "ClickyKit"),
        .executableTarget(name: "ZoomCanvas", dependencies: ["ClickyKit"]),
        .executableTarget(name: "EdgePeek", dependencies: ["ClickyKit"]),
        .executableTarget(name: "NotchBall", dependencies: ["ClickyKit"]),
    ]
)
