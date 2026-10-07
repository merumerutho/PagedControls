#pragma once
#ifndef PAGED_CONTROLS_H
#define PAGED_CONTROLS_H

#include <cstddef>
#include <cstdint>
#include <cmath>

// PagedControls - "parameter pages + soft-takeover pickup + LED indicator" with a
// *virtualization (macro) layer*:
// a small data-driven routing matrix that maps a few knob values ("macros") onto
// many derived parameters, through per-route input windows, output ranges, and
// curves. Targets can be real params OR other derived nodes, so routes chain.
//
// Everything is header-only, allocation-free, and hardware-agnostic (host-test it).
//
//   Template params:
//     NumPages  (>=1)   pages cycled by the button
//     NumKnobs  (>=1)   physical knobs (default 4)
//     NumNodes  (>=GridSize)  total values in the routing graph. The first
//                       GridSize = NumPages*NumKnobs nodes are the *inputs* (the
//                       grid, page-major); the rest are *derived* (virtual/real).
//                       Default = GridSize -> no derived nodes -> plain paged UI.
//     NumCurves (>=1)   number of reshaping curves (default 1 = linear).
//     CurveRes  (>=2)   samples per curve LUT (default 33).
//
// With the default NumNodes (== GridSize) and no routes, the macro layer is inert
// and this is just the classic paged-controls UI.

namespace pagedctl {

// ---- Routing vocabulary (namespace-level so apps can declare constexpr tables) ----

// How a route writes its target node.
enum class Op : uint8_t {
    Set,  // dst  = v
    Add,  // dst += v   (several sources summing into one target)
    Mul,  // dst *= v   (VCA-style: scale an already-written node)
    Max,  // dst  = max(dst, v)   (either source can "open" the target)
    Min,  // dst  = min(dst, v)
};

// A single mapping edge. Defaults give "full input range, full output range,
// linear, overwrite", so the common custom-output-range route is just:
//     { SRC, DST, out0, out1 }
struct Route {
    uint16_t src, dst;          // node indices (input or derived)
    float    out0 = 0, out1 = 1; // normalized 0->out0, 1->out1 (out0>out1 inverts)
    float    in0  = 0, in1  = 1; // input window [in0,in1] mapped to [0,1], clamped
    uint8_t  curve = 0;          // index into the curve table (0 == linear)
    Op       op    = Op::Set;
};

// A curve is sampled from `fn(t, param)` (t in [0,1]) into a LUT at Init, so the
// hot path is a table lookup + lerp, not a transcendental.
struct CurveDef {
    float (*fn)(float t, float param);
    float param;
};

// Library-supplied curve generators.
namespace curve {
inline float Linear(float t, float)   { return t; }
inline float Pow   (float t, float k) { return std::pow(t, k); }                 // k>1 ease-in
// Exponential rise; k>0 sets curvature (k->0 approaches linear).
inline float Exp(float t, float k)
{
    if (k == 0.0f) return t;
    return (std::exp(k * t) - 1.0f) / (std::exp(k) - 1.0f);
}
// Logarithmic rise (mirror of Exp): fast at first, easing toward 1.
inline float Log(float t, float k)
{
    if (k == 0.0f) return t;
    return 1.0f - (std::exp(k * (1.0f - t)) - 1.0f) / (std::exp(k) - 1.0f);
}
inline float SCurve(float t, float)   { return t * t * (3.0f - 2.0f * t); }     // smoothstep
} // namespace curve

// MacroMap - the routing/curve evaluator on its own, so a module can own a
// private route table (e.g. one table per effect) without a paged UI.
// Nodes [0, num_inputs) are written from the inputs passed to Evaluate();
// the remaining nodes are derived, seeded from `base` (or 0) before routing.
template <size_t NumNodes, size_t NumCurves = 1, size_t CurveRes = 33>
class MacroMap
{
    static_assert(NumNodes >= 1, "need at least one node");
    static_assert(NumCurves >= 1, "need at least one curve");
    static_assert(CurveRes  >= 2, "curve needs at least two samples");

public:
    void Init(const Route*    routes,
              size_t          num_routes,
              const CurveDef* curves     = nullptr,
              size_t          num_curves = 0,
              const float*    base       = nullptr)
    {
        routes_   = routes;
        n_routes_ = routes ? num_routes : 0;
        base_     = base;
        for (size_t c = 0; c < NumCurves; ++c)
            for (size_t j = 0; j < CurveRes; ++j)
            {
                const float t = (float)j / (float)(CurveRes - 1);
                curves_[c][j] = (c < num_curves && curves && curves[c].fn)
                                    ? curves[c].fn(t, curves[c].param)
                                    : t;
            }
    }

