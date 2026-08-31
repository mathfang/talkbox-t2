import AppKit
import SwiftUI

/// Trackpad handling via NSEvent monitors rather than a custom NSView.
///
/// Monitors see the event before it is dispatched to a view, so gestures work
/// no matter what SwiftUI puts on screen — no hit-testing fights, and pinch
/// keeps its real phases and momentum instead of being approximated.
///
/// Local monitors need no permissions. The global mouse monitor needs none
/// either (only keyboard taps would require Accessibility).
public final class GestureMonitor {

    public var onMagnify: ((CGFloat, NSEvent.Phase) -> Void)?
    public var onScroll: ((CGFloat, CGFloat, NSEvent.Phase, Bool) -> Void)?
    public var onSmartMagnify: (() -> Void)?
    public var onSwipe: ((CGFloat, CGFloat) -> Void)?
    /// Screen coordinates, bottom-left origin.
    public var onMouseMoved: ((NSPoint) -> Void)?
    /// Return true to swallow the key.
    public var onKeyDown: ((NSEvent) -> Bool)?

    private var tokens: [Any] = []

    public init() {}

    /// Global mouse and scroll monitors need no Accessibility permission
    /// (only keyboard taps would). A notch surface is never the focused app,
    /// so it must watch globally or its gestures simply never arrive.
    public func start(watchMouseGlobally: Bool = false,
                      watchScrollGlobally: Bool = false) {
        local(.magnify) { [weak self] e in
            self?.onMagnify?(e.magnification, e.phase)
            return nil
        }

        local(.scrollWheel) { [weak self] e in
            self?.onScroll?(e.scrollingDeltaX, e.scrollingDeltaY,
                            e.phase, e.hasPreciseScrollingDeltas)
            return nil
        }

        local(.smartMagnify) { [weak self] e in
            self?.onSmartMagnify?()
            return nil
        }

        local(.swipe) { [weak self] e in
            self?.onSwipe?(e.deltaX, e.deltaY)
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

        if watchMouseGlobally {
            let token = NSEvent.addGlobalMonitorForEvents(
                matching: [.mouseMoved],
                handler: { [weak self] _ in
                    self?.onMouseMoved?(NSEvent.mouseLocation)
                }
            )
            if let token { tokens.append(token) }
        }

        if watchScrollGlobally {
            let token = NSEvent.addGlobalMonitorForEvents(
                matching: [.scrollWheel],
                handler: { [weak self] e in
                    self?.onScroll?(e.scrollingDeltaX, e.scrollingDeltaY,
                                    e.phase, e.hasPreciseScrollingDeltas)
                }
            )
            if let token { tokens.append(token) }
        }
    }

    private func local(_ mask: NSEvent.EventTypeMask,
                       _ handler: @escaping (NSEvent) -> NSEvent?) {
        if let t = NSEvent.addLocalMonitorForEvents(matching: mask, handler: handler) {
            tokens.append(t)
        }
    }

    deinit {
        for t in tokens { NSEvent.removeMonitor(t) }
    }
}

// MARK: - Momentum-aware pan accumulator

/// Turns a stream of scroll deltas into a position that keeps moving when the
/// fingers lift, then settles. Without this, trackpad panning stops dead and
/// feels mechanical rather than physical.
public struct PanAccumulator {
    public var value: CGSize = .zero
    public private(set) var isCoasting = false

    public init() {}

    public mutating func apply(dx: CGFloat, dy: CGFloat,
                               phase: NSEvent.Phase, precise: Bool) {
        // A mouse wheel sends coarse, unphased deltas; scale them up so the
        // prototype is usable without a trackpad.
        let gain: CGFloat = precise ? 1.0 : 6.0
        value.width += dx * gain
        value.height += dy * gain
        isCoasting = phase == []
    }

    public mutating func reset() {
        value = .zero
        isCoasting = false
    }
}
