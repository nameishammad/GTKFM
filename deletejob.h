#ifndef DELETEJOB_H
#define DELETEJOB_H

#include "app.h"

/* Permanently deletes `paths` (each item may be a file or a whole directory
 * tree) on a background thread, showing a small progress dialog with a
 * Cancel button. Fixes the UI freeze that large/many-item permanent deletes
 * used to cause (which was long enough for the desktop's window manager to
 * offer to force-quit/terminate the app).
 *
 * Takes ownership of `paths` and `post_target` (frees both when done).
 * `post_target` (may be NULL) is the item to re-select afterwards.
 * For each successfully deleted item, its "<name>.trashinfo" sidecar (if
 * any, i.e. the item was sitting in Trash) is also removed. */
void deletejob_start_permanent(AppWidgets *app, GList *paths,
                                char *post_target);

/* Same background-thread delete, used for "Empty Trash": deletes every path
 * in `paths` (the top-level entries of the trash files/ and info/ dirs, for
 * every trash location), no per-item .trashinfo cleanup or re-selection. */
void deletejob_start_empty_trash(AppWidgets *app, GList *paths);

/* Moves `paths` to Trash (g_file_trash per top-level item) on a background
 * thread, showing the same small progress dialog as the other jobs. This is
 * the regular "Delete" / "Move to Trash" action. Needed because g_file_trash
 * has to actually copy the bytes (not just rename) when Trash lives on a
 * different filesystem than the item, which for a large (e.g. 1TB+) item can
 * take long enough that running it on the main thread freezes the UI and
 * the window manager offers to force-quit/terminate the app.
 *
 * Takes ownership of `paths` and `post_target` (frees both when done).
 * `post_target` (may be NULL) is the item to re-select afterwards. */
void deletejob_start_trash(AppWidgets *app, GList *paths, char *post_target);

#endif /* DELETEJOB_H */
