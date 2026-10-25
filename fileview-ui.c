#include "deletejob.h"
#include "fileview_internal.h"

GList *get_selected_paths(AppWidgets *app) {
  GList *out = NULL;
  GList *selected =
      gtk_icon_view_get_selected_items(GTK_ICON_VIEW(app->icon_view));
  for (GList *l = selected; l; l = l->next) {
    GtkTreePath *tpath = l->data;
    GtkTreeIter filter_iter, iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                                tpath)) {
      gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter, &iter,
                                                       &filter_iter);
      char *path;
      gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_PATH,
                         &path, -1);
      out = g_list_prepend(out, path);
    }
  }
  g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
  return out;
}

char *fileview_get_selected_path(AppWidgets *app) {
  GList *paths = get_selected_paths(app);

  if (!paths)
    return NULL;

  char *path = g_strdup((char *)paths->data);

  g_list_free_full(paths, g_free);

  return path;
}

static void set_cut_flags(AppWidgets *app, GList *paths, gboolean cut_flag) {
  if (!paths)
    return;
  GtkTreeIter iter;
  gboolean valid =
      gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->file_store), &iter);
  while (valid) {
    char *row_path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_PATH,
                       &row_path, -1);
    if (row_path) {
      for (GList *l = paths; l; l = l->next) {
        if (g_strcmp0(row_path, (const char *)l->data) == 0) {
          gtk_list_store_set(app->file_store, &iter, COL_CUT, cut_flag, -1);
          break;
        }
      }
      g_free(row_path);
    }
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->file_store), &iter);
  }
}

static void clear_clipboard(AppWidgets *app) {
  (void)app;
  if (g_clipboard_paths && g_clipboard_cut && g_clipboard_source_app)
    set_cut_flags(g_clipboard_source_app, g_clipboard_paths, FALSE);

  g_list_free_full(g_clipboard_paths, g_free);
  g_clipboard_paths = NULL;
  g_clipboard_cut = FALSE;
  g_clipboard_source_app = NULL;
}

void fileview_select_all(AppWidgets *app) {
  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0)
    return;
  gtk_icon_view_select_all(GTK_ICON_VIEW(app->icon_view));
}

struct BulkDeleteCtx {
  AppWidgets *app;
  GList *paths;
  gboolean permanent;
  char *post_target;
};

static void run_bulk_delete(struct BulkDeleteCtx *ctx) {
  if (ctx->permanent) {
    deletejob_start_permanent(ctx->app, ctx->paths, ctx->post_target);
    g_free(ctx);
    return;
  }

  deletejob_start_trash(ctx->app, ctx->paths, ctx->post_target);
  g_free(ctx);
}

static void on_bulk_delete_confirm(GtkDialog *dialog, gint response,
                                   gpointer user_data) {
  struct BulkDeleteCtx *ctx = user_data;
  if (response == GTK_RESPONSE_YES) {
    run_bulk_delete(ctx);
  } else {
    g_list_free_full(ctx->paths, g_free);
    g_free(ctx->post_target);
    g_free(ctx);
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

void fileview_delete_selected(AppWidgets *app, gboolean force_permanent) {
  // if (!app || !app->icon_view)
  //   return;
  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0)
    return;

  GList *paths = get_selected_paths(app);
  if (!paths) {
    set_status(app, "Delete ke liye pehle koi item select karein");
    return;
  }

  gboolean permanent = force_permanent || is_trash_dir(app);

  struct BulkDeleteCtx *ctx = g_new0(struct BulkDeleteCtx, 1);
  ctx->app = app;
  ctx->paths = paths;
  ctx->permanent = permanent;
  ctx->post_target = capture_post_delete_selection_target(app);

  if (!permanent) {
    run_bulk_delete(ctx);
    return;
  }

  guint n = g_list_length(paths);
  char *msg = g_strdup_printf(
      "%u item ko hamesha ke liye delete karna hai? Ye wapas nahi aa sakta.",
      n);
  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_YES_NO, "%s", msg);
  g_free(msg);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);

  g_signal_connect(dialog, "response", G_CALLBACK(on_bulk_delete_confirm), ctx);
  gtk_widget_show(dialog);
}

static void do_copy_or_cut(AppWidgets *app, gboolean cut) {
  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0)
    return;
  GList *paths = get_selected_paths(app);
  if (!paths) {
    set_status(app, "Copy/Cut ke liye pehle koi item select karein");
    return;
  }
  clear_clipboard(app);
  g_clipboard_paths = paths;
  g_clipboard_cut = cut;
  g_clipboard_source_app = cut ? app : NULL;

  if (cut)
    set_cut_flags(app, paths, TRUE);

  char *msg = g_strdup_printf("%u item %s", g_list_length(paths),
                              cut ? "cut ho gaye" : "copy ho gaye");
  set_status(app, msg);
  g_free(msg);
}

void fileview_copy(AppWidgets *app) { do_copy_or_cut(app, FALSE); }
void fileview_cut(AppWidgets *app) { do_copy_or_cut(app, TRUE); }

