# PagedControls

A tiny, header-only library to quickly deliver a PagedControls UX pattern.
Controlling many parameters from a few physical knobs by grouping
them into **pages** (cycled with a button), featuring **soft-takeover ("pickup")** so
switching pages never makes a parameter jump, and an **LED page indicator**.

## Features
- Holds a grid of `NumPages x NumKnobs` stored values (0..1).
- Each page remembers its own knob values.
- **Pickup**: after a page change, a knob only starts tracking once it moves
  within `pickup_threshold` of that page's stored value (no parameter jumps).
- **LED**: a count-flash pattern — page `p` blinks `(p+1)` times then rests, so
  you can read the page by counting flashes. Timing is in processing-block units
  (tune to your block rate).
- Optional `SaveGrid`/`LoadGrid` for persistence (e.g. to flash).


## Usage
```cpp
#include "PagedControls.hpp"
pagedctl::PagedControls<2, 4> pg;                 // 2 pages, 4 knobs

const float defaults[2][4] = {{0.42f,1,1,0.5f}, {0,0.5f,0.2f,0}};
pg.Init(defaults, /*pickup=*/0.03f, /*LED*/{15,40,110});

// once per block (host glue reads the knobs + button edge):
pg.Process(knob_values, button_falling_edge);
float rt60 = pg.Value(0, 0);                      // map grid -> your params
bool  led  = pg.LedOn();                          // -> drive your LED pin
```

## Tests
`tests/run.sh` (or `run.bat`): defaults, pickup, per-page independence + pickup
reset, page cycling, LED flash count, save/load roundtrip.
