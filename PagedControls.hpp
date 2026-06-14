#pragma once
#ifndef PAGED_CONTROLS_H
#define PAGED_CONTROLS_H

#include <cstddef>
#include <cstdint>

// PagedControls - reusable "parameter pages + soft-takeover knob pickup" logic.
// Template params: NumPages (>=1), NumKnobs (>=1, default 4).
namespace pagedctl {

template <size_t NumPages, size_t NumKnobs = 4>
class PagedControls
{
    static_assert(NumPages >= 1, "need at least one page");
    static_assert(NumKnobs >= 1, "need at least one knob");

public:
    // LED count-flash timing, in processing-block units (the rate at which you
    // call Process). Page p emits (p+1) pulses then rests, so you can count the
    // flashes. Tune to your block rate; defaults suit ~150-200 Hz blocks.
    struct LedTiming
    {
        uint32_t on_blocks;     // pulse "lit" duration
        uint32_t period_blocks; // pulse on+off duration (>= on_blocks)
        uint32_t rest_blocks;   // gap after the count, before repeating
    };

    // Seed every page's knob slots with defaults[page][knob]. `pickup_threshold`
    // is the soft-takeover catch distance (fraction of 0..1 travel).
    void Init(const float (&defaults)[NumPages][NumKnobs],
              float pickup_threshold = 0.03f,
              LedTiming led = {15, 40, 110})
    {
        pickup_ = pickup_threshold;
        led_    = led;
        page_   = 0;
        blink_  = 0;
        for (size_t p = 0; p < NumPages; ++p)
            for (size_t k = 0; k < NumKnobs; ++k)
                slot_[p][k] = defaults[p][k];
        for (size_t k = 0; k < NumKnobs; ++k) picked_[k] = false;
    }

    // Call once per block. `knobs` is NumKnobs values in 0..1; `advance_page` is
    // the (already-debounced) button-tap edge. On a page change the pickup is
    // reset so knobs must be re-caught on the new page.
    void Process(const float* knobs, bool advance_page)
    {
        if (advance_page) GoToPage((page_ + 1) % NumPages);

        for (size_t k = 0; k < NumKnobs; ++k)
            if (PickedUp(k, knobs[k])) slot_[page_][k] = knobs[k];

        const uint32_t cycle = CycleBlocks();
        if (++blink_ >= cycle) blink_ = 0;
    }

    // Jump directly to a page (e.g. restoring UI state); resets pickup + blink.
    void GoToPage(size_t page)
    {
        if (page >= NumPages) return;
        page_  = page;
        blink_ = 0;
        for (size_t k = 0; k < NumKnobs; ++k) picked_[k] = false;
    }

    // ---- Accessors ----
    float  Value(size_t page, size_t knob) const { return slot_[page][knob]; }
    float  Active(size_t knob)             const { return slot_[page_][knob]; }
    size_t Page()                          const { return page_; }
    bool   PickedUp(size_t knob)           const { return picked_[knob]; }

    // LED state for this block: page p blinks (p+1) times, then rests.
    bool LedOn() const
    {
        const uint32_t np = (uint32_t)page_ + 1;
        const uint32_t c  = blink_ % CycleBlocks();
        if (c < np * led_.period_blocks)
            return (c % led_.period_blocks) < led_.on_blocks;
        return false;
    }

    // Optional persistence: copy the whole grid out / in (e.g. to flash). The
    // destination/source must be NumPages*NumKnobs floats.
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
    }

    static constexpr size_t kNumPages = NumPages;
    static constexpr size_t kNumKnobs = NumKnobs;
    static constexpr size_t GridSize() { return NumPages * NumKnobs; }

private:
    uint32_t CycleBlocks() const
    {
        return (uint32_t)(page_ + 1) * led_.period_blocks + led_.rest_blocks;
    }

    bool PickedUp(size_t knob, float physical)
    {
        if (picked_[knob]) return true;
        float d = physical - slot_[page_][knob];
        if (d < 0.0f) d = -d;
        if (d < pickup_) { picked_[knob] = true; return true; }
        return false;
    }

    float     slot_[NumPages][NumKnobs] = {};
    bool      picked_[NumKnobs]         = {};
    size_t    page_   = 0;
    uint32_t  blink_  = 0;
    float     pickup_ = 0.03f;
    LedTiming led_    = {15, 40, 110};
};

} // namespace pagedctl

#endif // PAGED_CONTROLS_H
