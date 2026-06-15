#pragma once
#ifndef PAGED_CONTROLS_H
#define PAGED_CONTROLS_H

#include <cstddef>
#include <cstdint>
#include <cmath>

// PagedControls - "parameter pages + soft-takeover pickup + LED indicator" with a
// folded-in *virtualization (macro) layer*:
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

// A single mapping edge. Defaults give the common case "full input range, full
// output range, linear, overwrite". Field order is tuned so the frequent
// "custom output range, default input window" route is a two-liner:
//     { SRC, DST, out0, out1 }
struct Route {
    uint16_t src, dst;          // node indices (input or derived)
    float    out0 = 0, out1 = 1; // normalized 0->out0, 1->out1 (out0>out1 inverts)
    float    in0  = 0, in1  = 1; // input window [in0,in1] mapped to [0,1], clamped
    uint8_t  curve = 0;          // index into the curve table (0 == linear)
    Op       op    = Op::Set;
};

// A curve is sampled from `fn(t, param)` (t in [0,1]) into a LUT at Init time, so
// the per-block hot path is just a table lookup + lerp -- no transcendentals.
struct CurveDef {
    float (*fn)(float t, float param);
    float param;
};

// Library-supplied generators (the transcendental cost is paid once, at Init).
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

    // LED count-flash timing, in caller-defined *ticks* (whatever unit you pass to
    // LedOn -- e.g. audio blocks or milliseconds). Page p emits (p+1) pulses then
    // rests, so you can count the flashes. Defaults suit ticks at a ~150-200 Hz
    // block rate.
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
        routes_   = routes;
        n_routes_ = routes ? num_routes : 0;
        base_     = base;
        InitCommon(defaults, pickup_threshold, led);
        BuildCurves(curves, curves ? num_curves : 0);
        Evaluate();
    }

    // --- Plain Init (no routing): the classic paged-controls setup, for when the
    // macro layer is unused (default NumNodes, no routes). ---
    void Init(const float (&defaults)[NumPages][NumKnobs],
              float     pickup_threshold = 0.03f,
              LedTiming led = {15, 40, 110})
    {
        routes_   = nullptr;
        n_routes_ = 0;
        base_     = nullptr;
        InitCommon(defaults, pickup_threshold, led);
        BuildCurves(nullptr, 0); // curve 0 = linear
        Evaluate();
    }

    // The UI tick: paging + soft-takeover pickup. `knobs` is NumKnobs values in
    // 0..1; `advance_page` is the (already-debounced) button-tap edge. On a page
    // change the pickup is reset so knobs must be re-caught. (The LED is a separate
    // pure query, LedOn(tick), so this has no required cadence.)
    //
    // `do_eval` controls whether the routed node graph is refreshed here:
    //   * true (default): convenient one-call usage; nodes are fresh after Process.
    //   * false: skip routing. Pair with Resolve() called elsewhere (e.g. the main
    //     loop) so the (possibly costly) macro eval can run off the audio thread.
    void Process(const float* knobs, bool advance_page, bool do_eval = true)
    {
        if (advance_page) GoToPage((page_ + 1) % NumPages);

        for (size_t k = 0; k < NumKnobs; ++k)
            if (PickedUp(k, knobs[k])) slot_[page_][k] = knobs[k];

        if (do_eval) Evaluate();
    }

    // Refresh the routed node graph from the current grid. Decoupled from Process
    // so the macro eval can run off the audio thread (call from the main loop).
    void Resolve() { Evaluate(); }

    // Jump directly to a page (e.g. restoring UI state); resets knob pickup.
    void GoToPage(size_t page)
    {
        if (page >= NumPages) return;
        page_ = page;
        for (size_t k = 0; k < NumKnobs; ++k) picked_[k] = false;
    }

    // ---- Accessors ----
    // Resolved node value (input or derived) after the last Process/Init.
    float  Out(size_t node)                const { return node_[node < NumNodes ? node : 0]; }
    // Raw stored grid value (the macro the user set), independent of routing.
    float  Value(size_t page, size_t knob) const { return slot_[page][knob]; }
    float  Active(size_t knob)             const { return slot_[page_][knob]; }
    size_t Page()                          const { return page_; }
    bool   PickedUp(size_t knob)           const { return picked_[knob]; }

    // Pure LED state for a given monotonic `tick` (same unit as LedTiming). The
    // caller owns the clock and the GPIO write, so nothing in this class needs to
    // run at a fixed cadence or inside an audio ISR. Page p lights (p+1) pulses
    // then rests, so you can count the flashes.
    bool LedOn(uint32_t tick) const
    {
        const uint32_t np = (uint32_t)page_ + 1;
        const uint32_t c  = tick % CycleTicks();
        if (c < np * led_.period_ticks)
            return (c % led_.period_ticks) < led_.on_ticks;
        return false;
    }

    // Optional persistence: copy the whole *macro grid* out / in (e.g. to flash).
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
        for (size_t k = 0; k < NumKnobs; ++k) picked_[k] = false;
    }

    // Sample each curve into its LUT. Missing curves (index >= num supplied, or a
    // null fn) fall back to linear, so curve 0 is always a safe linear default.
    void BuildCurves(const CurveDef* defs, size_t num)
    {
        for (size_t c = 0; c < NumCurves; ++c)
            for (size_t j = 0; j < CurveRes; ++j)
            {
                const float t = (float)j / (float)(CurveRes - 1);
                curves_[c][j] = (c < num && defs && defs[c].fn)
                                    ? defs[c].fn(t, defs[c].param)
                                    : t;
            }
    }

    // Linear interpolation into a curve LUT. `t` is assumed clamped to [0,1].
    float ApplyCurve(uint8_t c, float t) const
    {
        if (c >= NumCurves) c = 0;
        const float  x = t * (float)(CurveRes - 1);
        size_t       i = (size_t)x;
        if (i >= CurveRes - 1) return curves_[c][CurveRes - 1];
        const float f = x - (float)i;
        return curves_[c][i] + f * (curves_[c][i + 1] - curves_[c][i]);
    }

    // Refresh the node graph: copy grid -> inputs, seed derived, run routes.
    void Evaluate()
    {
        for (size_t p = 0; p < NumPages; ++p)
            for (size_t k = 0; k < NumKnobs; ++k)
                node_[p * NumKnobs + k] = slot_[p][k];

        for (size_t i = kGridSize; i < NumNodes; ++i)
            node_[i] = base_ ? base_[i] : 0.0f;

        for (size_t r = 0; r < n_routes_; ++r)
        {
            const Route& e    = routes_[r];
            const float  span = e.in1 - e.in0;
            float        t;
            if (span == 0.0f)       t = (node_[e.src] >= e.in1) ? 1.0f : 0.0f; // step
            else                    t = (node_[e.src] - e.in0) / span;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            t = ApplyCurve(e.curve, t);

            const float v = e.out0 + t * (e.out1 - e.out0);
            float&      d = node_[e.dst];
            switch (e.op)
            {
                case Op::Set: d  = v;                 break;
                case Op::Add: d += v;                 break;
                case Op::Mul: d *= v;                 break;
                case Op::Max: if (v > d) d = v;       break;
                case Op::Min: if (v < d) d = v;       break;
            }
        }
    }

    uint32_t CycleTicks() const
    {
        return (uint32_t)(page_ + 1) * led_.period_ticks + led_.rest_ticks;
    }

    bool PickedUp(size_t knob, float physical)
    {
        if (picked_[knob]) return true;
        float d = physical - slot_[page_][knob];
        if (d < 0.0f) d = -d;
        if (d < pickup_) { picked_[knob] = true; return true; }
        return false;
    }

    // Paging / pickup / LED state (unchanged from the original PagedControls).
    float     slot_[NumPages][NumKnobs] = {};
    bool      picked_[NumKnobs]         = {};
    size_t    page_   = 0;
    float     pickup_ = 0.03f;
    LedTiming led_    = {15, 40, 110};

    // Macro routing state.
    float        node_[NumNodes]            = {};
    float        curves_[NumCurves][CurveRes] = {};
    const Route* routes_   = nullptr;
    size_t       n_routes_ = 0;
    const float* base_     = nullptr;
};

} // namespace pagedctl

#endif // PAGED_CONTROLS_H
