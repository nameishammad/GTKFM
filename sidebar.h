#ifndef SIDEBAR_H
#define SIDEBAR_H

#include "app.h"

GtkWidget *sidebar_create(AppWidgets *app);

/* Bookmark the given folder (Ctrl+D shortcut in main.c calls this).
 * Ignored silently if path is not a directory or is already bookmarked. */
void sidebar_add_bookmark(AppWidgets *app, const char *path);

/* Refreshes the Trash sidebar row's icon (empty vs full). Called from
 * fileview.c whenever an operation could have changed trash contents. */
void sidebar_update_trash_icon(AppWidgets *app);

/* Highlights whichever sidebar row (Places/Bookmarks/Devices) matches
 * app->current_dir, or clears the sidebar selection if none match. Call
 * this after any navigation (Back/Forward/Up/Backspace/breadcrumb/etc.)
 * so the sidebar highlight never stays stuck on a folder you've left. */
void sidebar_sync_selection(AppWidgets *app);

/* Shared MTP/gphoto2 "phone" detection, also used by fileview.c's
 * "computer://" view so both places dedupe a phone's duplicate
 * volume+mount entries the same way. See sidebar.c for details. */
gboolean gtkfm_looks_like_phone(GVolume *volume, GMount *mount);
gboolean gtkfm_is_generic_phone_label(const char *name);

#endif /* SIDEBAR_H */
