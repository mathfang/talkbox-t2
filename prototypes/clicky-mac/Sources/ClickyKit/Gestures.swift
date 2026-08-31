import AppKit
import SwiftUI

/// One scroll event, with everything the callers actually need to tell a real
/// gesture from a mouse wheel and a finger-drag from its momentum tail.
public struct ScrollInfo {
    public let dx: CGFloat
    public let dy: CGFloat
    public let phase: NSEvent.Phase
    public let momentum: NSEvent.Phase
    /// False for a traditional scroll wheel, which never sends phases at all.
    public let precise: Bool

    public var isMomentum: Bool { momentum != [] }
    public var ended: Bool { phase == .ended || phase == .cancelled }
    public var began: Bool { phase == .began }
}

/// Trackpad handling via NSEvent monitors rather than a custom NSView.
///
/// Monitors see the event before it is dispatched to a view, so gestures work
/// no matter what SwiftUI puts on screen — no hit-testing fights, and pinch
/// keeps its real phases and momentum instead of being approximated.
public final class GestureMonitor {

    public var onMagnify: ((CGFloat, NSEvent.Phase) -> Void)?
    public var onScroll: ((ScrollInfo) -> Void)?
    public var onSmartMagnify: (() -> Void)?
    /// Screen coordinates, bottom-left origin.
    public var onMouseMoved: ((NSPoint) -> Void)?
    /// A click anywhere, including in another app. Screen coordinates.
    public var onMouseDown: ((NSPoint) -> Void)?
    /// Return true to swallow the key.
    public var onKeyDown: ((NSEvent) -> Bool)?

    private var tokens: [Any] = []

    public init() {}

    /// Global mouse and scroll monitors need no Accessibility permission
    /// (only keyboard taps would). A notch surface is never the focused app,
    /// so it must watch globally or its gestures never arrive.
    public func start(watchMouseGlobally: Bool = false,
                      watchScrollGlobally: Bool = false,
                      watchClicksGlobally: Bool = false) {

        local(.magnify) { [weak self] e in
            self?.onMagnify?(e.magnification, e.phase)
            return nil
        }

        local(.scrollWheel) { [weak self] e in
            self?.onScroll?(Self.info(e))
            return nil
        }

        local(.smartMagnify) { [weak self] e in
            self?.onSmartMagnify?()
            return nil
        }

        local(.keyDown) { [weak self] e in
            if self?.onKeyDown?(e) == true { return nil }
            return e
        }

        local(.mouseMoved) { [weak self] e in
            self?.onMouseMoved?(NSEvent.mouseLocation)
            return e
        }

        // Global handlers are not documented to run on the main thread, and
        // they drive @Published state — so hop to main explicitly.
        if watchMouseGlobally {
            global([.mouseMoved]) { [weak self] _ in
                self?.onMouseMoved?(NSEvent.mouseLocation)
            }
        }

        if watchScrollGlobally {
            global([.scrollWheel]) { [weak self] e in
                self?.onScroll?(Self.info(e))
            }
        }

        if watchClicksGlobally {
            global([.leftMouseDown, .rightMouseDown]) { [weak self] _ in
                self?.onMouseDown?(NSEvent.mouseLocation)
            }
        }
    }

    private static func info(_ e: NSEvent) -> ScrollInfo {
        ScrollInfo(dx: e.scrollingDeltaX,
                   dy: e.scrollingDeltaY,
                   phase: e.phase,
                   momentum: e.momentumPhase,
                   precise: e.hasPreciseScrollingDeltas)
    }

    private func local(_ mask: NSEvent.EventTypeMask,
                       _ handler: @escaping (NSEvent) -> NSEvent?) {
        if let t = NSEvent.addLocalMonitorForEvents(matching: mask, handler: handler) {
            tokens.append(t)
        }
    }

    private func global(_ mask: NSEvent.EventTypeMask,
                        _ handler: @escaping (NSEvent) -> Void) {
        let t = NSEvent.addGlobalMonitorForEvents(matching: mask, handler: { e in
            if Thread.isMainThread {
                handler(e)
            } else {
                DispatchQueue.main.async { handler(e) }
            }
        })
        if let t { tokens.append(t) }
    }

    deinit {
        for t in tokens { NSEvent.removeMonitor(t) }
    }
}

/// A scroll wheel sends no phases, so a gesture built on `.ended` never
/// completes for mouse users. This fires a synthetic end when the events stop.
public final class WheelEndDetector {
    private var work: DispatchWorkItem?
    private let delay: TimeInterval
    public var onEnd: (() -> Void)?

    public init(delay: TimeInterval = 0.15) { self.delay = delay }

    public func bump() {
        work?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.onEnd?() }
        work = w
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: w)
    }

    public func cancel() {
        work?.cancel()
        work = nil
    }
}
