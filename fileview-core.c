#include "fileview_internal.h"

static char *settings_file_path(void) {
  char *dir = g_build_filename(g_get_user_config_dir(), "gtkfm", NULL);
  g_mkdir_with_parents(dir, 0700);
  char *file = g_build_filename(dir, "settings", NULL);
  g_free(dir);
  return file;
}

void fileview_load_settings(AppWidgets *app) {
  char *file = settings_file_path();
  GKeyFile *kf = g_key_file_new();

  app->auto_refresh_enabled = TRUE;

  if (g_key_file_load_from_file(kf, file, G_KEY_FILE_NONE, NULL)) {
    GError *err = NULL;

    gboolean auto_refresh_enabled =
        g_key_file_get_boolean(kf, "view", "auto_refresh_enabled", &err);
    if (!err)
      app->auto_refresh_enabled = auto_refresh_enabled;
    g_clear_error(&err);

    gint sort_mode = g_key_file_get_integer(kf, "view", "sort_mode", &err);
    if (!err && sort_mode >= SORT_NAME_ASC && sort_mode <= SORT_TYPE_DESC)
      app->sort_mode = (SortMode)sort_mode;
    g_clear_error(&err);

    gboolean folders_first =
        g_key_file_get_boolean(kf, "view", "folders_first", &err);
    if (!err)
      app->folders_first = folders_first;
    g_clear_error(&err);

    gboolean folder_previews =
        g_key_file_get_boolean(kf, "view", "folder_previews", &err);
    if (!err)
      app->folder_previews = folder_previews;
    g_clear_error(&err);

    gboolean video_thumbnail_frame =
        g_key_file_get_boolean(kf, "view", "video_thumbnail_frame", &err);
    if (!err)
      app->video_thumbnail_frame = video_thumbnail_frame;
    g_clear_error(&err);

    gboolean list_view = g_key_file_get_boolean(kf, "view", "list_view", &err);
    if (!err)
      app->list_view = list_view;
    g_clear_error(&err);

    gint icon_size = g_key_file_get_integer(kf, "view", "icon_size", &err);
    if (!err && icon_size >= ICON_SIZE_MIN && icon_size <= ICON_SIZE_MAX)
      app->icon_size = icon_size;
    g_clear_error(&err);

    gboolean show_hidden =
        g_key_file_get_boolean(kf, "view", "show_hidden", &err);
    if (!err)
      app->show_hidden = show_hidden;
    g_clear_error(&err);

    gboolean info_panel_visible =
        g_key_file_get_boolean(kf, "view", "info_panel_visible", &err);
    if (!err)
      app->info_panel_visible = info_panel_visible;
    g_clear_error(&err);

    gboolean terminal_panel_visible =
        g_key_file_get_boolean(kf, "view", "terminal_panel_visible", &err);
    if (!err)
      app->terminal_panel_visible = terminal_panel_visible;
    g_clear_error(&err);

    gboolean sidebar_visible =
        g_key_file_get_boolean(kf, "view", "sidebar_visible", &err);
    if (!err)
      app->sidebar_visible = sidebar_visible;
    g_clear_error(&err);

    gboolean split_view_active =
        g_key_file_get_boolean(kf, "view", "split_view_active", &err);
    if (!err)
      app->split_view_active = split_view_active;
    g_clear_error(&err);

    gboolean location_bar_always_edit =
        g_key_file_get_boolean(kf, "view", "location_bar_always_edit", &err);
    if (!err)
      app->location_bar_always_edit = location_bar_always_edit;
    g_clear_error(&err);

    gint sidebar_position =
        g_key_file_get_integer(kf, "layout", "sidebar_position", &err);
    if (!err && sidebar_position > 0)
      app->sidebar_position = sidebar_position;
    g_clear_error(&err);

    gint terminal_position =
        g_key_file_get_integer(kf, "layout", "terminal_position", &err);
    if (!err && terminal_position > 0)
      app->terminal_position = terminal_position;
    g_clear_error(&err);

    gint content_paned_position =
        g_key_file_get_integer(kf, "layout", "content_paned_position", &err);
    if (!err && content_paned_position > 0)
      app->content_paned_position = content_paned_position;
    g_clear_error(&err);

    gboolean window_maximized =
        g_key_file_get_boolean(kf, "window", "maximized", &err);
    if (!err)
      app->window_maximized = window_maximized;
    g_clear_error(&err);

    gint window_width = g_key_file_get_integer(kf, "window", "width", &err);
    if (!err && window_width > 0)
      app->window_width = window_width;
    g_clear_error(&err);

    gint window_height = g_key_file_get_integer(kf, "window", "height", &err);
    if (!err && window_height > 0)
      app->window_height = window_height;
    g_clear_error(&err);
  }

  g_key_file_free(kf);
  g_free(file);

  thumbnail_set_video_frame_enabled(app->video_thumbnail_frame);
}

void fileview_save_settings(AppWidgets *app) {
  char *file = settings_file_path();
  GKeyFile *kf = g_key_file_new();

  g_key_file_load_from_file(kf, file, G_KEY_FILE_NONE, NULL);

  g_key_file_set_integer(kf, "view", "sort_mode", (gint)app->sort_mode);
  g_key_file_set_boolean(kf, "view", "folders_first", app->folders_first);
  g_key_file_set_boolean(kf, "view", "folder_previews", app->folder_previews);
  g_key_file_set_boolean(kf, "view", "video_thumbnail_frame",
                         app->video_thumbnail_frame);
  g_key_file_set_boolean(kf, "view", "list_view", app->list_view);
  g_key_file_set_integer(kf, "view", "icon_size", app->icon_size);
  g_key_file_set_boolean(kf, "view", "show_hidden", app->show_hidden);
  g_key_file_set_boolean(kf, "view", "info_panel_visible",
                         app->info_panel_visible);
  g_key_file_set_boolean(kf, "view", "terminal_panel_visible",
                         app->terminal_panel_visible);
  if (app->paned) {
    g_key_file_set_boolean(kf, "view", "sidebar_visible", app->sidebar_visible);

    if (app->sidebar_position > 0)
      g_key_file_set_integer(kf, "layout", "sidebar_position",
                             app->sidebar_position);

    g_key_file_set_boolean(kf, "view", "split_view_active",
                           app->split_view_active);
  }

  g_key_file_set_boolean(kf, "view", "location_bar_always_edit",
                         app->location_bar_always_edit);
  g_key_file_set_boolean(kf, "view", "auto_refresh_enabled",
                         app->auto_refresh_enabled);
  if (app->terminal_position > 0)
    g_key_file_set_integer(kf, "layout", "terminal_position",
                           app->terminal_position);
  if (app->content_paned_position > 0)
    g_key_file_set_integer(kf, "layout", "content_paned_position",
                           app->content_paned_position);

  g_key_file_set_boolean(kf, "window", "maximized", app->window_maximized);
  if (app->window_width > 0)
    g_key_file_set_integer(kf, "window", "width", app->window_width);
  if (app->window_height > 0)
    g_key_file_set_integer(kf, "window", "height", app->window_height);

  gsize len = 0;
  char *data = g_key_file_to_data(kf, &len, NULL);
  if (data)
    g_file_set_contents(file, data, len, NULL);
  g_free(data);

  g_key_file_free(kf);
  g_free(file);
}

