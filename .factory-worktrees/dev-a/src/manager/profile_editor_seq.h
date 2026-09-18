/*
 * profile_editor_seq.h — Sequential binding mode for the profile editor
 * (Task 38, right panel / diagram).
 *
 * Mode 2 — Sequential binding.  The editor prompts for each button in
 * order; the diagram lights up the button currently being mapped.
 * Press a physical button -> captured -> auto-advance.  B skips,
 * Start cancels.  A progress bar shows completion.
 *
 * This module operates on the shared cbx_profile_editor struct from
 * profile_editor_list.h.  The diagram, binding list, status label,
 * and progress bar are shared between list mode and sequential mode.
 *
 * Task 38 — Profile editor — sequential binding mode and validation.
 */
#ifndef CBX_PROFILE_EDITOR_SEQ_H
#define CBX_PROFILE_EDITOR_SEQ_H

/*
 * The sequential mode API is declared in profile_editor_list.h
 * (cbx_profile_editor_begin_sequential, etc.) to keep the editor's
 * public interface in one place.  This header exists so that the
 * implementation file can be compiled separately.
 *
 * Include profile_editor_list.h for the API.
 */
#include "manager/profile_editor_list.h"

#endif /* CBX_PROFILE_EDITOR_SEQ_H */