    // Copy `num_inputs` input values into nodes [0, num_inputs), seed the
    // derived nodes, then run every route top-to-bottom.
    void Evaluate(const float* inputs, size_t num_inputs)
    {
        if (num_inputs > NumNodes) num_inputs = NumNodes;
        for (size_t i = 0; i < num_inputs; ++i) node_[i] = inputs[i];
        for (size_t i = num_inputs; i < NumNodes; ++i)
            node_[i] = base_ ? base_[i] : 0.0f;

        for (size_t r = 0; r < n_routes_; ++r)
        {
            const Route& e = routes_[r];
            if (e.src >= NumNodes || e.dst >= NumNodes) continue;
            const float span = e.in1 - e.in0;
            float       t;
            if (span == 0.0f) t = (node_[e.src] >= e.in1) ? 1.0f : 0.0f; // step
            else              t = (node_[e.src] - e.in0) / span;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            t = ApplyCurve(e.curve, t);

            const float v = e.out0 + t * (e.out1 - e.out0);
            float&      d = node_[e.dst];
            switch (e.op)
            {
                case Op::Set: d  = v;           break;
                case Op::Add: d += v;           break;
                case Op::Mul: d *= v;           break;
                case Op::Max: if (v > d) d = v; break;
                case Op::Min: if (v < d) d = v; break;
            }
        }
    }

    float Out(size_t node) const { return node_[node < NumNodes ? node : 0]; }

private:
    // Linear interpolation into a curve LUT. `t` is assumed clamped to [0,1].
    float ApplyCurve(uint8_t c, float t) const
    {
        if (c >= NumCurves) c = 0;
        const float x = t * (float)(CurveRes - 1);
        size_t      i = (size_t)x;
        if (i >= CurveRes - 1) return curves_[c][CurveRes - 1];
        const float f = x - (float)i;
        return curves_[c][i] + f * (curves_[c][i + 1] - curves_[c][i]);
    }

    float        node_[NumNodes]              = {};
    float        curves_[NumCurves][CurveRes] = {};
    const Route* routes_   = nullptr;
    size_t       n_routes_ = 0;
    const float* base_     = nullptr;
};

template <size_t NumPages,
          size_t NumKnobs  = 4,
          size_t NumNodes  = NumPages * NumKnobs,
          size_t NumCurves = 1,
          size_t CurveRes  = 33>
class PagedControls
{
    static_assert(NumPages >= 1, "need at least one page");
    static_assert(NumKnobs >= 1, "need at least one knob");
    static_assert(NumNodes >= NumPages * NumKnobs,
                  "NumNodes must cover the input grid (NumPages*NumKnobs)");
    static_assert(NumCurves >= 1, "need at least one curve");
    static_assert(CurveRes  >= 2, "curve needs at least two samples");

public:
    static constexpr size_t kGridSize = NumPages * NumKnobs;
    // LockKnob() argument meaning "follow the current page".
    static constexpr size_t kNoLock = static_cast<size_t>(-1);

    // LED count-flash timing, in the *tick* unit you pass to LedOn.
    struct LedTiming
    {
        uint32_t on_ticks;     // pulse "lit" duration
        uint32_t period_ticks; // pulse on+off duration (>= on_ticks)
        uint32_t rest_ticks;   // gap after the count, before repeating
    };