void fileview_paste(AppWidgets *app) {
  if (!g_clipboard_paths) {
    set_status(app, "Clipboard khali hai");
    return;
  }
  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0 ||
      g_strcmp0(app->current_dir, RECENT_URI) == 0) {
    set_status(app, "Yahan paste nahi kar sakte");
    return;
  }

  if (g_clipboard_cut && g_clipboard_source_app)
    set_cut_flags(g_clipboard_source_app, g_clipboard_paths, FALSE);

  GList *paths = g_clipboard_paths;
  gboolean is_move = g_clipboard_cut;
  g_clipboard_paths = NULL;
  g_clipboard_cut = FALSE;
  g_clipboard_source_app = NULL;

  copyjob_start(app, paths, app->current_dir, is_move);
}

static void on_path_entry_changed(GtkEditable *editable, gpointer user_data) {
  AppWidgets *app = user_data;
  GtkEntryCompletion *completion =
      gtk_entry_get_completion(GTK_ENTRY(app->path_entry));
  if (!completion)
    return;

  GtkListStore *store =
      GTK_LIST_STORE(gtk_entry_completion_get_model(completion));
  gtk_list_store_clear(store);

  const char *text = gtk_entry_get_text(GTK_ENTRY(editable));
  if (!text || !*text)
    return;

  const char *last_slash = strrchr(text, '/');
  if (!last_slash)
    return;

  char *dir_part = g_strndup(text, (gsize)(last_slash - text + 1));
  if (dir_part[0] == '\0') {
    g_free(dir_part);
    dir_part = g_strdup("/");
  }
  const char *base_part = last_slash + 1; /* jo naam abhi type ho raha hai */
  gboolean typing_hidden = (base_part[0] == '.');

  GDir *gdir = g_dir_open(dir_part, 0, NULL);
  if (gdir) {
    const char *name;
    while ((name = g_dir_read_name(gdir)) != NULL) {
      gboolean is_hidden_entry = (name[0] == '.');
      if (is_hidden_entry && !app->show_hidden && !typing_hidden)
        continue;

      char *full = g_strconcat(dir_part, name, NULL);
      gboolean is_dir = g_file_test(full, G_FILE_TEST_IS_DIR);
      char *suggestion = is_dir ? g_strconcat(full, "/", NULL) : g_strdup(full);

      GtkTreeIter iter;
      gtk_list_store_append(store, &iter);
      gtk_list_store_set(store, &iter, 0, suggestion, -1);

      g_free(suggestion);
      g_free(full);
    }
    g_dir_close(gdir);
  }

  g_free(dir_part);
}

static void on_path_entry_activate(GtkEntry *entry, gpointer user_data) {
  AppWidgets *app = user_data;
  const char *text = gtk_entry_get_text(entry);
  fileview_navigate(app, text, TRUE);
  gtk_widget_grab_focus(app->icon_view);
}

static gboolean search_visible_func(GtkTreeModel *model, GtkTreeIter *iter,
                                    gpointer user_data) {
  AppWidgets *app = user_data;
  if (!app->search_text)
    return TRUE;

  char *name = NULL;
  gtk_tree_model_get(model, iter, COL_NAME, &name, -1);
  if (!name)
    return FALSE;

  char *folded = g_utf8_casefold(name, -1);
  gboolean visible = (strstr(folded, app->search_text) != NULL);
  g_free(folded);
  g_free(name);
  return visible;
}

static char *make_display_name(const char *name) {
  const glong LINE1 = 9;
  const glong LINE2_MAX = 9; /* itne letters doosri line par, phir "..." */
  glong len = g_utf8_strlen(name, -1);

  if (len <= LINE1)
    return g_strdup(name);

  const char *dot = strrchr(name, '.');
  char *ext = NULL;
  glong ext_len = 0;
  if (dot && dot != name && *(dot + 1) != '\0') {
    ext = g_strdup(dot);
    ext_len = g_utf8_strlen(ext, -1);
  }

  char *line1 = g_utf8_substring(name, 0, LINE1);
  char *out;

  if (len <= LINE1 + 11) {
    char *line2 = g_utf8_substring(name, LINE1, len);
    out = g_strdup_printf("%s\n%s", line1, line2);
    g_free(line2);
  } else if (ext && ext_len > 0 && ext_len < LINE2_MAX) {
    glong avail = LINE2_MAX - ext_len;
    char *line2_start = g_utf8_substring(name, LINE1, LINE1 + avail);
    out = g_strdup_printf("%s\n%s...%s", line1, line2_start, ext);
    g_free(line2_start);
  } else {
    /* Format bohot lamba hai ya mojood nahi -- purana tareeqa. */
    char *line2 = g_utf8_substring(name, LINE1, LINE1 + LINE2_MAX);
    out = g_strdup_printf("%s\n%s...", line1, line2);
    g_free(line2);
  }
  g_free(line1);
  g_free(ext);
  return out;
}

