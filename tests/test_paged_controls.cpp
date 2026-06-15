// Host tests for PagedControls (pure paging + soft-takeover + LED logic).
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

using PC = pagedctl::PagedControls<3, 4>;

static void test_defaults_seeded()
{
    std::printf("- defaults_seeded\n");
    const float d[3][4] = {{0.1f,0.2f,0.3f,0.4f},
                           {0.5f,0.6f,0.7f,0.8f},
                           {0.9f,0.0f,0.1f,0.2f}};
    PC pc; pc.Init(d);
    NEAR(pc.Value(0,0), 0.1f); NEAR(pc.Value(1,2), 0.7f); NEAR(pc.Value(2,0), 0.9f);
    CHECK(pc.Page() == 0);
}

static void test_soft_takeover()
{
    std::printf("- soft_takeover_pickup\n");
    const float d[3][4] = {{0.5f,0.5f,0.5f,0.5f},{0,0,0,0},{0,0,0,0}};
    PC pc; pc.Init(d, 0.03f);

    float far[4] = {0.9f,0.9f,0.9f,0.9f};
    pc.Process(far, false);
    NEAR(pc.Value(0,0), 0.5f);           // too far -> not caught, stays at default
    CHECK(!pc.PickedUp(0));

    float near_[4] = {0.51f,0.51f,0.51f,0.51f};
    pc.Process(near_, false);
    CHECK(pc.PickedUp(0));
    NEAR(pc.Value(0,0), 0.51f);          // within threshold -> catches

    float moved[4] = {0.8f,0.8f,0.8f,0.8f};
    pc.Process(moved, false);
    NEAR(pc.Value(0,0), 0.8f);           // now tracks freely
}

static void test_pages_independent_and_reset()
{
    std::printf("- pages_independent_and_pickup_resets\n");
    const float d[3][4] = {{0.5f,0,0,0},{0.5f,0,0,0},{0.5f,0,0,0}};
    PC pc; pc.Init(d, 0.03f);

    // Catch knob0 on page 0, track to 0.7.
    float k[4] = {0.5f,0,0,0}; pc.Process(k, false);
    k[0] = 0.7f; pc.Process(k, false);
    NEAR(pc.Value(0,0), 0.7f);

    // Advance to page 1; knob far from page-1 stored (0.5) must NOT jump it.
    pc.Process(k, true);                 // advance_page
    CHECK(pc.Page() == 1);
    NEAR(pc.Value(1,0), 0.5f);           // page 1 slot untouched (pickup reset)
    NEAR(pc.Value(0,0), 0.7f);           // page 0 slot preserved

    // Catch on page 1.
    float k1[4] = {0.5f,0,0,0}; pc.Process(k1, false);
    k1[0] = 0.2f; pc.Process(k1, false);
    NEAR(pc.Value(1,0), 0.2f);
    NEAR(pc.Value(0,0), 0.7f);           // page 0 still preserved
}

static void test_page_cycles()
{
    std::printf("- page_advance_cycles\n");
    const float d[3][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0}};
    PC pc; pc.Init(d);
    float k[4] = {0,0,0,0};
    CHECK(pc.Page() == 0);
    pc.Process(k, true); CHECK(pc.Page() == 1);
    pc.Process(k, true); CHECK(pc.Page() == 2);
    pc.Process(k, true); CHECK(pc.Page() == 0); // wraps
}

static void test_led_counts_page()
{
    std::printf("- led_flashes_page_number\n");
    const float d[3][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0}};
    const PC::LedTiming t = {15, 40, 110};
    for (size_t page = 0; page < 3; ++page)
    {
        PC pc; pc.Init(d, 0.03f, t);
        pc.GoToPage(page);
        const uint32_t cycle = (uint32_t)(page + 1) * t.period_ticks + t.rest_ticks;
        // LedOn is pure in the external tick. Over exactly one cycle each tick value
        // in [0,cycle) occurs once, so the lit count is phase-independent:
        // (page+1) pulses * on_ticks each.
        int lit = 0;
        for (uint32_t tick = 0; tick < cycle; ++tick)
            if (pc.LedOn(tick)) ++lit;
        CHECK(lit == (int)((page + 1) * t.on_ticks));
    }
}

static void test_save_load_grid()
{
    std::printf("- save_load_grid_roundtrip\n");
    const float d[3][4] = {{0.1f,0.2f,0.3f,0.4f},{0.5f,0.6f,0.7f,0.8f},{0.9f,0.1f,0.2f,0.3f}};
    PC a; a.Init(d);
    float grid[PC::GridSize()];
    a.SaveGrid(grid);
    const float z[3][4] = {{0,0,0,0},{0,0,0,0},{0,0,0,0}};
    PC b; b.Init(z);
    b.LoadGrid(grid);
    for (size_t p = 0; p < 3; ++p)
        for (size_t kk = 0; kk < 4; ++kk)
            NEAR(b.Value(p,kk), a.Value(p,kk));
}

int main()
{
    std::printf("PagedControls tests\n");
    test_defaults_seeded();
    test_soft_takeover();
    test_pages_independent_and_reset();
    test_page_cycles();
    test_led_counts_page();
    test_save_load_grid();
    std::printf("\n%d check(s), %d failure(s)\n", g_checks, g_fail);
    if (g_fail == 0) std::printf("ALL PAGEDCONTROLS TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