    // --- Macro Init: paging + routing. ---
    // `routes`/`num_routes`  : the mapping table (evaluated top-to-bottom).
    // `curves`/`num_curves`  : curve generators sampled into LUTs. Any curve index
    //                          not supplied (incl. when curves==null) defaults to
    //                          linear, so route.curve==0 is always safe.
    // `base`                 : NumNodes seed values applied to derived nodes before
    //                          routing each block (holds constants for unrouted
    //                          params, and the baseline for Add/Mul/Max/Min). null
    //                          => derived nodes seed to 0. The input region is
    //                          overwritten from the grid and ignored here.
    void Init(const float (&defaults)[NumPages][NumKnobs],
              const Route*    routes,
              size_t          num_routes,
              const CurveDef* curves     = nullptr,
              size_t          num_curves = 0,
              const float*    base       = nullptr,
              float           pickup_threshold = 0.03f,
              LedTiming       led = {15, 40, 110})
    {
        InitCommon(defaults, pickup_threshold, led);
        map_.Init(routes, num_routes, curves, num_curves, base);
        Evaluate();
    }

    // --- Plain Init (no routing): the classic paged-controls setup, for when the
    // macro layer is unused (default NumNodes, no routes). ---
    void Init(const float (&defaults)[NumPages][NumKnobs],
              float     pickup_threshold = 0.03f,
              LedTiming led = {15, 40, 110})
    {
        InitCommon(defaults, pickup_threshold, led);
        map_.Init(nullptr, 0); // curve 0 = linear
        Evaluate();
    }

    // The UI tick: paging + soft-takeover pickup. `knobs` is NumKnobs values in
    // 0..1; `advance_page` is the (already-debounced) button-tap edge. On a page
    // change the pickup is reset so knobs must be re-caught. A knob picks up when
    // it comes within `pickup_threshold` of the stored value or crosses it
    // between two calls (so fast moves and stepped CV cannot skip the window).
    // A locked knob (LockKnob) always writes its locked page.
    //
    // `do_eval` controls whether the routed node graph is refreshed here:
    //   * true (default): nodes are fresh after Process.
    //   * false: skip routing; call Resolve() elsewhere to decouple the macro eval.
    void Process(const float* knobs, bool advance_page, bool do_eval = true)
    {
        if (advance_page) GoToPage((page_ + 1) % NumPages);

        for (size_t k = 0; k < NumKnobs; ++k)
            if (PickedUp(k, knobs[k])) slot_[TargetPage(k)][k] = knobs[k];

        if (do_eval) Evaluate();
    }

    // Refresh the routed node graph from the current grid. Decoupled from Process
    // so the macro eval can be run separately.
    void Resolve() { Evaluate(); }

    // Jump directly to a page (e.g. restoring UI state); resets the pickup of
    // every knob that follows the page. Locked knobs are unaffected.
    void GoToPage(size_t page)
    {
        if (page >= NumPages) return;
        page_ = page;
        for (size_t k = 0; k < NumKnobs; ++k)
            if (lock_[k] == kNoLock) picked_[k] = false;
    }

    // Pin a knob to one page regardless of the current page (kNoLock follows
    // the page again). The knob must be re-caught on its new target page.
    void LockKnob(size_t knob, size_t page)
    {
        if (knob >= NumKnobs) return;
        lock_[knob]   = page < NumPages ? page : kNoLock;
        picked_[knob] = false;
    }
    size_t KnobLock(size_t knob) const { return lock_[knob]; }

    // Overwrite one stored value (e.g. reset to a neutral position). Any knob
    // currently targeting that slot must be re-caught.
    void SetValue(size_t page, size_t knob, float value)
    {
        if (page >= NumPages || knob >= NumKnobs) return;
        slot_[page][knob] = value;
        if (TargetPage(knob) == page) picked_[knob] = false;
        Evaluate();
    }