static void pixbuf_cell_func(GtkCellLayout *layout, GtkCellRenderer *cell,
                             GtkTreeModel *model, GtkTreeIter *iter,
                             gpointer user_data) {
  (void)layout;
  AppWidgets *app = user_data;
  gboolean cut = FALSE;
  gboolean is_hidden = FALSE;
  GdkPixbuf *pixbuf = NULL;
  gtk_tree_model_get(model, iter, COL_PIXBUF, &pixbuf, COL_CUT, &cut,
                     COL_IS_HIDDEN, &is_hidden, -1);
  gboolean dim = cut || is_hidden;

  if (pixbuf) {
    int size = app->icon_size > 0 ? app->icon_size : THUMB_SIZE;
    int pw = gdk_pixbuf_get_width(pixbuf);
    int ph = gdk_pixbuf_get_height(pixbuf);
    int longest = MAX(pw, ph);

    if (longest > 0 && longest != size) {
      double scale = (double)size / (double)longest;
      int new_w = MAX(1, (int)(pw * scale + 0.5));
      int new_h = MAX(1, (int)(ph * scale + 0.5));
      GdkPixbuf *scaled =
          gdk_pixbuf_scale_simple(pixbuf, new_w, new_h, GDK_INTERP_BILINEAR);
      g_object_set(cell, "pixbuf", scaled, "sensitive", !dim, NULL);
      if (scaled)
        g_object_unref(scaled);
    } else {
      g_object_set(cell, "pixbuf", pixbuf, "sensitive", !dim, NULL);
    }
    g_object_unref(pixbuf);
  } else {
    g_object_set(cell, "pixbuf", NULL, "sensitive", !dim, NULL);
  }
}

static void name_cell_func(GtkCellLayout *layout, GtkCellRenderer *cell,
                           GtkTreeModel *model, GtkTreeIter *iter,
                           gpointer user_data) {
  (void)layout;
  AppWidgets *app = user_data;
  gboolean cut = FALSE;
  gboolean is_hidden = FALSE;
  char *name = NULL;
  gtk_tree_model_get(model, iter, COL_NAME, &name, COL_CUT, &cut, COL_IS_HIDDEN,
                     &is_hidden, -1);
  if (name) {
    gboolean list_mode = app && app->list_view;
    char *display = list_mode ? g_strdup(name) : make_display_name(name);
    g_object_set(cell, "text", display, "sensitive", !(cut || is_hidden),
                 "alignment", list_mode ? PANGO_ALIGN_LEFT : PANGO_ALIGN_CENTER,
                 "xalign", list_mode ? 0.0 : 0.5, "ellipsize",
                 list_mode ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE, NULL);
    g_free(display);
    g_free(name);
  }
}

static void on_selection_changed(GtkIconView *iv, gpointer user_data) {
  AppWidgets *app = user_data;

  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0) {
    infopanel_refresh(app);
    return;
  }

  GList *selected = gtk_icon_view_get_selected_items(iv);
  if (!selected) {
    app->select_anchor_idx = -1;
    set_status(app, app->default_status ? app->default_status : "");
    infopanel_refresh(app);
    return;
  }

  if (!selected->next) {
    gint *idx = gtk_tree_path_get_indices((GtkTreePath *)selected->data);
    if (idx)
      app->select_anchor_idx = idx[0];
  }

  guint n = 0;
  guint64 total_bytes = 0;
  gboolean any_file = FALSE;

  for (GList *l = selected; l; l = l->next) {
    GtkTreePath *tpath = l->data;
    GtkTreeIter filter_iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                                tpath)) {
      gboolean is_dir = FALSE;
      guint64 size = 0;
      gtk_tree_model_get(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                         COL_IS_DIR, &is_dir, COL_SIZE, &size, -1);
      if (!is_dir) {
        total_bytes += size;
        any_file = TRUE;
      }
      n++;
    }
  }
  g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);

  char *msg;
  if (any_file) {
    char *size_str = g_format_size_full(total_bytes, G_FORMAT_SIZE_IEC_UNITS);
    msg = g_strdup_printf("%u item%s selected (%s)", n, n == 1 ? "" : "",
                          size_str);
    g_free(size_str);
  } else {
    msg = g_strdup_printf("%u item%s selected", n, n == 1 ? "" : "");
  }
  set_status(app, msg);
  g_free(msg);
  infopanel_refresh(app);
}

static void on_fileview_drag_data_get(GtkWidget *widget,
                                      GdkDragContext *context,
                                      GtkSelectionData *data, guint info,
                                      guint time, gpointer user_data) {
  (void)widget;
  (void)context;
  (void)info;
  (void)time;
  AppWidgets *app = user_data;

  GList *paths = get_selected_paths(app);
  if (!paths)
    return;

  guint n = g_list_length(paths);
  char **uris = g_new0(char *, n + 1);
  guint i = 0;
  for (GList *l = paths; l; l = l->next) {
    char *uri = g_filename_to_uri((const char *)l->data, NULL, NULL);
    if (uri)
      uris[i++] = uri;
  }
  gtk_selection_data_set_uris(data, uris);
  g_strfreev(uris);
  g_list_free_full(paths, g_free);
}

