#ifndef BEER_MESSAGE_DIALOG_H
#define BEER_MESSAGE_DIALOG_H

/* A modal host window for the dialogs Windows shows on behalf of a program:
 * MessageBox, TaskDialogIndirect, FatalAppExit and NtRaiseHardError. It shows
 * the title, the (word-wrapped) text and one button per entry of `ids`/`labels`.
 * It returns the id of the button chosen with the mouse, Tab/arrows + Enter, or
 * Escape/close (the `cancel_index` button). It returns -1 when no dialog can be
 * shown: no X display, libX11 missing, or BEER_DIALOGS=0 for unattended runs.
 * Callers then answer as if the default button had been chosen. */
int beer_message_dialog(const char *title, const char *text, const int *ids,
                        const char *const *labels, int count, int default_index,
                        int cancel_index);

/* MessageBox semantics for a MB_* type: buttons from the low four bits
 * (MB_OK, MB_OKCANCEL, MB_ABORTRETRYIGNORE, MB_YESNOCANCEL, MB_YESNO,
 * MB_RETRYCANCEL, MB_CANCELTRYCONTINUE) and the default from MB_DEFBUTTONn.
 * Returns the IDOK/IDCANCEL/... value, or -1 when no dialog can be shown. */
int beer_message_box_dialog(const char *title, const char *text, unsigned type);

#endif