    // ---- Accessors ----
    // Resolved node value (input or derived) after the last Process/Init.
    float  Out(size_t node)                const { return map_.Out(node); }
    // Raw stored grid value (the macro the user set), independent of routing.
    float  Value(size_t page, size_t knob) const { return slot_[page][knob]; }
    // Stored value the knob currently drives (its locked page, else the current page).
    float  Active(size_t knob)             const { return slot_[TargetPage(knob)][knob]; }
    size_t Page()                          const { return page_; }
    bool   PickedUp(size_t knob)           const { return picked_[knob]; }

    // LED state for monotonic `tick` (same unit as LedTiming). Page p lights
    // (p+1) pulses then rests.
    bool LedOn(uint32_t tick) const
    {
        const uint32_t np = (uint32_t)page_ + 1;
        const uint32_t c  = tick % CycleTicks();
        if (c < np * led_.period_ticks)
            return (c % led_.period_ticks) < led_.on_ticks;
        return false;
    }

    // Optional persistence: copy the whole *macro grid* out / in.
    // The destination/source must be GridSize() floats.
    void SaveGrid(float* dst) const
    {
        for (size_t p = 0; p < NumPages; ++p)
            for (size_t k = 0; k < NumKnobs; ++k) *dst++ = slot_[p][k];
    }
    void LoadGrid(const float* src)
    {
        for (size_t p = 0; p < NumPages; ++p)
            for (size_t k = 0; k < NumKnobs; ++k) slot_[p][k] = *src++;
        for (size_t k = 0; k < NumKnobs; ++k) picked_[k] = false;
        Evaluate();
    }

    static constexpr size_t kNumPages  = NumPages;
    static constexpr size_t kNumKnobs  = NumKnobs;
    static constexpr size_t kNumNodes  = NumNodes;
    static constexpr size_t kNumCurves = NumCurves;
    static constexpr size_t kCurveRes  = CurveRes;
    static constexpr size_t GridSize() { return kGridSize; }

private:
    void InitCommon(const float (&defaults)[NumPages][NumKnobs],
                    float pickup_threshold, LedTiming led)
    {
        pickup_ = pickup_threshold;
        led_    = led;
        page_   = 0;
        for (size_t p = 0; p < NumPages; ++p)
            for (size_t k = 0; k < NumKnobs; ++k)
                slot_[p][k] = defaults[p][k];
        for (size_t k = 0; k < NumKnobs; ++k)
        {
            picked_[k]    = false;
            have_last_[k] = false;
            lock_[k]      = kNoLock;
        }
    }

    // Refresh the node graph: the grid (page-major) is the input region.
    void Evaluate() { map_.Evaluate(&slot_[0][0], kGridSize); }

    size_t TargetPage(size_t knob) const
    {
        return lock_[knob] == kNoLock ? page_ : lock_[knob];
    }

    uint32_t CycleTicks() const
    {
        return (uint32_t)(page_ + 1) * led_.period_ticks + led_.rest_ticks;
    }

    bool PickedUp(size_t knob, float physical)
    {
        const float stored = slot_[TargetPage(knob)][knob];
        const float prev   = last_[knob];
        const bool  had    = have_last_[knob];
        last_[knob]        = physical;
        have_last_[knob]   = true;
        if (picked_[knob]) return true;
        float d = physical - stored;
        if (d < 0.0f) d = -d;
        const bool crossed = had && (prev - stored) * (physical - stored) <= 0.0f;
        if (d < pickup_ || crossed) { picked_[knob] = true; return true; }
        return false;
    }

    // Paging / pickup / LED state.
    float     slot_[NumPages][NumKnobs] = {};
    bool      picked_[NumKnobs]         = {};
    float     last_[NumKnobs]           = {}; // previous physical value per knob
    bool      have_last_[NumKnobs]      = {};
    size_t    lock_[NumKnobs]           = {};
    size_t    page_   = 0;
    float     pickup_ = 0.03f;
    LedTiming led_    = {15, 40, 110};

    // Macro routing state.
    MacroMap<NumNodes, NumCurves, CurveRes> map_;
};

} // namespace pagedctl

#endif // PAGED_CONTROLS_H
