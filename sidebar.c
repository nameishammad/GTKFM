#include "sidebar.h"
#include "copyjob.h"
#include "fileview.h"
#include <gio/gio.h>
#include <string.h>

/* ---- icon helper --------------------------------------------------------*/

static GdkPixbuf *icon_for_gicon(GIcon *icon, int size) {
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

static GdkPixbuf *icon_for_name(const char *name, int size) {
  GIcon *icon = g_themed_icon_new(name);
  GdkPixbuf *pb = icon_for_gicon(icon, size);
  g_object_unref(icon);
  return pb;
}

/* ---- building the tree --------------------------------------------------*/

static void add_place_row(GtkTreeStore *store, GtkTreeIter *parent,
                          const char *name, const char *path,
                          const char *icon_name) {
  GtkTreeIter iter;
  GdkPixbuf *pb = icon_for_name(icon_name, 20);
  gtk_tree_store_append(store, &iter, parent);
  gtk_tree_store_set(store, &iter, SB_COL_ICON, pb, SB_COL_NAME, name,
                     SB_COL_PATH, path, SB_COL_IS_DEVICE, FALSE, SB_COL_MOUNTED,
                     TRUE, SB_COL_VOLUME, NULL, SB_COL_MOUNT, NULL, -1);
  if (pb)
    g_object_unref(pb);
}

static void __attribute__((unused))
add_device_row(GtkTreeStore *store, GtkTreeIter *parent, const char *name,
               GIcon *gicon, const char *mount_path, gboolean mounted,
               GVolume *volume, GMount *mount) {
  GtkTreeIter iter;
  GdkPixbuf *pb = icon_for_gicon(gicon, 20);
  if (!pb)
    pb = icon_for_name("drive-removable-media", 20);

  gtk_tree_store_append(store, &iter, parent);
  gtk_tree_store_set(store, &iter, SB_COL_ICON, pb, SB_COL_NAME, name,
                     SB_COL_PATH, mount_path ? mount_path : "",
                     SB_COL_IS_DEVICE, TRUE, SB_COL_MOUNTED, mounted,
                     SB_COL_VOLUME, volume, SB_COL_MOUNT, mount, -1);
  if (pb)
    g_object_unref(pb);
}

gboolean gtkfm_looks_like_phone(GVolume *volume, GMount *mount) {
  char *identifier =
      volume ? g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_CLASS)
             : NULL;
  gboolean is_phone = FALSE;
  if (identifier &&
      (strstr(identifier, "gphoto2") || strstr(identifier, "mtp"))) {
    is_phone = TRUE;
  }
  g_free(identifier);

  if (!is_phone && mount) {
    GFile *root = g_mount_get_root(mount);
    char *uri = g_file_get_uri(root);
    if (uri && (g_str_has_prefix(uri, "mtp://") ||
                g_str_has_prefix(uri, "gphoto2://"))) {
      is_phone = TRUE;
    }
    g_free(uri);
    g_object_unref(root);
  }
  return is_phone;
}