const GtkTargetEntry dnd_targets[1] = {
    {(char *)"text/uri-list", 0, DND_TARGET_URI_LIST},
};

GList *uri_list_to_paths(GtkSelectionData *data) {
  GList *out = NULL;
  char **uris = gtk_selection_data_get_uris(data);
  if (!uris)
    return NULL;
  for (int i = 0; uris[i]; i++) {
    char *path = g_filename_from_uri(uris[i], NULL, NULL);
    if (path)
      out = g_list_append(out, path);
  }
  g_strfreev(uris);
  return out;
}

GList *g_clipboard_paths = NULL;
gboolean g_clipboard_cut = FALSE;
AppWidgets *g_clipboard_source_app = NULL;

static void update_path_entry(AppWidgets *app) {
  char *tdir = trash_files_dir();
  gboolean showing_trash =
      app->current_dir && g_strcmp0(app->current_dir, tdir) == 0;
  if (app->path_entry) {
    gtk_entry_set_text(GTK_ENTRY(app->path_entry),
                       showing_trash
                           ? "trash://"
                           : (app->current_dir ? app->current_dir : ""));
  }
  g_free(tdir);
  fileview_refresh_location_bar(app);
}

void fileview_set_status(AppWidgets *app, const char *msg) {
  set_status(app, msg);
}

void set_status(AppWidgets *app, const char *msg) {
  gtk_statusbar_pop(GTK_STATUSBAR(app->statusbar), app->statusbar_ctx);
  gtk_statusbar_push(GTK_STATUSBAR(app->statusbar), app->statusbar_ctx, msg);
}

static GHashTable *g_themed_icon_cache = NULL;

static char *dot_directory_path(const char *dir_path) {
  return g_build_filename(dir_path, ".directory", NULL);
}

char *get_custom_folder_icon(const char *dir_path) {
  char *dotfile = dot_directory_path(dir_path);
  GKeyFile *kf = g_key_file_new();
  char *icon_value = NULL;

  if (g_key_file_load_from_file(kf, dotfile, G_KEY_FILE_NONE, NULL)) {
    icon_value = g_key_file_get_string(kf, "Desktop Entry", "Icon", NULL);
    if (icon_value && *icon_value == '\0') {
      g_free(icon_value);
      icon_value = NULL;
    }
  }

  g_key_file_free(kf);
  g_free(dotfile);
  return icon_value;
}

gboolean set_custom_folder_icon(const char *dir_path, const char *icon_value) {
  char *dotfile = dot_directory_path(dir_path);
  GKeyFile *kf = g_key_file_new();
  g_key_file_load_from_file(kf, dotfile, G_KEY_FILE_NONE, NULL);

  gboolean ok;
  if (icon_value && *icon_value) {
    g_key_file_set_string(kf, "Desktop Entry", "Icon", icon_value);
    char *data = g_key_file_to_data(kf, NULL, NULL);
    ok = g_file_set_contents(dotfile, data, -1, NULL);
    g_free(data);
  } else {
    g_key_file_remove_key(kf, "Desktop Entry", "Icon", NULL);
    gsize n_keys = 0;
    char **keys = g_key_file_get_keys(kf, "Desktop Entry", &n_keys, NULL);
    if (keys)
      g_strfreev(keys);
    if (n_keys == 0) {
      /* Ab kuch bacha hi nahin to ".directory" file hata do. */
      ok = (g_remove(dotfile) == 0) || (errno == ENOENT);
    } else {
      char *data = g_key_file_to_data(kf, NULL, NULL);
      ok = g_file_set_contents(dotfile, data, -1, NULL);
      g_free(data);
    }
  }

  g_key_file_free(kf);
  g_free(dotfile);
  return ok;
}

GdkPixbuf *themed_icon_for(const char *content_type, gboolean is_dir,
                           const char *path, int size) {

  if (!g_themed_icon_cache)
    g_themed_icon_cache =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

  char *custom_icon = NULL;
  const char *type_key;

  if (is_dir) {
    custom_icon = get_custom_folder_icon(path);

    if (custom_icon) {
      type_key = custom_icon;
    } else {
      char *base = g_path_get_basename(path);

      if (g_strcmp0(base, "Desktop") == 0)
        type_key = "folder-desktop";
      else if (g_strcmp0(base, "Documents") == 0)
        type_key = "folder-documents";
      else if (g_strcmp0(base, "Downloads") == 0)
        type_key = "folder-download";
      else if (g_strcmp0(base, "Music") == 0)
        type_key = "folder-music";
      else if (g_strcmp0(base, "Pictures") == 0)
        type_key = "folder-pictures";
      else if (g_strcmp0(base, "Videos") == 0)
        type_key = "folder-videos";
      else if (g_strcmp0(base, "Public") == 0)
        type_key = "folder-public";
      else if (g_strcmp0(base, "Templates") == 0)
        type_key = "folder-templates";
      else
        type_key = "folder";

      g_free(base);
    }
  } else {
    type_key = content_type ? content_type : "application/octet-stream";
  }

  char *cache_key = g_strdup_printf("%s@%d", type_key, size);

  gpointer cached = g_hash_table_lookup(g_themed_icon_cache, cache_key);
  if (cached) {
    g_free(cache_key);
    g_free(custom_icon);
    return g_object_ref(GDK_PIXBUF(cached));
  }

  GdkPixbuf *pixbuf = NULL;

  if (custom_icon && g_path_is_absolute(custom_icon) &&
      g_file_test(custom_icon, G_FILE_TEST_IS_REGULAR)) {
    pixbuf =
        gdk_pixbuf_new_from_file_at_scale(custom_icon, size, size, TRUE, NULL);
  }

  if (!pixbuf) {
    GIcon *icon = is_dir ? g_themed_icon_new(type_key)
                         : g_content_type_get_icon(type_key);

    GtkIconTheme *theme = gtk_icon_theme_get_default();

    GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(
        theme, icon, size, GTK_ICON_LOOKUP_FORCE_SIZE);

    if (info) {
      pixbuf = gtk_icon_info_load_icon(info, NULL);
      g_object_unref(info);
    }

    g_object_unref(icon);
  }

  g_free(custom_icon);

  if (pixbuf)
    g_hash_table_insert(g_themed_icon_cache, cache_key, g_object_ref(pixbuf));
  else
    g_free(cache_key);

  return pixbuf;
}

GdkPixbuf *icon_for_gicon_sized(GIcon *icon, int size) {
  if (!icon)
    return NULL;
  GtkIconTheme *theme = gtk_icon_theme_get_default();
  GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(
      theme, icon, size, GTK_ICON_LOOKUP_FORCE_SIZE);
  if (!info)
    return NULL;
  GdkPixbuf *pb = gtk_icon_info_load_icon(info, NULL);
  g_object_unref(info);
  return pb;
}

GdkPixbuf *icon_for_name_sized(const char *name, int size) {
  GIcon *icon = g_themed_icon_new(name);
  GdkPixbuf *pb = icon_for_gicon_sized(icon, size);
  g_object_unref(icon);
  return pb;
}

