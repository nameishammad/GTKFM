#include "app.h"
#include "fileview.h"
#include "infopanel.h"
#include "sidebar.h"
#include "terminal.h"
#include "thumbnail.h"
#include "undo.h"
#include <gdk/gdkkeysyms.h>
#include <string.h>

typedef struct {
  GtkWidget *root;
  AppWidgets *app;
} PaneUI;

typedef struct {
  GtkWidget *window;
  GtkWidget *content_slot;
  GtkWidget *split_paned;
  GtkWidget *root_paned;
  PaneUI *panes[2];
  gboolean busy;
  AppWidgets *last_active;
  gint split_position;
} MainWin;

static MainWin *g_mw = NULL;
static char *g_cli_start_dir = NULL; /* set by on_app_open(), consumed once
                                      * by the first build_pane() call */
static gboolean unlock_busy(gpointer data) {
  MainWin *mw = data;

  if (mw)
    mw->busy = FALSE;

  return G_SOURCE_REMOVE;
}

static PaneUI *build_pane(GtkWidget *window, gboolean with_sidebar);
static GtkWidget *build_overflow_menu(AppWidgets *app);
static void update_path_completion(GtkEditable *editable, gpointer data);
static void location_bar_set_edit_mode(AppWidgets *app, gboolean editing);
static void schedule_layout_save(AppWidgets *app);
static void save_settings_synced(AppWidgets *app);

// static AppWidgets *active_app(void) {
//   if (!g_mw || !g_mw->panes[0])
//     return NULL;
//   if (!g_mw->panes[1])
//     return g_mw->panes[0]->app;
//
//   GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(g_mw->window));
//   if (focus && gtk_widget_is_ancestor(focus, g_mw->panes[1]->root))
//     return g_mw->panes[1]->app;
//   return g_mw->panes[0]->app;
// }
static AppWidgets *active_app(void) {
  if (!g_mw)
    return NULL;

  if (g_mw->last_active)
    return g_mw->last_active;

  if (g_mw->panes[0])
    return g_mw->panes[0]->app;

  return NULL;
}

static AppWidgets *info_panel_target_app(void) {
  if (!g_mw || !g_mw->panes[0])
    return NULL;
  if (g_mw->panes[1])
    return g_mw->panes[1]->app;
  return g_mw->panes[0]->app;
}

static AppWidgets *terminal_owner_app(void) {
  if (!g_mw || !g_mw->panes[0])
    return NULL;
  return g_mw->panes[0]->app;
}

static AppWidgets *location_bar_target_app(void) {
  if (!g_mw || !g_mw->panes[0])
    return NULL;
  if (g_mw->last_active)
    return g_mw->last_active;
  return g_mw->panes[0]->app;
}

static gboolean on_pane_focus_in(GtkWidget *widget, GdkEventFocus *event,
                                 gpointer user_data) {
  (void)widget;
  (void)event;
  AppWidgets *app = user_data;

  if (!g_mw || !app)
    return FALSE;

  g_mw->last_active = app;
  fileview_refresh_location_bar(app);

  return FALSE;
}

static void on_back_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  AppWidgets *app = active_app();

  if (app)
    fileview_go_back(app);
}

static void on_fwd_clicked(GtkButton *b, gpointer user_data) {
  (void)b;

  AppWidgets *app = active_app();

  if (app)
    fileview_go_forward(app);
}

static void on_up_clicked(GtkButton *b, gpointer user_data) {
  (void)b;

  AppWidgets *app = active_app();

  if (app)
    fileview_go_up(app);
}

static void on_home_clicked(GtkButton *b, gpointer user_data) {
  (void)b;

  AppWidgets *app = active_app();

  if (app)
    fileview_navigate(app, g_get_home_dir(), TRUE);
}

static void do_split(void) {
  gint64 split_start = g_get_monotonic_time();
  if (!g_mw || !g_mw->panes[0])
    return;

  AppWidgets *owner = g_mw->panes[0]->app;

  gboolean terminal_was_visible = FALSE;

  if (owner && owner->terminal_panel_visible) {
    terminal_was_visible = TRUE;
    gtk_widget_hide(owner->terminal_panel);
  }

  AppWidgets *old_app = g_mw->panes[0]->app;
  gboolean migrate_info = old_app && old_app->info_panel_visible;

  g_object_ref(g_mw->panes[0]->root);
  gtk_container_remove(GTK_CONTAINER(g_mw->content_slot), g_mw->panes[0]->root);

  g_mw->split_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_pack1(GTK_PANED(g_mw->split_paned), g_mw->panes[0]->root, TRUE,
                  FALSE);
  g_object_unref(g_mw->panes[0]->root);

  g_mw->panes[1] = build_pane(g_mw->window, FALSE);
  gtk_paned_pack2(GTK_PANED(g_mw->split_paned), g_mw->panes[1]->root, TRUE,
                  FALSE);

  gtk_box_pack_start(GTK_BOX(g_mw->content_slot), g_mw->split_paned, TRUE, TRUE,
                     0);
  gtk_widget_show_all(g_mw->split_paned);

  GtkAllocation alloc;
  gtk_widget_get_allocation(g_mw->content_slot, &alloc);

  if (alloc.width > 1)
    gtk_paned_set_position(GTK_PANED(g_mw->split_paned), alloc.width / 2);
  g_mw->split_position = alloc.width / 2;

  gtk_widget_grab_focus(g_mw->panes[1]->app->icon_view);
  g_mw->last_active = g_mw->panes[1]->app;
  if (migrate_info) {
    infopanel_toggle(old_app);             /* left mein band */
    infopanel_toggle(g_mw->panes[1]->app); /* right mein khol */
  }
  if (terminal_was_visible) {
    gtk_widget_show(owner->terminal_panel);
  }
  g_print("do_split: %.2f ms\n",
          (g_get_monotonic_time() - split_start) / 1000.0);
}

static void do_unsplit(void) {
  if (!g_mw || !g_mw->panes[1])
    return;

  AppWidgets *app = g_mw->panes[1]->app;

  if (app) {
    fileview_cancel_pending(app);

    if (app->layout_save_debounce_id) {
      g_source_remove(app->layout_save_debounce_id);
      app->layout_save_debounce_id = 0;
    }

    /* right pane ke async callbacks ko widgets use karne se roko */
    app->icon_view = NULL;
    app->path_entry = NULL;
    app->search_entry = NULL;
  }
  // AppWidgets *app = g_mw->panes[1]->app;
  // // if (app) {
  // //   fileview_cancel_pending(app);
  // //
  // //   app->icon_view = NULL;
  // //   app->path_entry = NULL;
  // //   app->search_entry = NULL;
  // // }
  // if (app) {
  //   fileview_cancel_pending(app);
  // }

  if (g_mw->last_active == app)
    g_mw->last_active = g_mw->panes[0]->app;

  g_mw->last_active = g_mw->panes[0]->app;

  gboolean migrate_info = app && app->info_panel_visible;

  g_object_ref(g_mw->panes[0]->root);
  gtk_container_remove(GTK_CONTAINER(g_mw->split_paned), g_mw->panes[0]->root);

  gtk_container_remove(GTK_CONTAINER(g_mw->content_slot), g_mw->split_paned);

  g_mw->split_paned = NULL;

  // g_free(g_mw->panes[1]);
  // g_mw->panes[1] = NULL;
  PaneUI *old_pane = g_mw->panes[1];

  g_mw->panes[1] = NULL;

  g_free(old_pane);

  gtk_box_pack_start(GTK_BOX(g_mw->content_slot), g_mw->panes[0]->root, TRUE,
                     TRUE, 0);
  g_object_unref(g_mw->panes[0]->root);
  gtk_widget_show_all(g_mw->panes[0]->root);

  if (migrate_info) {

    AppWidgets *main_app = g_mw->panes[0]->app;

    if (!main_app->info_panel_visible) {
      main_app->info_panel_visible = TRUE;

      gtk_widget_show(main_app->info_panel);

      infopanel_refresh(main_app);
    }

    GtkAllocation alloc;
    gtk_widget_get_allocation(g_mw->content_slot, &alloc);

    if (alloc.width > 400) {
      gtk_paned_set_position(GTK_PANED(main_app->content_paned),
                             alloc.width - 320);
    }
  }

  fileview_refresh_location_bar(g_mw->panes[0]->app);
  if (g_mw->panes[0] && g_mw->panes[0]->app && g_mw->panes[0]->app->icon_view) {
    gtk_widget_grab_focus(g_mw->panes[0]->app->icon_view);
    g_mw->last_active = g_mw->panes[0]->app;
  }
  fileview_refresh_location_bar(g_mw->panes[0]->app);
}

