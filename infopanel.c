#include "infopanel.h"
#include "fileview.h"
#include "thumbnail.h"
#include <gio/gio.h>
#include <string.h>

#define INFOPANEL_WIDTH 200
#define INFOPANEL_ICON_SIZE 128
#define INFOPANEL_NAME_WRAP_CHARS 20

typedef struct {
  gint refcount;
  gboolean alive;
  void *ip;
} PanelToken;

typedef struct {
  AppWidgets *app;
  GtkWidget *preview_image;
  GtkWidget *name_label;
  GtkWidget *type_row, *type_value;
  GtkWidget *size_row, *size_value;
  GtkWidget *modified_row, *modified_value;
  GtkWidget *location_row, *location_value;
  char *shown_path;
  guint generation;
  PanelToken *token;
} InfoPanel;

typedef struct {
  PanelToken *token;
  guint generation;
} ThumbReq;

/* ---- token helpers -------------------------------------------------- */

static PanelToken *panel_token_ref(PanelToken *t) {
  t->refcount++;
  return t;
}

static void panel_token_unref(PanelToken *t) {
  if (--t->refcount <= 0)
    g_free(t);
}

/* ---- icon helpers (sidebar.c jaisa hi pattern) ----------------------- */

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

static GdkPixbuf *icon_for_content_type(const char *ctype, int size) {
  if (!ctype || !*ctype)
    return NULL;
  GIcon *gicon = g_content_type_get_icon(ctype);
  GdkPixbuf *pb = icon_for_gicon(gicon, size);
  if (gicon)
    g_object_unref(gicon);
  return pb;
}

static char *wrap_hard_every_n_chars(const char *text, glong n) {
  if (!text || n <= 0)
    return g_strdup(text ? text : "");

  glong len = g_utf8_strlen(text, -1);
  GString *out = g_string_new(NULL);
  const char *p = text;

  for (glong i = 0; i < len; i += n) {
    glong chunk = MIN(n, len - i);
    char *end = g_utf8_offset_to_pointer(p, chunk);
    if (out->len > 0)
      g_string_append_c(out, '\n');
    g_string_append_len(out, p, end - p);
    p = end;
  }

  return g_string_free(out, FALSE);
}

static void infopanel_set_name_text(InfoPanel *ip, const char *text) {
  char *wrapped =
      wrap_hard_every_n_chars(text ? text : "", INFOPANEL_NAME_WRAP_CHARS);
  gtk_label_set_text(GTK_LABEL(ip->name_label), wrapped);
  g_free(wrapped);
}

/* ---- widget building -------------------------------------------------*/

static GtkWidget *make_field(const char *caption, GtkWidget **value_out) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_bottom(box, 10);

  GtkWidget *cap = gtk_label_new(caption);
  gtk_label_set_xalign(GTK_LABEL(cap), 0.0);
  gtk_style_context_add_class(gtk_widget_get_style_context(cap), "dim-label");
  PangoAttrList *cattrs = pango_attr_list_new();
  pango_attr_list_insert(cattrs, pango_attr_scale_new(PANGO_SCALE_SMALL));
  gtk_label_set_attributes(GTK_LABEL(cap), cattrs);
  pango_attr_list_unref(cattrs);
  gtk_box_pack_start(GTK_BOX(box), cap, FALSE, FALSE, 0);

  GtkWidget *val = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(val), 0.0);
  gtk_label_set_line_wrap(GTK_LABEL(val), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(val), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_width_chars(GTK_LABEL(val), 1);
  gtk_label_set_max_width_chars(GTK_LABEL(val), 1);
  gtk_label_set_selectable(GTK_LABEL(val), TRUE);
  gtk_box_pack_start(GTK_BOX(box), val, FALSE, FALSE, 0);

  *value_out = val;
  return box;
}