static gboolean on_fileview_drag_motion(GtkWidget *widget,
                                        GdkDragContext *context, gint x, gint y,
                                        guint time, gpointer user_data) {
  AppWidgets *app = user_data;
  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0 ||
      g_strcmp0(app->current_dir, RECENT_URI) == 0) {
    gdk_drag_status(context, 0, time);
    return TRUE;
  }

  GtkTreePath *tpath =
      gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget), x, y);
  gboolean over_folder = FALSE;

  if (tpath) {
    GtkTreeIter filter_iter, iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                                tpath)) {
      gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter, &iter,
                                                       &filter_iter);
      gboolean is_dir = FALSE;
      gpointer volume_ptr = NULL;
      gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_IS_DIR,
                         &is_dir, COL_VOLUME, &volume_ptr, -1);
      over_folder = is_dir && !volume_ptr; /* skip unmounted computer:// rows */
    }
  }

  gtk_icon_view_set_drag_dest_item(GTK_ICON_VIEW(widget),
                                   over_folder ? tpath : NULL,
                                   GTK_ICON_VIEW_DROP_INTO);
  if (tpath)
    gtk_tree_path_free(tpath);

  gdk_drag_status(context, GDK_ACTION_COPY, time);
  return TRUE;
}

static void on_fileview_drag_data_received(GtkWidget *widget,
                                           GdkDragContext *context, gint x,
                                           gint y, GtkSelectionData *data,
                                           guint info, guint time,
                                           gpointer user_data) {
  (void)info;
  AppWidgets *app = user_data;
  gboolean success = FALSE;

  if (g_strcmp0(app->current_dir, COMPUTER_URI) != 0 &&
      g_strcmp0(app->current_dir, RECENT_URI) != 0 &&
      gtk_selection_data_get_length(data) > 0) {

    GList *src_paths = uri_list_to_paths(data);
    if (src_paths) {
      char *dest_dir = NULL;

      GtkTreePath *tpath =
          gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(widget), x, y);
      if (tpath) {
        GtkTreeIter filter_iter, iter;
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter),
                                    &filter_iter, tpath)) {
          gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter,
                                                           &iter, &filter_iter);
          gboolean is_dir = FALSE;
          gpointer volume_ptr = NULL;
          char *item_path = NULL;
          gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_IS_DIR,
                             &is_dir, COL_VOLUME, &volume_ptr, COL_PATH,
                             &item_path, -1);
          if (is_dir && !volume_ptr && item_path)
            dest_dir = g_strdup(item_path);
          g_free(item_path);
        }
        gtk_tree_path_free(tpath);
      }

      if (!dest_dir && app->current_dir)
        dest_dir = g_strdup(app->current_dir);

      gboolean all_already_here = dest_dir != NULL;
      for (GList *l = src_paths; dest_dir && l; l = l->next) {
        char *parent = g_path_get_dirname((const char *)l->data);
        gboolean same = (g_strcmp0(parent, dest_dir) == 0);
        g_free(parent);
        if (!same) {
          all_already_here = FALSE;
          break;
        }
      }

      if (dest_dir && !all_already_here) {
        copyjob_start(app, src_paths, dest_dir, FALSE /* copy, not move */);
        success = TRUE;
      } else {
        g_list_free_full(src_paths, g_free);
      }
      g_free(dest_dir);
    }
  }

  gtk_drag_finish(context, success, FALSE, time);
}

