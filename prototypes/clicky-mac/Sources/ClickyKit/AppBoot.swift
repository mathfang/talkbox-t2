import AppKit
import SwiftUI

/// SwiftPM executables have no app bundle, so the usual `@main` SwiftUI
/// lifecycle does not give us a focusable, menu-bearing app. These helpers
/// stand one up by hand — this is the boilerplate, not the design.

public final class ClickyDelegate: NSObject, NSApplicationDelegate {
    private let makeWindow: () -> NSWindow
    public private(set) var window: NSWindow?

    public init(makeWindow: @escaping () -> NSWindow) {
        self.makeWindow = makeWindow
    }

    public func applicationDidFinishLaunching(_ note: Notification) {
        let w = makeWindow()
        window = w
        w.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    public func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool {
        true
    }
}

/// Held for the process lifetime; NSApplication keeps only a weak delegate.
private var retainedDelegate: AnyObject?

public func clickyMainMenu(appName: String) -> NSMenu {
    let main = NSMenu()
    let appItem = NSMenuItem()
    main.addItem(appItem)

    let appMenu = NSMenu()
    appMenu.addItem(withTitle: "Hide \(appName)",
                    action: #selector(NSApplication.hide(_:)),
                    keyEquivalent: "h")
    appMenu.addItem(NSMenuItem.separator())
    appMenu.addItem(withTitle: "Quit \(appName)",
                    action: #selector(NSApplication.terminate(_:)),
                    keyEquivalent: "q")
    appItem.submenu = appMenu
    return main
}

public func runClicky(appName: String, makeWindow: @escaping () -> NSWindow) -> Never {
    let app = NSApplication.shared
    app.setActivationPolicy(.regular)
    app.mainMenu = clickyMainMenu(appName: appName)

    let delegate = ClickyDelegate(makeWindow: makeWindow)
    retainedDelegate = delegate
    app.delegate = delegate

    app.run()
    exit(0)
}

// MARK: - Windows

public func makeMainWindow<V: View>(title: String, size: CGSize, root: V) -> NSWindow {
    let w = NSWindow(
        contentRect: NSRect(origin: .zero, size: size),
        styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
        backing: .buffered,
        defer: false
    )
    w.title = title
    w.titlebarAppearsTransparent = true
    w.titleVisibility = .hidden
    w.backgroundColor = .black
    w.collectionBehavior.insert(.fullScreenPrimary)
    w.acceptsMouseMovedEvents = true   // required for .mouseMoved monitors
    w.contentView = NSHostingView(rootView: root)
    w.center()
    return w
}

/// Borderless, transparent, above everything, on every Space — what a
/// notch-anchored surface has to be.
public func makeNotchWindow<V: View>(size: CGSize, root: V) -> NSWindow {
    let w = NSWindow(
        contentRect: NSRect(origin: .zero, size: size),
        styleMask: [.borderless],
        backing: .buffered,
        defer: false
    )
    w.isOpaque = false
    w.backgroundColor = .clear
    w.hasShadow = false
    w.level = .statusBar
    w.collectionBehavior = [.canJoinAllSpaces, .stationary, .fullScreenAuxiliary]
    w.isMovableByWindowBackground = false
    w.contentView = NSHostingView(rootView: root)
    return w
}

// MARK: - Notch geometry

public enum Notch {
    /// The real notch, in screen coordinates, when the display has one.
    public static func rect(on screen: NSScreen) -> CGRect? {
        let top = screen.safeAreaInsets.top
        guard top > 0,
              let left = screen.auxiliaryTopLeftArea,
              let right = screen.auxiliaryTopRightArea,
              right.minX > left.maxX
        else { return nil }

        return CGRect(x: left.maxX,
                      y: screen.frame.maxY - top,
                      width: right.minX - left.maxX,
                      height: top)
    }

    /// Where to sit when there is no notch: a matching slot under the menu bar,
    /// centred. Keeps the prototype honest on an external display.
    public static func fallbackRect(on screen: NSScreen) -> CGRect {
        let w: CGFloat = 190
        let h: CGFloat = 32
        let menuBar = screen.frame.maxY - screen.visibleFrame.maxY
        return CGRect(x: screen.frame.midX - w / 2,
                      y: screen.frame.maxY - max(menuBar, h),
                      width: w,
                      height: h)
    }

    public static func anchor(on screen: NSScreen) -> (rect: CGRect, real: Bool) {
        if let r = rect(on: screen) { return (r, true) }
        return (fallbackRect(on: screen), false)
    }
}