gboolean gtkfm_is_generic_phone_label(const char *name) {
  if (!name)
    return TRUE;
  static const char *generic[] = {"mtp",    "mtp device", "gphoto2",
                                  "ptp",    "ptp device", "media player",
                                  "camera", NULL};
  for (int i = 0; generic[i]; i++) {
    if (g_ascii_strcasecmp(name, generic[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

static void rebuild_devices(AppWidgets *app, GtkTreeIter *devices_parent) {

  GtkTreeIter child;

  while (gtk_tree_model_iter_children(GTK_TREE_MODEL(app->sidebar_store),
                                      &child, devices_parent)) {
    gtk_tree_store_remove(app->sidebar_store, &child);
  }

  gtk_tree_store_set(app->sidebar_store, devices_parent, SB_COL_NAME, "Devices",
                     SB_COL_PATH, "", SB_COL_IS_DEVICE, FALSE, SB_COL_MOUNTED,
                     TRUE, -1);

  add_place_row(app->sidebar_store, devices_parent, "File System", "/",
                "drive-harddisk");

  GHashTable *seen_uris =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  gboolean phone_shown = FALSE;

  GList *mounts = g_volume_monitor_get_mounts(app->volume_monitor);
  for (GList *l = mounts; l; l = l->next) {
    GMount *mount = l->data;

    GFile *root = g_mount_get_root(mount);
    char *uri = g_file_get_uri(root);
    if (uri && g_hash_table_contains(seen_uris, uri)) {
      g_free(uri);
      g_object_unref(root);
      continue;
    }
    g_hash_table_add(seen_uris, g_strdup(uri ? uri : ""));
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

    // char *display_name =
    //     is_phone ? g_strdup_printf("📱 %s", name) : g_strdup(name);
    char *display_name = g_strdup(name);

    GtkTreeIter iter;
    gtk_tree_store_append(app->sidebar_store, &iter, devices_parent);
    // GdkPixbuf *pb = icon_for_gicon(icon, 20);
    GdkPixbuf *pb;

    if (is_phone)
      pb = icon_for_name("phone", 20);
    else
      pb = icon_for_gicon(icon, 20);

    if (!pb)
      pb = icon_for_name("drive-removable-media", 20);
    gtk_tree_store_set(app->sidebar_store, &iter, SB_COL_ICON, pb, SB_COL_NAME,
                       display_name, SB_COL_PATH, mount_path ? mount_path : "",
                       SB_COL_IS_DEVICE, TRUE, SB_COL_MOUNTED, TRUE,
                       SB_COL_VOLUME, NULL, SB_COL_MOUNT, mount, -1);
    if (pb)
      g_object_unref(pb);
    g_free(mount_path);
    g_free(display_name);
    g_free(name);
    g_object_unref(icon);
    g_object_unref(root);
  }
  g_list_free_full(mounts, g_object_unref);

  GHashTable *seen_volume_ids =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  GList *volumes = g_volume_monitor_get_volumes(app->volume_monitor);
  for (GList *l = volumes; l; l = l->next) {
    GVolume *volume = l->data;
    GMount *mount = g_volume_get_mount(volume);
    gboolean is_phone = gtkfm_looks_like_phone(volume, mount);

    char *mount_path = NULL;
    if (mount) {
      GFile *root = g_mount_get_root(mount);
      char *uri = g_file_get_uri(root);
      gboolean already_listed = uri && g_hash_table_contains(seen_uris, uri);
      if (already_listed) {
        g_free(uri);
        g_object_unref(root);
        g_object_unref(mount);
        continue;
      }
      g_hash_table_add(seen_uris, g_strdup(uri ? uri : ""));
      g_free(uri);
      mount_path = g_file_get_path(root);
      g_object_unref(root);
    } else if (is_phone) {
      continue;
    } else {
      char *vol_id =
          g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
      if (!vol_id)
        vol_id = g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UUID);
      char *dedupe_key = vol_id ? vol_id : g_volume_get_name(volume);
      gboolean dup =
          dedupe_key && g_hash_table_contains(seen_volume_ids, dedupe_key);
      if (dup) {
        g_free(dedupe_key);
        continue;
      }
      if (dedupe_key)
        g_hash_table_add(seen_volume_ids, dedupe_key);
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

    char *display_name =
        is_phone ? g_strdup_printf("📱 %s", name) : g_strdup(name);

    GtkTreeIter iter;
    gtk_tree_store_append(app->sidebar_store, &iter, devices_parent);
    GdkPixbuf *pb = icon_for_gicon(icon, 20);
    gtk_tree_store_set(app->sidebar_store, &iter, SB_COL_ICON, pb, SB_COL_NAME,
                       display_name, SB_COL_PATH, mount_path ? mount_path : "",
                       SB_COL_IS_DEVICE, TRUE, SB_COL_MOUNTED, mount != NULL,
                       SB_COL_VOLUME, volume, SB_COL_MOUNT, mount, -1);
    if (pb)
      g_object_unref(pb);
    g_free(mount_path);
    g_free(display_name);
    g_free(name);
    g_object_unref(icon);
    if (mount)
      g_object_unref(mount);
  }
  g_list_free_full(volumes, g_object_unref);

  g_hash_table_destroy(seen_uris);
  g_hash_table_destroy(seen_volume_ids);
}

static GtkTreeIter g_devices_parent_iter;
static gboolean g_devices_parent_valid = FALSE;

static GtkTreeIter g_bookmarks_parent_iter;
static gboolean g_bookmarks_parent_valid = FALSE;

static GtkTreeIter g_trash_iter;
static gboolean g_trash_iter_valid = FALSE;

/* ---- bookmarks (Ctrl+D) ---------------------------------------------- */

static void add_bookmark_row(GtkTreeStore *store, GtkTreeIter *parent,
                             const char *name, const char *path) {
  GtkTreeIter iter;

  GFile *file = g_file_new_for_path(path);

  GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_ICON,
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);

  GdkPixbuf *pb = NULL;

  if (info) {
    GIcon *icon = g_file_info_get_icon(info);
    pb = icon_for_gicon(icon, 20);
    g_object_unref(info);
  }

  g_object_unref(file);

  if (!pb)
    pb = icon_for_name("folder", 20);

  gtk_tree_store_append(store, &iter, parent);

  gtk_tree_store_set(store, &iter, SB_COL_ICON, pb, SB_COL_NAME, name,
                     SB_COL_PATH, path, SB_COL_IS_DEVICE, FALSE, SB_COL_MOUNTED,
                     TRUE, SB_COL_VOLUME, NULL, SB_COL_MOUNT, NULL,
                     SB_COL_IS_BOOKMARK, TRUE, -1);

  if (pb)
    g_object_unref(pb);
}

static char *bookmarks_file_path(void) {
  char *dir = g_build_filename(g_get_user_config_dir(), "gtkfm", NULL);
  g_mkdir_with_parents(dir, 0700);
  char *file = g_build_filename(dir, "bookmarks", NULL);
  g_free(dir);
  return file;
}

static void sidebar_load_bookmarks(AppWidgets *app) {
  char *file = bookmarks_file_path();
  char *contents = NULL;
  if (g_file_get_contents(file, &contents, NULL, NULL)) {
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
      char *path = g_strstrip(lines[i]);
      if (*path && g_file_test(path, G_FILE_TEST_IS_DIR)) {
        char *name = g_path_get_basename(path);
        add_bookmark_row(app->sidebar_store, &g_bookmarks_parent_iter, name,
                         path);
        g_free(name);
      }
    }
    g_strfreev(lines);
    g_free(contents);
  }
  g_free(file);
}

static void sidebar_save_bookmarks(AppWidgets *app) {
  char *file = bookmarks_file_path();
  GString *out = g_string_new(NULL);
  GtkTreeIter child;
  if (gtk_tree_model_iter_children(GTK_TREE_MODEL(app->sidebar_store), &child,
                                   &g_bookmarks_parent_iter)) {
    do {
      char *path;
      gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &child,
                         SB_COL_PATH, &path, -1);
      g_string_append_printf(out, "%s\n", path);
      g_free(path);
    } while (
        gtk_tree_model_iter_next(GTK_TREE_MODEL(app->sidebar_store), &child));
  }
  g_file_set_contents(file, out->str, out->len, NULL);
  g_string_free(out, TRUE);
  g_free(file);
}

static void remove_bookmark_row(AppWidgets *app, const char *path) {
  GtkTreeIter child;
  if (gtk_tree_model_iter_children(GTK_TREE_MODEL(app->sidebar_store), &child,
                                   &g_bookmarks_parent_iter)) {
    do {
      char *existing;
      gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &child,
                         SB_COL_PATH, &existing, -1);
      gboolean same = g_strcmp0(existing, path) == 0;
      g_free(existing);
      if (same) {
        gtk_tree_store_remove(app->sidebar_store, &child);
        sidebar_save_bookmarks(app);
        return;
      }
    } while (
        gtk_tree_model_iter_next(GTK_TREE_MODEL(app->sidebar_store), &child));
  }
}

void sidebar_add_bookmark(AppWidgets *app, const char *path) {
  if (!path || !*path || !g_bookmarks_parent_valid)
    return;
  if (!g_file_test(path, G_FILE_TEST_IS_DIR))
    return;
  if (g_strcmp0(path, COMPUTER_URI) == 0)
    return; /* virtual location, not a real folder */

  GtkTreeIter child;
  if (gtk_tree_model_iter_children(GTK_TREE_MODEL(app->sidebar_store), &child,
                                   &g_bookmarks_parent_iter)) {
    do {
      char *existing;
      gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &child,
                         SB_COL_PATH, &existing, -1);
      gboolean same = g_strcmp0(existing, path) == 0;
      g_free(existing);
      if (same) {
        gtk_statusbar_push(GTK_STATUSBAR(app->statusbar), app->statusbar_ctx,
                           "Ye folder pehle se bookmarked hai");
        return;
      }
    } while (
        gtk_tree_model_iter_next(GTK_TREE_MODEL(app->sidebar_store), &child));
  }

  char *name = g_path_get_basename(path);
  add_bookmark_row(app->sidebar_store, &g_bookmarks_parent_iter, name, path);
  g_free(name);
  gtk_tree_view_expand_all(GTK_TREE_VIEW(app->sidebar_tree));
  sidebar_save_bookmarks(app);
  gtk_statusbar_push(GTK_STATUSBAR(app->statusbar), app->statusbar_ctx,
                     "Bookmark add ho gaya (Ctrl+D)");
}