static void infopanel_free(gpointer data) {
  InfoPanel *ip = data;
  ip->token->alive = FALSE;
  ip->token->ip = NULL;
  panel_token_unref(ip->token);
  g_free(ip->shown_path);
  g_free(ip);
}

GtkWidget *infopanel_create(AppWidgets *app) {
  InfoPanel *ip = g_new0(InfoPanel, 1);
  ip->app = app;
  ip->token = g_new0(PanelToken, 1);
  ip->token->refcount = 1;
  ip->token->alive = TRUE;
  ip->token->ip = ip;

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(root, INFOPANEL_WIDTH, -1);
  gtk_style_context_add_class(gtk_widget_get_style_context(root), "sidebar");

  GtkWidget *header = gtk_label_new("Information");
  gtk_widget_set_halign(header, GTK_ALIGN_START);
  gtk_widget_set_margin_start(header, 10);
  gtk_widget_set_margin_top(header, 8);
  gtk_widget_set_margin_bottom(header, 6);
  PangoAttrList *hattrs = pango_attr_list_new();
  pango_attr_list_insert(hattrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  gtk_label_set_attributes(GTK_LABEL(header), hattrs);
  pango_attr_list_unref(hattrs);
  gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 0);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(root), scrolled, TRUE, TRUE, 0);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 10);
  gtk_widget_set_margin_bottom(content, 10);
  gtk_container_add(GTK_CONTAINER(scrolled), content);

  ip->preview_image = gtk_image_new();
  gtk_widget_set_halign(ip->preview_image, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom(ip->preview_image, 10);
  gtk_widget_set_size_request(ip->preview_image, -1, INFOPANEL_ICON_SIZE);
  gtk_box_pack_start(GTK_BOX(content), ip->preview_image, FALSE, FALSE, 0);

  ip->name_label = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(ip->name_label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(ip->name_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_width_chars(GTK_LABEL(ip->name_label), 1);
  gtk_label_set_max_width_chars(GTK_LABEL(ip->name_label),
                                INFOPANEL_NAME_WRAP_CHARS);
  gtk_label_set_justify(GTK_LABEL(ip->name_label), GTK_JUSTIFY_CENTER);
  gtk_label_set_selectable(GTK_LABEL(ip->name_label), TRUE);
  PangoAttrList *nattrs = pango_attr_list_new();
  pango_attr_list_insert(nattrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  gtk_label_set_attributes(GTK_LABEL(ip->name_label), nattrs);
  pango_attr_list_unref(nattrs);
  gtk_widget_set_margin_bottom(ip->name_label, 14);
  gtk_box_pack_start(GTK_BOX(content), ip->name_label, FALSE, FALSE, 0);

  ip->type_row = make_field("Type", &ip->type_value);
  gtk_box_pack_start(GTK_BOX(content), ip->type_row, FALSE, FALSE, 0);

  ip->size_row = make_field("Size", &ip->size_value);
  gtk_box_pack_start(GTK_BOX(content), ip->size_row, FALSE, FALSE, 0);

  ip->modified_row = make_field("Modified", &ip->modified_value);
  gtk_box_pack_start(GTK_BOX(content), ip->modified_row, FALSE, FALSE, 0);

  ip->location_row = make_field("Location", &ip->location_value);
  gtk_box_pack_start(GTK_BOX(content), ip->location_row, FALSE, FALSE, 0);

  g_object_set_data_full(G_OBJECT(root), "infopanel", ip, infopanel_free);

  gtk_widget_show_all(root);
  gtk_widget_set_no_show_all(root, TRUE);
  gtk_widget_hide(root);

  return root;
}

static void on_thumb_ready(const char *filepath, GdkPixbuf *pixbuf,
                           gpointer user_data) {
  ThumbReq *req = user_data;

  if (pixbuf) {
    if (req->token->alive) {
      InfoPanel *ip = req->token->ip;
      if (ip->generation == req->generation && ip->shown_path &&
          g_strcmp0(ip->shown_path, filepath) == 0)
        gtk_image_set_from_pixbuf(GTK_IMAGE(ip->preview_image), pixbuf);
    }
    g_object_unref(pixbuf);
  }

  panel_token_unref(req->token);
  g_free(req);
}

/* ---- content builders ------------------------------------------------ */

/* Kuch select nahi hai -- current folder ki info dikhate hain. */
static void infopanel_show_folder(InfoPanel *ip, AppWidgets *app) {
  if (!app->current_dir) {
    gtk_image_clear(GTK_IMAGE(ip->preview_image));
    infopanel_set_name_text(ip, "");
    gtk_label_set_text(GTK_LABEL(ip->type_value), "");
    gtk_label_set_text(GTK_LABEL(ip->size_value), "");
    gtk_label_set_text(GTK_LABEL(ip->modified_value), "");
    gtk_label_set_text(GTK_LABEL(ip->location_value), "");
    return;
  }

  char *base = g_path_get_basename(app->current_dir);
  if (g_strcmp0(base, "/") == 0 || g_strcmp0(base, ".") == 0) {
    g_free(base);
    base = g_strdup("Filesystem");
  }
  infopanel_set_name_text(ip, base);
  g_free(base);

  g_free(ip->shown_path);
  ip->shown_path = g_strdup(app->current_dir);

  GdkPixbuf *pb =
      themed_icon_for(NULL, TRUE, app->current_dir, INFOPANEL_ICON_SIZE);
  gtk_image_set_from_pixbuf(GTK_IMAGE(ip->preview_image), pb);
  if (pb && app->folder_previews) {
    ThumbReq *req = g_new0(ThumbReq, 1);
    req->token = panel_token_ref(ip->token);
    req->generation = ip->generation;
    folder_preview_get_async(app->current_dir, INFOPANEL_ICON_SIZE, pb,
                             on_thumb_ready, req);
    g_object_unref(pb);
  } else if (pb) {
    g_object_unref(pb);
  }

  gtk_label_set_text(GTK_LABEL(ip->type_value), "Folder");

  guint n = app->file_filter ? (guint)gtk_tree_model_iter_n_children(
                                   GTK_TREE_MODEL(app->file_filter), NULL)
                             : 0;
  char *size_text = g_strdup_printf("%u item%s", n, n == 1 ? "" : "s");
  gtk_label_set_text(GTK_LABEL(ip->size_value), size_text);
  g_free(size_text);

  GFile *f = g_file_new_for_path(app->current_dir);
  GFileInfo *info = g_file_query_info(f, "time::modified",
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (info) {
    GDateTime *dt = g_file_info_get_modification_date_time(info);
    char *mtxt = dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M") : g_strdup("--");
    gtk_label_set_text(GTK_LABEL(ip->modified_value), mtxt);
    g_free(mtxt);
    if (dt)
      g_date_time_unref(dt);
    g_object_unref(info);
  } else {
    gtk_label_set_text(GTK_LABEL(ip->modified_value), "--");
  }
  g_object_unref(f);

  char *parent = g_path_get_dirname(app->current_dir);
  gtk_label_set_text(GTK_LABEL(ip->location_value), parent);
  g_free(parent);
}

/* Bilkul ek item selected hai. */
static void infopanel_show_single(InfoPanel *ip, AppWidgets *app,
                                  GtkTreeIter *iter) {
  char *name = NULL, *path = NULL, *ctype = NULL;
  gboolean is_dir = FALSE;
  guint64 size = 0;
  GDateTime *dt = NULL;

  gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), iter, COL_NAME, &name,
                     COL_PATH, &path, COL_IS_DIR, &is_dir, COL_SIZE, &size,
                     COL_TYPE, &ctype, COL_DATE, &dt, -1);

  infopanel_set_name_text(ip, name ? name : "");

  g_free(ip->shown_path);
  ip->shown_path = g_strdup(path);

  if (is_dir) {
    GdkPixbuf *pb = themed_icon_for(NULL, TRUE, path, INFOPANEL_ICON_SIZE);
    gtk_image_set_from_pixbuf(GTK_IMAGE(ip->preview_image), pb);

    if (pb && app->folder_previews) {
      ThumbReq *req = g_new0(ThumbReq, 1);
      req->token = panel_token_ref(ip->token);
      req->generation = ip->generation;
      folder_preview_get_async(path, INFOPANEL_ICON_SIZE, pb, on_thumb_ready,
                               req);
      g_object_unref(pb);
    } else if (pb) {
      g_object_unref(pb);
    }

    gtk_label_set_text(GTK_LABEL(ip->type_value), "Folder");

    guint count = 0;
    GFile *f = g_file_new_for_path(path);
    GFileEnumerator *en = g_file_enumerate_children(
        f, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (en) {
      GFileInfo *ci;
      while ((ci = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        count++;
        g_object_unref(ci);
      }
      g_file_enumerator_close(en, NULL, NULL);
      g_object_unref(en);
    }
    g_object_unref(f);
    char *size_text =
        g_strdup_printf("%u item%s", count, count == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(ip->size_value), size_text);
    g_free(size_text);
  } else {
    char *type_desc = (ctype && *ctype) ? g_content_type_get_description(ctype)
                                        : g_strdup("Unknown");
    gtk_label_set_text(GTK_LABEL(ip->type_value), type_desc);
    g_free(type_desc);

    char *size_text = g_format_size_full(size, G_FORMAT_SIZE_IEC_UNITS);
    gtk_label_set_text(GTK_LABEL(ip->size_value), size_text);
    g_free(size_text);

    GdkPixbuf *pb = icon_for_content_type(ctype, INFOPANEL_ICON_SIZE);
    if (!pb)
      pb = icon_for_name("text-x-generic", INFOPANEL_ICON_SIZE);
    gtk_image_set_from_pixbuf(GTK_IMAGE(ip->preview_image), pb);
    if (pb)
      g_object_unref(pb);

    /* Behtar preview (image/video/pdf thumbnail) mile to upar wala
     * generic icon async replace ho jayega. */
    ThumbReq *req = g_new0(ThumbReq, 1);
    req->token = panel_token_ref(ip->token);
    req->generation = ip->generation;
    thumbnail_get_async(path, INFOPANEL_ICON_SIZE, on_thumb_ready, req);
  }

  if (dt) {
    char *mtxt = g_date_time_format(dt, "%Y-%m-%d %H:%M");
    gtk_label_set_text(GTK_LABEL(ip->modified_value), mtxt);
    g_free(mtxt);
    g_date_time_unref(dt);
  } else {
    gtk_label_set_text(GTK_LABEL(ip->modified_value), "--");
  }

  char *parent = g_path_get_dirname(path);
  gtk_label_set_text(GTK_LABEL(ip->location_value), parent);
  g_free(parent);

  g_free(name);
  g_free(path);
  g_free(ctype);
}

/* Ek se zyada item selected hain -- summary dikhate hain. */
static void infopanel_show_multi(InfoPanel *ip, AppWidgets *app,
                                 GList *selected) {
  gtk_image_clear(GTK_IMAGE(ip->preview_image));

  g_free(ip->shown_path);
  ip->shown_path = NULL;

  guint n = 0, n_files = 0, n_dirs = 0;
  guint64 total_bytes = 0;

  for (GList *l = selected; l; l = l->next) {
    GtkTreePath *tpath = l->data;
    GtkTreeIter filter_iter, iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                                 tpath))
      continue;
    gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter, &iter,
                                                     &filter_iter);
    gboolean is_dir = FALSE;
    guint64 size = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_IS_DIR,
                       &is_dir, COL_SIZE, &size, -1);
    if (is_dir) {
      n_dirs++;
    } else {
      n_files++;
      total_bytes += size;
    }
    n++;
  }

  char *name_text = g_strdup_printf("%u items selected", n);
  infopanel_set_name_text(ip, name_text);
  g_free(name_text);

  if (n_files && n_dirs)
    gtk_label_set_text(GTK_LABEL(ip->type_value), "Files and folders");
  else if (n_dirs)
    gtk_label_set_text(GTK_LABEL(ip->type_value), "Folders");
  else
    gtk_label_set_text(GTK_LABEL(ip->type_value), "Files");

  char *size_text = g_format_size_full(total_bytes, G_FORMAT_SIZE_IEC_UNITS);
  gtk_label_set_text(GTK_LABEL(ip->size_value), size_text);
  g_free(size_text);

  gtk_label_set_text(GTK_LABEL(ip->modified_value), "--");

  gtk_label_set_text(GTK_LABEL(ip->location_value),
                     app->current_dir ? app->current_dir : "");
}