static GIcon *desktop_file_gicon(const char *path) {
  GDesktopAppInfo *dinfo = g_desktop_app_info_new_from_filename(path);
  if (!dinfo)
    return NULL;
  GIcon *icon = g_app_info_get_icon(G_APP_INFO(dinfo));
  if (icon)
    g_object_ref(icon);
  g_object_unref(dinfo);
  return icon;
}

static GHashTable *g_symlink_emblem_cache = NULL;

static GdkPixbuf *get_symlink_emblem(int size) {
  if (!g_symlink_emblem_cache)
    g_symlink_emblem_cache =
        g_hash_table_new_full(NULL, NULL, NULL, g_object_unref);

  gpointer cached =
      g_hash_table_lookup(g_symlink_emblem_cache, GINT_TO_POINTER(size));
  if (cached)
    return GDK_PIXBUF(cached);

  GdkPixbuf *pb = icon_for_name_sized("emblem-symbolic-link", size);
  if (pb)
    g_hash_table_insert(g_symlink_emblem_cache, GINT_TO_POINTER(size), pb);
  return pb;
}

GdkPixbuf *add_symlink_emblem(GdkPixbuf *base) {
  if (!base)
    return NULL;

  int w = gdk_pixbuf_get_width(base);
  int h = gdk_pixbuf_get_height(base);

  int emblem_size = w / 2;
  if (emblem_size < 8)
    emblem_size = 8;

  GdkPixbuf *emblem = get_symlink_emblem(emblem_size);
  if (!emblem)
    return g_object_ref(base);

  GdkPixbuf *result = gdk_pixbuf_copy(base);
  if (!result)
    return g_object_ref(base);

  int ew = gdk_pixbuf_get_width(emblem);
  int eh = gdk_pixbuf_get_height(emblem);
  int x = 0;
  int y = h - eh;
  if (y < 0)
    y = 0;
  if (x + ew > w)
    ew = w - x;
  if (y + eh > h)
    eh = h - y;

  if (ew > 0 && eh > 0)
    gdk_pixbuf_composite(emblem, result, x, y, ew, eh, x, y, 1.0, 1.0,
                         GDK_INTERP_BILINEAR, 255);

  return result;
}

static GHashTable *g_readonly_emblem_cache =
    NULL; /* key: GINT_TO_POINTER(size),
           * value: GdkPixbuf (owned) */

static GdkPixbuf *get_readonly_emblem(int size) {
  if (!g_readonly_emblem_cache)
    g_readonly_emblem_cache =
        g_hash_table_new_full(NULL, NULL, NULL, g_object_unref);

  gpointer cached =
      g_hash_table_lookup(g_readonly_emblem_cache, GINT_TO_POINTER(size));
  if (cached)
    return GDK_PIXBUF(cached);

  GdkPixbuf *pb = icon_for_name_sized("emblem-readonly", size);
  if (!pb)
    pb = icon_for_name_sized("emblem-locked", size);
  if (pb)
    g_hash_table_insert(g_readonly_emblem_cache, GINT_TO_POINTER(size), pb);
  return pb;
}

GdkPixbuf *add_readonly_emblem(GdkPixbuf *base) {
  if (!base)
    return NULL;

  int w = gdk_pixbuf_get_width(base);
  int h = gdk_pixbuf_get_height(base);

  int emblem_size = w / 2;
  if (emblem_size < 8)
    emblem_size = 8;

  GdkPixbuf *emblem = get_readonly_emblem(emblem_size);
  if (!emblem)
    return g_object_ref(base);

  GdkPixbuf *result = gdk_pixbuf_copy(base);
  if (!result)
    return g_object_ref(base);

  int ew = gdk_pixbuf_get_width(emblem);
  int eh = gdk_pixbuf_get_height(emblem);
  int x = w - ew;
  int y = h - eh;
  if (x < 0)
    x = 0;
  if (y < 0)
    y = 0;
  if (x + ew > w)
    ew = w - x;
  if (y + eh > h)
    eh = h - y;

  if (ew > 0 && eh > 0)
    gdk_pixbuf_composite(emblem, result, x, y, ew, eh, x, y, 1.0, 1.0,
                         GDK_INTERP_BILINEAR, 255);

  return result;
}

void clear_file_store(AppWidgets *app) {
  GtkTreeIter iter;
  if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->file_store), &iter)) {
    do {
      gpointer vol = NULL;
      gpointer gicon = NULL;
      gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_VOLUME,
                         &vol, COL_GICON, &gicon, -1);
      if (vol)
        g_object_unref(G_VOLUME(vol));
      if (gicon)
        g_object_unref(G_ICON(gicon));
    } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->file_store), &iter));
  }
  gtk_list_store_clear(app->file_store);
}

void thumbnail_ready_cb(const char *filepath, GdkPixbuf *pixbuf,
                        gpointer user_data) {
  ThumbCtx *ctx = user_data;
  (void)filepath;

  if (ctx->generation != ctx->app->populate_generation) {
    if (pixbuf)
      g_object_unref(pixbuf);

    gtk_tree_row_reference_free(ctx->ref);
    g_free(ctx);
    return;
  }

  if (gtk_tree_row_reference_valid(ctx->ref)) {
    GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->ref);
    GtkTreeIter iter;
    if (pixbuf && gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->app->file_store),
                                          &iter, path)) {
      if (ctx->is_symlink) {
        GdkPixbuf *emblemed = add_symlink_emblem(pixbuf);
        if (emblemed) {
          g_object_unref(pixbuf);
          pixbuf = emblemed;
        }
      }
      if (ctx->is_readonly) {
        GdkPixbuf *emblemed = add_readonly_emblem(pixbuf);
        if (emblemed) {
          g_object_unref(pixbuf);
          pixbuf = emblemed;
        }
      }
      gtk_list_store_set(ctx->app->file_store, &iter, COL_PIXBUF, pixbuf,
                         COL_THEMED_ICON, FALSE, -1);
      if (ctx->app->icon_view)
        gtk_widget_queue_draw(ctx->app->icon_view);
    }
    gtk_tree_path_free(path);
  }
  if (pixbuf)
    g_object_unref(pixbuf);

  gtk_tree_row_reference_free(ctx->ref);
  g_free(ctx);
}

static void populate_computer(AppWidgets *app, const char *pending_select);
static void populate_recent(AppWidgets *app, const char *pending_select);