// void sidebar_update_trash_icon(AppWidgets *app) {
//   if (!g_trash_iter_valid)
//     return;
//   gboolean trash_empty = fileview_trash_is_empty();
//   GdkPixbuf *pb =
//       icon_for_name(trash_empty ? "user-trash" : "user-trash-full", 20);
//   gtk_tree_store_set(app->sidebar_store, &g_trash_iter, SB_COL_ICON, pb, -1);
//   if (pb)
//     g_object_unref(pb);
// }
void sidebar_update_trash_icon(AppWidgets *app)
{
  if (!app)
    return;

  if (!g_trash_iter_valid)
    return;

  if (!app->sidebar_store)
    return;

  gboolean trash_empty = fileview_trash_is_empty();

  GdkPixbuf *pb =
  icon_for_name(trash_empty ? "user-trash" : "user-trash-full", 20);

  gtk_tree_store_set(app->sidebar_store,
                     &g_trash_iter,
                     SB_COL_ICON, pb,
                     -1);

  if (pb)
    g_object_unref(pb);
}

static gboolean sidebar_path_matches(const char *a, const char *b) {
  if (!a || !b || !*a || !*b)
    return FALSE;
  size_t la = strlen(a), lb = strlen(b);
  if (la > 1 && a[la - 1] == '/')
    la--;
  if (lb > 1 && b[lb - 1] == '/')
    lb--;
  return la == lb && strncmp(a, b, la) == 0;
}

static gboolean sidebar_find_path_recursive(GtkTreeModel *model,
                                            GtkTreeIter *parent,
                                            const char *target,
                                            GtkTreeIter *out_iter) {
  GtkTreeIter child;
  if (!gtk_tree_model_iter_children(model, &child, parent))
    return FALSE;
  do {
    char *path = NULL;
    gtk_tree_model_get(model, &child, SB_COL_PATH, &path, -1);
    gboolean hit = sidebar_path_matches(path, target);
    g_free(path);
    if (hit) {
      *out_iter = child;
      return TRUE;
    }
    if (sidebar_find_path_recursive(model, &child, target, out_iter))
      return TRUE;
  } while (gtk_tree_model_iter_next(model, &child));
  return FALSE;
}

void sidebar_sync_selection(AppWidgets *app) {
  if (!app->sidebar_tree || !app->sidebar_store)
    return;

  GtkTreeSelection *selection =
      gtk_tree_view_get_selection(GTK_TREE_VIEW(app->sidebar_tree));
  GtkTreeModel *model = GTK_TREE_MODEL(app->sidebar_store);

  GtkTreeIter hit;
  if (app->current_dir &&
      sidebar_find_path_recursive(model, NULL, app->current_dir, &hit)) {
    GtkTreePath *path = gtk_tree_model_get_path(model, &hit);
    gtk_tree_selection_select_path(selection, path);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(app->sidebar_tree), path, NULL,
                             FALSE);
    gtk_tree_path_free(path);
  } else {
    gtk_tree_selection_unselect_all(selection);
  }
}

