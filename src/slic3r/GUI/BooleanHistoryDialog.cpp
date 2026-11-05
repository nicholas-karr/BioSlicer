#include "BooleanHistoryDialog.hpp"

#include "I18N.hpp"
#include "GUI.hpp"

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {

BooleanHistoryDialog::BooleanHistoryDialog(wxWindow *parent, const SolidEdit::History &history)
    : wxDialog(parent, wxID_ANY, _L("Boolean history"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_history(history)
{
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(this, wxID_ANY,
        _L("The part was built from a base shape and the steps below, applied from top to bottom.\n"
           "Switch a step off or change its operation, then press OK to rebuild the part.")), 0, wxALL, 8);
    sizer->Add(new wxStaticText(this, wxID_ANY, _L("Base shape") + ": " + from_u8(history.base_name)), 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

    auto *grid = new wxFlexGridSizer(2, 6, 12);
    for (const SolidEdit::HistoryStep &step : history.steps) {
        auto *enabled = new wxCheckBox(this, wxID_ANY, from_u8(step.name));
        enabled->SetValue(step.enabled);
        auto *op = new wxChoice(this, wxID_ANY);
        op->Append(_L("Union"));
        op->Append(_L("Subtract"));
        op->Append(_L("Intersect"));
        op->SetSelection(int(step.op));
        grid->Add(enabled, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(op, 0, wxEXPAND);
        m_enabled.push_back(enabled);
        m_op.push_back(op);
    }
    sizer->Add(grid, 0, wxALL, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, 8);
    SetSizerAndFit(sizer);
}

SolidEdit::History BooleanHistoryDialog::get_history() const
{
    SolidEdit::History h = m_history;
    for (size_t i = 0; i < h.steps.size(); ++ i) {
        h.steps[i].enabled = m_enabled[i]->GetValue();
        h.steps[i].op      = SolidEdit::BooleanOp(m_op[i]->GetSelection());
    }
    return h;
}

}} // namespace Slic3r::GUI
