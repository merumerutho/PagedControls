// Host tests for the local extensions: crossing pickup, knob lock, SetValue,
// and the standalone MacroMap.
#include "../PagedControls.hpp"
#include <cstdio>
#include <cmath>

static int g_checks = 0, g_fail = 0;
static void check(bool c, const char* e, int line)
{
    ++g_checks;
    if (!c) { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, line, e); }
}
#define CHECK(c) check((c), #c, __LINE__)
#define NEAR(a, b) check(std::fabs((a) - (b)) < 1e-6f, #a " ~= " #b, __LINE__)

using PC = pagedctl::PagedControls<3, 3>;

static void test_crossing_pickup()
{
    std::printf("- crossing_pickup\n");
    const float d[3][3] = {{0.5f,0.5f,0.5f},{0,0,0},{0,0,0}};
    PC pc; pc.Init(d, 0.03f);

    float lo[3] = {0.2f,0.2f,0.2f};
    pc.Process(lo, false);
    CHECK(!pc.PickedUp(0));
    float hi[3] = {0.8f,0.8f,0.8f};          // jumps over the 0.03 window
    pc.Process(hi, false);
    CHECK(pc.PickedUp(0));
    NEAR(pc.Value(0,0), 0.8f);

    // The first reading after Init has no history: a far value never picks up.
    PC fresh; fresh.Init(d, 0.03f);
    fresh.Process(hi, false);
    CHECK(!fresh.PickedUp(0));
    // Moving further away on the same side never picks up either.
    float higher[3] = {0.9f,0.9f,0.9f};
    fresh.Process(higher, false);
    CHECK(!fresh.PickedUp(0));
    NEAR(fresh.Value(0,0), 0.5f);
}

static void test_crossing_after_page_change()
{
    std::printf("- crossing_after_page_change\n");
    const float d[3][3] = {{0.5f,0.5f,0.5f},{0.3f,0.3f,0.3f},{0,0,0}};
    PC pc; pc.Init(d, 0.03f);
    float k[3] = {0.5f,0.5f,0.5f};
    pc.Process(k, false);                    // caught on page 0
    pc.GoToPage(1);
    CHECK(!pc.PickedUp(0));
    float down[3] = {0.1f,0.1f,0.1f};        // 0.5 -> 0.1 crosses stored 0.3
    pc.Process(down, false);
    CHECK(pc.PickedUp(0));
    NEAR(pc.Value(1,0), 0.1f);
    NEAR(pc.Value(0,0), 0.5f);               // page 0 untouched
}

static void test_knob_lock()
{
    std::printf("- knob_lock\n");
    const float d[3][3] = {{0.5f,0.5f,0.5f},{0.5f,0.5f,0.5f},{0.5f,0.5f,0.5f}};
    PC pc; pc.Init(d, 0.03f);
    pc.LockKnob(2, 2);                       // knob 2 always drives page 2
    CHECK(pc.KnobLock(2) == 2);

    float k[3] = {0.5f,0.5f,0.5f};
    pc.Process(k, false);
    float m[3] = {0.7f,0.7f,0.7f};
    pc.Process(m, false);
    NEAR(pc.Value(0,2), 0.5f);               // page 0 knob 2 not written
    NEAR(pc.Value(2,2), 0.7f);               // locked page written
    NEAR(pc.Value(0,0), 0.7f);               // unlocked knob follows page 0

    pc.GoToPage(1);
    CHECK(pc.PickedUp(2));                   // locked knob keeps its pickup
    CHECK(!pc.PickedUp(0));
    NEAR(pc.Active(2), 0.7f);                // Active reports the locked slot

    pc.LockKnob(2, PC::kNoLock);             // unlock: follows page 1 again
    CHECK(!pc.PickedUp(2));
    NEAR(pc.Active(2), 0.5f);
}

static void test_set_value()
{
    std::printf("- set_value\n");
    const float d[3][3] = {{0.5f,0.5f,0.5f},{0,0,0},{0,0,0}};
    PC pc; pc.Init(d, 0.03f);
    float k[3] = {0.5f,0.5f,0.5f};
    pc.Process(k, false);
    float m[3] = {0.9f,0.9f,0.9f};
    pc.Process(m, false);
    CHECK(pc.PickedUp(1));

    pc.SetValue(0, 1, 0.5f);                 // reset to centre: must re-catch
    NEAR(pc.Value(0,1), 0.5f);
    CHECK(!pc.PickedUp(1));
    CHECK(pc.PickedUp(0));                   // other knobs unaffected
    pc.Process(m, false);
    NEAR(pc.Value(0,1), 0.5f);               // knob still far away: no jump

    pc.SetValue(2, 0, 0.1f);                 // slot on another page: pickup kept
    CHECK(pc.PickedUp(0));
}

static void test_standalone_macro_map()
{
    std::printf("- standalone_macro_map\n");
    using namespace pagedctl;
    // One knob -> "level, then feedback" with an input window per half.
    static constexpr Route kRoutes[] = {
        { 0, 1, 0, 1, 0.0f, 0.5f },
        { 0, 2, 0, 0.9f, 0.5f, 1.0f },
    };
    MacroMap<3> mm;
    mm.Init(kRoutes, 2);
    float in = 0.25f;
    mm.Evaluate(&in, 1);
    NEAR(mm.Out(1), 0.5f);
    NEAR(mm.Out(2), 0.0f);
    in = 0.75f;
    mm.Evaluate(&in, 1);
    NEAR(mm.Out(1), 1.0f);
    NEAR(mm.Out(2), 0.45f);
}

int main()
{
    test_crossing_pickup();
    test_crossing_after_page_change();
    test_knob_lock();
    test_set_value();
    test_standalone_macro_map();
    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_fail);
    if (g_fail == 0) std::printf("ALL EXTENSION TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