static gboolean unlock_split(gpointer data) {
  MainWin *mw = data;

  if (mw)
    mw->busy = FALSE;

  return G_SOURCE_REMOVE;
}

static void toggle_split(void) {
  if (!g_mw || g_mw->busy)
    return;

  g_mw->busy = TRUE;

  if (g_mw->panes[1])
    do_unsplit();
  else
    do_split();

  if (g_mw->panes[0] && g_mw->panes[0]->app) {
    AppWidgets *owner = g_mw->panes[0]->app;
    owner->split_view_active = (g_mw->panes[1] != NULL);
    schedule_layout_save(owner);
  }

  /* rapid F3 press se crash bachane ke liye */
  g_timeout_add(300, unlock_split, g_mw);
}

static void sidebar_toggle(AppWidgets *app) {
  if (!app || !app->paned)
    return;

  GtkWidget *sidebar = gtk_paned_get_child1(GTK_PANED(app->paned));

  if (app->sidebar_visible) {

    app->sidebar_position = gtk_paned_get_position(GTK_PANED(app->paned));

    gtk_widget_hide(sidebar);

    app->sidebar_visible = FALSE;

  } else {

    gtk_widget_show(sidebar);

    if (app->sidebar_position > 0) {
      gtk_paned_set_position(GTK_PANED(app->paned), app->sidebar_position);
    }

    app->sidebar_visible = TRUE;
  }

  save_settings_synced(app);
}

static gboolean on_window_key_press(GtkWidget *widget, GdkEventKey *event,
                                    gpointer user_data) {
  (void)widget;
  (void)user_data;

  if (event->keyval == GDK_KEY_F3) {
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(g_mw->window));
    AppWidgets *cur = active_app();
    gboolean entry_focused =
        cur && (focus == cur->path_entry || focus == cur->search_entry);
    if (entry_focused)
      return FALSE;
    toggle_split();
    return TRUE;
  }

  if (event->keyval == GDK_KEY_F11) {

    if (g_mw->busy)
      return TRUE;

    g_mw->busy = TRUE;

    AppWidgets *cur = info_panel_target_app();

    if (cur)
      infopanel_toggle(cur);

    g_timeout_add(300, unlock_busy, g_mw);

    return TRUE;
  }

  if (event->keyval == GDK_KEY_F9) {

    AppWidgets *cur = active_app();

    if (cur)
      sidebar_toggle(cur);

    return TRUE;
  }

  if (event->keyval == GDK_KEY_F4) {

    if (g_mw->busy)
      return TRUE;

    g_mw->busy = TRUE;

    AppWidgets *cur = terminal_owner_app();

    if (cur)
      terminal_panel_toggle(cur);

    g_timeout_add(300, unlock_busy, g_mw);

    return TRUE;
  }

  AppWidgets *app = active_app();
  if (!app)
    return FALSE;

  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(g_mw->window));
  gboolean entry_focused =
      (focus == app->path_entry || focus == app->search_entry);

  if (event->keyval == GDK_KEY_Escape) {
    if (entry_focused) {
      if (focus == app->path_entry)
        location_bar_set_edit_mode(app, FALSE);
      gtk_widget_grab_focus(app->icon_view);
      return TRUE;
    }

    if (fileview_typeahead_is_active(app)) {
      fileview_typeahead_cancel(app);
      return TRUE;
    }
    if (focus == app->icon_view) {
      gtk_icon_view_unselect_all(GTK_ICON_VIEW(app->icon_view));
      return TRUE;
    }
    return FALSE;
  }

  if (event->keyval == GDK_KEY_Delete || event->keyval == GDK_KEY_KP_Delete) {
    if (entry_focused)
      return FALSE;
    gboolean permanent = (event->state & GDK_SHIFT_MASK) != 0;
    fileview_delete_selected(app, permanent);
    return TRUE;
  }

  if (event->keyval == GDK_KEY_BackSpace) {

    if (entry_focused)
      return FALSE;

    if (terminal_panel_has_focus(terminal_owner_app()))
      return FALSE;

    if (fileview_typeahead_is_active(app))
      return FALSE;

    fileview_go_back(app);
    return TRUE;
  }

  if (event->keyval == GDK_KEY_Menu) {
    if (entry_focused)
      return FALSE;
    fileview_show_context_menu_for_selection(app);
    return TRUE;
  }

  if (event->keyval == GDK_KEY_F2) {
    if (entry_focused)
      return FALSE;
    fileview_rename_selected(app);
    return TRUE;
  }

  if (event->keyval == GDK_KEY_F5) {
    if (entry_focused)
      return FALSE;
    fileview_refresh(app);
    return TRUE;
  }

  // if ((event->state & GDK_MOD1_MASK) &&
  //     (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C)) {
  //   if (entry_focused)
  //     return FALSE;
  //   fileview_navigate(app, COMPUTER_URI, TRUE);
  //   return TRUE;
  // }
  /* Alt + C = Computer location */
  if ((event->state & GDK_MOD1_MASK) && !(event->state & GDK_CONTROL_MASK) &&
      (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C)) {

    if (entry_focused)
      return FALSE;

    fileview_navigate(app, COMPUTER_URI, TRUE);
    return TRUE;
  }

  if ((event->state & GDK_MOD1_MASK) && event->keyval == GDK_KEY_Home) {

    if (entry_focused)
      return FALSE;

    fileview_navigate(app, g_get_home_dir(), TRUE);
    return TRUE;
  }

  /* Ctrl+Q / Super(Meta)+Q = close application */
  if ((event->state & GDK_CONTROL_MASK) &&
      (event->keyval == GDK_KEY_q || event->keyval == GDK_KEY_Q)) {
    gtk_window_close(GTK_WINDOW(g_mw->window));
    return TRUE;
  }

  /* Super/Meta + Q */
  if ((event->state & GDK_META_MASK) &&
      (event->keyval == GDK_KEY_q || event->keyval == GDK_KEY_Q)) {
    gtk_window_close(GTK_WINDOW(g_mw->window));
    return TRUE;
  }

  if (g_mw && g_mw->panes[1] && (event->state & GDK_CONTROL_MASK)) {

    // if (event->keyval == GDK_KEY_Left) {
    //
    //   gtk_widget_grab_focus(g_mw->panes[0]->app->icon_view);
    //   g_mw->last_active = g_mw->panes[0]->app;
    //   fileview_refresh_location_bar(g_mw->panes[0]->app);
    //
    //   return TRUE;
    // }

    if (event->keyval == GDK_KEY_Left) {

      AppWidgets *left = g_mw->panes[0]->app;

      if (left && left->icon_view) {
        gtk_widget_grab_focus(left->icon_view);
        g_mw->last_active = left;
        fileview_refresh_location_bar(left);
      }

      return TRUE;
    }

    // if (event->keyval == GDK_KEY_Right) {
    //
    //   gtk_widget_grab_focus(g_mw->panes[1]->app->icon_view);
    //   g_mw->last_active = g_mw->panes[1]->app;
    //   fileview_refresh_location_bar(g_mw->panes[1]->app);
    //
    //   return TRUE;
    // }
    if (event->keyval == GDK_KEY_Right) {

      AppWidgets *right = g_mw->panes[1]->app;

      if (right && right->icon_view) {
        gtk_widget_grab_focus(right->icon_view);
        g_mw->last_active = right;
        fileview_refresh_location_bar(right);
      }

      return TRUE;
    }
  }

  if (!(event->state & GDK_CONTROL_MASK))
    return FALSE;

  if ((event->state & GDK_SHIFT_MASK) &&
      (event->keyval == GDK_KEY_n || event->keyval == GDK_KEY_N)) {
    if (entry_focused)
      return FALSE;
    fileview_new_folder(app);
    return TRUE;
  }

  if ((event->state & GDK_CONTROL_MASK) &&
    (event->keyval == GDK_KEY_z ||
    event->keyval == GDK_KEY_Z))
  {
    if (!entry_focused)
    {
      AppWidgets *undo_app = active_app();

      if (undo_app)
        undo_last(undo_app);

      return TRUE;
    }
  }

  /* Ctrl + Alt + C = Copy item location */
  if ((event->state & GDK_CONTROL_MASK) && (event->state & GDK_MOD1_MASK) &&
      !(event->state & GDK_SHIFT_MASK) &&
      (event->keyval == GDK_KEY_c || event->keyval == GDK_KEY_C)) {

    if (entry_focused)
      return FALSE;

    fileview_copy_location(app);
    return TRUE;
  }

  switch (event->keyval) {
  case GDK_KEY_f:
  case GDK_KEY_F:
    if (app->search_entry) {
      gtk_widget_grab_focus(app->search_entry);
      gtk_editable_select_region(GTK_EDITABLE(app->search_entry), 0, -1);
      return TRUE;
    }
    return FALSE;
  // case GDK_KEY_l:
  // case GDK_KEY_L:
  //   location_bar_set_edit_mode(app, TRUE);
  //   return TRUE;
  case GDK_KEY_l:
  case GDK_KEY_L: {
    AppWidgets *loc_app = location_bar_target_app();

    if (!loc_app)
      loc_app = app;

    location_bar_set_edit_mode(loc_app, TRUE);
    return TRUE;
  }
  case GDK_KEY_h:
  case GDK_KEY_H:
    if (entry_focused)
      return FALSE;
    fileview_toggle_hidden(app);
    return TRUE;
  case GDK_KEY_d:
  case GDK_KEY_D: {
    char *selected = fileview_get_selected_path(app);

    if (selected) {
      sidebar_add_bookmark(app, selected);
      g_free(selected);
    } else {
      sidebar_add_bookmark(app, app->current_dir);
    }
    return TRUE;
  }
  case GDK_KEY_c:
  case GDK_KEY_C:
    if (entry_focused)
      return FALSE;
    fileview_copy(app);
    return TRUE;
  case GDK_KEY_x:
  case GDK_KEY_X:
    if (entry_focused)
      return FALSE;
    fileview_cut(app);
    return TRUE;
  case GDK_KEY_v:
  case GDK_KEY_V:
    if (entry_focused)
      return FALSE;
    fileview_paste(app);
    return TRUE;
  case GDK_KEY_plus:
  case GDK_KEY_equal:
  case GDK_KEY_KP_Add:
    fileview_zoom_in(app);
    return TRUE;
  case GDK_KEY_minus:
  case GDK_KEY_KP_Subtract:
    fileview_zoom_out(app);
    return TRUE;
  default:
    return FALSE;
  }
}

