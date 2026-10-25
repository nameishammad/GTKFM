#include "iconchooser.h"
#include <string.h>
static GdkPixbuf *load_icon_safe(GtkIconTheme *theme, const char *name,
                                 int size);

enum {
  RESP_IMAGE = 1,
  RESP_RESET = 2,
  RESP_ALL_ICONS = 3,
};

enum {
  COL_PIXBUF,
  COL_NAME,
  COL_LABEL,
  N_COLS,
};

static const char *g_folder_icon_names[] = {
    "folder",
    "folder-open",
    "folder-documents",
    "folder-download",
    "folder-music",
    "folder-pictures",
    "folder-videos",
    "folder-desktop",
    "folder-publicshare",
    "folder-templates",
    "folder-remote",
    "folder-print",
    "folder-saved-search",
    "folder-tar",
    "folder-important",
    "folder-favorites",
    "folder-games",
    "folder-development",
    "folder-blue",
    "folder-red",
    "folder-green",
    "folder-yellow",
    "folder-orange",
    "folder-purple",
    "folder-grey",
    "folder-cyan",
    "folder-black",
    "folder-brown",
    "folder-pink",
    "folder-violet",
    "folder-magenta",
    "folder-teal",
};

static const char *g_app_icon_names[] = {
    "application-x-executable",
    "utilities-terminal",
    "system-run",
    "applications-accessories",
    "applications-development",
    "applications-education",
    "applications-games",
    "applications-graphics",
    "applications-internet",
    "applications-multimedia",
    "applications-office",
    "applications-other",
    "applications-science",
    "applications-system",
    "applications-utilities",
    "preferences-system",
    "preferences-desktop",
    "text-x-script",
    "package-x-generic",
    "image-x-generic",
    "audio-x-generic",
    "video-x-generic",
    "folder",
};

const char **icon_chooser_folder_icon_names(guint *n_out) {
  if (n_out)
    *n_out = G_N_ELEMENTS(g_folder_icon_names);
  return g_folder_icon_names;
}

const char **icon_chooser_app_icon_names(guint *n_out) {
  if (n_out)
    *n_out = G_N_ELEMENTS(g_app_icon_names);
  return g_app_icon_names;
}

char *icon_chooser_label_for(const char *name) {
  if (!name)
    return g_strdup("");

  if (g_strcmp0(name, "folder") == 0)
    return g_strdup("Default");
  if (g_strcmp0(name, "folder-open") == 0)
    return g_strdup("Open");
  if (g_strcmp0(name, "application-x-executable") == 0)
    return g_strdup("Default");

  const char *rest = name;
  if (g_str_has_prefix(rest, "applications-"))
    rest += strlen("applications-");
  else if (g_str_has_prefix(rest, "application-"))
    rest += strlen("application-");
  else if (g_str_has_prefix(rest, "folder-"))
    rest += strlen("folder-");

  char *spaced = g_strdup(rest);
  for (char *p = spaced; *p; p++)
    if (*p == '-')
      *p = ' ';

  if (spaced[0])
    spaced[0] = g_ascii_toupper(spaced[0]);

  return spaced;
}

static GdkPixbuf *load_icon_safe(GtkIconTheme *theme, const char *name,
                                 int size) {
  if (!theme || !name || !*name)
    return NULL;

  GError *err = NULL;

  GdkPixbuf *pb = gtk_icon_theme_load_icon(theme, name, size, 0, &err);

  if (err)
    g_error_free(err);

  return pb;
}

static void on_item_activated(GtkIconView *iv, GtkTreePath *path,
                              gpointer user_data) {
  (void)iv;
  (void)path;
  gtk_dialog_response(GTK_DIALOG(user_data), GTK_RESPONSE_OK);
}

static gboolean all_icons_filter_visible(GtkTreeModel *model, GtkTreeIter *iter,
                                         gpointer user_data) {
  GtkEntry *search_entry = user_data;
  const char *needle = gtk_entry_get_text(search_entry);
  if (!needle || !*needle)
    return TRUE;

  char *name = NULL;
  char *label = NULL;
  gtk_tree_model_get(model, iter, COL_NAME, &name, COL_LABEL, &label, -1);

  gboolean visible = FALSE;
  if (name || label) {
    char *needle_folded = g_utf8_casefold(needle, -1);
    if (name) {
      char *name_folded = g_utf8_casefold(name, -1);
      if (strstr(name_folded, needle_folded))
        visible = TRUE;
      g_free(name_folded);
    }
    if (!visible && label) {
      char *label_folded = g_utf8_casefold(label, -1);
      if (strstr(label_folded, needle_folded))
        visible = TRUE;
      g_free(label_folded);
    }
    g_free(needle_folded);
  }

  g_free(name);
  g_free(label);
  return visible;
}