/* Mounted drive (internal ya external, USB/removable sab) ke liye
 * total/used/free space GIO se poochte hain. Ye stat-jaisi query hai,
 * poori disk scan nahi karti, isliye synchronously turant mil jaati hai. */
static char *infopanel_drive_size_text(const char *path) {
  if (!path || !*path)
    return g_strdup("Mounted");

  GFile *f = g_file_new_for_path(path);
  GError *err = NULL;
  GFileInfo *info = g_file_query_filesystem_info(
      f, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
      NULL, &err);
  g_object_unref(f);

  if (!info) {
    if (err)
      g_error_free(err);
    return g_strdup("Mounted");
  }

  gboolean has_total =
      g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
  gboolean has_free =
      g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);

  char *result;
  if (has_total && has_free) {
    guint64 total_bytes = g_file_info_get_attribute_uint64(
        info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    guint64 free_bytes = g_file_info_get_attribute_uint64(
        info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    guint64 used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;

    char *used_str = g_format_size_full(used_bytes, G_FORMAT_SIZE_IEC_UNITS);
    char *total_str = g_format_size_full(total_bytes, G_FORMAT_SIZE_IEC_UNITS);
    result = g_strdup_printf("%s used of %s", used_str, total_str);
    g_free(used_str);
    g_free(total_str);
  } else {
    result = g_strdup("Mounted");
  }

  g_object_unref(info);
  return result;
}

static void infopanel_show_computer_item(InfoPanel *ip, AppWidgets *app,
                                         GtkTreeIter *iter) {
  char *name = NULL, *path = NULL;
  gboolean mounted = FALSE;
  gpointer gicon_ptr = NULL;
  gpointer volume_ptr = NULL;

  gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), iter, COL_NAME, &name,
                     COL_PATH, &path, COL_IS_DIR, &mounted, COL_GICON,
                     &gicon_ptr, COL_VOLUME, &volume_ptr, -1);

  infopanel_set_name_text(ip, name ? name : "");

  g_free(ip->shown_path);
  ip->shown_path = (path && *path) ? g_strdup(path) : NULL;

  GdkPixbuf *pb =
      gicon_ptr ? icon_for_gicon(G_ICON(gicon_ptr), INFOPANEL_ICON_SIZE) : NULL;
  if (!pb)
    pb = icon_for_name("drive-harddisk", INFOPANEL_ICON_SIZE);
  gtk_image_set_from_pixbuf(GTK_IMAGE(ip->preview_image), pb);
  if (pb)
    g_object_unref(pb);

  gtk_label_set_text(GTK_LABEL(ip->type_value),
                     volume_ptr ? "Removable drive" : "Filesystem");

  if (mounted && path && *path) {
    char *size_text = infopanel_drive_size_text(path);
    gtk_label_set_text(GTK_LABEL(ip->size_value), size_text);
    g_free(size_text);
  } else {
    gtk_label_set_text(GTK_LABEL(ip->size_value), "Not mounted");
  }

  gtk_label_set_text(GTK_LABEL(ip->modified_value), "--");
  gtk_label_set_text(GTK_LABEL(ip->location_value),
                     (path && *path) ? path : "--");

  g_free(name);
  g_free(path);
}