static void update_freespace_label(AppWidgets *app) {
  if (!app->freespace_label)
    return;

  if (!app->current_dir || g_strcmp0(app->current_dir, COMPUTER_URI) == 0) {
    gtk_label_set_text(GTK_LABEL(app->freespace_label), "");
    return;
  }

  GFile *f = g_file_new_for_path(app->current_dir);
  GError *err = NULL;
  GFileInfo *info = g_file_query_filesystem_info(
      f, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL, &err);

  if (info) {
    guint64 free_bytes = g_file_info_get_attribute_uint64(
        info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    char *free_str = g_format_size_full(free_bytes, G_FORMAT_SIZE_IEC_UNITS);
    char *msg = g_strdup_printf("%s free", free_str);
    gtk_label_set_text(GTK_LABEL(app->freespace_label), msg);
    g_free(msg);
    g_free(free_str);
    g_object_unref(info);
  } else {
    gtk_label_set_text(GTK_LABEL(app->freespace_label), "");
  }
  if (err)
    g_error_free(err);
  g_object_unref(f);
}

void focus_first_item(AppWidgets *app) {
  if (!app->icon_view || !app->file_filter)
    return;
  gint n =
      gtk_tree_model_iter_n_children(GTK_TREE_MODEL(app->file_filter), NULL);
  if (n <= 0)
    return;

  GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
  gtk_icon_view_unselect_all(GTK_ICON_VIEW(app->icon_view));
  gtk_icon_view_select_path(GTK_ICON_VIEW(app->icon_view), path);
  gtk_icon_view_set_cursor(GTK_ICON_VIEW(app->icon_view), path, NULL, FALSE);

  GtkWidget *cur_focus = gtk_window_get_focus(GTK_WINDOW(app->window));
  gboolean user_typing_elsewhere =
      cur_focus == app->path_entry || cur_focus == app->search_entry;
  gboolean window_active = gtk_window_is_active(GTK_WINDOW(app->window));

  if (window_active && !user_typing_elsewhere && !terminal_panel_has_focus(app))
    gtk_widget_grab_focus(app->icon_view);

  gtk_tree_path_free(path);
}

static gboolean select_item_with_path(AppWidgets *app, const char *child_path) {
  if (!app->icon_view || !app->file_filter || !child_path)
    return FALSE;

  GtkTreeModel *model = GTK_TREE_MODEL(app->file_filter);
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter_first(model, &iter))
    return FALSE;

  do {
    char *path = NULL;
    gtk_tree_model_get(model, &iter, COL_PATH, &path, -1);
    gboolean match = g_strcmp0(path, child_path) == 0;
    g_free(path);
    if (match) {
      GtkTreePath *tpath = gtk_tree_model_get_path(model, &iter);
      gtk_icon_view_unselect_all(GTK_ICON_VIEW(app->icon_view));
      gtk_icon_view_select_path(GTK_ICON_VIEW(app->icon_view), tpath);
      gtk_icon_view_set_cursor(GTK_ICON_VIEW(app->icon_view), tpath, NULL,
                               FALSE);
      gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(app->icon_view), tpath, FALSE,
                                   0, 0);
      if (!terminal_panel_has_focus(app))
        gtk_widget_grab_focus(app->icon_view);
      gtk_tree_path_free(tpath);
      return TRUE;
    }
  } while (gtk_tree_model_iter_next(model, &iter));

  return FALSE;
}

gboolean fileview_select_path(AppWidgets *app, const char *child_path) {
  return select_item_with_path(app, child_path);
}

static gboolean select_items_with_paths(AppWidgets *app, GList *paths) {
  if (!app->icon_view || !app->file_filter || !paths)
    return FALSE;

  const char *focus_target = g_list_last(paths)->data;

  GHashTable *wanted = g_hash_table_new(g_str_hash, g_str_equal);
  for (GList *l = paths; l; l = l->next)
    g_hash_table_add(wanted, l->data);

  GtkTreeModel *model = GTK_TREE_MODEL(app->file_filter);
  gtk_icon_view_unselect_all(GTK_ICON_VIEW(app->icon_view));

  GtkTreePath *focus_tpath = NULL;
  GList *matched_tpaths = NULL; /* GtkTreePath*, model order mein */
  gboolean any = FALSE;
  GtkTreeIter iter;
  if (gtk_tree_model_get_iter_first(model, &iter)) {
    do {
      char *path = NULL;
      gtk_tree_model_get(model, &iter, COL_PATH, &path, -1);
      if (path && g_hash_table_contains(wanted, path)) {
        GtkTreePath *tpath = gtk_tree_model_get_path(model, &iter);
        matched_tpaths = g_list_append(matched_tpaths,
                                       tpath); /* ownership yahin rehti hai */
        any = TRUE;
        if (!focus_tpath && g_strcmp0(path, focus_target) == 0)
          focus_tpath = gtk_tree_path_copy(tpath);
      }
      g_free(path);
    } while (gtk_tree_model_iter_next(model, &iter));
  }
  g_hash_table_destroy(wanted);

  if (!focus_tpath && matched_tpaths)
    focus_tpath =
        gtk_tree_path_copy((GtkTreePath *)g_list_last(matched_tpaths)->data);

  if (focus_tpath) {
    gtk_icon_view_set_cursor(GTK_ICON_VIEW(app->icon_view), focus_tpath, NULL,
                             FALSE);
    gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(app->icon_view), focus_tpath,
                                 FALSE, 0, 0);
    if (!terminal_panel_has_focus(app))
      gtk_widget_grab_focus(app->icon_view);
  }

  for (GList *l = matched_tpaths; l; l = l->next)
    gtk_icon_view_select_path(GTK_ICON_VIEW(app->icon_view),
                              (GtkTreePath *)l->data);

  if (focus_tpath)
    gtk_tree_path_free(focus_tpath);
  g_list_free_full(matched_tpaths, (GDestroyNotify)gtk_tree_path_free);

  return any;
}

char *capture_post_delete_selection_target(AppWidgets *app) {
  if (!app->icon_view || !app->file_filter)
    return NULL;

  GList *selected =
      gtk_icon_view_get_selected_items(GTK_ICON_VIEW(app->icon_view));
  if (!selected)
    return NULL;

  gint min_idx = G_MAXINT, max_idx = -1;
  for (GList *l = selected; l; l = l->next) {
    GtkTreePath *tp = l->data;
    gint *indices = gtk_tree_path_get_indices(tp);
    gint idx = indices ? indices[0] : -1;
    if (idx < min_idx)
      min_idx = idx;
    if (idx > max_idx)
      max_idx = idx;
  }
  g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);

  if (max_idx < 0)
    return NULL;

  gint n =
      gtk_tree_model_iter_n_children(GTK_TREE_MODEL(app->file_filter), NULL);
  gint target_idx = max_idx + 1;
  if (target_idx >= n)
    target_idx = min_idx - 1;
  if (target_idx < 0 || target_idx >= n)
    return NULL;

  GtkTreePath *tpath = gtk_tree_path_new_from_indices(target_idx, -1);
  GtkTreeIter iter;
  char *path = NULL;
  if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &iter, tpath))
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_filter), &iter, COL_PATH, &path,
                       -1);
  gtk_tree_path_free(tpath);
  return path;
}

typedef struct {
  AppWidgets *app;
  guint32 generation;
  GFileEnumerator *enumerator;
  guint count;
  guint last_flush_count;
  char *pending_select;
  GList *pending_select_list;
  gboolean is_trash;
} PopulateCtx;

#define FLUSH_EVERY 1000

static void populate_ctx_free(PopulateCtx *ctx) {
  if (ctx->enumerator)
    g_object_unref(ctx->enumerator);
  g_free(ctx->pending_select);
  g_list_free_full(ctx->pending_select_list, g_free);
  g_free(ctx);
}

void reattach_file_model(AppWidgets *app) {
  if (gtk_icon_view_get_model(GTK_ICON_VIEW(app->icon_view)) == NULL)
    gtk_icon_view_set_model(GTK_ICON_VIEW(app->icon_view),
                            GTK_TREE_MODEL(app->file_filter));
}

static void restore_default_sort(AppWidgets *app) {
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->file_store),
                                       GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);
}

static void populate_next_batch(PopulateCtx *ctx);

