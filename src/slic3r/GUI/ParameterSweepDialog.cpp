#include "ParameterSweepDialog.hpp"

#include <fstream>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/spinctrl.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ParameterSweep.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

namespace {

struct Remembered
{
    int    subject { 0 }, count { 6 };
    double start { 0.6 }, step { 0.2 }, spacing { 5. };
    bool   label { true }, csv { true };
};
Remembered s_last;

} // namespace

ParameterSweepDialog::ParameterSweepDialog(wxWindow *parent)
    : ParamDialog(parent, _L("Generate Parameter Sweep"), _L("Place on bed"))
{
    m_subject = add_choice(_L("Vary:"), { _L("Wall thickness (mm)"), _L("Channel diameter (mm)"), _L("Distance between two channels (mm)"),
                                          _L("Droplet volume (nL)") }, s_last.subject);
    m_start   = add_value(_L("Value of the first copy"), _L("mm or nL"), 0.05, 100000., s_last.start, 0.1);
    m_step    = add_value(_L("Change per copy"), _L("mm or nL"), -1000., 1000., s_last.step, 0.05);
    m_count   = add_count(_L("Number of copies"), 1, 100, s_last.count);
    m_spacing = add_value(_L("Space between copies"), _L("mm"), 0., 50., s_last.spacing, 1., 1);
    m_label   = add_check(_L("Engrave the number of each copy"), s_last.label);
    m_csv     = add_check(_L("Save a table of the copies (CSV) after placing"), s_last.csv);
    add_note(_L("Every copy is a small separate object numbered 1, 2, 3 and so on. The table lists the number, the value, the position on "
                "the bed and the size of each copy. Droplet volumes need a droplet channel in the printer."));
    finish_layout();
}

bool ParameterSweepDialog::place()
{
    s_last = { m_subject->GetSelection(), m_count->GetValue(), m_start->GetValue(), m_step->GetValue(), m_spacing->GetValue(),
               m_label->GetValue(), m_csv->GetValue() };

    const DynamicPrintConfig &cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const BoundingBoxf bed(cfg.option<ConfigOptionPoints>("bed_shape")->values);

    SweepParams params;
    params.subject       = SweepSubject(s_last.subject);
    params.start         = s_last.start;
    params.step          = s_last.step;
    params.count         = s_last.count;
    params.spacing       = s_last.spacing;
    params.engrave_index = s_last.label;
    if (bed.defined)
        params.plate_size = Vec2d(bed.size().x(), bed.size().y());

    int droplet_extruder = 0;
    if (params.subject == SweepSubject::DropletVolume) {
        droplet_extruder = first_droplet_channel();
        if (droplet_extruder == 0) {
            set_status(_L("This printer has no droplet channel. Add one in the printer settings first."));
            return false;
        }
    }

    const SweepResult sweep = make_parameter_sweep(params);
    if (! sweep.valid()) {
        set_status(wxString::FromUTF8(sweep.error));
        return false;
    }

    Model model;
    const Vec2d center = bed.defined ? Vec2d(bed.center()) : Vec2d::Zero();
    add_sweep_to_model(model, sweep, center, droplet_extruder);

    Plater *plater = wxGetApp().plater();
    {
        Plater::TakeSnapshot snapshot(plater, _L("Add parameter sweep"));
        const std::vector<size_t> added = plater->add_model_objects(model.objects);
        // Placing alone arranges the copies; the sweep puts each at its own place.
        for (size_t i = 0; i < added.size() && i < sweep.items.size(); ++i)
            if (added[i] < plater->model().objects.size()) {
                for (ModelInstance *instance : plater->model().objects[added[i]]->instances)
                    instance->set_offset(Vec3d(center.x() + sweep.items[i].offset.x(), center.y() + sweep.items[i].offset.y(), instance->get_offset().z()));
                plater->changed_object(int(added[i]));
            }
    }

    if (s_last.csv) {
        wxFileDialog save(this, _L("Save the table of the copies"), wxEmptyString, "parameter_sweep.csv", "CSV (*.csv)|*.csv",
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (save.ShowModal() == wxID_OK) {
            std::ofstream out(save.GetPath().ToUTF8().data(), std::ios::binary);
            out << sweep.csv;
        }
    }
    return true;
}

} // namespace GUI
} // namespace Slic3r
