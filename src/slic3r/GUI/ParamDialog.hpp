#pragma once

#include <vector>
#include <wx/dialog.h>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxFlexGridSizer;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;

namespace Slic3r {
namespace GUI {

// A dialog of labelled number fields with units, a status line and a button that places the result on the bed.
// The generators behind it live in libslic3r; a derived dialog only reads its fields and calls them.
class ParamDialog : public wxDialog
{
public:
    ParamDialog(wxWindow *parent, const wxString &title, const wxString &place_label);

protected:
    wxChoice         *add_choice(const wxString &label, const std::vector<wxString> &items, int selection);
    // A length in millimetres, or another unit; the unit is shown in the label.
    wxSpinCtrlDouble *add_value(const wxString &label, const wxString &unit, double min, double max, double value, double step, int digits = 2);
    wxSpinCtrl       *add_count(const wxString &label, int min, int max, int value);
    wxCheckBox       *add_check(const wxString &label, bool value);
    wxStaticText     *add_note(const wxString &text);
    void              set_status(const wxString &text);
    // Call once all fields are added.
    void              finish_layout();

    // Reads the fields, makes the objects and adds them to the plater. Returns false, after showing why in the status
    // line, to keep the dialog open.
    virtual bool      place() = 0;

private:
    wxFlexGridSizer *m_grid { nullptr };
    wxStaticText    *m_status { nullptr };
    wxButton        *m_place { nullptr };
};

} // namespace GUI
} // namespace Slic3r