gboolean file_info_should_show_readonly_lock(GFileInfo *info) {
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
    guint32 mode =
        g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    return (mode & S_IWUSR) == 0;
  }

  gboolean can_write = TRUE;
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
    can_write = g_file_info_get_attribute_boolean(
        info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
  return !can_write;
}

static void append_file_row(AppWidgets *app, GFileInfo *info,
                            const char *parent_dir, guint *count) {
  gboolean is_hidden = g_file_info_get_is_hidden(info);
  if (!app->show_hidden && is_hidden)
    return;

  const char *name = g_file_info_get_display_name(info);
  gboolean is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
  gboolean is_symlink = g_file_info_get_is_symlink(info);
  const char *ctype = g_file_info_get_content_type(info);
  char *full_path =
      g_build_filename(parent_dir, g_file_info_get_name(info), NULL);

  gboolean is_readonly = file_info_should_show_readonly_lock(info);

  GdkPixbuf *icon = NULL;
  if (!is_dir && g_str_has_suffix(full_path, ".desktop")) {
    GIcon *gi = desktop_file_gicon(full_path);
    if (gi) {
      icon = icon_for_gicon_sized(gi, app->icon_size);
      g_object_unref(gi);
    }
  }
  if (!icon)
    icon = themed_icon_for(ctype, is_dir, full_path, app->icon_size);
  if (is_symlink && icon) {
    GdkPixbuf *emblemed = add_symlink_emblem(icon);
    if (emblemed) {
      g_object_unref(icon);
      icon = emblemed;
    }
  }
  if (is_readonly && icon) {
    GdkPixbuf *emblemed = add_readonly_emblem(icon);
    if (emblemed) {
      g_object_unref(icon);
      icon = emblemed;
    }
  }

  GtkTreeIter iter;
  gtk_list_store_append(app->file_store, &iter);
  GDateTime *dt = g_file_info_get_modification_date_time(info);

  gtk_list_store_set(app->file_store, &iter, COL_DATE, dt, COL_SIZE,
                     (guint64)g_file_info_get_size(info), COL_TYPE,
                     ctype ? ctype : "", -1);
  if (dt)
    g_date_time_unref(dt);

  gtk_list_store_set(app->file_store, &iter, COL_PIXBUF, icon, COL_NAME, name,
                     COL_PATH, full_path, COL_IS_DIR, is_dir, COL_VOLUME, NULL,
                     COL_IS_SYMLINK, is_symlink, COL_THEMED_ICON, TRUE,
                     COL_IS_HIDDEN, is_hidden, COL_IS_READONLY, is_readonly,
                     -1);

  if (!is_dir) {
    GtkTreePath *tpath =
        gtk_tree_model_get_path(GTK_TREE_MODEL(app->file_store), &iter);
    GtkTreeRowReference *ref =
        gtk_tree_row_reference_new(GTK_TREE_MODEL(app->file_store), tpath);
    gtk_tree_path_free(tpath);

    ThumbCtx *tctx = g_new0(ThumbCtx, 1);
    tctx->app = app;
    tctx->ref = ref;
    tctx->generation = app->populate_generation;
    tctx->is_symlink = is_symlink;
    tctx->is_readonly = is_readonly;
    thumbnail_get_async(full_path, app->icon_size, thumbnail_ready_cb, tctx);
  } else if (app->folder_previews && icon) {
    GtkTreePath *tpath =
        gtk_tree_model_get_path(GTK_TREE_MODEL(app->file_store), &iter);
    GtkTreeRowReference *ref =
        gtk_tree_row_reference_new(GTK_TREE_MODEL(app->file_store), tpath);
    gtk_tree_path_free(tpath);

    ThumbCtx *tctx = g_new0(ThumbCtx, 1);
    tctx->app = app;
    tctx->ref = ref;
    tctx->generation = app->populate_generation;
    tctx->is_symlink = is_symlink;
    tctx->is_readonly = is_readonly;

    folder_preview_get_async(full_path, app->icon_size, icon,
                             thumbnail_ready_cb, tctx);
  }

  if (icon)
    g_object_unref(icon);

  g_free(full_path);
  (*count)++;
}

static void append_extra_trash_entries(AppWidgets *app, PopulateCtx *ctx) {
  char *home_files = trash_files_dir();
  GList *locs = all_trash_locations(TRUE);
  for (GList *l = locs; l; l = l->next) {
    TrashLoc *loc = l->data;
    if (g_strcmp0(loc->files_dir, home_files) == 0)
      continue; /* home trash pehle hi async enumeration se list ho chuka */

    GFile *dir = g_file_new_for_path(loc->files_dir);
    GFileEnumerator *en = g_file_enumerate_children(
        dir,
        "standard::name,standard::display-name,standard::type,standard::"
        "content-type,standard::is-hidden,standard::is-symlink,standard::"
        "size,time::modified,access::can-write,unix::mode",
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (en) {
      GFileInfo *info;
      while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        append_file_row(app, info, loc->files_dir, &ctx->count);
        g_object_unref(info);
      }
      g_file_enumerator_close(en, NULL, NULL);
      g_object_unref(en);
    }
    g_object_unref(dir);
  }
  g_list_free_full(locs, (GDestroyNotify)trash_loc_free);
  g_free(home_files);
}

static void populate_next_files_cb(GObject *source, GAsyncResult *res,
                                   gpointer user_data) {
  PopulateCtx *ctx = user_data;
  AppWidgets *app = ctx->app;

  if (ctx->generation != app->populate_generation) {
    g_file_enumerator_close_async(ctx->enumerator, G_PRIORITY_DEFAULT, NULL,
                                  NULL, NULL);
    populate_ctx_free(ctx);
    return;
  }

  GError *err = NULL;
  GList *infos =
      g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(source), res, &err);

  if (err) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      char *msg =
          g_strdup_printf("Directory padhte waqt error: %s", err->message);
      set_status(app, msg);
      g_free(msg);
    }
    g_error_free(err);
    g_file_enumerator_close_async(ctx->enumerator, G_PRIORITY_DEFAULT, NULL,
                                  NULL, NULL);
    restore_default_sort(app);
    reattach_file_model(app);
    populate_ctx_free(ctx);
    return;
  }

  if (!infos) {
    g_file_enumerator_close_async(ctx->enumerator, G_PRIORITY_DEFAULT, NULL,
                                  NULL, NULL);

    if (ctx->is_trash)
      append_extra_trash_entries(app, ctx);

    char *msg = g_strdup_printf("%u items", ctx->count);
    set_status(app, msg);
    g_free(app->default_status);
    app->default_status = msg;

    restore_default_sort(app);
    reattach_file_model(app);
    if (ctx->pending_select_list) {
      if (!select_items_with_paths(app, ctx->pending_select_list))
        focus_first_item(app);
    } else if (!ctx->pending_select ||
               !select_item_with_path(app, ctx->pending_select)) {
      focus_first_item(app);
    }

    populate_ctx_free(ctx);
    return;
  }

  for (GList *l = infos; l; l = l->next) {
    GFileInfo *info = l->data;
    append_file_row(app, info, app->current_dir, &ctx->count);
    g_object_unref(info);
  }
  g_list_free(infos);

  char *progress_msg =
      g_strdup_printf("%u items load ho rahe hain...", ctx->count);
  set_status(app, progress_msg);
  g_free(progress_msg);

  if (ctx->count - ctx->last_flush_count >= FLUSH_EVERY) {
    ctx->last_flush_count = ctx->count;
    restore_default_sort(app);
    reattach_file_model(app);
    gtk_icon_view_set_model(GTK_ICON_VIEW(app->icon_view), NULL);
    gtk_tree_sortable_set_sort_column_id(
        GTK_TREE_SORTABLE(app->file_store),
        GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
  }

  populate_next_batch(ctx);
}