static void on_menu_toggle_folder_previews(GtkCheckMenuItem *mi,
                                           gpointer user_data) {
  AppWidgets *app = user_data;
  if (!app)
    return;
  gboolean active = gtk_check_menu_item_get_active(mi);
  if (active == app->folder_previews)
    return;
  app->folder_previews = active;
  save_settings_synced(app);
  fileview_refresh(app);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
  AppWidgets *app = user_data;
  fileview_set_search(app, gtk_entry_get_text(GTK_ENTRY(entry)));
}

static void on_search_stop(GtkSearchEntry *entry, gpointer user_data) {
  AppWidgets *app = user_data;
  gtk_entry_set_text(GTK_ENTRY(entry), "");
  gtk_widget_grab_focus(app->icon_view);
}

static void on_search_entry_activate(GtkEntry *entry, gpointer user_data) {
  (void)entry;
  AppWidgets *app = user_data;
  gtk_widget_grab_focus(app->icon_view);
}

static void on_go_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AppWidgets *owner = user_data;
  AppWidgets *target = location_bar_target_app();
  if (!target)
    target = owner;
  const char *text = gtk_entry_get_text(GTK_ENTRY(owner->path_entry));
  fileview_navigate(target, text, TRUE);
  gtk_widget_grab_focus(target->icon_view);
}

static void on_location_entry_activate(GtkEntry *entry, gpointer user_data) {
  AppWidgets *owner = user_data;

  AppWidgets *target = location_bar_target_app();
  if (!target)
    target = owner;

  const char *text = gtk_entry_get_text(entry);

  if (text && *text) {
    fileview_navigate(target, text, TRUE);
    gtk_widget_grab_focus(target->icon_view);
  }
}