static void on_monitor_changed(GVolumeMonitor *vm, gpointer obj,
                               gpointer user_data) {
  (void)vm;
  (void)obj;

  AppWidgets *app = user_data;

  GtkTreeSelection *selection =
      gtk_tree_view_get_selection(GTK_TREE_VIEW(app->sidebar_tree));

  gtk_tree_selection_unselect_all(selection);

  if (g_devices_parent_valid) {
    rebuild_devices(app, &g_devices_parent_iter);
  }

  /* keep Devices expanded */
  GtkTreePath *path = gtk_tree_model_get_path(
      GTK_TREE_MODEL(app->sidebar_store), &g_devices_parent_iter);

  gtk_tree_view_expand_row(GTK_TREE_VIEW(app->sidebar_tree), path, FALSE);

  gtk_tree_path_free(path);

  if (app->current_dir) {
    if (g_strcmp0(app->current_dir, COMPUTER_URI) != 0 &&
        g_strcmp0(app->current_dir, RECENT_URI) != 0 &&
        !g_file_test(app->current_dir, G_FILE_TEST_IS_DIR)) {
      /* Jo folder/device abhi khula hua tha wahi nikal diya gaya --
       * home chale jao taake khali ya broken view na dikhe. */
      fileview_navigate(app, g_get_home_dir(), TRUE);
    } else {
      /*             fileview_refresh(app); */
      printf("Removed device\n");
    }
  }
}

/* ---- mount / unmount ------------------------------------------------------*/

typedef struct {
  AppWidgets *app;
} MountCtx;

static void on_mount_finish(GObject *source, GAsyncResult *res,
                            gpointer user_data) {
  MountCtx *ctx = user_data;
  GVolume *volume = G_VOLUME(source);
  GError *err = NULL;
  if (!g_volume_mount_finish(volume, res, &err)) {
    g_warning("Mount fail: %s", err ? err->message : "unknown");
    if (err)
      g_error_free(err);
    g_free(ctx);
    return;
  }
  GMount *mount = g_volume_get_mount(volume);
  if (mount) {
    GFile *root = g_mount_get_root(mount);
    char *path = g_file_get_path(root);
    if (path)
      fileview_navigate(ctx->app, path, TRUE);
    g_free(path);
    g_object_unref(root);
    g_object_unref(mount);
  }
  g_free(ctx);
}

static void mount_volume(AppWidgets *app, GVolume *volume) {
  MountCtx *ctx = g_new0(MountCtx, 1);
  ctx->app = app;
  GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(app->window));
  g_volume_mount(volume, G_MOUNT_MOUNT_NONE, op, NULL, on_mount_finish, ctx);
  g_object_unref(op);
}

static void on_unmount_finish(GObject *source, GAsyncResult *res,
                              gpointer user_data) {
  (void)user_data;
  GMount *mount = G_MOUNT(source);
  GError *err = NULL;
  if (!g_mount_unmount_with_operation_finish(mount, res, &err)) {
    g_warning("Unmount fail: %s", err ? err->message : "unknown");
    if (err)
      g_error_free(err);
  }
}

static void unmount_mount(AppWidgets *app, GMount *mount) {
  GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(app->window));
  g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, op, NULL,
                                 on_unmount_finish, app);
  g_object_unref(op);
}

/* ---- signal handlers ------------------------------------------------------*/

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer user_data) {
  (void)col;
  AppWidgets *app = user_data;
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sidebar_store), &iter, path))
    return;

  char *item_path;
  gboolean is_device, mounted;
  gpointer volume_ptr, mount_ptr;
  gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &iter, SB_COL_PATH,
                     &item_path, SB_COL_IS_DEVICE, &is_device, SB_COL_MOUNTED,
                     &mounted, SB_COL_VOLUME, &volume_ptr, SB_COL_MOUNT,
                     &mount_ptr, -1);

  if (!is_device) {
    if (item_path && *item_path)
      fileview_navigate(app, item_path, TRUE);
  } else if (mounted) {
    if (item_path && *item_path) {
      fileview_navigate(app, item_path, TRUE);
    } else {
      gtk_statusbar_push(GTK_STATUSBAR(app->statusbar), app->statusbar_ctx,
                         "Ye mount local path se browse nahi ho sakta (gvfs "
                         "URI mount) — Files app / gio open try karein");
    }
  } else if (volume_ptr) {
    mount_volume(app, G_VOLUME(volume_ptr));
  }
  g_free(item_path);
  (void)tv;
}

enum { SB_DND_TARGET_URI_LIST = 1, SB_DND_TARGET_BOOKMARK_ROW = 2 };

static const GtkTargetEntry sb_dnd_dest_targets[] = {
    {(char *)"text/uri-list", 0, SB_DND_TARGET_URI_LIST},
    {(char *)"GTKFM_BOOKMARK_ROW", GTK_TARGET_SAME_WIDGET,
     SB_DND_TARGET_BOOKMARK_ROW},
};

static const GtkTargetEntry sb_dnd_source_targets[] = {
    {(char *)"GTKFM_BOOKMARK_ROW", GTK_TARGET_SAME_WIDGET,
     SB_DND_TARGET_BOOKMARK_ROW},
};

static GtkTreePath *g_bookmark_drag_source_path = NULL;