static void populate_next_batch(PopulateCtx *ctx) {
  g_file_enumerator_next_files_async(ctx->enumerator, 200, G_PRIORITY_DEFAULT,
                                     ctx->app->populate_cancellable,
                                     populate_next_files_cb, ctx);
}

static void populate_enumerate_cb(GObject *source, GAsyncResult *res,
                                  gpointer user_data) {
  PopulateCtx *ctx = user_data;
  AppWidgets *app = ctx->app;

  if (ctx->generation != app->populate_generation) {
    populate_ctx_free(ctx);
    return;
  }

  GError *err = NULL;
  GFileEnumerator *en =
      g_file_enumerate_children_finish(G_FILE(source), res, &err);
  if (!en) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      char *msg = g_strdup_printf("Directory khol nahi saka: %s",
                                  err ? err->message : "unknown error");
      set_status(app, msg);
      g_free(msg);
    }
    if (err)
      g_error_free(err);
    restore_default_sort(app);
    reattach_file_model(app);
    populate_ctx_free(ctx);
    return;
  }

  ctx->enumerator = en;
  populate_next_batch(ctx);
}

void populate(AppWidgets *app) {
  update_freespace_label(app);

  char *pending_select = app->pending_select_path;
  app->pending_select_path = NULL;
  GList *pending_select_list = app->pending_select_paths;
  app->pending_select_paths = NULL;

  if (app->populate_cancellable) {
    g_cancellable_cancel(app->populate_cancellable);
    g_object_unref(app->populate_cancellable);
    app->populate_cancellable = NULL;
  }
  app->populate_generation++;

  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0) {
    reattach_file_model(app);
    g_list_free_full(pending_select_list, g_free);
    populate_computer(app, pending_select);
    g_free(pending_select);
    return;
  }

  if (g_strcmp0(app->current_dir, RECENT_URI) == 0) {
    reattach_file_model(app);
    g_list_free_full(pending_select_list, g_free);
    populate_recent(app, pending_select);
    g_free(pending_select);
    return;
  }

  app->populate_cancellable = g_cancellable_new();

  clear_file_store(app);

  gtk_icon_view_set_model(GTK_ICON_VIEW(app->icon_view), NULL);
  gtk_tree_sortable_set_sort_column_id(
      GTK_TREE_SORTABLE(app->file_store),
      GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);

  GFile *dir = g_file_new_for_path(app->current_dir);

  char *tdir_check = trash_files_dir();
  gboolean is_trash_view = g_strcmp0(app->current_dir, tdir_check) == 0;
  g_free(tdir_check);

  PopulateCtx *ctx = g_new0(PopulateCtx, 1);
  ctx->app = app;
  ctx->generation = app->populate_generation;
  ctx->pending_select = pending_select;           /* may be NULL, that's fine */
  ctx->pending_select_list = pending_select_list; /* may be NULL, that's fine */
  ctx->is_trash = is_trash_view;

  g_file_enumerate_children_async(
      dir,
      "standard::name,standard::display-name,standard::type,standard::content-"
      "type,standard::is-hidden,standard::is-symlink,standard::size,time::"
      "modified,access::can-write,unix::mode",
      G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, app->populate_cancellable,
      populate_enumerate_cb, ctx);

  g_object_unref(dir);
}

static void add_computer_row(AppWidgets *app, const char *name, GIcon *icon,
                             const char *mount_path,
                             GVolume *volume_if_unmounted) {
  GtkTreeIter iter;
  GdkPixbuf *pb = icon_for_gicon_sized(icon, app->icon_size);
  if (!pb)
    pb = icon_for_name_sized("drive-removable-media", app->icon_size);

  gtk_list_store_append(app->file_store, &iter);
  gtk_list_store_set(
      app->file_store, &iter, COL_PIXBUF, pb, COL_NAME, name, COL_PATH,
      mount_path ? mount_path : "", COL_IS_DIR, mount_path != NULL, COL_VOLUME,
      volume_if_unmounted ? g_object_ref(volume_if_unmounted) : NULL,
      COL_THEMED_ICON, TRUE, COL_GICON, icon ? g_object_ref(icon) : NULL, -1);
  if (pb)
    g_object_unref(pb);
}

#define RECENT_MAX_ITEMS 60

/* Query a real absolute path and append it via the normal file-row path
 * (icon lookup, thumbnails, folder previews) -- recent:// entries are real
 * files scattered across the filesystem, so we reuse append_file_row by
 * splitting the path into parent_dir + basename, which is exactly what it
 * expects. `rank` records how recently the file was used (0 = most recent)
 * so the view can default to newest-first even though the underlying
 * GtkListStore is otherwise sorted by the usual Sort By criteria. */
static void add_recent_row(AppWidgets *app, const char *path, guint rank,
                           guint *count) {
  GFile *f = g_file_new_for_path(path);
  GFileInfo *info = g_file_query_info(
      f,
      "standard::name,standard::display-name,standard::type,standard::content-"
      "type,standard::is-hidden,standard::is-symlink,standard::size,time::"
      "modified,access::can-write,unix::mode",
      G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info) {
    char *parent_dir = g_path_get_dirname(path);
    guint before = *count;
    append_file_row(app, info, parent_dir, count);
    if (*count != before) {
      /* row abhi-abhi append hui hai (last row) -- uska recency rank set
       * kar dein */
      gint n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(app->file_store),
                                              NULL);
      GtkTreeIter iter;
      if (n > 0 && gtk_tree_model_iter_nth_child(
                       GTK_TREE_MODEL(app->file_store), &iter, NULL, n - 1))
        gtk_list_store_set(app->file_store, &iter, COL_RECENT_RANK, rank, -1);
    }
    g_free(parent_dir);
    g_object_unref(info);
  }
  g_object_unref(f);
}

static gint recent_info_compare_modified(gconstpointer a, gconstpointer b) {
  time_t ta = gtk_recent_info_get_modified((GtkRecentInfo *)a);
  time_t tb = gtk_recent_info_get_modified((GtkRecentInfo *)b);
  if (ta == tb)
    return 0;
  return ta > tb ? -1 : 1; /* most recently used first */
}