static void fileview_apply_css(GtkWidget *icon_view) {
  GtkCssProvider *provider = gtk_css_provider_new();
  // const char *css = "iconview.gtkfm-fileview {"
  //                   "  background-color: transparent;"
  //                   "  -GtkWidget-focus-line-width: 1;"
  //                   "  -GtkWidget-focus-padding: 0;"
  //                   "}"
  //                   "iconview.gtkfm-fileview:selected {"
  //                   "  background-color: alpha(@theme_selected_bg_color, 0.85);"
  //                   "  color: @theme_selected_fg_color;"
  //                   "  border-radius: 10px;"
  //                   "}"
  //                   "iconview.gtkfm-fileview:selected:focus {"
  //                   "  background-color: @theme_selected_bg_color;"
  //                   "}";
  const char *css = "iconview.gtkfm-fileview {"
  "  background-color: transparent;"
  "  -GtkWidget-focus-line-width: 0;"
  "  -GtkWidget-focus-padding: 0;"
  "}"
  "iconview.gtkfm-fileview:selected {"
  "  background-color: alpha(@theme_selected_bg_color, 0.75);"
  "  color: @theme_selected_fg_color;"
  "  border-radius: 10px;"
  "}"
  "iconview.gtkfm-fileview:selected:focus {"
  "  background-color: alpha(@theme_selected_bg_color, 0.85);"
  "}";
  gtk_css_provider_load_from_data(provider, css, -1, NULL);
  gtk_style_context_add_provider(gtk_widget_get_style_context(icon_view),
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

GtkWidget *fileview_create(AppWidgets *app) {
  app->file_store = gtk_list_store_new(
      N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN,
      G_TYPE_POINTER, G_TYPE_DATE_TIME, G_TYPE_UINT64, G_TYPE_STRING,
      G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_POINTER,
      G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_UINT);

  app->file_filter = GTK_TREE_MODEL_FILTER(
      gtk_tree_model_filter_new(GTK_TREE_MODEL(app->file_store), NULL));
  gtk_tree_model_filter_set_visible_func(app->file_filter, search_visible_func,
                                         app, NULL);

  app->icon_view =
      gtk_icon_view_new_with_model(GTK_TREE_MODEL(app->file_filter));
  gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(app->icon_view), COL_PIXBUF);
  gtk_icon_view_set_text_column(GTK_ICON_VIEW(app->icon_view), COL_NAME);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->icon_view),
                              "gtkfm-fileview");
  fileview_apply_css(app->icon_view);
  app->icon_size = ICON_SIZE_MIN;
  gtk_icon_view_set_item_width(GTK_ICON_VIEW(app->icon_view),
                               app->icon_size + ITEM_WIDTH_PAD);

  // gtk_icon_view_set_item_padding(GTK_ICON_VIEW(app->icon_view), 2);
  // gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(app->icon_view), 2);
  // gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(app->icon_view), 2);
  // gtk_icon_view_set_margin(GTK_ICON_VIEW(app->icon_view), 2);
  // gtk_icon_view_set_spacing(GTK_ICON_VIEW(app->icon_view), 2);
  gtk_icon_view_set_item_padding(GTK_ICON_VIEW(app->icon_view), 0);
  gtk_icon_view_set_column_spacing(GTK_ICON_VIEW(app->icon_view), 0);
  gtk_icon_view_set_row_spacing(GTK_ICON_VIEW(app->icon_view), 0);
  gtk_icon_view_set_margin(GTK_ICON_VIEW(app->icon_view), 0);
  gtk_icon_view_set_spacing(GTK_ICON_VIEW(app->icon_view), 0);
  gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(app->icon_view),
                                   GTK_SELECTION_MULTIPLE);

  {
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(app->icon_view));
    for (GList *l = cells; l; l = l->next) {
      GtkCellRenderer *cell = GTK_CELL_RENDERER(l->data);
      g_object_set(cell, "xpad", 0, "ypad", 0, NULL);
      if (GTK_IS_CELL_RENDERER_PIXBUF(cell)) {
        g_object_set(cell, "xalign", 0.5, "yalign", 0.5, "xpad", 0, "ypad", 0,
                     NULL);

        gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(app->icon_view),
                                           cell, pixbuf_cell_func, app, NULL);

      } else if (GTK_IS_CELL_RENDERER_TEXT(cell)) {
        g_object_set(cell, "alignment", PANGO_ALIGN_CENTER, "xalign", 0.5,
                     NULL);

        gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(app->icon_view),
                                           cell, name_cell_func, app, NULL);
      }
    }
    g_list_free(cells);
  }

  app->folders_first = TRUE;
  app->folder_previews = TRUE;
  app->video_thumbnail_frame = TRUE;
  app->show_hidden = FALSE;
  app->sort_mode = SORT_NAME_ASC;
  app->list_view = FALSE;
  fileview_load_settings(app);
  fileview_set_list_view(app, app->list_view);

  gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(app->file_store),
                                          file_sort_func, app, NULL);

  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->file_store),
                                       GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);

  g_signal_connect(app->icon_view, "item-activated",
                   G_CALLBACK(on_item_activated), app);
  g_signal_connect(app->icon_view, "button-press-event",
                   G_CALLBACK(on_button_press), app);
  gtk_widget_add_events(app->icon_view,
                        GDK_POINTER_MOTION_MASK | GDK_BUTTON1_MOTION_MASK);
  g_signal_connect(app->icon_view, "motion-notify-event",
                   G_CALLBACK(on_iconview_motion), app);
  g_signal_connect(app->icon_view, "button-release-event",
                   G_CALLBACK(on_iconview_button_release), app);
  g_signal_connect(app->icon_view, "selection-changed",
                   G_CALLBACK(on_selection_changed), app);
  g_signal_connect(app->icon_view, "key-press-event",
                   G_CALLBACK(on_iconview_key_press), app);

  if (app->path_entry && GTK_IS_ENTRY(app->path_entry)) {
    g_signal_connect(app->path_entry, "activate",
                     G_CALLBACK(on_path_entry_activate), app);
  }

  {
    GtkListStore *completion_store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion,
                                   GTK_TREE_MODEL(completion_store));
    g_object_unref(completion_store);
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_completion_set_inline_completion(completion, FALSE);
    gtk_entry_completion_set_popup_completion(completion, TRUE);
    gtk_entry_completion_set_popup_set_width(completion, TRUE);
    gtk_entry_completion_set_minimum_key_length(completion, 1);
    if (app->path_entry && GTK_IS_ENTRY(app->path_entry)) {
      gtk_entry_set_completion(GTK_ENTRY(app->path_entry), completion);
    }
    g_object_unref(completion);
  }

  if (app->path_entry && GTK_IS_ENTRY(app->path_entry)) {
    g_signal_connect(app->path_entry, "changed",
                     G_CALLBACK(on_path_entry_changed), app);
  }

  gtk_icon_view_enable_model_drag_source(
      GTK_ICON_VIEW(app->icon_view), GDK_BUTTON1_MASK, dnd_targets,
      G_N_ELEMENTS(dnd_targets), GDK_ACTION_COPY);
  g_signal_connect(app->icon_view, "drag-data-get",
                   G_CALLBACK(on_fileview_drag_data_get), app);

  gtk_drag_dest_set(app->icon_view, GTK_DEST_DEFAULT_DROP, dnd_targets,
                    G_N_ELEMENTS(dnd_targets), GDK_ACTION_COPY);
  g_signal_connect(app->icon_view, "drag-motion",
                   G_CALLBACK(on_fileview_drag_motion), app);
  g_signal_connect(app->icon_view, "drag-data-received",
                   G_CALLBACK(on_fileview_drag_data_received), app);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(scrolled), app->icon_view);
  return scrolled;
}

