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

Quit with ⌘Q. `NotchBall` has no window in the normal sense — look at your notch.

## Controls

**ZoomCanvas**
- Pinch out / in on the trackpad — tracks your fingers directly, no easing
- Two-finger scroll to pan while zoomed out
- Two-finger double-tap (smart zoom) or Space to toggle overview
- `-` overview, `=` home, `esc` home
- Click an agent to fly to it

**EdgePeek**
- Two-finger push toward an edge. Keep pushing to commit; let go early and it
  rubber-bands back — the same contract as Safari's back-swipe
- Arrow keys travel in a direction; `esc` comes home
- Edge glow brightness is agent state, and it stretches as you push

**NotchBall**
- Hover the notch — peeks, shows the count
- Swipe (two-finger scroll) over the notch — opens the minimap
- Click the ball — toggles
- ⌥Space — toggles, but only while this app is focused

## Colour rule

Orange is you. Sky is an agent. That holds in all three, and in the direction
sketches under `design/clicky-window/`.

## What is real and what is faked

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

- Both `ZoomCanvas` and `EdgePeek` swallow scroll and pinch app-wide via
  `NSEvent` local monitors. Fine for a single-surface prototype, wrong for a
  real app — scope it to a view before this becomes product code.
- Scroll direction assumes macOS natural scrolling. If travel feels inverted,
  flip the two signs in `PeekModel.onScroll`.
- `swift run` builds an executable with no bundle, so there is no Dock icon or
  code signature. That is expected.

## Layout

```
Sources/ClickyKit/    Tokens, agent model, gesture monitors, window boot, mock scenery
Sources/ZoomCanvas/   Direction A
Sources/EdgePeek/     Direction D
Sources/NotchBall/    The notch ball + minimap
```