static gboolean sidebar_row_is_drop_target(AppWidgets *app, GtkTreeIter *iter,
                                           char **out_path) {
  char *path = NULL;
  gboolean mounted = FALSE;
  gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), iter, SB_COL_PATH,
                     &path, SB_COL_MOUNTED, &mounted, -1);

  gboolean ok = path && *path && mounted && g_strcmp0(path, "trash://") != 0 &&
                g_strcmp0(path, COMPUTER_URI) != 0 &&
                g_file_test(path, G_FILE_TEST_IS_DIR);

  if (ok && out_path)
    *out_path = g_strdup(path);
  g_free(path);
  return ok;
}

static gboolean sidebar_drag_wants_reorder(GdkDragContext *context) {
  GdkAtom row_atom = gdk_atom_intern("GTKFM_BOOKMARK_ROW", FALSE);
  for (GList *l = gdk_drag_context_list_targets(context); l; l = l->next) {
    if (l->data == row_atom)
      return TRUE;
  }
  return FALSE;
}

static void on_sidebar_drag_begin(GtkWidget *widget, GdkDragContext *context,
                                  gpointer user_data) {
  (void)widget;
  (void)user_data;
  if (!g_bookmark_drag_source_path) {
    gtk_drag_cancel(context);
  }
}

static void on_sidebar_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                                     GtkSelectionData *data, guint info,
                                     guint time, gpointer user_data) {
  (void)widget;
  (void)context;
  (void)time;
  (void)user_data;
  if (info != SB_DND_TARGET_BOOKMARK_ROW || !g_bookmark_drag_source_path)
    return;

  char *path_str = gtk_tree_path_to_string(g_bookmark_drag_source_path);
  if (!path_str)
    return;

  GdkAtom target = gdk_atom_intern("GTKFM_BOOKMARK_ROW", FALSE);
  gtk_selection_data_set(data, target, 8, (const guchar *)path_str,
                         (gint)strlen(path_str) + 1);
  g_free(path_str);
}

static void on_sidebar_drag_end(GtkWidget *widget, GdkDragContext *context,
                                gpointer user_data) {
  (void)context;
  (void)user_data;
  g_clear_pointer(&g_bookmark_drag_source_path, gtk_tree_path_free);
  gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), NULL, 0);
}

static gboolean on_sidebar_drag_motion(GtkWidget *widget,
                                       GdkDragContext *context, gint x, gint y,
                                       guint time, gpointer user_data) {
  AppWidgets *app = user_data;

  if (sidebar_drag_wants_reorder(context)) {
    GtkTreePath *tpath = NULL;
    GtkTreeViewDropPosition pos = GTK_TREE_VIEW_DROP_BEFORE;
    gboolean valid = FALSE;

    if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget), x, y, &tpath,
                                          &pos) &&
        tpath) {
      GtkTreeIter iter;
      if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sidebar_store), &iter,
                                  tpath)) {
        gboolean is_bookmark = FALSE;
        gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &iter,
                           SB_COL_IS_BOOKMARK, &is_bookmark, -1);
        if (is_bookmark) {
          valid = TRUE;
          if (pos == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE)
            pos = GTK_TREE_VIEW_DROP_BEFORE;
          else if (pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER)
            pos = GTK_TREE_VIEW_DROP_AFTER;
        }
      }
    }

    if (valid && tpath) {
      gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), tpath, pos);
      gdk_drag_status(context, GDK_ACTION_COPY, time);
    } else {
      gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), NULL, 0);
      gdk_drag_status(context, 0, time);
    }
    if (tpath)
      gtk_tree_path_free(tpath);
    return TRUE;
  }

  GtkTreePath *tpath = NULL;
  GtkTreeViewDropPosition pos;
  gboolean valid = FALSE;

  if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget), x, y, &tpath,
                                        &pos)) {
    GtkTreeIter iter;
    if (tpath && gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sidebar_store),
                                         &iter, tpath))
      valid = sidebar_row_is_drop_target(app, &iter, NULL);
  }

  if (valid && tpath) {
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), tpath,
                                    GTK_TREE_VIEW_DROP_INTO_OR_AFTER);
    gdk_drag_status(context, GDK_ACTION_COPY, time);
  } else {
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), NULL, 0);
    gdk_drag_status(context, 0, time);
  }
  if (tpath)
    gtk_tree_path_free(tpath);
  return TRUE;
}

