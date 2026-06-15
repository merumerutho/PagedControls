// Host tests for the macro (virtualization) routing layer folded into
// PagedMacroControls: input windows + clamping, output ranges + inversion, every
// op (Set/Add/Mul/Max/Min), curve LUTs vs their generators (sample points + interp),
// virtual->virtual chaining + evaluation order, and the back-compat alias.
#include "../PagedMacroControls.hpp"
#include <cstdio>
#include <cmath>

using namespace pagedctl;

static int g_checks = 0, g_fail = 0;
static void check(bool c, const char* e, int line)
{
    ++g_checks;
    if (!c) { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, line, e); }
}
#define CHECK(c)    check((c), #c, __LINE__)
#define NEAR(a, b)  check(std::fabs((a) - (b)) < 1e-6f, #a " ~= " #b, __LINE__)
#define NEARt(a,b,t) check(std::fabs((a) - (b)) < (t), #a " ~= " #b, __LINE__)

// 2 pages x 4 knobs -> 8 input nodes; derived nodes start at index 8.
using MC = PagedMacroControls<2, 4, /*NumNodes=*/16, /*NumCurves=*/4, /*CurveRes=*/33>;
static constexpr size_t G = MC::kGridSize; // 8

// Node aliases for readability.
enum { M0 = 0, M1, M2, M3, M4, M5, M6, M7,
       D0 = 8, D1, D2, D3, D4, D5, D6, D7 };

// Defaults helper: a 2x4 grid initialised from a flat list.
static void grid(float (&d)[2][4], float a, float b, float c, float e,
                 float f, float g, float h, float i)
{
    d[0][0]=a; d[0][1]=b; d[0][2]=c; d[0][3]=e;
    d[1][0]=f; d[1][1]=g; d[1][2]=h; d[1][3]=i;
}

// Reference curve LUT eval (mirrors PagedMacroControls::ApplyCurve) so interp
// expectations are computed identically to the implementation.
static float refCurve(float (*fn)(float,float), float param, float t)
{
    const int    R = (int)MC::kCurveRes;
    const float  x = t * (float)(R - 1);
    int          i = (int)x;
    if (i >= R - 1) return fn(1.0f, param);
    const float  f = x - (float)i;
    const float  a = fn((float)i       / (float)(R - 1), param);
    const float  b = fn((float)(i + 1) / (float)(R - 1), param);
    return a + f * (b - a);
}

static void test_window_and_inversion()
{
    std::printf("- window_clamp_and_inversion\n");
    static constexpr Route routes[] = {
        { M0, D0, 0.0f, 10.0f, 0.25f, 0.75f },  // windowed: [.25,.75] -> [0,10]
        { M0, D1, 10.0f, 0.0f },                // inverted full range -> [10,0]
        { M0, D2, 0.0f, 1.0f, 0.60f, 1.00f },   // window above value -> clamps low
    };
    float d[2][4]; grid(d, 0.5f,0,0,0, 0,0,0,0);
    MC mc; mc.Init(d, routes, 3);
    NEAR(mc.Out(D0), 5.0f);   // t=(0.5-0.25)/0.5=0.5 -> 5
    NEAR(mc.Out(D1), 5.0f);   // inverted: 10 - 0.5*10 = 5
    NEAR(mc.Out(D2), 0.0f);   // 0.5 below window -> t clamps to 0

    // Push the macro above the window's top -> clamps high.
    float d2[2][4]; grid(d2, 1.0f,0,0,0, 0,0,0,0);
    MC mc2; mc2.Init(d2, routes, 3);
    NEAR(mc2.Out(D0), 10.0f); // 1.0 above .75 -> t clamps to 1 -> 10
    NEAR(mc2.Out(D2), 1.0f);
}