#define SEARCH_MAX_RESULTS 3000
#define SEARCH_MAX_DEPTH 64
#define SEARCH_INSERT_BATCH 150

typedef struct {
  char *full_path;
  char *name;
  gboolean is_dir;
} SearchResult;

typedef struct {
  AppWidgets *app;
  guint generation;
  char *root_dir;
  char *needle_folded;
  gboolean show_hidden;
  GCancellable *cancellable;
  GPtrArray *results;
  guint insert_index;
} SearchScanCtx;

static void search_result_free(gpointer p) {
  SearchResult *r = p;
  g_free(r->full_path);
  g_free(r->name);
  g_free(r);
}

static void search_walk(const char *dir_path, SearchScanCtx *ctx, int depth) {
  if (depth > SEARCH_MAX_DEPTH)
    return;
  if (g_cancellable_is_cancelled(ctx->cancellable))
    return;
  if (ctx->results->len >= SEARCH_MAX_RESULTS)
    return;

  GDir *gdir = g_dir_open(dir_path, 0, NULL);
  if (!gdir)
    return;

  const char *name;
  while ((name = g_dir_read_name(gdir)) != NULL) {
    if (g_cancellable_is_cancelled(ctx->cancellable))
      break;
    if (ctx->results->len >= SEARCH_MAX_RESULTS)
      break;

    gboolean hidden_entry = (name[0] == '.');
    if (hidden_entry && !ctx->show_hidden)
      continue;

    char *full = g_build_filename(dir_path, name, NULL);
    gboolean is_symlink = g_file_test(full, G_FILE_TEST_IS_SYMLINK);
    gboolean is_dir = g_file_test(full, G_FILE_TEST_IS_DIR);

    char *folded = g_utf8_casefold(name, -1);
    if (strstr(folded, ctx->needle_folded) != NULL) {
      SearchResult *r = g_new0(SearchResult, 1);
      r->full_path = g_strdup(full);
      r->name = g_strdup(name);
      r->is_dir = is_dir;
      g_ptr_array_add(ctx->results, r);
    }
    g_free(folded);

    if (is_dir && !is_symlink)
      search_walk(full, ctx, depth + 1);

    g_free(full);
  }
  g_dir_close(gdir);
}

static void search_scan_ctx_free(SearchScanCtx *ctx) {
  g_object_unref(ctx->cancellable);
  g_free(ctx->root_dir);
  g_free(ctx->needle_folded);
  g_ptr_array_free(ctx->results, TRUE);
  g_free(ctx);
}

static void add_search_result_row(AppWidgets *app, SearchResult *r) {
  GFile *gf = g_file_new_for_path(r->full_path);
  GFileInfo *info = g_file_query_info(
      gf,
      "standard::size,standard::is-symlink,standard::content-type,time::"
      "modified,access::can-write,unix::mode",
      G_FILE_QUERY_INFO_NONE, NULL, NULL);

  gboolean is_symlink = info ? g_file_info_get_is_symlink(info) : FALSE;
  gboolean is_readonly =
      info ? file_info_should_show_readonly_lock(info) : FALSE;
  const char *ctype = info ? g_file_info_get_content_type(info) : NULL;
  GDateTime *dt = info ? g_file_info_get_modification_date_time(info) : NULL;
  guint64 size = info ? (guint64)g_file_info_get_size(info) : 0;

  GdkPixbuf *icon =
      themed_icon_for(ctype, r->is_dir, r->full_path, app->icon_size);
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
  gtk_list_store_set(app->file_store, &iter, COL_PIXBUF, icon, COL_NAME,
                     r->name, COL_PATH, r->full_path, COL_IS_DIR, r->is_dir,
                     COL_VOLUME, NULL, COL_DATE, dt, COL_SIZE, size, COL_TYPE,
                     ctype ? ctype : "", COL_IS_SYMLINK, is_symlink,
                     COL_THEMED_ICON, TRUE, COL_IS_HIDDEN, (r->name[0] == '.'),
                     COL_IS_READONLY, is_readonly, -1);

  if (!r->is_dir) {
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
    thumbnail_get_async(r->full_path, app->icon_size, thumbnail_ready_cb, tctx);
  } else if (app->folder_previews && icon) {
    /* Normal listing (append_file_row) generates a folder-preview collage
     * for directories too -- search results were missing this branch, so
     * folders found via the search bar never got (or cached) a preview. */
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

    folder_preview_get_async(r->full_path, app->icon_size, icon,
                             thumbnail_ready_cb, tctx);
  }

  if (dt)
    g_date_time_unref(dt);
  if (info)
    g_object_unref(info);
  g_object_unref(gf);
  if (icon)
    g_object_unref(icon);
}