static void on_all_icons_search_changed(GtkSearchEntry *entry,
                                        gpointer user_data) {
  (void)entry;
  gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(user_data));
}

typedef struct {
  GtkListStore *store;
  GtkIconTheme *theme;
  GList *all_names_owner;
  GList *pending;
  const char *last_name;
  char *current_icon;
  GtkTreeIter select_iter;
  gboolean have_select_iter;
  guint total;
  guint done;
  GtkWidget *icon_view;
  GtkTreeModel *filter;
  GtkWidget *loading_box;
  GtkWidget *progress_label;
  gboolean cancelled;
  guint idle_id;
} AllIconsLoadCtx;

static void all_icons_ctx_free(gpointer data) {
  AllIconsLoadCtx *ctx = data;

  if (!ctx)
    return;

  if (ctx->idle_id)
    g_source_remove(ctx->idle_id);

  if (ctx->filter) {
    g_object_unref(ctx->filter);
    ctx->filter = NULL;
  }

  if (ctx->all_names_owner)
    g_list_free_full(ctx->all_names_owner, g_free);

  g_free(ctx->current_icon);

  g_free(ctx);
}

static void on_all_icons_dialog_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;

  AllIconsLoadCtx *ctx = user_data;

  if (!ctx)
    return;

  ctx->cancelled = TRUE;

  if (ctx->idle_id) {
    g_source_remove(ctx->idle_id);
    ctx->idle_id = 0;
  }
}

static gboolean all_icons_load_idle(gpointer user_data) {
  AllIconsLoadCtx *ctx = user_data;

  if (ctx->cancelled) {
    if (ctx->all_names_owner) {
      g_list_free_full(ctx->all_names_owner, g_free);
      ctx->all_names_owner = NULL;
      ctx->pending = NULL;
    }
    return FALSE;
  }

  gint64 deadline = g_get_monotonic_time() + 8000;
  // gint64 deadline = g_get_monotonic_time() + 50000;

  while (ctx->pending && g_get_monotonic_time() < deadline) {
    GList *node = ctx->pending;
    ctx->pending = node->next;
    const char *name = node->data;

    if (!(ctx->last_name && g_strcmp0(ctx->last_name, name) == 0)) {
      ctx->last_name = name;
      GdkPixbuf *pb = load_icon_safe(ctx->theme, name, 64);
      if (pb) {
        char *label = icon_chooser_label_for(name);
        GtkTreeIter iter;
        gtk_list_store_append(ctx->store, &iter);
        gtk_list_store_set(ctx->store, &iter, COL_PIXBUF, pb, COL_NAME, name,
                           COL_LABEL, label, -1);
        g_object_unref(pb);
        g_free(label);

        if (!ctx->have_select_iter && ctx->current_icon &&
            g_strcmp0(ctx->current_icon, name) == 0) {
          ctx->select_iter = iter;
          ctx->have_select_iter = TRUE;
        }
      }
    }
    ctx->done++;
  }

  if (ctx->progress_label) {
    char *text = g_strdup_printf("Icons load ho rahe hain... (%u/%u)",
                                 ctx->done, ctx->total);
    gtk_label_set_text(GTK_LABEL(ctx->progress_label), text);
    g_free(text);
  }

  if (ctx->pending)
    return TRUE;

  ctx->idle_id = 0;

  g_list_free_full(ctx->all_names_owner, g_free);
  ctx->all_names_owner = NULL;

  if (ctx->loading_box)
    gtk_widget_hide(ctx->loading_box);

  if (ctx->have_select_iter && ctx->filter) {
    GtkTreeIter filter_iter;
    if (gtk_tree_model_filter_convert_child_iter_to_iter(
            GTK_TREE_MODEL_FILTER(ctx->filter), &filter_iter,
            &ctx->select_iter)) {
      GtkTreePath *tpath = gtk_tree_model_get_path(ctx->filter, &filter_iter);
      gtk_icon_view_select_path(GTK_ICON_VIEW(ctx->icon_view), tpath);
      gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(ctx->icon_view), tpath, FALSE,
                                   0, 0);
      gtk_tree_path_free(tpath);
    }
  }

  return FALSE;
}

static gboolean on_dialog_key_press(GtkWidget *widget, GdkEventKey *event,
                                    gpointer data) {
  (void)data;

  if (event->keyval == GDK_KEY_Escape) {
    gtk_dialog_response(GTK_DIALOG(widget), GTK_RESPONSE_CANCEL);
    return TRUE;
  }

  return FALSE;
}