static void populate_recent(AppWidgets *app, const char *pending_select) {
  reattach_file_model(app);
  clear_file_store(app);
  guint count = 0;

  GtkRecentManager *manager = gtk_recent_manager_get_default();
  GList *items = gtk_recent_manager_get_items(manager);
  items = g_list_sort(items, recent_info_compare_modified);

  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  guint added = 0;
  for (GList *l = items; l && added < RECENT_MAX_ITEMS; l = l->next) {
    GtkRecentInfo *rinfo = l->data;
    if (!gtk_recent_info_is_local(rinfo))
      continue;

    char *path =
        g_filename_from_uri(gtk_recent_info_get_uri(rinfo), NULL, NULL);
    if (!path)
      continue;

    // if (g_hash_table_contains(seen, path) ||
    //     !g_file_test(path, G_FILE_TEST_EXISTS)) {
    //   g_free(path);
    //   continue;
    // }
    if (g_hash_table_contains(seen, path) ||
      !g_file_test(path, G_FILE_TEST_EXISTS) ||
      g_file_test(path, G_FILE_TEST_IS_DIR)) {
      g_free(path);
    continue;
      }

    g_hash_table_add(seen, g_strdup(path));
    add_recent_row(app, path, added, &count);
    g_free(path);
    added++;
  }

  g_hash_table_destroy(seen);
  g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);

  char *msg = g_strdup_printf("%u recent files", count);
  set_status(app, msg);
  g_free(app->default_status);
  app->default_status = msg;

  if (!pending_select || !select_item_with_path(app, pending_select))
    focus_first_item(app);
}

static void populate_computer(AppWidgets *app, const char *pending_select) {
  reattach_file_model(app);
  clear_file_store(app);
  guint count = 0;

  // GIcon *fs_icon = g_themed_icon_new("drive-harddisk");
  GIcon *fs_icon = g_themed_icon_new("drive-harddisk-system");
  add_computer_row(app, "Filesystem", fs_icon, "/", NULL);
  g_object_unref(fs_icon);
  count += 1;

  GHashTable *seen_uris =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean phone_shown = FALSE;

  GList *mounts = g_volume_monitor_get_mounts(app->volume_monitor);
  for (GList *l = mounts; l; l = l->next) {
    GMount *mount = l->data;
    GVolume *vol = g_mount_get_volume(mount);
    if (vol) {
      g_object_unref(vol);
      continue;
    }

    GFile *root = g_mount_get_root(mount);
    char *uri = g_file_get_uri(root);
    g_hash_table_add(seen_uris, g_strdup(uri));
    g_free(uri);

    char *name = g_mount_get_name(mount);
    GIcon *icon = g_mount_get_icon(mount);
    char *mount_path = g_file_get_path(root);
    gboolean is_phone = gtkfm_looks_like_phone(NULL, mount);

    if (is_phone && phone_shown) {
      g_free(name);
      g_object_unref(icon);
      g_free(mount_path);
      g_object_unref(root);
      continue;
    }
    if (is_phone) {
      phone_shown = TRUE;
      if (gtkfm_is_generic_phone_label(name)) {
        g_free(name);
        name = g_strdup("Mobile");
      }
    }

    add_computer_row(app, name, icon, mount_path, NULL);

    g_free(mount_path);
    g_free(name);
    g_object_unref(icon);
    g_object_unref(root);
    count++;
  }
  g_list_free_full(mounts, g_object_unref);

  GList *volumes = g_volume_monitor_get_volumes(app->volume_monitor);
  for (GList *l = volumes; l; l = l->next) {
    GVolume *volume = l->data;
    GMount *mount = g_volume_get_mount(volume);
    gboolean is_phone = gtkfm_looks_like_phone(volume, mount);

    char *mount_path = NULL;
    if (mount) {
      GFile *root = g_mount_get_root(mount);
      char *uri = g_file_get_uri(root);
      gboolean already_listed = g_hash_table_contains(seen_uris, uri);
      g_free(uri);
      if (already_listed) {
        g_object_unref(root);
        g_object_unref(mount);
        continue;
      }
      mount_path =
          g_file_get_path(root); /* NULL for non-local (e.g. mtp://) mounts */
      g_object_unref(root);
    } else if (is_phone) {
      continue;
    }

    char *name = g_volume_get_name(volume);
    GIcon *icon = g_volume_get_icon(volume);

    if (is_phone && phone_shown) {
      g_free(name);
      g_object_unref(icon);
      g_free(mount_path);
      if (mount)
        g_object_unref(mount);
      continue;
    }
    if (is_phone) {
      phone_shown = TRUE;
      if (gtkfm_is_generic_phone_label(name)) {
        g_free(name);
        name = g_strdup("Mobile");
      }
    }

    add_computer_row(app, name, icon, mount_path, mount ? NULL : volume);
    g_free(mount_path);
    g_free(name);
    g_object_unref(icon);
    if (mount)
      g_object_unref(mount);
    count++;
  }
  g_list_free_full(volumes, g_object_unref);

  g_hash_table_destroy(seen_uris);

  char *msg = g_strdup_printf("%u devices", count);
  set_status(app, msg);
  g_free(app->default_status);
  app->default_status = msg;

  if (!pending_select || !select_item_with_path(app, pending_select))
    focus_first_item(app);
}

static void on_computer_mount_finish(GObject *source, GAsyncResult *res,
                                     gpointer user_data) {
  AppWidgets *app = user_data;
  GVolume *volume = G_VOLUME(source);
  GError *err = NULL;
  if (!g_volume_mount_finish(volume, res, &err)) {
    char *msg =
        g_strdup_printf("Mount fail: %s", err ? err->message : "unknown");
    set_status(app, msg);
    g_free(msg);
    if (err)
      g_error_free(err);
    return;
  }

  GMount *mount = g_volume_get_mount(volume);
  if (mount) {
    GFile *root = g_mount_get_root(mount);
    char *path = g_file_get_path(root);
    if (path) {
      fileview_navigate(app, path, TRUE);
    } else if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0) {
      populate_computer(app, NULL);
    }
    g_free(path);
    g_object_unref(root);
    g_object_unref(mount);
  } else if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0) {
    populate_computer(app, NULL);
  }
}

void mount_computer_volume(AppWidgets *app, GVolume *volume) {
  GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(app->window));
  g_volume_mount(volume, G_MOUNT_MOUNT_NONE, op, NULL, on_computer_mount_finish,
                 app);
  g_object_unref(op);
}

/* ---- live folder monitor (external changes -> auto refresh) ----------- */

static gboolean dir_monitor_debounced_refresh(gpointer user_data) {
  AppWidgets *app = user_data;
  app->dir_monitor_debounce_id = 0;
  if (app->auto_refresh_enabled)
    fileview_refresh(app);
  return G_SOURCE_REMOVE;
}

static void on_dir_monitor_event(GFileMonitor *monitor, GFile *file,
                                 GFile *other_file,
                                 GFileMonitorEvent event_type,
                                 gpointer user_data) {
  (void)monitor;
  (void)file;
  (void)other_file;
  AppWidgets *app = user_data;
  switch (event_type) {
  case G_FILE_MONITOR_EVENT_CREATED:
  case G_FILE_MONITOR_EVENT_DELETED:
  case G_FILE_MONITOR_EVENT_RENAMED:
  case G_FILE_MONITOR_EVENT_MOVED_IN:
  case G_FILE_MONITOR_EVENT_MOVED_OUT:
  case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
  case G_FILE_MONITOR_EVENT_UNMOUNTED:
  case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
  case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    break;
  case G_FILE_MONITOR_EVENT_CHANGED:
    break;
  default:
    return;
  }

  if (app->dir_monitor_suppress_until &&
      g_get_monotonic_time() < app->dir_monitor_suppress_until)
    return;

  if (app->dir_monitor_debounce_id)
    g_source_remove(app->dir_monitor_debounce_id);
  app->dir_monitor_debounce_id =
      g_timeout_add(300, dir_monitor_debounced_refresh, app);
}