static gboolean search_scan_insert_idle(gpointer data) {
  SearchScanCtx *ctx = data;
  AppWidgets *app = ctx->app;

  if (ctx->generation != app->search_generation ||
      !app->search_recursive_active) {
    search_scan_ctx_free(ctx);
    return G_SOURCE_REMOVE;
  }

  if (ctx->insert_index == 0) {
    clear_file_store(app);
    gtk_icon_view_set_model(GTK_ICON_VIEW(app->icon_view), NULL);
    gtk_tree_sortable_set_sort_column_id(
        GTK_TREE_SORTABLE(app->file_store),
        GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
  }

  guint end = MIN(ctx->insert_index + SEARCH_INSERT_BATCH, ctx->results->len);
  for (; ctx->insert_index < end; ctx->insert_index++)
    add_search_result_row(app,
                          g_ptr_array_index(ctx->results, ctx->insert_index));

  if (ctx->insert_index < ctx->results->len) {
    char *progress =
        g_strdup_printf("%u matches mil rahe hain...", ctx->insert_index);
    set_status(app, progress);
    g_free(progress);
    return G_SOURCE_CONTINUE;
  }

  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->file_store),
                                       GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);
  reattach_file_model(app);

  char *msg = (ctx->results->len >= SEARCH_MAX_RESULTS)
                  ? g_strdup_printf("%u+ matches mile (sub-folders samet, sirf "
                                    "pehle %u dikhaye ja rahe hain)",
                                    ctx->results->len, SEARCH_MAX_RESULTS)
                  : g_strdup_printf("%u matches mile (sub-folders samet)",
                                    ctx->results->len);
  set_status(app, msg);
  g_free(msg);

  focus_first_item(app);

  search_scan_ctx_free(ctx);
  return G_SOURCE_REMOVE;
}

static gpointer search_thread_func(gpointer data) {
  SearchScanCtx *ctx = data;
  search_walk(ctx->root_dir, ctx, 0);
  g_idle_add(search_scan_insert_idle, ctx);
  return NULL;
}

static void start_recursive_search(AppWidgets *app, const char *needle_folded) {
  if (app->search_cancellable) {
    g_cancellable_cancel(app->search_cancellable);
    g_object_unref(app->search_cancellable);
    app->search_cancellable = NULL;
  }
  app->search_generation++;
  app->search_recursive_active = TRUE;

  app->populate_generation++;
  if (app->populate_cancellable) {
    g_cancellable_cancel(app->populate_cancellable);
    g_object_unref(app->populate_cancellable);
    app->populate_cancellable = NULL;
  }

  SearchScanCtx *ctx = g_new0(SearchScanCtx, 1);
  ctx->app = app;
  ctx->generation = app->search_generation;
  ctx->root_dir = g_strdup(app->current_dir);
  ctx->needle_folded = g_strdup(needle_folded);
  ctx->show_hidden = app->show_hidden;
  ctx->cancellable = g_cancellable_new();
  ctx->results = g_ptr_array_new_with_free_func(search_result_free);
  ctx->insert_index = 0;

  app->search_cancellable = g_object_ref(ctx->cancellable);

  set_status(app, "Is folder aur sub-folders mein search ho raha hai...");

  GThread *thread = g_thread_new("gtkfm-search", search_thread_func, ctx);
  g_thread_unref(thread);
}

void fileview_set_search(AppWidgets *app, const char *needle) {
  g_free(app->search_text);
  app->search_text = (needle && *needle) ? g_utf8_casefold(needle, -1) : NULL;

  gboolean is_computer_view = g_strcmp0(app->current_dir, COMPUTER_URI) == 0;

  if (app->search_text && !is_computer_view) {
    /* Sub-folders samet poori tree recursively dhoondhein. */
    start_recursive_search(app, app->search_text);
  } else if (app->search_recursive_active) {
    /* Search box khali ho gaya -- normal (sirf isi folder ki) listing
     * wapis load kar dein. */
    app->search_recursive_active = FALSE;
    app->search_generation++; /* koi purana pending result ab discard ho */
    if (app->search_cancellable) {
      g_cancellable_cancel(app->search_cancellable);
      g_object_unref(app->search_cancellable);
      app->search_cancellable = NULL;
    }
    populate(app);
  }

  gtk_tree_model_filter_refilter(app->file_filter);
}

