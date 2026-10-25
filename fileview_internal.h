#ifndef FILEVIEW_INTERNAL_H
#define FILEVIEW_INTERNAL_H

/* fileview.c ko 4 files (fileview-core.c, fileview-fileops.c,
 * fileview-actions.c, fileview-ui.c) mein split kiya gaya hai. Ye header
 * un chaaron ke beech shared cheezein rakhta hai: common includes/defines,
 * TrashLoc typedef, clipboard globals, aur wo helper functions jo ek file
 * mein define hote hain lekin doosri file(s) mein use hote hain (isliye
 * unhe "static" nahi rakha ja sakta). */

#include "fileview.h"
#include "copyjob.h"
#include "compressjob.h"
#include "sidebar.h"
#include "thumbnail.h"
#include "infopanel.h"
#include "terminal.h"
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define THUMB_SIZE 60

#define ICON_SIZE_MIN 11
#define ICON_SIZE_MAX 128
#define ICON_SIZE_STEP 11

#define ITEM_WIDTH_PAD 0

/* Thumbnail-load-hone-ke-baad callback ke liye context. fileview-core.c
 * (thumbnail_ready_cb) aur fileview-ui.c (search results, raster refresh)
 * dono ismein instances allocate/consume karte hain. */
typedef struct {
  AppWidgets *app;
  GtkTreeRowReference *ref;
  guint generation;
  gboolean is_symlink;
  gboolean is_readonly;
} ThumbCtx;

/* Ek file ke liye "readonly lock" emblem dikhana chahiye ya nahi -- sirf
 * access::can-write == FALSE hona kaafi nahi hai (fileview-core.c mein
 * definition ke paas iski poori wajah likhi hai). Definition
 * fileview-core.c mein hai, fileview-ui.c (search results) bhi ise use
 * karta hai. */
gboolean file_info_should_show_readonly_lock(GFileInfo *info);

/* Trash ke ek "location" (files/ + info/ dir pair) ko represent karta hai.
 * fileview-core.c aur fileview-fileops.c dono ismein use karte hain. */
typedef struct {
  char *files_dir;
  char *info_dir;
} TrashLoc;

/* Drag-and-drop target list -- fileview-core.c mein define hoti hai,
 * fileview-actions.c aur fileview-ui.c dono drag/drop setup ke liye
 * isko use karte hain. */
enum { DND_TARGET_URI_LIST = 1 };
extern const GtkTargetEntry dnd_targets[1];

/* Copy/Cut/Paste clipboard state -- fileview-core.c mein define hoti hai,
 * fileview-actions.c (context menu) aur fileview-ui.c (copy/cut/paste)
 * dono isko access karte hain. */
extern GList *g_clipboard_paths;
extern gboolean g_clipboard_cut;
extern AppWidgets *g_clipboard_source_app;

/* ---- fileview-core.c mein defined, doosri files mein use hote hain ---- */
void set_status(AppWidgets *app, const char *msg);
GdkPixbuf *themed_icon_for(const char *content_type, gboolean is_dir,
                           const char *path, int size);
GdkPixbuf *icon_for_gicon_sized(GIcon *icon, int size);
GdkPixbuf *icon_for_name_sized(const char *name, int size);
GdkPixbuf *add_symlink_emblem(GdkPixbuf *base);
GdkPixbuf *add_readonly_emblem(GdkPixbuf *base);
void clear_file_store(AppWidgets *app);
void focus_first_item(AppWidgets *app);
char *capture_post_delete_selection_target(AppWidgets *app);
void reattach_file_model(AppWidgets *app);
void populate(AppWidgets *app);
void mount_computer_volume(AppWidgets *app, GVolume *volume);
void fileview_load_settings(AppWidgets *app);
gboolean set_custom_folder_icon(const char *dir_path, const char *icon_value);
char *get_custom_folder_icon(const char *dir_path);
GList *uri_list_to_paths(GtkSelectionData *data);

/* ---- fileview-fileops.c mein defined, doosri files mein use hote hain ---- */
char *trash_files_dir(void);
char *trash_info_dir(void);
void trash_loc_free(TrashLoc *loc);
GList *all_trash_locations(gboolean only_with_entries);
gboolean is_trash_dir(AppWidgets *app);
gboolean delete_recursive(GFile *f, GError **error);
char *info_dir_for_files_dir(const char *files_dir);
gboolean path_is_desktop_file(const char *path);
gboolean path_is_executable(const char *path);
gboolean run_desktop_file(AppWidgets *app, const char *path);
void run_executable_file(AppWidgets *app, const char *path);
void open_path(AppWidgets *app, const char *path, gboolean is_dir);
void show_app_chooser(AppWidgets *app, const char *path,
                       gboolean set_default);
void open_terminal_here(const char *dir_path);
void show_rename_dialog(AppWidgets *app, const char *path,
                         const char *current_name);
void show_edit_launcher_dialog(AppWidgets *app, const char *path);
void confirm_delete(AppWidgets *app, const char *path, const char *name);
void confirm_delete_permanent(AppWidgets *app, const char *path,
                               const char *name);
void confirm_empty_trash(AppWidgets *app);
void count_recursive(GFile *f, guint64 *total_size, guint *count);
void do_restore(AppWidgets *app, const char *item_path,
                 const char *item_name);
void do_restore_multiple(AppWidgets *app, GList *paths);
/* Trash se ek item ko uski asal jagah par restore karta hai. Ownership/UI
 * side-effects (status, refresh) caller ki zimmedari hain -- ye sirf move +
 * trashinfo cleanup karta hai. undo.c UNDO_DELETE ke liye isay seedha
 * istemal karta hai. */
gboolean do_restore_one(const char *item_path, char **out_err);
void show_properties_dialog(AppWidgets *app, const char *path,
                             const char *name, gboolean is_dir);
void show_properties_dialog_multi(AppWidgets *app, GList *paths);
void show_drive_properties_dialog(AppWidgets *app, const char *path,
                                  const char *name, gboolean is_removable);
void new_folder_dialog(AppWidgets *app);
gboolean has_archive_extension(const char *path);
void do_extract_archive(AppWidgets *app, const char *archive_path);
char *preferred_archive_extension(void);

void thumbnail_ready_cb(const char *filepath, GdkPixbuf *pixbuf,
                        gpointer user_data);

/* on_open_with_app aur new_file_type_activate fileview-fileops.c mein
 * defined hain, lekin context menu banane wala code (fileview-actions.c)
 * inhe G_CALLBACK() ke zariye signal handlers ke tor par connect karta hai. */
void on_open_with_app(GtkMenuItem *item, gpointer user_data);
void new_file_type_activate(GtkMenuItem *mi, gpointer user_data);

/* ---- fileview-ui.c mein defined, fileview-actions.c mein use hota hai ---- */
GList *get_selected_paths(AppWidgets *app);

/* ---- fileview-actions.c mein defined, fileview-ui.c (fileview_create)
 * mein signal handlers/sort-func ke tor par connect hote hain ---- */
gint file_sort_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                     gpointer data);
void on_item_activated(GtkIconView *iv, GtkTreePath *path,
                        gpointer user_data);
gboolean on_iconview_key_press(GtkWidget *widget, GdkEventKey *event,
                                gpointer user_data);
gboolean on_iconview_motion(GtkWidget *widget, GdkEventMotion *event,
                             gpointer user_data);
gboolean on_iconview_button_release(GtkWidget *widget, GdkEventButton *event,
                                     gpointer user_data);
gboolean on_button_press(GtkWidget *widget, GdkEventButton *event,
                          gpointer user_data);

#endif /* FILEVIEW_INTERNAL_H */