static void on_sidebar_drag_data_received(GtkWidget *widget,
                                          GdkDragContext *context, gint x,
                                          gint y, GtkSelectionData *data,
                                          guint info, guint time,
                                          gpointer user_data) {
  AppWidgets *app = user_data;
  gboolean success = FALSE;

  if (info == SB_DND_TARGET_BOOKMARK_ROW) {
    /* Bookmark ko naye position par move karna hai. */
    if (gtk_selection_data_get_length(data) > 0) {
      const guchar *raw = gtk_selection_data_get_data(data);
      GtkTreePath *src_path = gtk_tree_path_new_from_string((const char *)raw);

      GtkTreePath *dest_tpath = NULL;
      GtkTreeViewDropPosition dest_pos;
      if (src_path &&
          gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget), x, y,
                                            &dest_tpath, &dest_pos) &&
          dest_tpath) {

        GtkTreeIter src_iter, dest_iter;
        gboolean src_ok = gtk_tree_model_get_iter(
            GTK_TREE_MODEL(app->sidebar_store), &src_iter, src_path);
        gboolean dest_ok = gtk_tree_model_get_iter(
            GTK_TREE_MODEL(app->sidebar_store), &dest_iter, dest_tpath);

        if (src_ok && dest_ok) {
          gboolean src_is_bookmark = FALSE, dest_is_bookmark = FALSE;
          gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &src_iter,
                             SB_COL_IS_BOOKMARK, &src_is_bookmark, -1);
          gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &dest_iter,
                             SB_COL_IS_BOOKMARK, &dest_is_bookmark, -1);

          if (src_is_bookmark && dest_is_bookmark) {
            if (gtk_tree_path_compare(src_path, dest_tpath) != 0) {
              if (dest_pos == GTK_TREE_VIEW_DROP_BEFORE ||
                  dest_pos == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE) {
                gtk_tree_store_move_before(app->sidebar_store, &src_iter,
                                           &dest_iter);
              } else {
                gtk_tree_store_move_after(app->sidebar_store, &src_iter,
                                          &dest_iter);
              }
              sidebar_save_bookmarks(app);
            }
            success = TRUE;
          }
        }
        gtk_tree_path_free(dest_tpath);
      }
      if (src_path)
        gtk_tree_path_free(src_path);
    }

    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), NULL, 0);
    gtk_drag_finish(context, success, FALSE, time);
    return;
  }

  GtkTreePath *tpath = NULL;
  GtkTreeViewDropPosition pos;
  if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget), x, y, &tpath,
                                        &pos)) {
    GtkTreeIter iter;
    char *dest_path = NULL;
    if (tpath &&
        gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sidebar_store), &iter,
                                tpath) &&
        sidebar_row_is_drop_target(app, &iter, &dest_path) &&
        gtk_selection_data_get_length(data) > 0) {

      char **uris = gtk_selection_data_get_uris(data);
      if (uris) {
        GList *src_paths = NULL;
        for (int i = 0; uris[i]; i++) {
          char *p = g_filename_from_uri(uris[i], NULL, NULL);
          if (p)
            src_paths = g_list_append(src_paths, p);
        }
        g_strfreev(uris);
        if (src_paths) {
          copyjob_start(app, src_paths, dest_path, FALSE /* copy, not move */);
          success = TRUE;
        }
      }
    }
    g_free(dest_path);
    if (tpath)
      gtk_tree_path_free(tpath);
  }

  gtk_drag_finish(context, success, FALSE, time);
}

static void on_sidebar_menu_mount(GtkMenuItem *mi, gpointer user_data) {
  struct {
    AppWidgets *app;
    GVolume *volume;
  } *ctx = user_data;
  mount_volume(ctx->app, ctx->volume);
  g_object_unref(ctx->volume);
  g_free(ctx);
  (void)mi;
}

static void on_sidebar_menu_unmount(GtkMenuItem *mi, gpointer user_data) {
  struct {
    AppWidgets *app;
    GMount *mount;
  } *ctx = user_data;
  unmount_mount(ctx->app, ctx->mount);
  g_object_unref(ctx->mount);
  g_free(ctx);
  (void)mi;
}

static void on_sidebar_menu_remove_bookmark(GtkMenuItem *mi,
                                            gpointer user_data) {
  struct {
    AppWidgets *app;
    char *path;
  } *ctx = user_data;
  remove_bookmark_row(ctx->app, ctx->path);
  g_free(ctx->path);
  g_free(ctx);
  (void)mi;
}

