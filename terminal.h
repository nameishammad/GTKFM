#ifndef TERMINAL_H
#define TERMINAL_H

#include "app.h"

/* Dolphin jaisa neeche wala embedded terminal panel banata hai. Terminal
 * hamesha current folder (app->current_dir) mein hi khulta hai, aur wahi
 * default shell use karta hai jo system mein already set hai ($SHELL,
 * warna /etc/passwd wali shell, warna /bin/sh). Shuru mein hidden hota
 * hai -- F4 se dikhta/chupta hai (main.c dekhein). Caller ko is widget ko
 * root vbox ke sabse neeche (statusbar se bhi neeche) pack karna chahiye,
 * taake terminal open/close hone se statusbar ki jagah na badle. */
GtkWidget *terminal_panel_create(AppWidgets *app);

/* F4 shortcut: panel show/hide karta hai. Pehli baar show hone par hi
 * andar shell spawn hoti hai (lazy) -- baar baar F4 dabane se naya
 * process nahi banta, wahi purana session wapis dikh jata hai. */
void terminal_panel_toggle(AppWidgets *app);

/* Folder navigate hone par terminal ko bhi usi folder mein "cd" kar deta
 * hai (Dolphin jaisa behaviour). Panel hidden ho, ya shell abhi spawn na
 * hui ho, to no-op hai. fileview-core.c isko fileview_navigate/go_back/
 * go_forward ke end mein call karta hai. */
void terminal_panel_sync_cwd(AppWidgets *app);

/* True agar keyboard focus is waqt terminal (vte) par hai. fileview-core.c
 * isko focus_first_item()/select_item_with_path() mein use karta hai taake
 * folder refresh (dir monitor ya "cd" sync ki wajah se) terminal se focus
 * na cheeni -- jaise path_entry/search_entry ke saath pehle se hota hai. */
gboolean terminal_panel_has_focus(AppWidgets *app);

#endif /* TERMINAL_H */