static void fileview_refresh_themed_icons(AppWidgets *app) {
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->file_store), &iter))
    return;

  do {
    gboolean themed = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_THEMED_ICON,
                       &themed, -1);
    if (!themed)
      continue;

    gpointer gicon_ptr = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_GICON,
                       &gicon_ptr, -1);

    if (gicon_ptr) {
      /* "computer://" row -- device/drive icon, GIcon se dobara fetch. */
      GdkPixbuf *icon = icon_for_gicon_sized(G_ICON(gicon_ptr), app->icon_size);
      if (!icon)
        icon = icon_for_name_sized("drive-removable-media", app->icon_size);
      if (icon) {
        gtk_list_store_set(app->file_store, &iter, COL_PIXBUF, icon, -1);
        g_object_unref(icon);
      }
      continue;
    }

    gboolean is_dir = FALSE;
    gboolean is_symlink = FALSE;
    gboolean is_readonly = FALSE;
    char *path = NULL;
    char *ctype = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_IS_DIR,
                       &is_dir, COL_IS_SYMLINK, &is_symlink, COL_IS_READONLY,
                       &is_readonly, COL_PATH, &path, COL_TYPE, &ctype, -1);

    if (!path) {
      g_free(ctype);
      continue;
    }

    GdkPixbuf *icon = themed_icon_for(ctype, is_dir, path, app->icon_size);
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

    if (icon) {
      gtk_list_store_set(app->file_store, &iter, COL_PIXBUF, icon, -1);
      g_object_unref(icon);
    }

    g_free(path);
    g_free(ctype);
  } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->file_store), &iter));
}

static void fileview_refresh_raster_thumbnails(AppWidgets *app) {
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->file_store), &iter))
    return;

  do {
    gboolean themed = TRUE;
    gpointer gicon_ptr = NULL;
    GdkPixbuf *cur_pixbuf = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_THEMED_ICON,
                       &themed, COL_GICON, &gicon_ptr, COL_PIXBUF, &cur_pixbuf,
                       -1);

    if (themed || gicon_ptr) {
      if (cur_pixbuf)
        g_object_unref(cur_pixbuf);
      continue;
    }

    if (cur_pixbuf &&
        MAX(gdk_pixbuf_get_width(cur_pixbuf),
            gdk_pixbuf_get_height(cur_pixbuf)) == app->icon_size) {
      g_object_unref(cur_pixbuf);
      continue;
    }
    if (cur_pixbuf)
      g_object_unref(cur_pixbuf);

    gboolean is_dir = FALSE;
    gboolean is_symlink = FALSE;
    gboolean is_readonly = FALSE;
    char *path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_IS_DIR,
                       &is_dir, COL_IS_SYMLINK, &is_symlink, COL_IS_READONLY,
                       &is_readonly, COL_PATH, &path, -1);
    if (!path)
      continue;

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

    if (is_dir) {
      GdkPixbuf *base_icon = themed_icon_for(NULL, TRUE, path, app->icon_size);
      folder_preview_get_async(path, app->icon_size, base_icon,
                               thumbnail_ready_cb, tctx);
      if (base_icon)
        g_object_unref(base_icon);
    } else {
      thumbnail_get_async(path, app->icon_size, thumbnail_ready_cb, tctx);
    }

    g_free(path);
  } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(app->file_store), &iter));
}

void fileview_zoom_in(AppWidgets *app) {
  int new_size = app->icon_size + ICON_SIZE_STEP;
  if (new_size > ICON_SIZE_MAX)
    new_size = ICON_SIZE_MAX;
  if (new_size == app->icon_size)
    return;
  app->icon_size = new_size;
  gtk_icon_view_set_item_width(GTK_ICON_VIEW(app->icon_view),
                               app->icon_size + ITEM_WIDTH_PAD);
  fileview_refresh_themed_icons(app);
  fileview_refresh_raster_thumbnails(app);
  gtk_widget_queue_draw(app->icon_view);
  fileview_save_settings(app);
}

/* Ctrl+- -- icons ko chhota karta hai. */
void fileview_zoom_out(AppWidgets *app) {
  int new_size = app->icon_size - ICON_SIZE_STEP;
  if (new_size < ICON_SIZE_MIN)
    new_size = ICON_SIZE_MIN;
  if (new_size == app->icon_size)
    return;
  app->icon_size = new_size;
  gtk_icon_view_set_item_width(GTK_ICON_VIEW(app->icon_view),
                               app->icon_size + ITEM_WIDTH_PAD);
  fileview_refresh_themed_icons(app);
  fileview_refresh_raster_thumbnails(app);
  gtk_widget_queue_draw(app->icon_view);
  fileview_save_settings(app);
}

void fileview_toggle_hidden(AppWidgets *app) {
  app->show_hidden = !app->show_hidden;
  fileview_save_settings(app);
  fileview_refresh(app);
}

void fileview_set_list_view(AppWidgets *app, gboolean enabled) {
  app->list_view = enabled;
  GtkIconView *iv = GTK_ICON_VIEW(app->icon_view);

  gtk_icon_view_set_item_orientation(iv, enabled ? GTK_ORIENTATION_HORIZONTAL
                                                 : GTK_ORIENTATION_VERTICAL);
  gtk_icon_view_set_columns(iv, enabled ? 1 : -1);
  gtk_icon_view_set_item_width(iv,
                               enabled ? -1 : app->icon_size + ITEM_WIDTH_PAD);
  // gtk_icon_view_set_row_spacing(iv, enabled ? 2 : 4);
  // gtk_icon_view_set_item_padding(iv, enabled ? 4 : 6);
  gtk_icon_view_set_row_spacing(iv, enabled ? 2 : 4);
  gtk_icon_view_set_item_padding(iv, enabled ? 2 : 20);

  gtk_widget_queue_draw(app->icon_view);
}