static void test_ops()
{
    std::printf("- ops_set_add_mul_max_min\n");
    static constexpr Route routes[] = {
        { M0, D0, 0,1, 0,1, 0, Op::Set },
        { M1, D1, 0,1, 0,1, 0, Op::Add },
        { M2, D2, 0,1, 0,1, 0, Op::Mul },
        { M3, D3, 0,1, 0,1, 0, Op::Max },
        { M4, D4, 0,1, 0,1, 0, Op::Min },
    };
    float base[16] = {};
    base[D1] = 0.1f; base[D2] = 2.0f; base[D3] = 0.3f; base[D4] = 0.7f;

    float d[2][4]; grid(d, 0.4f,0.5f,0.25f,0.2f, 0.9f,0,0,0);
    MC mc; mc.Init(d, routes, 5, nullptr, 0, base);
    NEAR(mc.Out(D0), 0.4f);              // Set
    NEAR(mc.Out(D1), 0.6f);              // Add: 0.1 + 0.5
    NEAR(mc.Out(D2), 0.5f);              // Mul: 2.0 * 0.25
    NEAR(mc.Out(D3), 0.3f);              // Max(0.3, 0.2)
    NEAR(mc.Out(D4), 0.7f);             // Min(0.7, 0.9)
}

static void test_curves()
{
    std::printf("- curve_luts_vs_generators\n");
    static constexpr CurveDef curves[] = {
        { curve::Linear, 0.0f },   // 0
        { curve::Pow,    2.0f },   // 1
        { curve::SCurve, 0.0f },   // 2
        { curve::Exp,    3.0f },   // 3
    };
    static constexpr Route routes[] = {
        { M0, D0, 0,1, 0,1, 1 },   // pow2
        { M0, D1, 0,1, 0,1, 2 },   // smoothstep
        { M0, D2, 0,1, 0,1, 3 },   // exp
        { M0, D3, 0,1, 0,1, 0 },   // linear
    };

    // Sample point t = 0.5 (j = 16 of 32): LUT == generator exactly.
    {
        float d[2][4]; grid(d, 0.5f,0,0,0, 0,0,0,0);
        MC mc; mc.Init(d, routes, 4, curves, 4);
        NEAR(mc.Out(D0), 0.25f);              // pow(0.5,2)
        NEAR(mc.Out(D1), 0.5f);               // smoothstep(0.5)
        NEAR(mc.Out(D3), 0.5f);               // linear
        NEAR(mc.Out(D2), curve::Exp(0.5f,3.0f));
    }
    // Endpoints exact for every curve (0->0, 1->1).
    {
        float d0[2][4]; grid(d0, 0,0,0,0, 0,0,0,0);
        float d1[2][4]; grid(d1, 1,0,0,0, 0,0,0,0);
        MC lo, hi; lo.Init(d0, routes, 4, curves, 4); hi.Init(d1, routes, 4, curves, 4);
        for (int n = D0; n <= D3; ++n) { NEAR(lo.Out(n), 0.0f); NEAR(hi.Out(n), 1.0f); }
    }
    // Off-sample value -> matches the LUT's linear interpolation (not the raw
    // generator). 0.7 falls between samples 22 and 23.
    {
        const float t = 0.7f;
        float d[2][4]; grid(d, t,0,0,0, 0,0,0,0);
        MC mc; mc.Init(d, routes, 4, curves, 4);
        NEARt(mc.Out(D0), refCurve(curve::Pow,    2.0f, t), 1e-6f);
        NEARt(mc.Out(D2), refCurve(curve::Exp,    3.0f, t), 1e-6f);
        // LUT interp stays close to the true generator (coarse-grid sanity).
        NEARt(mc.Out(D0), curve::Pow(t, 2.0f), 2e-3f);
    }
    // Unsupplied curve index falls back to linear (curve 0 always safe).
    {
        static constexpr Route r2[] = { { M0, D0, 0,1, 0,1, 9 } }; // index out of range
        float d[2][4]; grid(d, 0.3f,0,0,0, 0,0,0,0);
        MC mc; mc.Init(d, r2, 1, curves, 4);
        NEAR(mc.Out(D0), 0.3f); // treated as linear
    }
}