static void apply_location_bar_css(void) {
  static gboolean applied = FALSE;
  if (applied)
    return;
  applied = TRUE;

  GtkCssProvider *provider = gtk_css_provider_new();
  const char *css = "#location-entry {"
                    "  border-radius: 6px;"
                    "  padding: 4px 8px;"
                    "  font-weight: 500;"
                    "}"
                    "#nav-back,"
                    "#nav-forward,"
                    "#nav-up,"
                    "#nav-home {"
                    "font-family: Adwaita Mono;"
                    "font-size: 20px;"
                    "font-weight: bold;"
                    "}"
                    "#location-go-button {"
                    "  border-radius: 6px;"
                    "  margin-left: 2px;"
                    "  padding: 2px 6px;"
                    "  background-color: alpha(@theme_selected_bg_color, 0.15);"
                    "}"
                    "#location-go-button:hover {"
                    "  background-color: alpha(@theme_selected_bg_color, 0.35);"
                    "}"
                    "#location-go-button:active {"
                    "  background-color: @theme_selected_bg_color;"
                    "  color: @theme_selected_fg_color;"
                    "}"
                    "#location-edit-button {"
                    "  border-radius: 6px;"
                    "  padding: 2px 6px;"
                    "}"
                    ".gtkfm-breadcrumb {"
                    "  border-radius: 6px;"
                    "  padding: 3px 8px;"
                    "  background-image: none;"
                    "  border-width: 0;"
                    "  box-shadow: none;"
                    "}"
                    ".gtkfm-breadcrumb:hover {"
                    "  background-color: alpha(@theme_selected_bg_color, 0.25);"
                    "}"
                    ".gtkfm-breadcrumb:active {"
                    "  background-color: alpha(@theme_selected_bg_color, 0.45);"
                    "}"
                    ".gtkfm-breadcrumb-current {"
                    "  font-weight: 700;"
                    "}"
                    ".gtkfm-breadcrumb-sep {"
                    "  opacity: 0.45;"
                    "}";
  gtk_css_provider_load_from_data(provider, css, -1, NULL);
  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static void reveal_tool_item(GtkWidget *item) {
  gtk_widget_set_no_show_all(item, FALSE);
  gtk_widget_show_all(item);
  gtk_widget_set_no_show_all(item, TRUE);
}

static void location_bar_apply_mode(AppWidgets *app, gboolean editing) {
  if (editing) {
    gtk_widget_hide(app->location_breadcrumb_item);
    reveal_tool_item(app->location_entry_item);
    reveal_tool_item(app->location_go_item);
  } else {
    gtk_widget_hide(app->location_entry_item);
    gtk_widget_hide(app->location_go_item);
    reveal_tool_item(app->location_breadcrumb_item);
  }
}

static void location_bar_set_edit_mode(AppWidgets *app, gboolean editing) {

  AppWidgets *target = location_bar_target_app();

  if (!target)
    target = app;

  /* location widgets hamesha toolbar owner (left pane) ke hain */
  AppWidgets *owner = g_mw->panes[0]->app;

  location_bar_apply_mode(owner, editing);

  if (editing) {

    if (target->current_dir && owner->path_entry) {

      char *display_path;

      if (g_str_has_suffix(target->current_dir, "/")) {
        display_path = g_strdup(target->current_dir);
      } else {
        display_path = g_strconcat(target->current_dir, "/", NULL);
      }

      gtk_entry_set_text(GTK_ENTRY(owner->path_entry), display_path);

      g_free(display_path);
    }

    gtk_widget_grab_focus(owner->path_entry);

    gtk_editable_select_region(GTK_EDITABLE(owner->path_entry), 0, -1);
  }
}
// static void location_bar_set_edit_mode(AppWidgets *app, gboolean editing) {
//   location_bar_apply_mode(app, editing);
//
//   if (editing) {
//
//     AppWidgets *target = location_bar_target_app();
//
//     if (!target)
//       target = app;
//
//     // if (target->current_dir) {
//     //   gtk_entry_set_text(GTK_ENTRY(app->path_entry), target->current_dir);
//     // }
//
//     if (target->current_dir && target->path_entry) {
//       char *display_path;
//
//       if (g_str_has_suffix(target->current_dir, "/")) {
//         display_path = g_strdup(target->current_dir);
//       } else {
//         display_path = g_strconcat(target->current_dir, "/", NULL);
//       }
//
//       gtk_entry_set_text(GTK_ENTRY(target->path_entry), display_path);
//
//       g_free(display_path);
//     }
//
//     gtk_widget_grab_focus(app->path_entry);
//
//     gtk_editable_select_region(GTK_EDITABLE(app->path_entry), 0, -1);
//   }
// }

static gboolean on_breadcrumb_area_clicked(GtkWidget *widget,
                                           GdkEventButton *event,
                                           gpointer user_data) {
  (void)widget;
  if (event->button != 1)
    return FALSE;
  location_bar_set_edit_mode(user_data, TRUE);
  return TRUE;
}

static void on_breadcrumb_button_clicked(GtkButton *btn, gpointer user_data) {
  AppWidgets *app = user_data;
  const char *path = g_object_get_data(G_OBJECT(btn), "gtkfm-seg-path");
  if (path && *path)
    fileview_navigate(app, path, TRUE);
}

static void on_location_edit_button_clicked(GtkButton *btn,
                                            gpointer user_data) {
  (void)btn;
  location_bar_set_edit_mode(user_data, TRUE);
}

static void rebuild_location_breadcrumbs(AppWidgets *widget_owner,
                                         AppWidgets *target_app,
                                         const char *dir) {
  GtkWidget *box = widget_owner->location_breadcrumb_box;

  /* Purane buttons hata dein */
  GList *children = gtk_container_get_children(GTK_CONTAINER(box));
  for (GList *l = children; l; l = l->next)
    gtk_widget_destroy(GTK_WIDGET(l->data));
  g_list_free(children);

  if (!dir || !*dir)
    return;

  gboolean is_computer = g_strcmp0(dir, COMPUTER_URI) == 0;
  gboolean is_recent = !is_computer && g_strcmp0(dir, RECENT_URI) == 0;
  char *tdir = trash_files_dir();
  gboolean is_trash =
      !is_computer && !is_recent && tdir && g_strcmp0(dir, tdir) == 0;

  if (is_computer || is_recent || is_trash) {
    GtkWidget *btn = gtk_button_new_with_label(
        is_computer ? "Computer" : (is_recent ? "Recent" : "Trash"));
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn),
                                "gtkfm-breadcrumb");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn),
                                "gtkfm-breadcrumb-current");
    g_object_set_data_full(G_OBJECT(btn), "gtkfm-seg-path", g_strdup(dir),
                           g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_breadcrumb_button_clicked),
                     target_app);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    gtk_widget_show(btn);
    g_free(tdir);
    return;
  }
  g_free(tdir);

  const char *home = g_get_home_dir();
  gboolean under_home = home && *home && g_str_has_prefix(dir, home) &&
                        (dir[strlen(home)] == '\0' || dir[strlen(home)] == '/');

  GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  char *accum;
  const char *rel;

  if (under_home) {
    g_ptr_array_add(labels, g_strdup("Home"));
    g_ptr_array_add(paths, g_strdup(home));
    accum = g_strdup(home);
    rel = dir + strlen(home);
    if (*rel == '/')
      rel++;
  } else {
    g_ptr_array_add(labels, g_strdup("/"));
    g_ptr_array_add(paths, g_strdup("/"));
    accum = g_strdup("/");
    rel = (*dir == '/') ? dir + 1 : dir;
  }

  char **parts = g_strsplit(rel, "/", -1);
  for (int i = 0; parts[i]; i++) {
    if (!*parts[i])
      continue;
    char *next = g_build_filename(accum, parts[i], NULL);
    g_ptr_array_add(labels, g_strdup(parts[i]));
    g_ptr_array_add(paths, g_strdup(next));
    g_free(accum);
    accum = next;
  }
  g_strfreev(parts);
  g_free(accum);

  for (guint i = 0; i < labels->len; i++) {
    if (i > 0) {
      GtkWidget *sep = gtk_label_new("\xE2\x80\xBA"); /* "›" chevron */
      gtk_style_context_add_class(gtk_widget_get_style_context(sep),
                                  "gtkfm-breadcrumb-sep");
      gtk_box_pack_start(GTK_BOX(box), sep, FALSE, FALSE, 0);
      gtk_widget_show(sep);
    }

    gboolean is_last = (i == labels->len - 1);
    GtkWidget *btn = gtk_button_new_with_label(g_ptr_array_index(labels, i));
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(btn),
                                "gtkfm-breadcrumb");
    if (is_last)
      gtk_style_context_add_class(gtk_widget_get_style_context(btn),
                                  "gtkfm-breadcrumb-current");
    g_object_set_data_full(G_OBJECT(btn), "gtkfm-seg-path",
                           g_strdup(g_ptr_array_index(paths, i)), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_breadcrumb_button_clicked),
                     target_app);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
    gtk_widget_show(btn);
  }

  g_ptr_array_free(labels, TRUE);
  g_ptr_array_free(paths, TRUE);
}

void fileview_refresh_location_bar(AppWidgets *app) {
  if (!g_mw || !g_mw->panes[0])
    return;

  AppWidgets *owner =
      g_mw->panes[0]->app; /* widgets hamesha yahin bante hain */
  if (!owner || !owner->location_breadcrumb_box)
    return;

  AppWidgets *target = location_bar_target_app();
  if (!target)
    target = owner;

  if (app != target)
    return;

  rebuild_location_breadcrumbs(owner, target, target->current_dir);
  location_bar_apply_mode(owner, owner->location_bar_always_edit);
  if (owner->location_bar_always_edit && target->current_dir &&
      owner->path_entry) {
    gtk_entry_set_text(GTK_ENTRY(owner->path_entry), target->current_dir);
  }
}

static void update_path_completion(GtkEditable *editable, gpointer data) {
  (void)data;

  const char *text = gtk_entry_get_text(GTK_ENTRY(editable));

  GtkEntryCompletion *completion =
      gtk_entry_get_completion(GTK_ENTRY(editable));

  if (!completion)
    return;

  GtkListStore *store =
      GTK_LIST_STORE(gtk_entry_completion_get_model(completion));

  gtk_list_store_clear(store);

  if (!text || !*text)
    return;

  char *dir;
  char *prefix;

  if (g_str_has_suffix(text, "/")) {

    dir = g_strdup(text);
    prefix = g_strdup("");

  } else {

    char *slash = strrchr(text, '/');

    if (!slash)
      return;

    dir = g_strndup(text, slash - text + 1);
    prefix = g_strdup(slash + 1);
  }

  GDir *d = g_dir_open(dir, 0, NULL);

  if (!d) {
    g_free(dir);
    g_free(prefix);
    return;
  }

  const char *name;

  while ((name = g_dir_read_name(d))) {

    if (!g_str_has_prefix(name, prefix))
      continue;

    char *full = g_build_filename(dir, name, NULL);

    GtkTreeIter iter;

    gtk_list_store_append(store, &iter);

    gtk_list_store_set(store, &iter, 0, full, -1);

    g_free(full);
  }

  g_dir_close(d);

  g_free(dir);
  g_free(prefix);
}