static char *show_all_icons_dialog(GtkWindow *parent,
                                   const char *current_icon) {
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Sab Icons", parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_OK, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 480);
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(on_dialog_key_press),
                   NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 8);
  gtk_box_set_spacing(GTK_BOX(content), 8);

  GtkWidget *search_entry = gtk_search_entry_new();
  gtk_box_pack_start(GTK_BOX(content), search_entry, FALSE, FALSE, 0);

  GtkWidget *loading_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *spinner = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(spinner));
  GtkWidget *progress_label = gtk_label_new("Icons load ho rahe hain...");
  gtk_box_pack_start(GTK_BOX(loading_box), spinner, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(loading_box), progress_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), loading_box, FALSE, FALSE, 0);

  GtkListStore *store =
      gtk_list_store_new(N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING);
  GtkIconTheme *theme = gtk_icon_theme_get_default();

  GList *all_names = gtk_icon_theme_list_icons(theme, NULL);
  all_names = g_list_sort(all_names, (GCompareFunc)g_strcmp0);

  GtkTreeModel *filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL);

  gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter),
                                         all_icons_filter_visible, search_entry,
                                         NULL);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);

  gtk_widget_set_vexpand(scroll, TRUE);

  GtkWidget *icon_view = gtk_icon_view_new_with_model(filter);

  gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(icon_view), COL_PIXBUF);
  gtk_icon_view_set_text_column(GTK_ICON_VIEW(icon_view), COL_LABEL);
  // gtk_icon_view_set_item_width(GTK_ICON_VIEW(icon_view), 72);
  gtk_icon_view_set_item_width(GTK_ICON_VIEW(icon_view), 96);
  gtk_icon_view_set_columns(GTK_ICON_VIEW(icon_view), -1);
  gtk_icon_view_set_margin(GTK_ICON_VIEW(icon_view), 4);
  gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(icon_view),
                                   GTK_SELECTION_SINGLE);
  // g_object_unref(filter);

  g_signal_connect(icon_view, "item-activated", G_CALLBACK(on_item_activated),
                   dialog);
  g_signal_connect(search_entry, "search-changed",
                   G_CALLBACK(on_all_icons_search_changed), filter);

  gtk_container_add(GTK_CONTAINER(scroll), icon_view);
  gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

  AllIconsLoadCtx *ctx = g_new0(AllIconsLoadCtx, 1);
  ctx->store = store;
  ctx->theme = theme;
  ctx->all_names_owner = all_names;
  ctx->pending = all_names;
  ctx->current_icon = g_strdup(current_icon);
  ctx->total = g_list_length(all_names);
  ctx->icon_view = icon_view;
  ctx->filter = filter;
  ctx->loading_box = loading_box;
  ctx->progress_label = progress_label;

  g_object_set_data_full(G_OBJECT(dialog), "all-icons-ctx", ctx,
                         all_icons_ctx_free);
  g_signal_connect(dialog, "destroy", G_CALLBACK(on_all_icons_dialog_destroy),
                   ctx);
  // g_idle_add(all_icons_load_idle, ctx);
  ctx->idle_id = g_idle_add(all_icons_load_idle, ctx);

  gtk_widget_show_all(dialog);

  gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  char *picked_name = NULL;

  if (response == GTK_RESPONSE_OK) {
    GList *selected =
        gtk_icon_view_get_selected_items(GTK_ICON_VIEW(icon_view));
    if (selected) {
      GtkTreeIter iter;
      if (gtk_tree_model_get_iter(filter, &iter, selected->data))
        gtk_tree_model_get(filter, &iter, COL_NAME, &picked_name, -1);
      g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    }
  }

  gtk_widget_destroy(dialog);
  return picked_name;
}

static void on_image_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GtkWidget *dialog = user_data;

  GtkWidget *chooser = gtk_file_chooser_dialog_new(
      "Image Chunein", GTK_WINDOW(dialog), GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Image Files");
  gtk_file_filter_add_mime_type(filter, "image/*");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

  gint resp = gtk_dialog_run(GTK_DIALOG(chooser));
  if (resp == GTK_RESPONSE_ACCEPT) {
    char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    if (path) {
      g_object_set_data_full(G_OBJECT(dialog), "icon-chooser-image-path", path,
                             g_free);
      gtk_dialog_response(GTK_DIALOG(dialog), RESP_IMAGE);
      return;
    }
    return;
  }

  gtk_widget_destroy(chooser);
}

static void on_reset_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  gtk_dialog_response(GTK_DIALOG(user_data), RESP_RESET);
}

static void on_all_icons_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  gtk_dialog_response(GTK_DIALOG(user_data), RESP_ALL_ICONS);
}