static void test_virtual_chain()
{
    std::printf("- virtual_to_virtual_chain_and_order\n");
    // Correct order: producer (M0->D0) before consumer (D0->D1).
    static constexpr Route ok[] = {
        { M0, D0, 0,1 },          // virtual = macro
        { D0, D1, 0,2 },          // real = 2 * virtual
    };
    float d[2][4]; grid(d, 0.5f,0,0,0, 0,0,0,0);
    MC mc; mc.Init(d, ok, 2);
    NEAR(mc.Out(D0), 0.5f);
    NEAR(mc.Out(D1), 1.0f);       // 2 * 0.5

    // Reversed order: consumer runs first, reads D0's seed (base) of 0.
    static constexpr Route rev[] = {
        { D0, D1, 0,2 },          // reads D0 before it is written -> seed
        { M0, D0, 0,1 },
    };
    float base[16] = {}; // D0 seed = 0
    MC mc2; mc2.Init(d, rev, 2, nullptr, 0, base);
    NEAR(mc2.Out(D1), 0.0f);      // consumed the seed, not the later value
    NEAR(mc2.Out(D0), 0.5f);
}

static void test_fanout_and_live_update()
{
    std::printf("- fanout_one_macro_many_targets_plus_pickup\n");
    // One "Shimmer"-style macro: full-range amount + windowed second voice.
    static constexpr Route routes[] = {
        { M0, D0, 0,1 },                  // amount: full range
        { M0, D1, 0,1, 0.5f, 1.0f },      // 2nd voice: only past halfway
    };
    float d[2][4]; grid(d, 0.0f,0,0,0, 0,0,0,0); // macro default 0 (off)
    MC mc; mc.Init(d, routes, 2);
    NEAR(mc.Out(D0), 0.0f);
    NEAR(mc.Out(D1), 0.0f);

    // Sweep the live knob up through soft-takeover; routes refresh each Process.
    float k[4] = {0.0f,0,0,0};
    mc.Process(k, false);             // catch at the default (0)
    CHECK(mc.PickedUp(0));
    k[0] = 0.75f; mc.Process(k, false);
    NEAR(mc.Out(D0), 0.75f);
    NEAR(mc.Out(D1), 0.5f);           // (0.75-0.5)/0.5
}

static void test_deferred_eval()
{
    std::printf("- deferred_eval_process_then_resolve\n");
    static constexpr Route routes[] = { { M0, D0, 0, 1 } };
    float d[2][4]; grid(d, 0.2f,0,0,0, 0,0,0,0);
    MC mc; mc.Init(d, routes, 1);
    NEAR(mc.Out(D0), 0.2f);                  // Init resolves once

    // Move the knob with do_eval=false: grid updates (pickup) but nodes are stale.
    float k[4] = {0.2f,0,0,0};
    mc.Process(k, false, /*do_eval=*/false); // catch at default
    k[0] = 0.8f;
    mc.Process(k, false, /*do_eval=*/false); // grid now 0.8, node still 0.2
    NEAR(mc.Value(0, 0), 0.8f);
    NEAR(mc.Out(D0),     0.2f);              // not refreshed yet

    mc.Resolve();                            // explicit off-thread refresh
    NEAR(mc.Out(D0),     0.8f);
}

static void test_alias_compat()
{
    std::printf("- pagedcontrols_alias_no_routing\n");
    PagedControls<2, 4> pg;            // alias -> PagedMacroControls<2,4,8>
    const float d[2][4] = {{0.1f,0.2f,0.3f,0.4f},{0.5f,0.6f,0.7f,0.8f}};
    pg.Init(d, 0.03f);                 // legacy Init signature
    NEAR(pg.Value(0, 2), 0.3f);
    CHECK(pg.Page() == 0);
    // With no derived nodes, Out() mirrors the flattened grid (page-major).
    NEAR(pg.Out(0), 0.1f);
    NEAR(pg.Out(5), 0.6f);
}

int main()
{
    std::printf("PagedMacroControls macro-layer tests\n");
    test_window_and_inversion();
    test_ops();
    test_curves();
    test_virtual_chain();
    test_fanout_and_live_update();
    test_deferred_eval();
    test_alias_compat();
    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_fail);
    if (g_fail == 0) std::printf("ALL MACRO-LAYER TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