static GtkWidget *build_toolbar(AppWidgets *app) {
  apply_location_bar_css();

  GtkWidget *bar = gtk_toolbar_new();
  gtk_toolbar_set_style(GTK_TOOLBAR(bar), GTK_TOOLBAR_ICONS);
  gtk_toolbar_set_icon_size(GTK_TOOLBAR(bar), GTK_ICON_SIZE_BUTTON);

  GtkToolItem *back = gtk_tool_button_new(gtk_label_new(""), "Back");

  GtkToolItem *fwd = gtk_tool_button_new(gtk_label_new(""), "Forward");

  GtkToolItem *up = gtk_tool_button_new(gtk_label_new(""), "Up");

  GtkToolItem *home = gtk_tool_button_new(gtk_label_new("󰟒"), "Home");
  gtk_widget_set_name(GTK_WIDGET(back), "nav-back");
  gtk_widget_set_name(GTK_WIDGET(fwd), "nav-forward");
  gtk_widget_set_name(GTK_WIDGET(up), "nav-up");
  gtk_widget_set_name(GTK_WIDGET(home), "nav-home");

  gtk_widget_set_margin_start(GTK_WIDGET(back), 4);
  gtk_widget_set_margin_end(GTK_WIDGET(back), 4);

  gtk_widget_set_margin_start(GTK_WIDGET(fwd), 4);
  gtk_widget_set_margin_end(GTK_WIDGET(fwd), 4);

  gtk_widget_set_margin_start(GTK_WIDGET(up), 4);
  gtk_widget_set_margin_end(GTK_WIDGET(up), 4);

  gtk_widget_set_margin_start(GTK_WIDGET(home), 4);
  gtk_widget_set_margin_end(GTK_WIDGET(home), 4);

  gtk_toolbar_insert(GTK_TOOLBAR(bar), back, -1);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), fwd, -1);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), up, -1);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), home, -1);

  GtkToolItem *space = gtk_separator_tool_item_new();
  gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(space), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(space), 30, -1);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), space, -1);

  app->location_breadcrumb_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  GtkWidget *breadcrumb_scroller = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(breadcrumb_scroller),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_overlay_scrolling(
      GTK_SCROLLED_WINDOW(breadcrumb_scroller), TRUE);
  gtk_container_add(GTK_CONTAINER(breadcrumb_scroller),
                    app->location_breadcrumb_box);

  GtkWidget *breadcrumb_click_area = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(breadcrumb_click_area), breadcrumb_scroller);
  g_signal_connect(breadcrumb_click_area, "button-press-event",
                   G_CALLBACK(on_breadcrumb_area_clicked), app);

  GtkToolItem *breadcrumb_item = gtk_tool_item_new();
  gtk_container_add(GTK_CONTAINER(breadcrumb_item), breadcrumb_click_area);
  gtk_tool_item_set_expand(breadcrumb_item, TRUE);
  app->location_breadcrumb_item = GTK_WIDGET(breadcrumb_item);
  gtk_widget_set_no_show_all(app->location_breadcrumb_item, TRUE);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), breadcrumb_item, -1);
  gtk_widget_set_margin_start(GTK_WIDGET(breadcrumb_item), 6);
  gtk_widget_set_margin_end(GTK_WIDGET(breadcrumb_item), 6);

  GtkToolItem *entry_item = gtk_tool_item_new();
  app->path_entry = gtk_entry_new();
  gtk_widget_set_hexpand(app->path_entry, TRUE);
  gtk_widget_set_name(app->path_entry, "location-entry");
  GtkEntryCompletion *completion;

  completion = gtk_entry_completion_new();

  GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);

  gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(store));

  gtk_entry_completion_set_text_column(completion, 0);

  if (app->path_entry && GTK_IS_ENTRY(app->path_entry)) {
    gtk_entry_set_completion(GTK_ENTRY(app->path_entry), completion);
  }

  g_object_unref(completion);

  g_signal_connect(app->path_entry, "changed",
                   G_CALLBACK(update_path_completion), app);

  g_signal_connect(app->path_entry, "activate",
                   G_CALLBACK(on_location_entry_activate), app);

  gtk_container_add(GTK_CONTAINER(entry_item), app->path_entry);
  gtk_tool_item_set_expand(entry_item, TRUE);
  app->location_entry_item = GTK_WIDGET(entry_item);
  gtk_widget_set_no_show_all(app->location_entry_item, TRUE);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), entry_item, -1);

  GtkToolItem *go_item =
      gtk_tool_button_new(gtk_image_new_from_icon_name("object-select-symbolic",
                                                       GTK_ICON_SIZE_BUTTON),
                          "Go");
  gtk_widget_set_name(GTK_WIDGET(go_item), "location-go-button");
  gtk_widget_set_tooltip_text(GTK_WIDGET(go_item), "Is location per jayein");
  app->location_go_item = GTK_WIDGET(go_item);
  gtk_widget_set_no_show_all(app->location_go_item, TRUE);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), go_item, -1);
  g_signal_connect(go_item, "clicked", G_CALLBACK(on_go_clicked), app);

  GtkToolItem *edit_item =
      gtk_tool_button_new(gtk_image_new_from_icon_name("document-edit-symbolic",
                                                       GTK_ICON_SIZE_BUTTON),
                          "Edit Path");
  gtk_widget_set_name(GTK_WIDGET(edit_item), "location-edit-button");
  gtk_widget_set_tooltip_text(GTK_WIDGET(edit_item),
                              "Path type karein (Ctrl+L)");
  gtk_toolbar_insert(GTK_TOOLBAR(bar), edit_item, -1);
  g_signal_connect(edit_item, "clicked",
                   G_CALLBACK(on_location_edit_button_clicked), app);

  location_bar_set_edit_mode(app, FALSE);

  GtkToolItem *overflow_item = gtk_tool_item_new();
  GtkWidget *overflow_btn = gtk_menu_button_new();
  gtk_button_set_relief(GTK_BUTTON(overflow_btn), GTK_RELIEF_NONE);
  gtk_button_set_image(
      GTK_BUTTON(overflow_btn),
      gtk_image_new_from_icon_name("view-more-symbolic", GTK_ICON_SIZE_BUTTON));
  gtk_widget_set_tooltip_text(overflow_btn, "Menu");
  gtk_menu_button_set_popup(GTK_MENU_BUTTON(overflow_btn),
                            build_overflow_menu(app));
  gtk_container_add(GTK_CONTAINER(overflow_item), overflow_btn);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), overflow_item, -1);

  gtk_toolbar_insert(GTK_TOOLBAR(bar), gtk_separator_tool_item_new(), -1);

  GtkToolItem *search_item = gtk_tool_item_new();
  app->search_entry = gtk_search_entry_new();
  gtk_entry_set_placeholder_text(
      GTK_ENTRY(app->search_entry),
      "Is folder aur sub-folders mein search karein...");
  gtk_widget_set_size_request(app->search_entry, 220, -1);
  gtk_container_add(GTK_CONTAINER(search_item), app->search_entry);
  gtk_toolbar_insert(GTK_TOOLBAR(bar), search_item, -1);

  app->btn_back = GTK_WIDGET(back);
  app->btn_forward = GTK_WIDGET(fwd);
  app->btn_up = GTK_WIDGET(up);

  g_signal_connect(back, "clicked", G_CALLBACK(on_back_clicked), NULL);
  g_signal_connect(fwd, "clicked", G_CALLBACK(on_fwd_clicked), NULL);
  g_signal_connect(up, "clicked", G_CALLBACK(on_up_clicked), NULL);
  g_signal_connect(home, "clicked", G_CALLBACK(on_home_clicked), NULL);
  g_signal_connect(app->search_entry, "search-changed",
                   G_CALLBACK(on_search_changed), app);
  g_signal_connect(app->search_entry, "stop-search", G_CALLBACK(on_search_stop),
                   app);
  g_signal_connect(app->search_entry, "activate",
                   G_CALLBACK(on_search_entry_activate), app);

  return bar;
}

static void on_menu_bookmark(GtkMenuItem *mi, gpointer user_data) {
  (void)mi;
  (void)user_data;
  AppWidgets *app = active_app();
  if (app)
    sidebar_add_bookmark(app, app->current_dir);
}

static void on_menu_refresh(GtkMenuItem *mi, gpointer user_data) {
  (void)mi;
  (void)user_data;
  AppWidgets *app = active_app();
  if (app)
    fileview_refresh(app);
}