static void stop_dir_monitor(AppWidgets *app) {
  if (app->dir_monitor_debounce_id) {
    g_source_remove(app->dir_monitor_debounce_id);
    app->dir_monitor_debounce_id = 0;
  }
  if (app->dir_monitor) {
    g_file_monitor_cancel(app->dir_monitor);
    g_object_unref(app->dir_monitor);
    app->dir_monitor = NULL;
  }
}

static void start_dir_monitor(AppWidgets *app, const char *dir_path) {
  stop_dir_monitor(app);

  if (!dir_path || g_strcmp0(dir_path, COMPUTER_URI) == 0)
    return;

  GFile *dir = g_file_new_for_path(dir_path);
  GError *err = NULL;
  GFileMonitor *monitor =
      g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL, &err);

  if (!monitor) {
    g_warning("dir_monitor: WATCH_MOVES pe monitor banane mein nakami (%s): %s",
              dir_path, err ? err->message : "unknown error");
    g_clear_error(&err);

    monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL, &err);
  }

  g_object_unref(dir);

  if (!monitor) {
    g_warning("dir_monitor: folder monitor bilkul nahi ban saka (%s): %s",
              dir_path, err ? err->message : "unknown error");
    g_clear_error(&err);
    return;
  }

  g_signal_connect(monitor, "changed", G_CALLBACK(on_dir_monitor_event), app);
  app->dir_monitor = monitor;
}

void fileview_cancel_pending(AppWidgets *app) {
  if (!app)
    return;

  app->populate_generation++;
  if (app->populate_cancellable) {
    g_cancellable_cancel(app->populate_cancellable);
    g_object_unref(app->populate_cancellable);
    app->populate_cancellable = NULL;
  }

  app->search_generation++;
  if (app->search_cancellable) {
    g_cancellable_cancel(app->search_cancellable);
    g_object_unref(app->search_cancellable);
    app->search_cancellable = NULL;
  }

  stop_dir_monitor(app);

  if (app->typeahead_timeout_id) {
    g_source_remove(app->typeahead_timeout_id);
    app->typeahead_timeout_id = 0;
  }

  if (app->layout_save_debounce_id) {
    g_source_remove(app->layout_save_debounce_id);
    app->layout_save_debounce_id = 0;
  }
}

void fileview_navigate(AppWidgets *app, const char *dir_path,
                       gboolean push_history) {
  gboolean is_computer = g_strcmp0(dir_path, COMPUTER_URI) == 0;

  char *resolved_trash = NULL;
  if (!is_computer && g_strcmp0(dir_path, "trash://") == 0) {
    resolved_trash = trash_files_dir();
    dir_path = resolved_trash;
  }

  if (push_history && app->current_dir) {
    app->history_back =
        g_list_prepend(app->history_back, g_strdup(app->current_dir));
    g_list_free_full(app->history_forward, g_free);
    app->history_forward = NULL;
  }

  g_free(app->current_dir);
  app->current_dir = g_strdup(dir_path);
  g_free(resolved_trash);

  start_dir_monitor(app, app->current_dir);

  char *saved_pending_select = app->pending_select_path;
  app->pending_select_path = NULL;

  if (app->search_entry)
    gtk_entry_set_text(GTK_ENTRY(app->search_entry), "");
  fileview_set_search(app, NULL);

  if (saved_pending_select) {
    g_free(app->pending_select_path);
    app->pending_select_path = saved_pending_select;
  }

  fileview_typeahead_cancel(app);

  populate(app);
  update_path_entry(app);
  sidebar_sync_selection(app);

  if (app->btn_back)
    gtk_widget_set_sensitive(app->btn_back, app->history_back != NULL);
  if (app->btn_forward)
    gtk_widget_set_sensitive(app->btn_forward, app->history_forward != NULL);

  infopanel_refresh(app);
  terminal_panel_sync_cwd(app);
}

void fileview_refresh(AppWidgets *app) {
  if (!app->current_dir)
    return;

  if (!app->pending_select_path && !app->pending_select_paths &&
      app->icon_view && app->file_filter) {
    GList *selected =
        gtk_icon_view_get_selected_items(GTK_ICON_VIEW(app->icon_view));
    if (selected) {
      GtkTreeIter iter;
      if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &iter,
                                  selected->data))
        gtk_tree_model_get(GTK_TREE_MODEL(app->file_filter), &iter, COL_PATH,
                           &app->pending_select_path, -1);
      g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    }
  }

  populate(app);
  infopanel_refresh(app);
}

void fileview_go_up(AppWidgets *app) {
  if (!app->current_dir)
    return;
  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0 ||
      g_strcmp0(app->current_dir, RECENT_URI) == 0)
    return; /* top-level virtual location */
  char *parent = g_path_get_dirname(app->current_dir);
  if (g_strcmp0(parent, app->current_dir) != 0) {
    g_free(app->pending_select_path);
    app->pending_select_path = g_strdup(app->current_dir);
    fileview_navigate(app, parent, TRUE);
  }
  g_free(parent);
}

void fileview_go_back(AppWidgets *app) {
  if (!app->history_back)
    return;
  char *target = app->history_back->data;
  app->history_back = g_list_remove(app->history_back, target);

  app->history_forward =
      g_list_prepend(app->history_forward, g_strdup(app->current_dir));

  g_free(app->pending_select_path);
  app->pending_select_path = g_strdup(app->current_dir);

  g_free(app->current_dir);
  app->current_dir = target; /* takes ownership */

  char *saved_pending_select = app->pending_select_path;
  app->pending_select_path = NULL;

  if (app->search_entry)
    gtk_entry_set_text(GTK_ENTRY(app->search_entry), "");
  fileview_set_search(app, NULL);

  if (saved_pending_select) {
    g_free(app->pending_select_path);
    app->pending_select_path = saved_pending_select;
  }

  fileview_typeahead_cancel(app);

  populate(app);
  update_path_entry(app);
  sidebar_sync_selection(app);
  if (app->btn_back)
    gtk_widget_set_sensitive(app->btn_back, app->history_back != NULL);
  if (app->btn_forward)
    gtk_widget_set_sensitive(app->btn_forward, app->history_forward != NULL);
  infopanel_refresh(app);
  terminal_panel_sync_cwd(app);
}

void fileview_go_forward(AppWidgets *app) {
  if (!app->history_forward)
    return;
  char *target = app->history_forward->data;
  app->history_forward = g_list_remove(app->history_forward, target);

  app->history_back =
      g_list_prepend(app->history_back, g_strdup(app->current_dir));

  g_free(app->current_dir);
  app->current_dir = target;

  if (app->search_entry)
    gtk_entry_set_text(GTK_ENTRY(app->search_entry), "");
  fileview_set_search(app, NULL);
  fileview_typeahead_cancel(app);

  populate(app);
  update_path_entry(app);
  sidebar_sync_selection(app);
  if (app->btn_back)
    gtk_widget_set_sensitive(app->btn_back, app->history_back != NULL);
  if (app->btn_forward)
    gtk_widget_set_sensitive(app->btn_forward, app->history_forward != NULL);
  infopanel_refresh(app);
  terminal_panel_sync_cwd(app);
}
