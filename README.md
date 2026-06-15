# PagedControls

A tiny, header-only library to quickly deliver a PagedControls UX pattern.
Controlling many parameters from a few physical knobs by grouping
them into **pages** (cycled with a button), featuring **soft-takeover ("pickup")** so
switching pages never makes a parameter jump, and an **LED page indicator**.

`pagedctl::PagedControls` (in `PagedControls.hpp`) folds the paging UX together with
an optional **macro / virtualization layer** — a small data-driven routing matrix
that maps a few knob values onto many derived parameters. Used with its default
template args and no routes, it is just the classic paged-controls UI.

## Features
- Holds a grid of `NumPages x NumKnobs` stored values (0..1).
- Each page remembers its own knob values.
- **Pickup**: after a page change, a knob only starts tracking once it moves
  within `pickup_threshold` of that page's stored value (no parameter jumps).
- **LED**: a count-flash pattern — page `p` blinks `(p+1)` times then rests, so
  you can read the page by counting flashes. `LedOn(tick)` is a **pure** function of
  a caller-supplied monotonic tick (blocks, milliseconds, whatever matches your
  `LedTiming`), so the class holds the blink *timing* but not the *clock* — nothing
  here needs to run at a fixed cadence or in an audio ISR.
- **Macro layer** (optional): route knobs ("macros") to many derived parameters
  through per-route input windows, output ranges (with inversion), reshaping
  curves, and combine ops. Targets can be real params *or* other derived nodes,
  so routes chain. See below.
- Optional `SaveGrid`/`LoadGrid` for persistence (e.g. to flash).


## Usage
```cpp
#include "PagedControls.hpp"
pagedctl::PagedControls<2, 4> pg;                 // 2 pages, 4 knobs

const float defaults[2][4] = {{0.42f,1,1,0.5f}, {0,0.5f,0.2f,0}};
pg.Init(defaults, /*pickup=*/0.03f, /*LED*/{15,40,110});

// call regularly (host glue reads the knobs + button edge) -- no fixed rate needed:
pg.Process(knob_values, button_falling_edge);
float rt60 = pg.Value(0, 0);                      // map grid -> your params
bool  led  = pg.LedOn(tick++);                    // pure: pass your own monotonic tick
```

## Macro layer

The node graph is one flat array. The first `GridSize = NumPages*NumKnobs` nodes
are the **inputs** (the grid, page-major); the remaining nodes are **derived**
(virtuals and real params). A `constexpr` table of `Route`s (lives in flash) is
evaluated top-to-bottom once per `Process()`:

```cpp
using namespace pagedctl;
// 2 pages x 4 knobs (8 inputs) + 4 derived params; 2 curves.
PagedControls<2, 4, /*NumNodes=*/12, /*NumCurves=*/2> ctl;

enum { M_DECAY=0, M_TONE, M_SHIMMER, M_MIX,   /* page 0 */
       M_SPACE, M_MOTION, M_VOICE, M_FREEZE,  /* page 1 */
       P_RT60=8, P_DAMP, P_SH_AMT, P_V2_LVL };

constexpr CurveDef kCurves[] = { {curve::Linear,0}, {curve::Pow,1.5f} };
constexpr Route kRoutes[] = {
    // { src, dst, out0, out1, in0, in1, curve, op }   (trailing fields default)
    { M_DECAY,   P_RT60,   0, 1, 0, 1, 0 },          // identity
    { M_TONE,    P_DAMP,   0.15f, 1.0f },            // custom output range
    { M_SHIMMER, P_SH_AMT, 0, 1, 0, 1, 1 },          // ease-in (Pow 1.5) curve
    { M_SHIMMER, P_V2_LVL, 0, 1, 0.5f, 1.0f },       // input window: only past halfway
};
const float kBase[12] = {}; // seeds derived nodes (constants / Add-Mul baselines)

ctl.Init(defaults, kRoutes, 4, kCurves, 2, kBase);
ctl.Process(knob_values, button_falling_edge);
float rt60 = ctl.Out(P_RT60);     // resolved derived value
```

- **Input window** `in0..in1` is mapped to `[0,1]` and clamped (a zero-width
  window acts as a step). **Output range** `out0..out1` scales it; `out0 > out1`
  inverts.
- **Curves** are sampled from a generator `fn(t,param)` into a LUT at `Init` time,
  so the hot path is just a lookup + lerp (no transcendentals per block). Built-in
  generators: `curve::Linear/Pow/Exp/Log/SCurve`; pass any `float(*)(float,float)`
  for custom shapes. Curve `0` is always linear.
- **Ops** combine into the target: `Set` (default), `Add`, `Mul` (VCA-style
  scaling), `Max`, `Min`. The `base` seed provides the baseline they build on, and
  holds constants for params you don't route.
- **Chaining**: a route may read a derived node written by an earlier route
  (virtual → virtual → real). Order matters — producers before consumers.

`Out(node)` returns the resolved value; `Value/Active/Page/LedOn/SaveGrid/LoadGrid`
behave as before (persistence still stores the *macro* grid). With the default
`NumNodes == GridSize` and no routes, the layer is inert.

**Threading / the audio ISR:** because `LedOn(tick)` is pure (you own the clock),
`Process()` and the routing have no required cadence — run the whole control update
in your main loop and let the ISR read the resolved `Out()` snapshot, computing
`LedOn(blockCounter++)` (the ISR is a natural block-rate clock) only to drive the
LED pin. If you do prefer to call `Process()` from the ISR, pass
`Process(knobs, edge, /*do_eval=*/false)` and run the heavier `Resolve()` from the
main loop.

## Tests
`tests/run.sh` (or `run.bat`):
- `test_paged_controls`: defaults, pickup, per-page independence + pickup reset,
  page cycling, LED flash count, save/load roundtrip.
- `test_macro_map`: input window clamp + output inversion, every op
  (Set/Add/Mul/Max/Min), curve LUTs vs their generators (sample points + interp),
  virtual→virtual chaining + evaluation order, macro fan-out with live pickup, and
  the plain (no-routing) configuration.