static gboolean on_sidebar_button_press(GtkWidget *widget,
                                        GdkEventButton *event,
                                        gpointer user_data) {
  AppWidgets *app = user_data;

  if (event->button == 1) {
    g_clear_pointer(&g_bookmark_drag_source_path, gtk_tree_path_free);

    GtkTreePath *tpath = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)event->x,
                                      (gint)event->y, &tpath, NULL, NULL,
                                      NULL) &&
        tpath) {
      GtkTreeIter iter;
      gboolean is_bookmark = FALSE;
      if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sidebar_store), &iter,
                                  tpath)) {
        gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &iter,
                           SB_COL_IS_BOOKMARK, &is_bookmark, -1);
      }
      if (is_bookmark) {
        g_bookmark_drag_source_path = gtk_tree_path_copy(tpath);
      }
      gtk_tree_path_free(tpath);
    }
    return FALSE;
  }

  if (event->button != 3)
    return FALSE;

  GtkTreePath *tpath;
  if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)event->x,
                                     (gint)event->y, &tpath, NULL, NULL,
                                     NULL)) {
    return TRUE;
  }
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->sidebar_store), &iter,
                               tpath)) {
    gtk_tree_path_free(tpath);
    return TRUE;
  }
  gtk_tree_path_free(tpath);

  gboolean is_device, mounted, is_bookmark;
  gpointer volume_ptr, mount_ptr;
  char *item_path;
  gtk_tree_model_get(GTK_TREE_MODEL(app->sidebar_store), &iter,
                     SB_COL_IS_DEVICE, &is_device, SB_COL_MOUNTED, &mounted,
                     SB_COL_VOLUME, &volume_ptr, SB_COL_MOUNT, &mount_ptr,
                     SB_COL_IS_BOOKMARK, &is_bookmark, SB_COL_PATH, &item_path,
                     -1);
  if (!is_device && !is_bookmark) {
    g_free(item_path);
    return TRUE;
  }

  GtkWidget *menu = gtk_menu_new();

  if (is_bookmark) {
    GtkWidget *item = gtk_menu_item_new_with_label("Remove Bookmark");
    struct {
      AppWidgets *app;
      char *path;
    } *ctx = g_new0(typeof(*ctx), 1);
    ctx->app = app;
    ctx->path = g_strdup(item_path);
    g_signal_connect(item, "activate",
                     G_CALLBACK(on_sidebar_menu_remove_bookmark), ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  }
  g_free(item_path);

  if (!mounted && volume_ptr) {
    GtkWidget *item = gtk_menu_item_new_with_label("Mount");
    struct {
      AppWidgets *app;
      GVolume *volume;
    } *ctx = g_new0(typeof(*ctx), 1);
    ctx->app = app;
    ctx->volume = g_object_ref(G_VOLUME(volume_ptr));
    g_signal_connect(item, "activate", G_CALLBACK(on_sidebar_menu_mount), ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  }
  if (mounted && mount_ptr) {
    GtkWidget *item = gtk_menu_item_new_with_label("Unmount / Eject");
    struct {
      AppWidgets *app;
      GMount *mount;
    } *ctx = g_new0(typeof(*ctx), 1);
    ctx->app = app;
    ctx->mount = g_object_ref(G_MOUNT(mount_ptr));
    g_signal_connect(item, "activate", G_CALLBACK(on_sidebar_menu_unmount),
                     ctx);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  }
  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
  return TRUE;
}

static void sidebar_text_cell_data_func(GtkTreeViewColumn *col,
                                        GtkCellRenderer *renderer,
                                        GtkTreeModel *model, GtkTreeIter *iter,
                                        gpointer data) {
  (void)col;
  (void)data;
  GtkTreePath *path = gtk_tree_model_get_path(model, iter);
  gint depth = gtk_tree_path_get_depth(path);
  gtk_tree_path_free(path);

  if (depth == 1) {
    /* section header: "Places", "Devices", "Bookmarks" */
    g_object_set(renderer, "weight", PANGO_WEIGHT_BOLD, "scale",
                 PANGO_SCALE_SMALL, "foreground-rgba",
                 &(GdkRGBA){0.55, 0.55, 0.55, 1.0}, "ypad", 6, NULL);
  } else {
    g_object_set(renderer, "weight", PANGO_WEIGHT_NORMAL, "scale",
                 PANGO_SCALE_MEDIUM, "foreground-set", FALSE, "ypad", 4, NULL);
  }
}

static gboolean sidebar_select_func(GtkTreeSelection *sel, GtkTreeModel *model,
                                    GtkTreePath *path,
                                    gboolean currently_selected,
                                    gpointer data) {
  (void)sel;
  (void)model;
  (void)currently_selected;
  (void)data;
  return gtk_tree_path_get_depth(path) != 1;
}

static void sidebar_apply_css(GtkWidget *tree_view) {
  GtkCssProvider *provider = gtk_css_provider_new();

  const char *css =
      "treeview.gtkfm-sidebar {"
      // "    background: transparent;"
      // "    -GtkTreeView-horizontal-separator: 0;"
      "}"

      /* Rows */
      "treeview.gtkfm-sidebar row {"
      "    min-height: 30px;"
      "    padding: 3px 8px;"
      "    margin: 2px 6px;"
      "    border-radius: 8px;"
      "    border: 1px solid transparent;"
      "    transition: 150ms ease;"
      "}"

      /* Hover */
      "treeview.gtkfm-sidebar row:hover {"
      "    background-color: alpha(@theme_fg_color,0.06);"
      "}"

      /* Keyboard focus (NOT selected) */
      "treeview.gtkfm-sidebar row:focus:not(:selected) {"
      "    background-color: alpha(@theme_selected_bg_color,0.18);"
      "    border-color: alpha(@theme_selected_bg_color,0.45);"
      "}"

      /* Selection */
      "treeview.gtkfm-sidebar row:selected {"
      "    background-color: @theme_selected_bg_color;"
      "    border-color: @theme_selected_bg_color;"
      "}"

      /* Selection + Hover */
      "treeview.gtkfm-sidebar row:selected:hover {"
      "    background-color: shade(@theme_selected_bg_color,1.05);"
      "}"

      /* Selected but window lost focus */
      "treeview.gtkfm-sidebar row:selected:not(:focus) {"
      "    background-color: alpha(@theme_selected_bg_color,0.55);"
      "    border-color: transparent;"
      "}"

      /* Keep text default everywhere */
      "treeview.gtkfm-sidebar row,"
      "treeview.gtkfm-sidebar row label,"
      "treeview.gtkfm-sidebar row cell,"
      "treeview.gtkfm-sidebar row:selected,"
      "treeview.gtkfm-sidebar row:selected label,"
      "treeview.gtkfm-sidebar row:focus,"
      "treeview.gtkfm-sidebar row:focus label {"
      "    color: @theme_fg_color;"
      "}";

  gtk_css_provider_load_from_data(provider, css, -1, NULL);

  gtk_style_context_add_provider(gtk_widget_get_style_context(tree_view),
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_object_unref(provider);
}

/* ---- public entry point ---------------------------------------------------*/

GtkWidget *sidebar_create(AppWidgets *app) {
  app->sidebar_store = gtk_tree_store_new(
      SB_N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN,
      G_TYPE_BOOLEAN, G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_BOOLEAN);

  gtk_tree_sortable_set_sort_column_id(
      GTK_TREE_SORTABLE(app->sidebar_store),
      GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);

  app->sidebar_tree =
      gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->sidebar_store));

  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->sidebar_tree), FALSE);

  gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(app->sidebar_tree),
                                             TRUE);

  /* Column */
  GtkTreeViewColumn *col = gtk_tree_view_column_new();

  GtkCellRenderer *pix_r = gtk_cell_renderer_pixbuf_new();
  GtkCellRenderer *txt_r = gtk_cell_renderer_text_new();

  g_object_set(txt_r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);

  gtk_tree_view_column_pack_start(col, pix_r, FALSE);
  gtk_tree_view_column_add_attribute(col, pix_r, "pixbuf", SB_COL_ICON);

  gtk_tree_view_column_pack_start(col, txt_r, TRUE);
  gtk_tree_view_column_add_attribute(col, txt_r, "text", SB_COL_NAME);

  gtk_tree_view_column_set_cell_data_func(
      col, txt_r, sidebar_text_cell_data_func, NULL, NULL);

  gtk_tree_view_append_column(GTK_TREE_VIEW(app->sidebar_tree), col);

  gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(app->sidebar_tree), FALSE);

  gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(app->sidebar_tree), 6);

  gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(app->sidebar_tree), FALSE);

  gtk_style_context_add_class(gtk_widget_get_style_context(app->sidebar_tree),
                              "gtkfm-sidebar");

  sidebar_apply_css(app->sidebar_tree);

  gtk_tree_selection_set_select_function(
      gtk_tree_view_get_selection(GTK_TREE_VIEW(app->sidebar_tree)),
      sidebar_select_func, NULL, NULL);

  /* ---------------- Places ---------------- */

  GtkTreeIter places_parent;

  gtk_tree_store_append(app->sidebar_store, &places_parent, NULL);

  gtk_tree_store_set(app->sidebar_store, &places_parent, SB_COL_NAME, "Places",
                     SB_COL_PATH, "", SB_COL_IS_DEVICE, FALSE, SB_COL_MOUNTED,
                     TRUE, -1);

  add_place_row(app->sidebar_store, &places_parent, "Computer", COMPUTER_URI,
                "computer");

  add_place_row(app->sidebar_store, &places_parent, "Recent", RECENT_URI,
                "document-open-recent");

  /* ---------------- Bookmarks (Ctrl+D) ---------------- */

  g_bookmarks_parent_iter = places_parent;
  g_bookmarks_parent_valid = TRUE;

  sidebar_load_bookmarks(app);

  gboolean trash_empty = fileview_trash_is_empty();

  GdkPixbuf *pb =
      icon_for_name(trash_empty ? "user-trash" : "user-trash-full", 20);

  gtk_tree_store_append(app->sidebar_store, &g_trash_iter, &places_parent);

  gtk_tree_store_set(app->sidebar_store, &g_trash_iter, SB_COL_ICON, pb,
                     SB_COL_NAME, "Trash", SB_COL_PATH, "trash://",
                     SB_COL_IS_DEVICE, FALSE, SB_COL_MOUNTED, TRUE, -1);

  if (pb)
    g_object_unref(pb);

  g_trash_iter_valid = TRUE;

  /* ---------------- Devices ---------------- */

  gtk_tree_store_append(app->sidebar_store, &g_devices_parent_iter, NULL);

  gtk_tree_store_set(app->sidebar_store, &g_devices_parent_iter, SB_COL_NAME,
                     "Devices", SB_COL_PATH, "", SB_COL_IS_DEVICE, FALSE,
                     SB_COL_MOUNTED, TRUE, -1);

  g_devices_parent_valid = TRUE;

  app->volume_monitor = g_volume_monitor_get();

  rebuild_devices(app, &g_devices_parent_iter);

  g_signal_connect(app->volume_monitor, "volume-added",
                   G_CALLBACK(on_monitor_changed), app);

  g_signal_connect(app->volume_monitor, "volume-removed",
                   G_CALLBACK(on_monitor_changed), app);

  g_signal_connect(app->volume_monitor, "mount-added",
                   G_CALLBACK(on_monitor_changed), app);

  g_signal_connect(app->volume_monitor, "mount-removed",
                   G_CALLBACK(on_monitor_changed), app);

  gtk_tree_view_expand_all(GTK_TREE_VIEW(app->sidebar_tree));

  g_signal_connect(app->sidebar_tree, "row-activated",
                   G_CALLBACK(on_row_activated), app);

  g_signal_connect(app->sidebar_tree, "button-press-event",
                   G_CALLBACK(on_sidebar_button_press), app);

  gtk_drag_dest_set(app->sidebar_tree, GTK_DEST_DEFAULT_DROP,
                    sb_dnd_dest_targets, G_N_ELEMENTS(sb_dnd_dest_targets),
                    GDK_ACTION_COPY);

  gtk_tree_view_enable_model_drag_source(
      GTK_TREE_VIEW(app->sidebar_tree), GDK_BUTTON1_MASK, sb_dnd_source_targets,
      G_N_ELEMENTS(sb_dnd_source_targets), GDK_ACTION_COPY);

  g_signal_connect(app->sidebar_tree, "drag-begin",
                   G_CALLBACK(on_sidebar_drag_begin), app);

  g_signal_connect(app->sidebar_tree, "drag-data-get",
                   G_CALLBACK(on_sidebar_drag_data_get), app);

  g_signal_connect(app->sidebar_tree, "drag-motion",
                   G_CALLBACK(on_sidebar_drag_motion), app);

  g_signal_connect(app->sidebar_tree, "drag-data-received",
                   G_CALLBACK(on_sidebar_drag_data_received), app);

  g_signal_connect(app->sidebar_tree, "drag-end",
                   G_CALLBACK(on_sidebar_drag_end), app);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);

  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled),
                                      GTK_SHADOW_NONE);

  gtk_container_add(GTK_CONTAINER(scrolled), app->sidebar_tree);

  gtk_widget_set_margin_start(scrolled, 4);

  gtk_widget_set_size_request(scrolled, 100, -1);

  return scrolled;
}