IconChooserResult icon_chooser_run(GtkWindow *parent, const char *title,
                                   const char **candidate_names,
                                   guint n_candidates, const char *current_icon,
                                   gboolean show_reset, char **out_value) {
  if (out_value)
    *out_value = NULL;

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      title ? title : "Icon Chunein", parent, GTK_DIALOG_MODAL, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_OK, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 460, 380);
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(on_dialog_key_press),
                   NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 8);
  gtk_box_set_spacing(GTK_BOX(content), 8);

  GtkListStore *store =
      gtk_list_store_new(N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING);
  GtkIconTheme *theme = gtk_icon_theme_get_default();

  GtkTreeIter select_iter;
  gboolean have_select_iter = FALSE;

  for (guint i = 0; i < n_candidates && candidate_names; i++) {
    const char *name = candidate_names[i];
    // GdkPixbuf *pb = load_icon_safe(theme, name, 48);
    GdkPixbuf *pb = load_icon_safe(theme, name, 64);
    if (!pb)
      continue;

    char *label = icon_chooser_label_for(name);
    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, COL_PIXBUF, pb, COL_NAME, name, COL_LABEL,
                       label, -1);
    g_object_unref(pb);
    g_free(label);

    if (!have_select_iter && current_icon &&
        g_strcmp0(current_icon, name) == 0) {
      select_iter = iter;
      have_select_iter = TRUE;
    }
  }

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);

  GtkWidget *icon_view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(store));
  gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(icon_view), COL_PIXBUF);
  gtk_icon_view_set_text_column(GTK_ICON_VIEW(icon_view), COL_LABEL);
  gtk_icon_view_set_item_width(GTK_ICON_VIEW(icon_view), 72);
  gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(icon_view),
                                   GTK_SELECTION_SINGLE);

  if (have_select_iter) {
    GtkTreePath *tpath =
        gtk_tree_model_get_path(GTK_TREE_MODEL(store), &select_iter);
    gtk_icon_view_select_path(GTK_ICON_VIEW(icon_view), tpath);
    gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(icon_view), tpath, FALSE, 0, 0);
    gtk_tree_path_free(tpath);
  }

  g_signal_connect(icon_view, "item-activated", G_CALLBACK(on_item_activated),
                   dialog);

  gtk_container_add(GTK_CONTAINER(scroll), icon_view);
  gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

  GtkWidget *bottom_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *image_btn = gtk_button_new_with_label("Image...");
  gtk_box_pack_start(GTK_BOX(bottom_box), image_btn, FALSE, FALSE, 0);
  GtkWidget *all_btn = gtk_button_new_with_label("All Icons...");
  gtk_box_pack_start(GTK_BOX(bottom_box), all_btn, FALSE, FALSE, 0);

  GtkWidget *reset_btn = NULL;
  if (show_reset) {
    reset_btn = gtk_button_new_with_label("Reset to Default");
    gtk_box_pack_end(GTK_BOX(bottom_box), reset_btn, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(content), bottom_box, FALSE, FALSE, 0);

  g_signal_connect(image_btn, "clicked", G_CALLBACK(on_image_clicked), dialog);
  g_signal_connect(all_btn, "clicked", G_CALLBACK(on_all_icons_clicked),
                   dialog);
  if (reset_btn)
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(on_reset_clicked),
                     dialog);

  gtk_widget_show_all(dialog);

  gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  IconChooserResult result = ICON_CHOOSER_CANCEL;

  if (response == GTK_RESPONSE_OK) {
    GList *selected =
        gtk_icon_view_get_selected_items(GTK_ICON_VIEW(icon_view));
    if (selected) {
      GtkTreeIter iter;
      if (gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter,
                                  selected->data)) {
        char *icon_name = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, COL_NAME, &icon_name,
                           -1);
        if (icon_name) {
          result = ICON_CHOOSER_NAME;
          if (out_value)
            *out_value = icon_name;
          else
            g_free(icon_name);
        }
      }
      g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    }
  } else if (response == RESP_IMAGE) {
    const char *chosen =
        g_object_get_data(G_OBJECT(dialog), "icon-chooser-image-path");
    if (chosen) {
      result = ICON_CHOOSER_IMAGE;
      if (out_value)
        *out_value = g_strdup(chosen);
    }
  } else if (response == RESP_RESET) {
    result = ICON_CHOOSER_RESET;
  } else if (response == RESP_ALL_ICONS) {
    char *picked = show_all_icons_dialog(GTK_WINDOW(dialog), current_icon);
    if (picked) {
      result = ICON_CHOOSER_NAME;
      if (out_value)
        *out_value = picked;
      else
        g_free(picked);
    }
  }

  gtk_widget_destroy(dialog);
  return result;
}
