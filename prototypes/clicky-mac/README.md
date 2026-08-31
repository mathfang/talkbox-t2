# Clicky — interaction prototypes

Three runnable macOS prototypes for the agent-window problem: agents spawn
outside your display, and you need to perceive and reach them.

| Run | What it is |
|---|---|
| `swift run ZoomCanvas` | **Direction A** — your display is one tile on a larger desktop; pinch out and the agents are there |
| `swift run EdgePeek` | **Direction D** — no UI at rest; agents bleed light in at the bezel, push past an edge to travel |
| `swift run NotchBall` | **The ball** — sits at the notch, swipe/hover to open a minimap of where agents are |

Requires macOS 13+ and a Swift 5.9 toolchain (Xcode 15 or newer).

```
cd prototypes/clicky-mac
swift build
swift run ZoomCanvas
```

Quit `ZoomCanvas` / `EdgePeek` with ⌘Q. `NotchBall` runs as an accessory app —
no Dock icon, no menu bar, never takes focus — so quit it with ctrl-C in the
terminal you launched it from. Look at your notch to find it.

## Controls

**ZoomCanvas**
- **Squeeze** two fingers together to step back and reveal the agents. Spreading
  them returns home — at rest you are already at maximum zoom, so spreading does
  nothing, which is why the hint says squeeze
- Two-finger scroll to pan while zoomed out
- Two-finger double-tap (smart zoom) or Space to toggle overview
- `−` overview, `=` home, `esc` home
- Click an agent to fly to it

**EdgePeek**
- Two-finger push toward an edge. Keep pushing to commit; let go early and it
  rubber-bands back — the same contract as Safari's back-swipe
- One agent per direction: Xcode right, Gmail down, Sheets left, Figma up
- Arrow keys travel; press again (or `esc`) to come home
- Edge glow is agent state, and it stretches as you push. The agent that needs
  you gets a wide wash, not a brighter hairline
- Works with a plain scroll wheel too — a wheel sends no gesture phases, so the
  end of the gesture is synthesised after 150ms of quiet

**NotchBall**
- Hover the notch for ~0.2s — peeks, shows the count
- Swipe (two-finger scroll) over the notch — opens the minimap
- Click the ball — toggles
- To close: swipe away, click anywhere outside, or `esc`
- ⌥Space toggles, but only while this app is focused — a global *keyboard* tap
  is the one kind that would need Accessibility permission

## Colour rule

Orange is you. Sky is an agent. That holds in all three, and in the direction
sketches under `design/clicky-window/`.

## What is real and what is faked

**Real:** the orb. It is liquid glass built on a live `NSVisualEffectView`
backdrop, so it genuinely refracts the desktop behind it rather than painting a
gradient that imitates one — the rim, the two speculars and the three stacked
shadows are what sell it as glass. On macOS 26 the native `.glassEffect()` would
replace the hand-rolled rim; this targets macOS 13, so it is assembled by hand.

**Real:** every gesture. Pinch phases, scroll momentum, rubber-band commit,
notch geometry (`safeAreaInsets` + `auxiliaryTopLeftArea`, with a centred
fallback on displays without a notch), window levels and Space behaviour,
`NSVisualEffectView` materials.

**Faked:** the desktop behind it all. `DisplayMock` draws generic window
chrome — these prototypes do not capture, composite, or control your real
windows. The agents are four fixed entries in `Mock.agents`.

**Not possible without private API:** true Exposé / Mission Control hooking.
macOS does not report Mission Control state to an ordinary app, so `NotchBall`
uses hover, swipe, click and a hotkey instead. Hover and swipe are watched with
global monitors, which need no permission; only a global *keyboard* tap would
require Accessibility, which is why ⌥Space is focus-only.

## Gotchas

- All three swallow scroll and pinch app-wide via `NSEvent` monitors. Fine for a
  single-surface prototype, wrong for a real app — scope it to a view before
  this becomes product code.
- Scroll direction assumes macOS natural scrolling. If travel feels inverted,
  flip the two signs in `PeekModel`'s `onScroll`.
- `ZoomCanvas`'s core gesture is trackpad-only — pinch cannot be produced by any
  mouse. The `−` / `=` keys are the fallback and are shown in the on-screen hint.
- `swift run` builds an executable with no bundle, so there is no Dock icon or
  code signature. That is expected.
- The orb is matched by eye from the `agent` Figma frame, not exported from it.
  Shell egress to figma.com is blocked here, so the artwork could not be pulled
  down — send a PNG export if you want it pixel-exact.
- **Not compiled.** These were written in a Linux container with no Swift
  toolchain and no macOS SDK, so `swift build` has never run against them. They
  were reviewed for API availability, type errors and launch crashes, but the
  first real compile is on your Mac.

## Layout

```
Sources/ClickyKit/    Tokens, agent model, gesture monitors, window boot, mock scenery
Sources/ZoomCanvas/   Direction A
Sources/EdgePeek/     Direction D
Sources/NotchBall/    The notch ball + minimap
```