static void on_menu_toggle_infopanel(void) {
  AppWidgets *app = info_panel_target_app();
  if (app)
    infopanel_toggle(app);
}

static void on_menu_toggle_terminal(void) {
  AppWidgets *app = terminal_owner_app();
  if (app)
    terminal_panel_toggle(app);
}

static void on_menu_toggle_hidden(GtkCheckMenuItem *mi, gpointer user_data) {
  AppWidgets *app = user_data;
  if (!app)
    return;
  gboolean active = gtk_check_menu_item_get_active(mi);
  if (active != app->show_hidden)
    fileview_toggle_hidden(app);
}

static void on_menu_toggle_location_style(GtkCheckMenuItem *mi,
                                          gpointer user_data) {
  AppWidgets *app = user_data;
  if (!app)
    return;
  gboolean active = gtk_check_menu_item_get_active(mi);
  if (active == app->location_bar_always_edit)
    return;
  app->location_bar_always_edit = active;
  save_settings_synced(app);
  location_bar_apply_mode(app, active);
}

static void on_menu_toggle_video_frame(GtkCheckMenuItem *mi,
                                       gpointer user_data) {
  AppWidgets *app = user_data;
  if (!app)
    return;
  gboolean active = gtk_check_menu_item_get_active(mi);
  if (active == app->video_thumbnail_frame)
    return;
  app->video_thumbnail_frame = active;
  thumbnail_set_video_frame_enabled(active);
  save_settings_synced(app);
  fileview_refresh(app);
}

static void on_menu_toggle_auto_refresh(GtkCheckMenuItem *mi,
                                        gpointer user_data) {
  AppWidgets *app = user_data;
  if (!app)
    return;
  gboolean active = gtk_check_menu_item_get_active(mi);
  if (active == app->auto_refresh_enabled)
    return;
  app->auto_refresh_enabled = active;
  save_settings_synced(app);
}

static void on_menu_toggle_list_view(GtkCheckMenuItem *mi, gpointer user_data) {
  AppWidgets *app = user_data;
  if (!app)
    return;
  gboolean active = gtk_check_menu_item_get_active(mi);
  if (active == app->list_view)
    return;
  fileview_set_list_view(app, active);
  save_settings_synced(app);
  fileview_refresh(app);
}

static void on_overflow_menu_show(GtkWidget *menu, gpointer user_data) {
  (void)menu;
  AppWidgets *app = user_data;
  if (!app)
    return;
  if (app->hidden_menu_item) {
    g_signal_handlers_block_by_func(app->hidden_menu_item,
                                    on_menu_toggle_hidden, app);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->hidden_menu_item),
                                   app->show_hidden);
    g_signal_handlers_unblock_by_func(app->hidden_menu_item,
                                      on_menu_toggle_hidden, app);
  }
  if (app->location_style_menu_item) {
    g_signal_handlers_block_by_func(app->location_style_menu_item,
                                    on_menu_toggle_location_style, app);
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(app->location_style_menu_item),
        app->location_bar_always_edit);
    g_signal_handlers_unblock_by_func(app->location_style_menu_item,
                                      on_menu_toggle_location_style, app);
  }
  if (app->video_frame_menu_item) {
    g_signal_handlers_block_by_func(app->video_frame_menu_item,
                                    on_menu_toggle_video_frame, app);
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(app->video_frame_menu_item),
        app->video_thumbnail_frame);
    g_signal_handlers_unblock_by_func(app->video_frame_menu_item,
                                      on_menu_toggle_video_frame, app);
  }
  if (app->preview_toggle) {
    g_signal_handlers_block_by_func(app->preview_toggle,
                                    on_menu_toggle_folder_previews, app);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->preview_toggle),
                                   app->folder_previews);
    g_signal_handlers_unblock_by_func(app->preview_toggle,
                                      on_menu_toggle_folder_previews, app);
  }
  if (app->list_view_menu_item) {
    g_signal_handlers_block_by_func(app->list_view_menu_item,
                                    on_menu_toggle_list_view, app);
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(app->list_view_menu_item), app->list_view);
    g_signal_handlers_unblock_by_func(app->list_view_menu_item,
                                      on_menu_toggle_list_view, app);
  }
  if (app->auto_refresh_menu_item) {
    g_signal_handlers_block_by_func(app->auto_refresh_menu_item,
                                    on_menu_toggle_auto_refresh, app);
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(app->auto_refresh_menu_item),
        app->auto_refresh_enabled);
    g_signal_handlers_unblock_by_func(app->auto_refresh_menu_item,
                                      on_menu_toggle_auto_refresh, app);
  }
}

static GtkWidget *build_overflow_menu(AppWidgets *app) {
  GtkWidget *menu = gtk_menu_new();

  /* File menu ke items */
  GtkWidget *bookmark_item =
      gtk_menu_item_new_with_label("Bookmark This Folder (Ctrl+D)");
  g_signal_connect(bookmark_item, "activate", G_CALLBACK(on_menu_bookmark),
                   NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), bookmark_item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  /* View menu ke items */
  GtkWidget *refresh_item = gtk_menu_item_new_with_label("Refresh");
  g_signal_connect(refresh_item, "activate", G_CALLBACK(on_menu_refresh), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), refresh_item);

  GtkWidget *split_item =
      gtk_menu_item_new_with_label("Toggle Split View (F3)");
  g_signal_connect_swapped(split_item, "activate", G_CALLBACK(toggle_split),
                           NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), split_item);

  GtkWidget *info_item =
      gtk_menu_item_new_with_label("Toggle Information Panel (F11)");
  g_signal_connect_swapped(info_item, "activate",
                           G_CALLBACK(on_menu_toggle_infopanel), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), info_item);

  GtkWidget *terminal_item =
      gtk_menu_item_new_with_label("Toggle Terminal (F4)");
  g_signal_connect_swapped(terminal_item, "activate",
                           G_CALLBACK(on_menu_toggle_terminal), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), terminal_item);

  GtkWidget *auto_refresh_item =
      gtk_check_menu_item_new_with_label("Auto Refresh on External Changes");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(auto_refresh_item),
                                 app->auto_refresh_enabled);
  gtk_widget_set_tooltip_text(
      auto_refresh_item,
      "Chalu (default): archive extract, terminal se copy/move waghera se\n"
      "bahar hue changes is folder mein khud-ba-khud nazar aa jayenge.\n"
      "Band: sirf manual Refresh (ya F5) se update hoga.");
  g_signal_connect(auto_refresh_item, "toggled",
                   G_CALLBACK(on_menu_toggle_auto_refresh), app);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), auto_refresh_item);
  app->auto_refresh_menu_item = auto_refresh_item;

  /* Hidden files dikhane/chupane ka checkbox option */
  GtkWidget *hidden_item =
      gtk_check_menu_item_new_with_label("Show Hidden Files (Ctrl+H)");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(hidden_item),
                                 app->show_hidden);
  g_signal_connect(hidden_item, "toggled", G_CALLBACK(on_menu_toggle_hidden),
                   app);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), hidden_item);
  app->hidden_menu_item = hidden_item;

  GtkWidget *location_style_item =
      gtk_check_menu_item_new_with_label("Location Bar Always Editable");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(location_style_item),
                                 app->location_bar_always_edit);
  gtk_widget_set_tooltip_text(
      location_style_item,
      "Chalu: location bar hamesha text-entry (path type karne wali) style "
      "mein rahegi.\n"
      "Band: Dolphin jaisi folder-button (breadcrumb) style, jise Ctrl+L ya "
      "pencil se edit kiya ja sakta hai.");
  g_signal_connect(location_style_item, "toggled",
                   G_CALLBACK(on_menu_toggle_location_style), app);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), location_style_item);
  app->location_style_menu_item = location_style_item;

  GtkWidget *video_frame_item =
      gtk_check_menu_item_new_with_label("Video Thumbnail Frame");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(video_frame_item),
                                 app->video_thumbnail_frame);
  gtk_widget_set_tooltip_text(
      video_frame_item,
      "Video files ke thumbnails ke around Thunar/GNOME Files jaisa halka\n"
      "border/frame dikhayein, taake list mein video turant pehchana ja sake.");
  g_signal_connect(video_frame_item, "toggled",
                   G_CALLBACK(on_menu_toggle_video_frame), app);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), video_frame_item);
  app->video_frame_menu_item = video_frame_item;

  GtkWidget *preview_item =
      gtk_check_menu_item_new_with_label("Folder Previews");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(preview_item),
                                 app->folder_previews);
  gtk_widget_set_tooltip_text(preview_item,
                              "Folders ke andar mojood files ki thumbnail "
                              "preview dikhayein (Dolphin jaisa)");
  g_signal_connect(preview_item, "toggled",
                   G_CALLBACK(on_menu_toggle_folder_previews), app);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), preview_item);
  app->preview_toggle = preview_item;

  GtkWidget *list_view_item = gtk_check_menu_item_new_with_label("List View");
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(list_view_item),
                                 app->list_view);
  gtk_widget_set_tooltip_text(
      list_view_item,
      "Files/folders ko grid ki bajaye ek column mein, har item apni\n"
      "alag row par dikhayein (list jaisa).");
  g_signal_connect(list_view_item, "toggled",
                   G_CALLBACK(on_menu_toggle_list_view), app);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), list_view_item);
  app->list_view_menu_item = list_view_item;

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

  GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
  g_signal_connect_swapped(quit_item, "activate", G_CALLBACK(gtk_main_quit),
                           NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

  g_signal_connect(menu, "show", G_CALLBACK(on_overflow_menu_show), app);

  gtk_widget_show_all(menu);
  return menu;
}

