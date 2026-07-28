///|/ Copyright (c) Prusa Research 2020 - 2022 Vojtěch Bubník @bubnikv, Oleksandra Iushchenko @YuSanka, Enrico Turri @enricoturri1966, Lukáš Matěna @lukasmatena
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_GUI_DoubleSliderForGcode_hpp_
#define slic3r_GUI_DoubleSliderForGcode_hpp_

#include "ImGuiDoubleSlider.hpp"

namespace DoubleSlider {

class DSForGcode : public Manager<unsigned int>
{
public:
    DSForGcode() : Manager<unsigned int>() {}
    DSForGcode( int lowerPos,
                int higherPos,
                int minPos,
                int maxPos) 
    {
        Init(lowerPos, higherPos, minPos, maxPos, "moves_slider", true);
    }
    ~DSForGcode() {}

    void Render(const int canvas_width, const int canvas_height, float extra_scale = 1.f, float offset = 0.f) override;

    void set_render_as_disabled(bool value) { m_render_as_disabled = value; }
    bool is_rendering_as_disabled() const   { return m_render_as_disabled; }

protected:
    // SLA sentinel positions carry value 0 (not a real gcode index).
    // The alternate_values slot holds a 1-based per-layer step counter so
    // multiple SLA channels on the same layer get distinct labels:
    //   step 1 → "SLA",  step 2 → "SLA2",  step 3 → "SLA3", …
    std::string get_label(int pos) const override {
        if (!m_values.empty() && pos >= 0 && pos < (int)m_values.size()
                && m_values[pos] == 0u) {
            const unsigned int step = (!m_alternate_values.empty()
                                       && pos < (int)m_alternate_values.size())
                                      ? m_alternate_values[pos] : 1u;
            return step <= 1u ? "SLA" : "SLA" + std::to_string(step);
        }
        return Manager<unsigned int>::get_label(pos);
    }

private:

    bool        m_render_as_disabled{ false };
};

} // DoubleSlider;


#endif // slic3r_GUI_DoubleSliderForGcode_hpp_
