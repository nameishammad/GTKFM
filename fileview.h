#ifndef FILEVIEW_H
#define FILEVIEW_H

#include "app.h"
char *fileview_get_selected_path(AppWidgets *app);
GtkWidget *fileview_create(AppWidgets *app);

void fileview_navigate(AppWidgets *app, const char *dir_path, gboolean push_history);
void fileview_cancel_pending(AppWidgets *app);

/* Dolphin jaisi breadcrumb location bar ko current_dir ke mutabiq dobara
 * banata hai aur usay edit mode se wapas breadcrumb (button) mode mein
 * le aata hai. main.c mein implement hai; fileview-core.c har navigate
 * ke baad ise call karta hai. */
void fileview_refresh_location_bar(AppWidgets *app);

/* Trash ka asal "files/" folder path deta hai (caller free kare) -- main.c
 * breadcrumb bar mein ise dikhane ke liye use karta hai. */
char *trash_files_dir(void);

void fileview_go_up(AppWidgets *app);
void fileview_go_back(AppWidgets *app);
void fileview_go_forward(AppWidgets *app);
void fileview_refresh(AppWidgets *app);

gboolean fileview_select_path(AppWidgets *app, const char *child_path);

void fileview_set_status(AppWidgets *app, const char *msg);

void fileview_copy(AppWidgets *app);
void fileview_cut(AppWidgets *app);
void fileview_copy_location(AppWidgets *app);
void fileview_paste(AppWidgets *app);

void fileview_set_search(AppWidgets *app, const char *needle);

/* Thunar jaisa type-ahead quick search box (main.c ko Escape/BackSpace
 * shortcuts ke saath conflict na hone dene ke liye chahiye). */
gboolean fileview_typeahead_is_active(AppWidgets *app);
void     fileview_typeahead_cancel(AppWidgets *app);

void fileview_select_all(AppWidgets *app);

void fileview_delete_selected(AppWidgets *app, gboolean force_permanent);

void fileview_new_folder(AppWidgets *app);

void fileview_show_context_menu_for_selection(AppWidgets *app);

void fileview_rename_selected(AppWidgets *app);

void fileview_zoom_in(AppWidgets *app);
void fileview_zoom_out(AppWidgets *app);

void fileview_toggle_hidden(AppWidgets *app);
void fileview_set_list_view(AppWidgets *app, gboolean enabled);

gboolean fileview_trash_is_empty(void);

void fileview_save_settings(AppWidgets *app);

/* Kisi file/folder ke liye "sahi" themed icon deta hai -- folder ke liye
 * ye special folder names (Downloads, Videos, Pictures waghera) aur
 * Properties dialog se set kiya hua custom icon (.directory file) bhi
 * dhyan mein rakhta hai, sirf generic "folder" icon nahi deta. icon-view
 * (fileview-*.c) aur info panel (infopanel.c) dono isi ek function se
 * icon nikalte hain, taake dono jagah hamesha ek hi (sahi) icon dikhe. */
GdkPixbuf *themed_icon_for(const char *content_type, gboolean is_dir,
                          const char *path, int size);

#endif /* FILEVIEW_H */