static void save_settings_synced(AppWidgets *app) {
  AppWidgets *info_src = info_panel_target_app();
  gboolean real_flag = app->info_panel_visible;
  if (info_src && info_src != app)
    app->info_panel_visible = info_src->info_panel_visible;

  fileview_save_settings(app);

  app->info_panel_visible = real_flag;
}

static gboolean layout_save_debounce_cb(gpointer user_data) {
  AppWidgets *app = user_data;
  app->layout_save_debounce_id = 0;

  save_settings_synced(app);

  return G_SOURCE_REMOVE;
}

static void schedule_layout_save(AppWidgets *app) {
  if (app->layout_save_debounce_id)
    g_source_remove(app->layout_save_debounce_id);
  app->layout_save_debounce_id =
      g_timeout_add(400, layout_save_debounce_cb, app);
}

static void on_sidebar_paned_position_changed(GObject *obj, GParamSpec *pspec,
                                              gpointer user_data) {
  (void)obj;
  (void)pspec;
  AppWidgets *app = user_data;
  if (!app->paned)
    return;
  app->sidebar_position = gtk_paned_get_position(GTK_PANED(app->paned));
  schedule_layout_save(app);
}

static void on_root_paned_position_changed(GObject *obj, GParamSpec *pspec,
                                           gpointer user_data) {
  (void)obj;
  (void)pspec;
  AppWidgets *app = user_data;
  if (!app->root_paned || !app->terminal_panel_visible)
    return;
  app->terminal_position = gtk_paned_get_position(GTK_PANED(app->root_paned));
  schedule_layout_save(app);
}

static void on_content_paned_position_changed(GObject *obj, GParamSpec *pspec,
                                              gpointer user_data) {
  (void)obj;
  (void)pspec;
  AppWidgets *app = user_data;
  if (!app->content_paned || !app->info_panel_visible)
    return;
  app->content_paned_position =
      gtk_paned_get_position(GTK_PANED(app->content_paned));
  schedule_layout_save(app);
}

static gboolean on_window_state_event(GtkWidget *widget,
                                      GdkEventWindowState *event,
                                      gpointer user_data) {
  (void)widget;
  (void)user_data;
  if (!(event->changed_mask & GDK_WINDOW_STATE_MAXIMIZED))
    return FALSE;
  if (!g_mw || !g_mw->panes[0] || !g_mw->panes[0]->app)
    return FALSE;
  AppWidgets *app = g_mw->panes[0]->app;
  app->window_maximized =
      (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
  schedule_layout_save(app);
  return FALSE;
}

static gboolean on_window_configure_event(GtkWidget *widget,
                                          GdkEventConfigure *event,
                                          gpointer user_data) {
  (void)widget;
  (void)user_data;
  if (!g_mw || !g_mw->panes[0] || !g_mw->panes[0]->app)
    return FALSE;
  AppWidgets *app = g_mw->panes[0]->app;
  if (app->window_maximized)
    return FALSE;
  if (event->width > 0 && event->height > 0) {
    app->window_width = event->width;
    app->window_height = event->height;
    schedule_layout_save(app);
  }
  return FALSE;
}

static gboolean restore_terminal_panel_idle(gpointer user_data) {
  AppWidgets *app = user_data;
  if (app->terminal_panel_visible) {
    app->terminal_panel_visible = FALSE;
    terminal_panel_toggle(app);
  }
  return G_SOURCE_REMOVE;
}

static gboolean restore_split_view_idle(gpointer user_data) {
  (void)user_data;

  // if (g_mw && !g_mw->panes[1])
  //   do_split();
  if (g_mw && !g_mw->panes[1]) {
    do_split();

    if (g_mw->split_paned) {
      GtkAllocation alloc;
      gtk_widget_get_allocation(g_mw->content_slot, &alloc);

      if (alloc.width > 1)
        gtk_paned_set_position(GTK_PANED(g_mw->split_paned),
                               alloc.width / 2);
    }
  }

  return G_SOURCE_REMOVE;
}

static PaneUI *build_pane(GtkWidget *window, gboolean with_sidebar) {
  gint64 pane_start = g_get_monotonic_time();
  AppWidgets *app = g_new0(AppWidgets, 1);
  app->window = window;
  app->select_anchor_idx = -1;
  app->window_maximized = TRUE;
  app->window_width = 1000;
  app->window_height = 650;

  app->sidebar_visible = TRUE;
  app->sidebar_position = 220;

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *fileview_widget = fileview_create(app);

  if (!with_sidebar) {
    app->info_panel_visible = FALSE;
    app->terminal_panel_visible = FALSE;
  }

  app->content_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

  gtk_paned_pack1(GTK_PANED(app->content_paned), fileview_widget, TRUE, FALSE);

  app->info_panel = infopanel_create(app);
  gtk_paned_pack2(GTK_PANED(app->content_paned), app->info_panel, FALSE, FALSE);

  if (app->content_paned_position > 0)
    gtk_paned_set_position(GTK_PANED(app->content_paned),
                           app->content_paned_position);
  g_signal_connect(app->content_paned, "notify::position",
                   G_CALLBACK(on_content_paned_position_changed), app);

  if (app->info_panel_visible) {
    gtk_widget_show(app->info_panel);
    infopanel_refresh(app);
  }

  if (with_sidebar) {
    app->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    GtkWidget *sidebar_widget = sidebar_create(app);
    gtk_paned_pack1(GTK_PANED(app->paned), sidebar_widget, FALSE, FALSE);

    gtk_widget_show_all(sidebar_widget);
    gtk_widget_set_no_show_all(sidebar_widget, TRUE);

    if (!app->sidebar_visible)
      gtk_widget_hide(sidebar_widget);

    gtk_paned_pack2(GTK_PANED(app->paned), app->content_paned, TRUE, FALSE);

    if (app->sidebar_position > 0)
      gtk_paned_set_position(GTK_PANED(app->paned), app->sidebar_position);

    g_signal_connect(app->paned, "notify::position",
                     G_CALLBACK(on_sidebar_paned_position_changed), app);
  }

  app->statusbar = gtk_statusbar_new();
  app->statusbar_ctx =
      gtk_statusbar_get_context_id(GTK_STATUSBAR(app->statusbar), "main");

  GtkWidget *status_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(status_hbox), app->statusbar, TRUE, TRUE, 0);

  app->freespace_label = gtk_label_new("");
  gtk_widget_set_margin_start(app->freespace_label, 8);
  gtk_widget_set_margin_end(app->freespace_label, 8);
  gtk_widget_set_margin_top(app->freespace_label, 2);
  gtk_widget_set_margin_bottom(app->freespace_label, 2);

  GtkWidget *freespace_frame = gtk_frame_new(NULL);
  gtk_frame_set_shadow_type(GTK_FRAME(freespace_frame), GTK_SHADOW_IN);
  gtk_container_add(GTK_CONTAINER(freespace_frame), app->freespace_label);
  gtk_box_pack_end(GTK_BOX(status_hbox), freespace_frame, FALSE, FALSE, 2);
  GtkWidget *top_and_status_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(top_and_status_vbox),
                     with_sidebar ? app->paned : app->content_paned, TRUE, TRUE,
                     0);
  gtk_box_pack_start(GTK_BOX(top_and_status_vbox), status_hbox, FALSE, FALSE,
                     0);

  gtk_box_pack_start(GTK_BOX(vbox), top_and_status_vbox, TRUE, TRUE, 0);

  if (app->btn_back)
    gtk_widget_set_sensitive(app->btn_back, FALSE);

  if (app->btn_forward)
    gtk_widget_set_sensitive(app->btn_forward, FALSE);

  if (app->preview_toggle) {
    g_signal_handlers_block_by_func(
        app->preview_toggle, G_CALLBACK(on_menu_toggle_folder_previews), app);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->preview_toggle),
                                   app->folder_previews);
    g_signal_handlers_unblock_by_func(
        app->preview_toggle, G_CALLBACK(on_menu_toggle_folder_previews), app);
  }

  thumbnail_init();
  AppWidgets *start_app = NULL;

  if (g_mw && g_mw->panes[0])
    start_app = g_mw->panes[0]->app;

  if (g_cli_start_dir) {

    char *dir_copy = g_cli_start_dir;
    g_cli_start_dir = NULL; /* consume once so a split-pane doesn't reuse it */

    fileview_navigate(app, dir_copy, FALSE);

    g_free(dir_copy);

  } else if (start_app && start_app->current_dir) {

    char *dir_copy = g_strdup(start_app->current_dir);

    fileview_navigate(app, dir_copy, FALSE);

    g_free(dir_copy);

  } else {

    fileview_navigate(app, g_get_home_dir(), FALSE);
  }

  if (app->icon_view) {
    g_signal_connect(app->icon_view, "focus-in-event",
                     G_CALLBACK(on_pane_focus_in), app);
  }

  PaneUI *p = g_new0(PaneUI, 1);
  p->root = vbox;
  p->app = app;
  g_print("build_pane: %.2f ms\n",
          (g_get_monotonic_time() - pane_start) / 1000.0);
  return p;
}