static void infopanel_show_computer_root(InfoPanel *ip, AppWidgets *app) {
  GdkPixbuf *pb = icon_for_name("computer", INFOPANEL_ICON_SIZE);
  gtk_image_set_from_pixbuf(GTK_IMAGE(ip->preview_image), pb);
  if (pb)
    g_object_unref(pb);

  infopanel_set_name_text(ip, "Computer");
  gtk_label_set_text(GTK_LABEL(ip->type_value), "Location");

  guint n = app->file_filter ? (guint)gtk_tree_model_iter_n_children(
                                   GTK_TREE_MODEL(app->file_filter), NULL)
                             : 0;
  char *size_text = g_strdup_printf("%u drive%s", n, n == 1 ? "" : "s");
  gtk_label_set_text(GTK_LABEL(ip->size_value), size_text);
  g_free(size_text);

  gtk_label_set_text(GTK_LABEL(ip->modified_value), "--");
  gtk_label_set_text(GTK_LABEL(ip->location_value), "--");

  g_free(ip->shown_path);
  ip->shown_path = NULL;
}

void infopanel_refresh(AppWidgets *app) {
  if (!app || !app->info_panel || !app->info_panel_visible)
    return;

  InfoPanel *ip = g_object_get_data(G_OBJECT(app->info_panel), "infopanel");
  if (!ip)
    return;

  ip->generation++;

  gboolean is_computer = g_strcmp0(app->current_dir, COMPUTER_URI) == 0;

  GList *selected =
      app->icon_view
          ? gtk_icon_view_get_selected_items(GTK_ICON_VIEW(app->icon_view))
          : NULL;
  guint n_selected = g_list_length(selected);

  if (is_computer) {
    if (n_selected == 1) {
      GtkTreeIter filter_iter, iter;
      if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter),
                                  &filter_iter, selected->data)) {
        gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter,
                                                         &iter, &filter_iter);
        infopanel_show_computer_item(ip, app, &iter);
      }
    } else if (n_selected > 1) {
      infopanel_show_multi(ip, app, selected);
    } else {
      infopanel_show_computer_root(ip, app);
    }
    g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    return;
  }

  if (n_selected == 0) {
    infopanel_show_folder(ip, app);
  } else if (n_selected == 1) {
    GtkTreeIter filter_iter, iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                                selected->data)) {
      gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter, &iter,
                                                       &filter_iter);
      infopanel_show_single(ip, app, &iter);
    }
  } else {
    infopanel_show_multi(ip, app, selected);
  }

  g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
}

void infopanel_toggle(AppWidgets *app) {
  if (!app || !app->info_panel)
    return;

  app->info_panel_visible = !app->info_panel_visible;
  fileview_save_settings(app);

  if (app->info_panel_visible) {
    gtk_widget_show(app->info_panel);

    if (app->content_paned) {
      GtkAllocation alloc;
      gtk_widget_get_allocation(app->content_paned, &alloc);
      if (app->content_paned_position > 0 &&
          app->content_paned_position < alloc.width) {
        /* User pehle jitni width par info panel chhod chuka tha,
         * wahi wapas laga dete hain (settings file se load hui). */
        gtk_paned_set_position(GTK_PANED(app->content_paned),
                               app->content_paned_position);
      } else if (alloc.width > INFOPANEL_WIDTH + 100) {
        gtk_paned_set_position(GTK_PANED(app->content_paned),
                               alloc.width - INFOPANEL_WIDTH - 8);
      }
    }
    infopanel_refresh(app);
  } else {
    gtk_widget_hide(app->info_panel);
  }
}
