# Clicky agent window — direction sketches

Five strategic directions for how Clicky's off-screen agents become visible:
agents spawn outside the display, and you zoom out / use a minimap to reach them.

| Board | File | Bet |
|---|---|---|
| A · Zoom-Out Canvas | `Main.dc.html` | Your display is one tile on a larger desktop |
| B · Minimap HUD | `DirectionB.dc.html` | Persistent corner map, you never leave your screen |
| C · The Control Room | `DirectionC.dc.html` | One window, a live tile per agent |
| D · Edge Peek | `DirectionD.dc.html` | No UI at rest; agents bleed light in at the bezel |
| E · The Table | `DirectionE.dc.html` | Zoom out onto a table; agents are seats, not panes |

`canvas.json` lays the boards out and carries the brief as a sticky note.
Colour key: orange = you, sky = an agent.

These are static direction sketches, not prototypes — the goal is choosing a
direction. They were checked against `DESIGN-PREFERENCES.md` at the repo root.
They are NOT matched to the real `agentfigma` Figma file, which was never
reachable in the session that produced them.

## Rebuild the canvas

The published canvas is generated and git-ignored. To regenerate it:

```
node <design-skill>/seed-canvas.mjs \
  --template <design-skill>/payload.template.html \
  --out clicky-agent-window.html \
  --title "Clicky Agent Window" \
  --artboard Main.dc.html \
  --artboard DirectionB.dc.html \
  --artboard DirectionC.dc.html \
  --artboard DirectionD.dc.html \
  --artboard DirectionE.dc.html \
  --canvas canvas.json
```