static void on_main_window_destroy(GtkWidget *window, gpointer user_data) {
  (void)window;
  (void)user_data;
  if (!g_mw || !g_mw->panes[0] || !g_mw->panes[0]->app)
    return;
  AppWidgets *app = g_mw->panes[0]->app;
  if (app->layout_save_debounce_id) {
    g_source_remove(app->layout_save_debounce_id);
    app->layout_save_debounce_id = 0;
  }
  save_settings_synced(app);
}

static void activate(GtkApplication *gapp, gpointer user_data) {
  (void)user_data;

  g_mw = g_new0(MainWin, 1);

  g_mw->window = gtk_application_window_new(gapp);
  gtk_window_set_title(GTK_WINDOW(g_mw->window), "GTK File Manager");
  gtk_window_set_default_size(GTK_WINDOW(g_mw->window), 1000, 650);

  GtkWidget *outer_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_container_add(GTK_CONTAINER(g_mw->window), outer_vbox);

  g_mw->panes[0] = build_pane(g_mw->window, TRUE);

  AppWidgets *app0 = g_mw->panes[0]->app;

  if (app0) {

    GtkWidget *toolbar = build_toolbar(app0);

    gtk_box_pack_start(GTK_BOX(outer_vbox), toolbar, FALSE, FALSE, 0);
  }

  fileview_refresh_location_bar(app0);

  GtkWidget *content_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  g_mw->content_slot = content_slot;

  gtk_box_pack_start(GTK_BOX(content_slot), g_mw->panes[0]->root, TRUE, TRUE,
                     0);

  if (app0) {

    app0->terminal_panel = terminal_panel_create(app0);

    g_mw->root_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

    gtk_paned_pack1(GTK_PANED(g_mw->root_paned), content_slot, TRUE, FALSE);

    gtk_paned_pack2(GTK_PANED(g_mw->root_paned), app0->terminal_panel, FALSE,
                    TRUE);

    gtk_box_pack_start(GTK_BOX(outer_vbox), g_mw->root_paned, TRUE, TRUE, 0);

    app0->root_paned = g_mw->root_paned;

    g_signal_connect(g_mw->root_paned, "notify::position",
                     G_CALLBACK(on_root_paned_position_changed), app0);

  } else {

    gtk_box_pack_start(GTK_BOX(outer_vbox), content_slot, TRUE, TRUE, 0);
  }

  if (app0) {

    if (!app0->window_maximized && app0->window_width > 0 &&
        app0->window_height > 0) {
      gtk_window_resize(GTK_WINDOW(g_mw->window), app0->window_width,
                        app0->window_height);
    }

    if (app0->window_maximized)
      gtk_window_maximize(GTK_WINDOW(g_mw->window));

    if (app0->terminal_panel_visible) {
      g_idle_add(restore_terminal_panel_idle, app0);
    }

    if (app0->split_view_active) {
      g_idle_add(restore_split_view_idle, NULL);
    }
  }

  g_signal_connect(g_mw->window, "key-press-event",
                   G_CALLBACK(on_window_key_press), NULL);

  g_signal_connect(g_mw->window, "window-state-event",
                   G_CALLBACK(on_window_state_event), NULL);

  g_signal_connect(g_mw->window, "configure-event",
                   G_CALLBACK(on_window_configure_event), NULL);

  g_signal_connect(g_mw->window, "destroy", G_CALLBACK(on_main_window_destroy),
                   NULL);

  gtk_widget_show_all(g_mw->window);

  if (g_mw->panes[0] && g_mw->panes[0]->app && g_mw->panes[0]->app->icon_view) {
    gtk_widget_grab_focus(g_mw->panes[0]->app->icon_view);
  }
}

static void on_app_open(GApplication *gapp, GFile **files, gint n_files,
                        const gchar *hint, gpointer user_data) {
  (void)hint;

  if (n_files > 0) {
    char *path = g_file_get_path(files[0]);
    if (path) {
      g_free(g_cli_start_dir);
      if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_cli_start_dir = path; /* takes ownership */
      } else {
        /* A file path was passed -- open its containing folder. */
        g_cli_start_dir = g_path_get_dirname(path);
        g_free(path);
      }
    }
  }

  /* GApplication emits "open" instead of "activate" when there are file
   * args, so build the window here too. */
  activate(GTK_APPLICATION(gapp), user_data);
}

int main(int argc, char **argv) {

  for (int i = 1; i < argc; i++) {
    if (g_strcmp0(argv[i], "--version") == 0 ||
      g_strcmp0(argv[i], "--v") == 0) {

      g_print("gtkfm version 1.0.0\n");
    return 0;
      }
  }


  GtkApplication *gapp =
  gtk_application_new("com.example.gtkfm",
                      G_APPLICATION_NON_UNIQUE |
                      G_APPLICATION_HANDLES_OPEN);

  g_signal_connect(gapp, "activate",
                   G_CALLBACK(activate), NULL);

  g_signal_connect(gapp, "open",
                   G_CALLBACK(on_app_open), NULL);

  int status =
  g_application_run(G_APPLICATION(gapp), argc, argv);

  thumbnail_shutdown();

  g_object_unref(gapp);

  return status;
}
// int main(int argc, char **argv) {
//   GtkApplication *gapp =
//       gtk_application_new("com.example.gtkfm", G_APPLICATION_NON_UNIQUE |
//                                                    G_APPLICATION_HANDLES_OPEN);
//
//   g_signal_connect(gapp, "activate", G_CALLBACK(activate), NULL);
//   g_signal_connect(gapp, "open", G_CALLBACK(on_app_open), NULL);
//
//   int status = g_application_run(G_APPLICATION(gapp), argc, argv);
//
//   thumbnail_shutdown();
//   g_object_unref(gapp);
//   return status;
// }
