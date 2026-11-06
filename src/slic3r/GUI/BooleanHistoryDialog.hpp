#ifndef slic3r_GUI_BooleanHistoryDialog_hpp_
#define slic3r_GUI_BooleanHistoryDialog_hpp_

#include <vector>

#include <wx/dialog.h>

#include "libslic3r/SolidEdit.hpp"

class wxCheckBox;
class wxChoice;

namespace Slic3r {
namespace GUI {

// Shows the chain of booleans that made a part (base, then each step) and lets the user switch steps off
// or change their operation. The part is rebuilt from the edited chain by the caller.
class BooleanHistoryDialog : public wxDialog
{
public:
    BooleanHistoryDialog(wxWindow *parent, const SolidEdit::History &history);

    // The history with the user's edits applied.
    SolidEdit::History get_history() const;

private:
    SolidEdit::History      m_history;
    std::vector<wxCheckBox*> m_enabled;
    std::vector<wxChoice*>   m_op;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_BooleanHistoryDialog_hpp_
