#include "deletejob.h"
#include "fileview_internal.h"
#include "iconchooser.h"
#include "undo.h"
#include <fcntl.h>
#include <glib/gstdio.h>

gboolean path_is_desktop_file(const char *path) {
  return path && g_str_has_suffix(path, ".desktop");
}

gboolean path_is_executable(const char *path) {
  if (!path)
    return FALSE;
  GFile *f = g_file_new_for_path(path);
  GFileInfo *info = g_file_query_info(
      f, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
      G_FILE_QUERY_INFO_NONE, NULL, NULL);
  gboolean has_x = FALSE;
  gboolean is_regular = FALSE;
  if (info) {
    is_regular = g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR;
    has_x = g_file_info_get_attribute_boolean(
        info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE);
    g_object_unref(info);
  }
  g_object_unref(f);

  if (!is_regular || !has_x)
    return FALSE;

  guchar buf[512];
  gsize n_read = 0;
  gboolean result = FALSE;
  GFile *f2 = g_file_new_for_path(path);
  GFileInputStream *in = g_file_read(f2, NULL, NULL);
  if (in) {
    gssize r =
        g_input_stream_read(G_INPUT_STREAM(in), buf, sizeof(buf), NULL, NULL);
    if (r > 0)
      n_read = (gsize)r;
    g_object_unref(in);
  }
  g_object_unref(f2);

  gboolean uncertain;
  char *ctype =
      g_content_type_guess(path, n_read ? buf : NULL, n_read, &uncertain);
  if (ctype) {
    /* g_content_type_can_be_executable() galat taur par TRUE deta hai har
     * text/plain-based type (.c, .txt, .py, .md waghera) ke liye, sirf
     * shebang wale scripts ke liye nahi. Is wajah se MTP/Android se copy ki
     * hui files -- jinhein gvfs-mtp aksar galti se executable-permission
     * ke sath report karta hai -- text file hone ke bawajood "run" karne ki
     * koshish hoti thi (jo fail hoti hai) is liye "open nahi hoti" lagta
     * tha. Fix: text-based content ko sirf tab executable maano jab file
     * mein shebang (#!) ho, warna sirf asal binary/executable types ke
     * liye TRUE karo. */
    if (g_content_type_is_a(ctype, "text/plain")) {
      result = (n_read >= 2 && buf[0] == '#' && buf[1] == '!');
    } else if (g_content_type_can_be_executable(ctype) && !uncertain) {
      result = TRUE;
    }
    g_free(ctype);
  }
  return result;
}

gboolean run_desktop_file(AppWidgets *app, const char *path) {
  GDesktopAppInfo *dinfo = g_desktop_app_info_new_from_filename(path);
  if (!dinfo)
    return FALSE;

  GError *err = NULL;
  gboolean ok = g_app_info_launch(G_APP_INFO(dinfo), NULL, NULL, &err);
  if (!ok) {
    char *msg = g_strdup_printf("Launch nahi kar saka: %s",
                                err ? err->message : "unknown");
    set_status(app, msg);
    g_free(msg);
    if (err)
      g_error_free(err);
  }
  g_object_unref(dinfo);
  return ok;
}

void run_executable_file(AppWidgets *app, const char *path) {
  char *dir = g_path_get_dirname(path);
  char *argv[2] = {(char *)path, NULL};
  GError *err = NULL;

  if (!g_spawn_async(dir, argv, NULL, 0, NULL, NULL, NULL, &err)) {
    char *msg = g_strdup_printf("Run nahi kar saka: %s",
                                err ? err->message : "unknown");
    set_status(app, msg);
    g_free(msg);
    if (err)
      g_error_free(err);
  }
  g_free(dir);
}

void open_path(AppWidgets *app, const char *path, gboolean is_dir) {
  if (is_dir) {
    fileview_navigate(app, path, TRUE);
    return;
  }

  if (path_is_desktop_file(path)) {
    if (run_desktop_file(app, path))
      return;
    /* Invalid .desktop file -- neeche normal "open with default app" try
     * karega. */
  } else if (path_is_executable(path)) {
    run_executable_file(app, path);
    return;
  }

  GFile *f = g_file_new_for_path(path);
  char *uri = g_file_get_uri(f);
  GError *err = NULL;
  if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
    char *msg = g_strdup_printf("Open nahi kar saka: %s",
                                err ? err->message : "unknown");
    set_status(app, msg);
    g_free(msg);
    if (err)
      g_error_free(err);
  } else {
    /* Default app se successfully khula -- ab isay "recently used" list
     * (recent:// sidebar location) mein bhi register kar dein. Zyada tar
     * media players (mpv, vlc, etc.) khud is list mein register nahi karte,
     * is liye gtkfm khud kar deta hai taake played/opened files Recent mein
     * dikhein. */
    gtk_recent_manager_add_item(gtk_recent_manager_get_default(), uri);
  }
  g_free(uri);
  g_object_unref(f);
}

void on_open_with_app(GtkMenuItem *item, gpointer user_data) {
  GAppInfo *info = g_object_get_data(G_OBJECT(item), "appinfo");
  char *path = user_data;
  GFile *f = g_file_new_for_path(path);
  GList *files = g_list_append(NULL, f);
  GError *err = NULL;
  g_app_info_launch(info, files, NULL, &err);
  if (err)
    g_error_free(err);
  g_list_free(files);
  g_object_unref(f);
  g_free(path);
}

static void on_appchooser_response(GtkDialog *dialog, gint response,
                                   gpointer user_data) {
  struct {
    AppWidgets *app;
    char *path;
    gboolean set_default;
  } *ctx = user_data;
  if (response == GTK_RESPONSE_OK) {
    GAppInfo *info = gtk_app_chooser_get_app_info(GTK_APP_CHOOSER(dialog));
    if (info) {
      if (ctx->set_default) {
        gboolean uncertain;
        char *ctype = g_content_type_guess(ctx->path, NULL, 0, &uncertain);
        GError *err = NULL;
        char *msg;
        if (ctype && g_app_info_set_as_default_for_type(info, ctype, &err)) {
          msg = g_strdup_printf("'%s' ab default app hai is file type ke liye",
                                g_app_info_get_name(info));
        } else {
          msg = g_strdup_printf("Default set nahi ho saka: %s",
                                err ? err->message
                                    : "content type pata nahi chala");
        }
        set_status(ctx->app, msg);
        g_free(msg);
        if (err)
          g_error_free(err);
        g_free(ctype);
      } else {
        GFile *f = g_file_new_for_path(ctx->path);
        GList *files = g_list_append(NULL, f);
        GError *err = NULL;
        if (!g_app_info_launch(info, files, NULL, &err)) {
          char *msg = g_strdup_printf("Open nahi kar saka: %s",
                                      err ? err->message : "unknown");
          set_status(ctx->app, msg);
          g_free(msg);
          if (err)
            g_error_free(err);
        }
        g_list_free(files);
        g_object_unref(f);
      }
      g_object_unref(info);
    }
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(ctx->path);
  g_free(ctx);
}

void show_app_chooser(AppWidgets *app, const char *path, gboolean set_default) {
  GFile *f = g_file_new_for_path(path);
  GtkWidget *dialog =
      gtk_app_chooser_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, f);
  gtk_app_chooser_dialog_set_heading(
      GTK_APP_CHOOSER_DIALOG(dialog),
      set_default ? "Is file type ke liye default application chunein:"
                  : "Application chunein jis se ye file open karni hai:");
  g_object_unref(f);

  struct {
    AppWidgets *app;
    char *path;
    gboolean set_default;
  } *ctx = g_new0(typeof(*ctx), 1);
  ctx->app = app;
  ctx->path = g_strdup(path);
  ctx->set_default = set_default;
  g_signal_connect(dialog, "response", G_CALLBACK(on_appchooser_response), ctx);
  gtk_widget_show(dialog);
}

void open_terminal_here(const char *dir_path) {
  /* Try a handful of common terminal emulators; first one found wins. */
  const char *candidates[] = {"wezterm-gui",    "x-terminal-emulator",
                              "gnome-terminal", "konsole",
                              "xfce4-terminal", "tilix",
                              "alacritty",      "kitty",
                              "xterm",          NULL};
  for (int i = 0; candidates[i]; i++) {
    char *found = g_find_program_in_path(candidates[i]);
    if (found) {
      GError *err = NULL;

      if (g_str_has_suffix(found, "wezterm") ||
          g_str_has_suffix(found, "wezterm-gui")) {

        char *argv[] = {found, "start", "--cwd", (char *)dir_path, NULL};

        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
                      &err);

      } else {

        char *argv[] = {found, NULL};

        g_spawn_async(dir_path, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                      NULL, &err);
      }

      if (err)
        g_error_free(err);

      g_free(found);
      return;
      if (err)
        g_error_free(err);
      g_free(found);
      return;
    }
  }
  g_warning("Koi terminal emulator nahi mila (gnome-terminal / xterm waghera "
            "install karein)");
}

static void on_rename_response(GtkDialog *dialog, gint response,
                               gpointer user_data) {
  struct {
    AppWidgets *app;
    char *old_path;
    GtkWidget *entry;
  } *ctx = user_data;
  if (response == GTK_RESPONSE_OK) {
    const char *new_name = gtk_entry_get_text(GTK_ENTRY(ctx->entry));
    char *dir = g_path_get_dirname(ctx->old_path);
    char *new_path = g_build_filename(dir, new_name, NULL);
    GFile *src = g_file_new_for_path(ctx->old_path);
    GFile *dst = g_file_new_for_path(new_path);
    GError *err = NULL;
    if (!g_file_move(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err)) {
      char *msg =
          g_strdup_printf("Rename fail: %s", err ? err->message : "unknown");
      set_status(ctx->app, msg);
      g_free(msg);
      if (err)
        g_error_free(err);
    } else {
      UndoAction *u = g_new0(UndoAction, 1);
      u->type = UNDO_RENAME;
      u->src = g_strdup(ctx->old_path);
      u->dest = g_strdup(new_path);
      undo_push(ctx->app, u);

      g_free(ctx->app->pending_select_path);
      ctx->app->pending_select_path = g_strdup(new_path);
      fileview_refresh(ctx->app);
    }
    g_object_unref(src);
    g_object_unref(dst);
    g_free(dir);
    g_free(new_path);
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(ctx->old_path);
  g_free(ctx);
}

void show_rename_dialog(AppWidgets *app, const char *path,
                        const char *current_name) {
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Rename", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Rename", GTK_RESPONSE_OK, NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), current_name);
  gtk_container_set_border_width(GTK_CONTAINER(content), 10);
  gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 6);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  gtk_widget_show_all(dialog);

  struct {
    AppWidgets *app;
    char *old_path;
    GtkWidget *entry;
  } *ctx = g_new0(typeof(*ctx), 1);
  ctx->app = app;
  ctx->old_path = g_strdup(path);
  ctx->entry = entry;

  g_signal_connect(dialog, "response", G_CALLBACK(on_rename_response), ctx);
}

static GdkPixbuf *load_icon_value_pixbuf(const char *icon_value, int size) {
  GdkPixbuf *pb = NULL;

  if (icon_value && *icon_value) {
    if (g_path_is_absolute(icon_value) &&
        g_file_test(icon_value, G_FILE_TEST_EXISTS)) {
      pb =
          gdk_pixbuf_new_from_file_at_scale(icon_value, size, size, TRUE, NULL);
    } else {
      pb = icon_for_name_sized(icon_value, size);
    }
  }

  if (!pb)
    pb = icon_for_name_sized("application-x-executable", size);

  return pb;
}

static char *desktop_file_get_icon(const char *path) {
  GKeyFile *kf = g_key_file_new();
  char *icon = NULL;
  if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
    icon = g_key_file_get_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                                 G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
  }
  g_key_file_free(kf);
  return icon;
}

static gboolean desktop_file_set_icon(const char *path,
                                      const char *icon_value) {
  GKeyFile *kf = g_key_file_new();
  GError *err = NULL;
  if (!g_key_file_load_from_file(
          kf, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
          &err)) {
    if (err)
      g_error_free(err);
    g_key_file_free(kf);
    return FALSE;
  }

  if (icon_value && *icon_value)
    g_key_file_set_string(kf, G_KEY_FILE_DESKTOP_GROUP,
                          G_KEY_FILE_DESKTOP_KEY_ICON, icon_value);
  else
    g_key_file_remove_key(kf, G_KEY_FILE_DESKTOP_GROUP,
                          G_KEY_FILE_DESKTOP_KEY_ICON, NULL);

  gsize len = 0;
  char *data = g_key_file_to_data(kf, &len, NULL);
  gboolean ok = g_file_set_contents(path, data, (gssize)len, NULL);
  g_free(data);
  g_key_file_free(kf);
  return ok;
}

typedef struct {
  AppWidgets *app;
  char *path;
  GtkWidget *name_entry;
  GtkWidget *comment_entry;
  GtkWidget *exec_entry;
  GtkWidget *icon_entry;
  GtkWidget *icon_preview;
  GtkWidget *terminal_check;
} LauncherEditCtx;

static void launcher_edit_ctx_free(gpointer data) {
  LauncherEditCtx *ctx = data;
  g_free(ctx->path);
  g_free(ctx);
}

static void update_launcher_icon_preview(LauncherEditCtx *ctx) {
  const char *icon_value = gtk_entry_get_text(GTK_ENTRY(ctx->icon_entry));
  GdkPixbuf *pb = load_icon_value_pixbuf(icon_value, 48);
  gtk_image_set_from_pixbuf(GTK_IMAGE(ctx->icon_preview), pb);
  if (pb)
    g_object_unref(pb);
}

static void on_launcher_icon_entry_changed(GtkEditable *editable,
                                           gpointer user_data) {
  (void)editable;
  update_launcher_icon_preview((LauncherEditCtx *)user_data);
}

static void on_launcher_choose_icon_clicked(GtkButton *button,
                                            gpointer user_data) {
  (void)button;
  LauncherEditCtx *ctx = user_data;
  GtkWidget *parent_window = gtk_widget_get_toplevel(ctx->icon_entry);

  guint n = 0;
  const char **names = icon_chooser_app_icon_names(&n);
  const char *current_icon = gtk_entry_get_text(GTK_ENTRY(ctx->icon_entry));

  char *picked = NULL;
  IconChooserResult result = icon_chooser_run(
      GTK_WINDOW(parent_window), "Launcher Icon Chunein", names, n,
      (current_icon && *current_icon) ? current_icon : NULL, TRUE, &picked);

  if (result == ICON_CHOOSER_NAME || result == ICON_CHOOSER_IMAGE)
    gtk_entry_set_text(GTK_ENTRY(ctx->icon_entry), picked ? picked : "");
  else if (result == ICON_CHOOSER_RESET)
    gtk_entry_set_text(GTK_ENTRY(ctx->icon_entry), "");

  g_free(picked);
}

static void on_edit_launcher_response(GtkDialog *dialog, gint response,
                                      gpointer user_data) {
  LauncherEditCtx *ctx = user_data;

  if (response == GTK_RESPONSE_OK) {
    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(
            kf, ctx->path,
            G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, &err)) {
      char *msg = g_strdup_printf("Launcher parh nahi saka: %s",
                                  err ? err->message : "unknown");
      set_status(ctx->app, msg);
      g_free(msg);
      if (err)
        g_error_free(err);
      g_key_file_free(kf);
      gtk_widget_destroy(GTK_WIDGET(dialog));
      return;
    }

    const char *group = G_KEY_FILE_DESKTOP_GROUP;
    const char *name = gtk_entry_get_text(GTK_ENTRY(ctx->name_entry));
    const char *comment = gtk_entry_get_text(GTK_ENTRY(ctx->comment_entry));
    const char *exec = gtk_entry_get_text(GTK_ENTRY(ctx->exec_entry));
    const char *icon = gtk_entry_get_text(GTK_ENTRY(ctx->icon_entry));
    gboolean terminal =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->terminal_check));

    g_key_file_set_string(kf, group, G_KEY_FILE_DESKTOP_KEY_NAME, name);

    if (comment && *comment)
      g_key_file_set_string(kf, group, G_KEY_FILE_DESKTOP_KEY_COMMENT, comment);
    else
      g_key_file_remove_key(kf, group, G_KEY_FILE_DESKTOP_KEY_COMMENT, NULL);

    g_key_file_set_string(kf, group, G_KEY_FILE_DESKTOP_KEY_EXEC, exec);

    if (icon && *icon)
      g_key_file_set_string(kf, group, G_KEY_FILE_DESKTOP_KEY_ICON, icon);
    else
      g_key_file_remove_key(kf, group, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);

    g_key_file_set_boolean(kf, group, G_KEY_FILE_DESKTOP_KEY_TERMINAL,
                           terminal);

    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, NULL);
    GError *save_err = NULL;
    if (!g_file_set_contents(ctx->path, data, (gssize)len, &save_err)) {
      char *msg = g_strdup_printf("Launcher save nahi ho saka: %s",
                                  save_err ? save_err->message : "unknown");
      set_status(ctx->app, msg);
      g_free(msg);
      if (save_err)
        g_error_free(save_err);
    } else {
      fileview_refresh(ctx->app);
    }
    g_free(data);
    g_key_file_free(kf);
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));
}

void show_edit_launcher_dialog(AppWidgets *app, const char *path) {
  GKeyFile *kf = g_key_file_new();
  GError *err = NULL;
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
    char *msg = g_strdup_printf("Launcher parh nahi saka: %s",
                                err ? err->message : "unknown");
    set_status(app, msg);
    g_free(msg);
    if (err)
      g_error_free(err);
    g_key_file_free(kf);
    return;
  }

  const char *group = G_KEY_FILE_DESKTOP_GROUP;
  char *name = g_key_file_get_locale_string(
      kf, group, G_KEY_FILE_DESKTOP_KEY_NAME, NULL, NULL);
  char *comment = g_key_file_get_locale_string(
      kf, group, G_KEY_FILE_DESKTOP_KEY_COMMENT, NULL, NULL);
  char *exec =
      g_key_file_get_string(kf, group, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
  char *icon =
      g_key_file_get_string(kf, group, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
  gboolean terminal =
      g_key_file_get_boolean(kf, group, G_KEY_FILE_DESKTOP_KEY_TERMINAL, NULL);
  g_key_file_free(kf);

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Launcher Edit Karein", GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_OK, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 420, -1);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);
  gtk_box_set_spacing(GTK_BOX(content), 10);

  LauncherEditCtx *ctx = g_new0(LauncherEditCtx, 1);
  ctx->app = app;
  ctx->path = g_strdup(path);

  /* Upar: icon preview + icon name/path entry + Browse... button. */
  GtkWidget *icon_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  GtkWidget *icon_preview = gtk_image_new();
  ctx->icon_preview = icon_preview;
  gtk_box_pack_start(GTK_BOX(icon_row), icon_preview, FALSE, FALSE, 0);

  GtkWidget *icon_entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *icon_entry = gtk_entry_new();
  ctx->icon_entry = icon_entry;
  gtk_entry_set_text(GTK_ENTRY(icon_entry), icon ? icon : "");
  gtk_entry_set_placeholder_text(GTK_ENTRY(icon_entry),
                                 "Icon naam ya image file ka path");
  gtk_widget_set_hexpand(icon_entry, TRUE);
  GtkWidget *browse_btn = gtk_button_new_with_label("Choose Icon...");
  gtk_box_pack_start(GTK_BOX(icon_entry_box), icon_entry, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(icon_entry_box), browse_btn, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(icon_row), icon_entry_box, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(content), icon_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 0);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

  int row = 0;

  GtkWidget *name_label = gtk_label_new("Name:");
  gtk_widget_set_halign(name_label, GTK_ALIGN_END);
  GtkWidget *name_entry = gtk_entry_new();
  ctx->name_entry = name_entry;
  gtk_entry_set_text(GTK_ENTRY(name_entry), name ? name : "");
  gtk_widget_set_hexpand(name_entry, TRUE);
  gtk_grid_attach(GTK_GRID(grid), name_label, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), name_entry, 1, row, 1, 1);
  row++;

  GtkWidget *comment_label = gtk_label_new("Comment:");
  gtk_widget_set_halign(comment_label, GTK_ALIGN_END);
  GtkWidget *comment_entry = gtk_entry_new();
  ctx->comment_entry = comment_entry;
  gtk_entry_set_text(GTK_ENTRY(comment_entry), comment ? comment : "");
  gtk_grid_attach(GTK_GRID(grid), comment_label, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), comment_entry, 1, row, 1, 1);
  row++;

  GtkWidget *exec_label = gtk_label_new("Command:");
  gtk_widget_set_halign(exec_label, GTK_ALIGN_END);
  GtkWidget *exec_entry = gtk_entry_new();
  ctx->exec_entry = exec_entry;
  gtk_entry_set_text(GTK_ENTRY(exec_entry), exec ? exec : "");
  gtk_grid_attach(GTK_GRID(grid), exec_label, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), exec_entry, 1, row, 1, 1);
  row++;

  GtkWidget *terminal_check =
      gtk_check_button_new_with_label("Run in Terminal");
  ctx->terminal_check = terminal_check;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(terminal_check), terminal);
  gtk_grid_attach(GTK_GRID(grid), terminal_check, 1, row, 1, 1);
  row++;

  g_free(name);
  g_free(comment);
  g_free(exec);
  g_free(icon);

  update_launcher_icon_preview(ctx);

  g_signal_connect(icon_entry, "changed",
                   G_CALLBACK(on_launcher_icon_entry_changed), ctx);
  g_signal_connect(browse_btn, "clicked",
                   G_CALLBACK(on_launcher_choose_icon_clicked), ctx);

  gtk_entry_set_activates_default(GTK_ENTRY(name_entry), TRUE);
  gtk_entry_set_activates_default(GTK_ENTRY(exec_entry), TRUE);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

  g_object_set_data_full(G_OBJECT(dialog), "launcher-edit-ctx", ctx,
                         launcher_edit_ctx_free);
  g_signal_connect(dialog, "response", G_CALLBACK(on_edit_launcher_response),
                   ctx);

  gtk_widget_show_all(dialog);
}

char *trash_files_dir(void) {
  return g_build_filename(g_get_home_dir(), ".local", "share", "Trash", "files",
                          NULL);
}

char *trash_info_dir(void) {
  return g_build_filename(g_get_home_dir(), ".local", "share", "Trash", "info",
                          NULL);
}

static gboolean dir_has_entries(const char *dir) {
  gboolean has = FALSE;
  GDir *d = g_dir_open(dir, 0, NULL);
  if (d) {
    has = (g_dir_read_name(d) != NULL);
    g_dir_close(d);
  }
  return has;
}

void trash_loc_free(TrashLoc *loc) {
  if (!loc)
    return;
  g_free(loc->files_dir);
  g_free(loc->info_dir);
  g_free(loc);
}

char *info_dir_for_files_dir(const char *files_dir) {
  if (!files_dir || !g_str_has_suffix(files_dir, "/files"))
    return NULL;
  size_t base_len = strlen(files_dir) - strlen("/files");
  return g_strdup_printf("%.*s/info", (int)base_len, files_dir);
}

GList *all_trash_locations(gboolean only_with_entries) {
  GList *out = NULL;

  char *hf = trash_files_dir();
  char *hi = trash_info_dir();
  if (g_file_test(hf, G_FILE_TEST_IS_DIR) &&
      (!only_with_entries || dir_has_entries(hf))) {
    TrashLoc *loc = g_new0(TrashLoc, 1);
    loc->files_dir = hf;
    loc->info_dir = hi;
    out = g_list_append(out, loc);
  } else {
    g_free(hf);
    g_free(hi);
  }

  unsigned uid = (unsigned)getuid();
  GVolumeMonitor *mon = g_volume_monitor_get();
  GList *mounts = g_volume_monitor_get_mounts(mon);
  for (GList *l = mounts; l; l = l->next) {
    GMount *m = l->data;
    GFile *root = g_mount_get_root(m);
    char *root_path = root ? g_file_get_path(root) : NULL;
    if (root_path) {
      char *f1 = g_strdup_printf("%s/.Trash/%u/files", root_path, uid);
      char *i1 = g_strdup_printf("%s/.Trash/%u/info", root_path, uid);
      if (g_file_test(f1, G_FILE_TEST_IS_DIR) &&
          (!only_with_entries || dir_has_entries(f1))) {
        TrashLoc *loc = g_new0(TrashLoc, 1);
        loc->files_dir = f1;
        loc->info_dir = i1;
        out = g_list_append(out, loc);
      } else {
        g_free(f1);
        g_free(i1);
      }

      char *f2 = g_strdup_printf("%s/.Trash-%u/files", root_path, uid);
      char *i2 = g_strdup_printf("%s/.Trash-%u/info", root_path, uid);
      if (g_file_test(f2, G_FILE_TEST_IS_DIR) &&
          (!only_with_entries || dir_has_entries(f2))) {
        TrashLoc *loc = g_new0(TrashLoc, 1);
        loc->files_dir = f2;
        loc->info_dir = i2;
        out = g_list_append(out, loc);
      } else {
        g_free(f2);
        g_free(i2);
      }
      g_free(root_path);
    }
    if (root)
      g_object_unref(root);
  }
  g_list_free_full(mounts, g_object_unref);
  g_object_unref(mon);

  return out;
}

gboolean fileview_trash_is_empty(void) {
  GList *locs = all_trash_locations(TRUE);
  gboolean empty = (locs == NULL);
  g_list_free_full(locs, (GDestroyNotify)trash_loc_free);
  return empty;
}

gboolean is_trash_dir(AppWidgets *app) {
  if (!app->current_dir)
    return FALSE;
  char *tdir = trash_files_dir();
  gboolean r = g_strcmp0(app->current_dir, tdir) == 0;
  g_free(tdir);
  return r;
}

gboolean delete_recursive(GFile *f, GError **error) {
  GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL);
  if (type == G_FILE_TYPE_DIRECTORY) {
    GFileEnumerator *en = g_file_enumerate_children(
        f, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, error);
    if (!en)
      return FALSE;
    gboolean ok = TRUE;
    GFileInfo *info;
    while (ok && (info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
      GFile *child = g_file_get_child(f, g_file_info_get_name(info));
      ok = delete_recursive(child, error);
      g_object_unref(child);
      g_object_unref(info);
    }
    g_file_enumerator_close(en, NULL, NULL);
    g_object_unref(en);
    if (!ok)
      return FALSE;
  }
  return g_file_delete(f, NULL, error);
}

static char *read_trashinfo_original_path(const char *info_dir,
                                          const char *topdir,
                                          const char *basename) {
  char *info_path = g_strdup_printf("%s/%s.trashinfo", info_dir, basename);

  GKeyFile *kf = g_key_file_new();
  char *result = NULL;
  if (g_key_file_load_from_file(kf, info_path, G_KEY_FILE_NONE, NULL)) {
    char *raw = g_key_file_get_string(kf, "Trash Info", "Path", NULL);
    if (raw) {
      char *decoded = g_uri_unescape_string(raw, NULL);
      if (decoded) {
        result = g_path_is_absolute(decoded)
                     ? g_strdup(decoded)
                     : g_build_filename(topdir, decoded, NULL);
        g_free(decoded);
      }
      g_free(raw);
    }
  }
  g_key_file_free(kf);
  g_free(info_path);
  return result;
}

static char *topdir_for_files_dir(const char *files_dir) {
  char *home_files = trash_files_dir();
  if (g_strcmp0(files_dir, home_files) == 0) {
    g_free(home_files);
    return g_strdup(g_get_home_dir());
  }
  g_free(home_files);

  unsigned uid = (unsigned)getuid();
  char *suffix1 = g_strdup_printf("/.Trash/%u/files", uid);
  char *suffix2 = g_strdup_printf("/.Trash-%u/files", uid);
  char *topdir = NULL;
  if (g_str_has_suffix(files_dir, suffix1))
    topdir = g_strndup(files_dir, strlen(files_dir) - strlen(suffix1));
  else if (g_str_has_suffix(files_dir, suffix2))
    topdir = g_strndup(files_dir, strlen(files_dir) - strlen(suffix2));
  g_free(suffix1);
  g_free(suffix2);
  return topdir;
}

gboolean do_restore_one(const char *item_path, char **out_err) {
  char *item_name = g_path_get_basename(item_path);
  char *files_dir = g_path_get_dirname(item_path);
  char *info_dir = info_dir_for_files_dir(files_dir);
  char *topdir = info_dir ? topdir_for_files_dir(files_dir) : NULL;
  gboolean ok = FALSE;

  if (!info_dir || !topdir) {
    *out_err =
        g_strdup_printf("'%s': trash location samajh nahi aayi", item_name);
    g_free(files_dir);
    g_free(info_dir);
    g_free(topdir);
    g_free(item_name);
    return FALSE;
  }

  char *orig_path = read_trashinfo_original_path(info_dir, topdir, item_name);
  if (!orig_path) {
    *out_err = g_strdup_printf("'%s': original location nahi mili", item_name);
    g_free(files_dir);
    g_free(info_dir);
    g_free(topdir);
    g_free(item_name);
    return FALSE;
  }

  char *parent = g_path_get_dirname(orig_path);
  g_mkdir_with_parents(parent, 0755);
  g_free(parent);

  // GFile *src = g_file_new_for_path(item_path);
  // GFile *dst = g_file_new_for_path(orig_path);
  // GError *err = NULL;
  // if (!g_file_move(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err)) {
  GFile *src = g_file_new_for_path(item_path);
  GFile *dst = g_file_new_for_path(orig_path);
  GError *err = NULL;

  /* destination pehle se hai? */
  if (g_file_query_exists(dst, NULL)) {

    GtkWidget *dlg = gtk_message_dialog_new(
      NULL,
      GTK_DIALOG_MODAL,
      GTK_MESSAGE_WARNING,
      GTK_BUTTONS_NONE,
      "'%s' already exists.",
      item_name);

    gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dlg),
                                             "Do you want to replace the existing file?");

    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "Cancel", GTK_RESPONSE_CANCEL,
                           "Skip",   GTK_RESPONSE_NO,
                           "Replace", GTK_RESPONSE_YES,
                           NULL);

    gint res = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (res == GTK_RESPONSE_CANCEL || res == GTK_RESPONSE_NO) {
      g_object_unref(src);
      g_object_unref(dst);
      g_free(orig_path);
      g_free(files_dir);
      g_free(info_dir);
      g_free(topdir);
      g_free(item_name);
      return FALSE;
    }

    if (!g_file_move(src, dst, G_FILE_COPY_OVERWRITE,
      NULL, NULL, NULL, &err)) {

      *out_err = g_strdup_printf(
        "'%s': %s",
        item_name,
        err ? err->message : "unknown");

      if (err)
        g_error_free(err);

      } else {

        char *info_path =
        g_strdup_printf("%s/%s.trashinfo",
                        info_dir,
                        item_name);

        g_remove(info_path);
        g_free(info_path);
        ok = TRUE;
      }

  } else {

    if (!g_file_move(src, dst, G_FILE_COPY_NONE,
      NULL, NULL, NULL, &err)) {

      *out_err = g_strdup_printf(
        "'%s': %s",
        item_name,
        err ? err->message : "unknown");

      if (err)
        g_error_free(err);

      } else {

        char *info_path =
        g_strdup_printf("%s/%s.trashinfo",
                        info_dir,
                        item_name);

        g_remove(info_path);
        g_free(info_path);
        ok = TRUE;
      }
  }
   /* *out_err =
        g_strdup_printf("'%s': %s", item_name, err ? err->message : "unknown");
    if (err)
      g_error_free(err);
  }*/
  //  else {
  //   char *info_path = g_strdup_printf("%s/%s.trashinfo", info_dir, item_name);
  //   g_remove(info_path);
  //   g_free(info_path);
  //   ok = TRUE;
  // }

  g_object_unref(src);
  g_object_unref(dst);
  g_free(orig_path);
  g_free(files_dir);
  g_free(info_dir);
  g_free(topdir);
  g_free(item_name);
  return ok;
}

void do_restore(AppWidgets *app, const char *item_path, const char *item_name) {
  (void)item_name; /* do_restore_one derives the name from item_path itself */
  char *err_msg = NULL;
  if (do_restore_one(item_path, &err_msg)) {
    char *base = g_path_get_basename(item_path);
    char *msg = g_strdup_printf("'%s' wapas restore ho gaya", base);
    set_status(app, msg);
    g_free(msg);
    g_free(base);
    fileview_refresh(app);
    sidebar_update_trash_icon(app);
  } else {
    char *msg = g_strdup_printf("Restore fail: %s", err_msg);
    set_status(app, msg);
    g_free(msg);
  }
  g_free(err_msg);
}

void do_restore_multiple(AppWidgets *app, GList *paths) {
  guint ok = 0, fail = 0;
  for (GList *l = paths; l; l = l->next) {
    char *err_msg = NULL;
    if (do_restore_one(l->data, &err_msg))
      ok++;
    else
      fail++;
    g_free(err_msg);
  }

  char *msg =
      fail ? g_strdup_printf("%u item restore ho gaye, %u fail", ok, fail)
           : g_strdup_printf("%u item restore ho gaye", ok);
  set_status(app, msg);
  g_free(msg);
  fileview_refresh(app);
  sidebar_update_trash_icon(app);
}

struct DeletePermCtx {
  AppWidgets *app;
  char *path;
  char *post_target;
};

static void on_delete_permanent_confirm(GtkDialog *dialog, gint response,
                                        gpointer user_data) {
  struct DeletePermCtx *ctx = user_data;
  if (response == GTK_RESPONSE_YES) {
    GList *paths = g_list_append(NULL, ctx->path);
    ctx->path = NULL;
    deletejob_start_permanent(ctx->app, paths, ctx->post_target);
    ctx->post_target = NULL;
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(ctx->path);
  g_free(ctx->post_target);
  g_free(ctx);
}

void confirm_delete_permanent(AppWidgets *app, const char *path,
                              const char *name) {
  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_YES_NO,
      "'%s' ko hamesha ke liye delete karna hai? Ye wapas nahi aa sakta.",
      name);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
  struct DeletePermCtx *ctx = g_new0(struct DeletePermCtx, 1);
  ctx->app = app;
  ctx->path = g_strdup(path);
  ctx->post_target = capture_post_delete_selection_target(app);
  g_signal_connect(dialog, "response", G_CALLBACK(on_delete_permanent_confirm),
                   ctx);
  gtk_widget_show(dialog);
}

static void on_empty_trash_confirm(GtkDialog *dialog, gint response,
                                   gpointer user_data) {
  AppWidgets *app = user_data;
  if (response == GTK_RESPONSE_YES) {
    GList *paths = NULL;
    GList *locs = all_trash_locations(FALSE);
    for (GList *l = locs; l; l = l->next) {
      TrashLoc *loc = l->data;
      const char *dirs[2] = {loc->files_dir, loc->info_dir};

      for (int i = 0; i < 2; i++) {
        GFile *dir = g_file_new_for_path(dirs[i]);
        GFileEnumerator *en = g_file_enumerate_children(
            dir, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (en) {
          GFileInfo *info;
          while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
            char *child_path =
                g_build_filename(dirs[i], g_file_info_get_name(info), NULL);
            paths = g_list_prepend(paths, child_path);
            g_object_unref(info);
          }
          g_file_enumerator_close(en, NULL, NULL);
          g_object_unref(en);
        }
        g_object_unref(dir);
      }
    }
    g_list_free_full(locs, (GDestroyNotify)trash_loc_free);

    if (paths)
      deletejob_start_empty_trash(app, paths);
    else
      set_status(app, "Trash pehle se khali hai");
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

void confirm_empty_trash(AppWidgets *app) {
  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_YES_NO,
      "Trash ko hamesha ke liye khali karna hai? Ye wapas nahi aa sakta.");
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
  g_signal_connect(dialog, "response", G_CALLBACK(on_empty_trash_confirm), app);
  gtk_widget_show(dialog);
}

/* Recursively sums up size/item-count for the Properties dialog. */
void count_recursive(GFile *f, guint64 *total_size, guint *count) {
  GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL);
  if (type != G_FILE_TYPE_DIRECTORY)
    return;

  GFileEnumerator *en = g_file_enumerate_children(
      f, "standard::name,standard::size,standard::type", G_FILE_QUERY_INFO_NONE,
      NULL, NULL);
  if (!en)
    return;
  GFileInfo *info;
  while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
    GFile *child = g_file_get_child(f, g_file_info_get_name(info));
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
      count_recursive(child, total_size, count);
    } else {
      *total_size += (guint64)g_file_info_get_size(info);
      (*count)++;
    }
    g_object_unref(child);
    g_object_unref(info);
  }
  g_file_enumerator_close(en, NULL, NULL);
  g_object_unref(en);
}

static char *format_permissions(guint32 mode, gboolean is_dir) {
  char buf[11];
  buf[0] = is_dir ? 'd' : '-';
  buf[1] = (mode & S_IRUSR) ? 'r' : '-';
  buf[2] = (mode & S_IWUSR) ? 'w' : '-';
  buf[3] = (mode & S_IXUSR) ? 'x' : '-';
  buf[4] = (mode & S_IRGRP) ? 'r' : '-';
  buf[5] = (mode & S_IWGRP) ? 'w' : '-';
  buf[6] = (mode & S_IXGRP) ? 'x' : '-';
  buf[7] = (mode & S_IROTH) ? 'r' : '-';
  buf[8] = (mode & S_IWOTH) ? 'w' : '-';
  buf[9] = (mode & S_IXOTH) ? 'x' : '-';
  buf[10] = '\0';
  return g_strdup(buf);
}

static GtkWidget *add_properties_row(GtkWidget *grid, int row,
                                     const char *label_text,
                                     const char *value_text) {
  GtkWidget *l = gtk_label_new(label_text);
  gtk_label_set_xalign(GTK_LABEL(l), 0.0);
  PangoAttrList *attrs = pango_attr_list_new();
  pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  gtk_label_set_attributes(GTK_LABEL(l), attrs);
  pango_attr_list_unref(attrs);

  GtkWidget *v = gtk_label_new(value_text);
  gtk_label_set_xalign(GTK_LABEL(v), 0.0);
  gtk_label_set_selectable(GTK_LABEL(v), TRUE);
  gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);

  gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
  return v;
}

/* ---- Properties dialog: async size calculation -------------------------- */
typedef struct {
  GList *paths;
  GtkWidget *size_label;
  GCancellable *cancellable;
  GThread *thread;
  guint timeout_id;
  GtkWidget *dialog;
  gulong destroy_handler_id;
  GMutex mutex;
  guint64 total_size;
  guint total_items;
  gboolean finished;
} PropSizeJob;

/* Har file mile par turant (mutex ke sath) job->total_size /
 * total_items update karte hain -- pehle sirf poore top-level path
 * ka scan mukammal hone ke baad ek hi baar update hota tha, is liye
 * bade folders (jaise /home/user/) par label "Calculating... 0
 * bytes" par dair tak atka hua dikhta tha, jabke scan andar hi andar
 * chal raha hota tha. NOFOLLOW_SYMLINKS bhi use karte hain taake
 * kisi symlink loop ki wajah se scan kabhi na-khatam na ho jaye. */
static void count_recursive_cancellable(GFile *f, PropSizeJob *job) {
  if (g_cancellable_is_cancelled(job->cancellable))
    return;

  GFileType type = g_file_query_file_type(
      f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable);
  if (type != G_FILE_TYPE_DIRECTORY)
    return;

  GFileEnumerator *en = g_file_enumerate_children(
      f, "standard::name,standard::size,standard::type",
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable, NULL);
  if (!en)
    return;
  GFileInfo *info;
  while (!g_cancellable_is_cancelled(job->cancellable) &&
         (info = g_file_enumerator_next_file(en, job->cancellable, NULL)) !=
             NULL) {
    GFile *child = g_file_get_child(f, g_file_info_get_name(info));
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
      count_recursive_cancellable(child, job);
    } else {
      g_mutex_lock(&job->mutex);
      job->total_size += (guint64)g_file_info_get_size(info);
      job->total_items++;
      g_mutex_unlock(&job->mutex);
    }
    g_object_unref(child);
    g_object_unref(info);
  }
  g_file_enumerator_close(en, NULL, NULL);
  g_object_unref(en);
}

static gpointer prop_size_worker(gpointer data) {
  PropSizeJob *job = data;

  for (GList *l = job->paths;
       l && !g_cancellable_is_cancelled(job->cancellable); l = l->next) {
    const char *p = l->data;
    GFile *f = g_file_new_for_path(p);
    GFileType type = g_file_query_file_type(
        f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, job->cancellable);
    if (type == G_FILE_TYPE_DIRECTORY) {
      count_recursive_cancellable(f, job);
    } else {
      GFileInfo *info =
          g_file_query_info(f, "standard::size",
                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                            job->cancellable, NULL);
      guint64 sz = info ? (guint64)g_file_info_get_size(info) : 0;
      if (info)
        g_object_unref(info);
      g_mutex_lock(&job->mutex);
      job->total_size += sz;
      job->total_items++;
      g_mutex_unlock(&job->mutex);
    }
    g_object_unref(f);
  }

  g_mutex_lock(&job->mutex);
  job->finished = TRUE;
  g_mutex_unlock(&job->mutex);
  return NULL;
}

static void prop_size_job_free(PropSizeJob *job) {
  g_list_free_full(job->paths, g_free);
  g_object_unref(job->cancellable);
  g_mutex_clear(&job->mutex);
  g_free(job);
}

static gboolean prop_size_update_cb(gpointer data) {
  PropSizeJob *job = data;

  g_mutex_lock(&job->mutex);
  guint64 total = job->total_size;
  guint count = job->total_items;
  gboolean finished = job->finished;
  g_mutex_unlock(&job->mutex);

  if (job->size_label) {
    char *human = g_format_size_full(total, G_FORMAT_SIZE_IEC_UNITS);
    char *text = finished
                     ? g_strdup_printf("%s (%u item%s)", human, count,
                                       count == 1 ? "" : "s")
                     : g_strdup_printf("Calculating... %s (%u item%s so far)",
                                       human, count, count == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(job->size_label), text);
    g_free(text);
    g_free(human);
  }

  if (finished) {
    job->timeout_id = 0;
    if (job->dialog) {
      g_signal_handler_disconnect(job->dialog, job->destroy_handler_id);
      job->dialog = NULL;
    }
    g_thread_join(job->thread);
    prop_size_job_free(job);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void on_props_dialog_destroy(GtkWidget *dialog, gpointer data) {
  (void)dialog;
  PropSizeJob *job = data;
  job->size_label = NULL;
  job->dialog = NULL;
  g_cancellable_cancel(job->cancellable);
}

static void start_prop_size_job(GtkWidget *dialog, GtkWidget *size_label,
                                GList *paths) {
  PropSizeJob *job = g_new0(PropSizeJob, 1);
  job->paths = paths; /* ownership taken */
  job->size_label = size_label;
  job->cancellable = g_cancellable_new();
  job->dialog = dialog;
  g_mutex_init(&job->mutex);

  job->destroy_handler_id = g_signal_connect(
      dialog, "destroy", G_CALLBACK(on_props_dialog_destroy), job);

  job->thread = g_thread_new("prop-size", prop_size_worker, job);
  job->timeout_id = g_timeout_add(150, prop_size_update_cb, job);
}

static char *get_format_display(const char *name, gboolean is_dir) {
  if (is_dir)
    return g_strdup("Folder");

  const char *dot = name ? strrchr(name, '.') : NULL;
  if (dot && dot != name && *(dot + 1) != '\0')
    return g_ascii_strup(dot + 1, -1);

  return g_strdup("Unknown");
}

typedef struct {
  AppWidgets *app;
  char *dir_path;
  GtkWidget *preview_image;
} FolderIconCtx;

static void folder_icon_ctx_free(gpointer data) {
  FolderIconCtx *ctx = data;
  g_free(ctx->dir_path);
  g_free(ctx);
}

static void folder_icon_ctx_update_preview(FolderIconCtx *ctx) {
  GdkPixbuf *pb = themed_icon_for(NULL, TRUE, ctx->dir_path, 48);
  gtk_image_set_from_pixbuf(GTK_IMAGE(ctx->preview_image), pb);
  if (pb)
    g_object_unref(pb);
}

static void on_change_folder_icon_clicked(GtkButton *button,
                                          gpointer user_data) {
  (void)button;
  FolderIconCtx *ctx = user_data;
  GtkWidget *parent_window = gtk_widget_get_toplevel(ctx->preview_image);

  guint n = 0;
  const char **names = icon_chooser_folder_icon_names(&n);
  char *current_icon = get_custom_folder_icon(ctx->dir_path);

  char *picked = NULL;
  IconChooserResult result =
      icon_chooser_run(GTK_WINDOW(parent_window), "Folder Icon Chunein", names,
                       n, current_icon, TRUE, &picked);
  g_free(current_icon);

  if (result == ICON_CHOOSER_NAME || result == ICON_CHOOSER_IMAGE) {
    if (set_custom_folder_icon(ctx->dir_path, picked)) {
      folder_icon_ctx_update_preview(ctx);
      fileview_refresh(ctx->app);
    } else {
      set_status(ctx->app, "Folder icon set nahi ho saka.");
    }
  } else if (result == ICON_CHOOSER_RESET) {
    set_custom_folder_icon(ctx->dir_path, NULL);
    folder_icon_ctx_update_preview(ctx);
    fileview_refresh(ctx->app);
  }

  g_free(picked);
}

static void on_reset_folder_icon_clicked(GtkButton *button,
                                         gpointer user_data) {
  (void)button;
  FolderIconCtx *ctx = user_data;
  set_custom_folder_icon(ctx->dir_path, NULL);
  folder_icon_ctx_update_preview(ctx);
  fileview_refresh(ctx->app);
}

typedef struct {
  AppWidgets *app;
  char *desktop_path;
  GtkWidget *preview_image;
} LauncherIconCtx;

static void launcher_icon_ctx_free(gpointer data) {
  LauncherIconCtx *ctx = data;
  g_free(ctx->desktop_path);
  g_free(ctx);
}

static void launcher_icon_ctx_update_preview(LauncherIconCtx *ctx) {
  char *icon = desktop_file_get_icon(ctx->desktop_path);
  GdkPixbuf *pb = load_icon_value_pixbuf(icon, 48);
  gtk_image_set_from_pixbuf(GTK_IMAGE(ctx->preview_image), pb);
  if (pb)
    g_object_unref(pb);
  g_free(icon);
}

static void on_change_launcher_icon_clicked(GtkButton *button,
                                            gpointer user_data) {
  (void)button;
  LauncherIconCtx *ctx = user_data;
  GtkWidget *parent_window = gtk_widget_get_toplevel(ctx->preview_image);

  guint n = 0;
  const char **names = icon_chooser_app_icon_names(&n);
  char *current_icon = desktop_file_get_icon(ctx->desktop_path);

  char *picked = NULL;
  IconChooserResult result =
      icon_chooser_run(GTK_WINDOW(parent_window), "Launcher Icon Chunein",
                       names, n, current_icon, TRUE, &picked);
  g_free(current_icon);

  if (result == ICON_CHOOSER_NAME || result == ICON_CHOOSER_IMAGE) {
    if (desktop_file_set_icon(ctx->desktop_path, picked)) {
      launcher_icon_ctx_update_preview(ctx);
      fileview_refresh(ctx->app);
    } else {
      set_status(ctx->app, "Launcher icon set nahi ho saka.");
    }
  } else if (result == ICON_CHOOSER_RESET) {
    desktop_file_set_icon(ctx->desktop_path, NULL);
    launcher_icon_ctx_update_preview(ctx);
    fileview_refresh(ctx->app);
  }

  g_free(picked);
}

static void on_reset_launcher_icon_clicked(GtkButton *button,
                                           gpointer user_data) {
  (void)button;
  LauncherIconCtx *ctx = user_data;
  desktop_file_set_icon(ctx->desktop_path, NULL);
  launcher_icon_ctx_update_preview(ctx);
  fileview_refresh(ctx->app);
}

void show_properties_dialog(AppWidgets *app, const char *path, const char *name,
                            gboolean is_dir) {
  GFile *f = g_file_new_for_path(path);
  GFileInfo *info = g_file_query_info(
      f, "standard::size,standard::content-type,time::modified,unix::mode",
      G_FILE_QUERY_INFO_NONE, NULL, NULL);

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Properties", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "_Close",
      GTK_RESPONSE_CLOSE, NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);

  if (is_dir) {
    GtkWidget *icon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(icon_box), 0);

    GdkPixbuf *preview_pb = themed_icon_for(NULL, TRUE, path, 48);
    GtkWidget *preview_image = gtk_image_new_from_pixbuf(preview_pb);
    if (preview_pb)
      g_object_unref(preview_pb);
    gtk_box_pack_start(GTK_BOX(icon_box), preview_image, FALSE, FALSE, 0);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *change_btn = gtk_button_new_with_label("Change Icon...");
    GtkWidget *reset_btn = gtk_button_new_with_label("Reset to Default");
    gtk_box_pack_start(GTK_BOX(btn_box), change_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), reset_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(icon_box), btn_box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), icon_box, FALSE, FALSE, 0);

    FolderIconCtx *ctx = g_new0(FolderIconCtx, 1);
    ctx->app = app;
    ctx->dir_path = g_strdup(path);
    ctx->preview_image = preview_image;
    g_object_set_data_full(G_OBJECT(dialog), "folder-icon-ctx", ctx,
                           folder_icon_ctx_free);

    g_signal_connect(change_btn, "clicked",
                     G_CALLBACK(on_change_folder_icon_clicked), ctx);
    g_signal_connect(reset_btn, "clicked",
                     G_CALLBACK(on_reset_folder_icon_clicked), ctx);
  } else if (path_is_desktop_file(path)) {
    GtkWidget *icon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(icon_box), 0);

    char *current_icon = desktop_file_get_icon(path);
    GdkPixbuf *preview_pb = load_icon_value_pixbuf(current_icon, 48);
    g_free(current_icon);
    GtkWidget *preview_image = gtk_image_new_from_pixbuf(preview_pb);
    if (preview_pb)
      g_object_unref(preview_pb);
    gtk_box_pack_start(GTK_BOX(icon_box), preview_image, FALSE, FALSE, 0);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *change_btn = gtk_button_new_with_label("Change Icon...");
    GtkWidget *reset_btn = gtk_button_new_with_label("Reset to Default");
    gtk_box_pack_start(GTK_BOX(btn_box), change_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), reset_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(icon_box), btn_box, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), icon_box, FALSE, FALSE, 0);

    LauncherIconCtx *ctx = g_new0(LauncherIconCtx, 1);
    ctx->app = app;
    ctx->desktop_path = g_strdup(path);
    ctx->preview_image = preview_image;
    g_object_set_data_full(G_OBJECT(dialog), "launcher-icon-ctx", ctx,
                           launcher_icon_ctx_free);

    g_signal_connect(change_btn, "clicked",
                     G_CALLBACK(on_change_launcher_icon_clicked), ctx);
    g_signal_connect(reset_btn, "clicked",
                     G_CALLBACK(on_reset_launcher_icon_clicked), ctx);
  }

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_widget_set_size_request(grid, 360, -1);
  gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

  int row = 0;
  add_properties_row(grid, row++, "Name:", name);

  char *format_desc = get_format_display(name, is_dir);
  add_properties_row(grid, row++, "Format:", format_desc);
  g_free(format_desc);

  char *parent = g_path_get_dirname(path);
  add_properties_row(grid, row++, "Location:", parent);
  g_free(parent);

  const char *ctype = info ? g_file_info_get_content_type(info) : NULL;
  char *type_desc = is_dir ? g_strdup("Folder")
                           : (ctype ? g_content_type_get_description(ctype)
                                    : g_strdup("Unknown"));
  add_properties_row(grid, row++, "Type:", type_desc);
  g_free(type_desc);

  GtkWidget *size_label;
  if (is_dir) {
    size_label = add_properties_row(grid, row++, "Size:", "Calculating...");
  } else {
    guint64 sz = info ? (guint64)g_file_info_get_size(info) : 0;
    char *size_text = g_format_size_full(sz, G_FORMAT_SIZE_IEC_UNITS);
    size_label = add_properties_row(grid, row++, "Size:", size_text);
    g_free(size_text);
  }

  if (info) {
    guint64 mtime =
        g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    GDateTime *dt = g_date_time_new_from_unix_local((gint64)mtime);
    char *mtime_str =
        dt ? g_date_time_format(dt, "%Y-%m-%d %H:%M:%S") : g_strdup("Unknown");
    add_properties_row(grid, row++, "Modified:", mtime_str);
    g_free(mtime_str);
    if (dt)
      g_date_time_unref(dt);

    guint32 mode =
        g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    char *perms = format_permissions(mode, is_dir);
    add_properties_row(grid, row++, "Permissions:", perms);
    g_free(perms);
  }

  add_properties_row(grid, row++, "Full Path:", path);

  /* Folder ke andar ki files ka size ginne mein waqt lagta hai (poori
   * scan), lekin drive ka total/free space turant mil sakta hai --
   * is liye ye alag se, foran dikha dete hain, "Calculating..." ka
   * intezar kiye baghair. */
  if (is_dir) {
    GFile *disk_f = g_file_new_for_path(path);
    GFileInfo *fs_info = g_file_query_filesystem_info(
        disk_f,
        G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
        NULL, NULL);
    if (fs_info &&
        g_file_info_has_attribute(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) &&
        g_file_info_has_attribute(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
      guint64 total_bytes = g_file_info_get_attribute_uint64(
          fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
      guint64 free_bytes = g_file_info_get_attribute_uint64(
          fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
      char *total_str = g_format_size_full(total_bytes, G_FORMAT_SIZE_IEC_UNITS);
      char *free_str = g_format_size_full(free_bytes, G_FORMAT_SIZE_IEC_UNITS);
      char *disk_text = g_strdup_printf("%s free of %s", free_str, total_str);
      add_properties_row(grid, row++, "Disk:", disk_text);
      g_free(disk_text);
      g_free(total_str);
      g_free(free_str);
    }
    if (fs_info)
      g_object_unref(fs_info);
    g_object_unref(disk_f);
  }

  if (is_dir)
    start_prop_size_job(dialog, size_label,
                        g_list_append(NULL, g_strdup(path)));

  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  if (info)
    g_object_unref(info);
  g_object_unref(f);
}

/* Poori drive (internal filesystem ya external/USB drive) ki Properties.
 * Yahan folder ki tarah har file ko recursively enumerate nahi karte --
 * us se poori disk scan honi padti hai jo bohot slow hai aur size bohot
 * dair tak "0 bytes"/"Calculating..." dikhata rehta hai. Iski jagah
 * filesystem::size aur filesystem::free (statvfs jaisi fast query) se
 * turant Total/Used/Free mil jaata hai -- Nautilus/Files jaisa hi. */
void show_drive_properties_dialog(AppWidgets *app, const char *path,
                                  const char *name, gboolean is_removable) {
  GFile *f = g_file_new_for_path(path);
  GError *err = NULL;
  GFileInfo *fs_info = g_file_query_filesystem_info(
      f, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE
      "," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
      NULL, &err);
  if (err)
    g_error_free(err);

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Properties", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "_Close",
      GTK_RESPONSE_CLOSE, NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);

  GtkWidget *icon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GdkPixbuf *preview_pb = icon_for_name_sized(
      is_removable ? "drive-removable-media" : "drive-harddisk-system", 48);
  GtkWidget *preview_image = gtk_image_new_from_pixbuf(preview_pb);
  if (preview_pb)
    g_object_unref(preview_pb);
  gtk_box_pack_start(GTK_BOX(icon_box), preview_image, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), icon_box, FALSE, FALSE, 0);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_widget_set_size_request(grid, 360, -1);
  gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

  int row = 0;
  add_properties_row(grid, row++, "Name:", name);
  add_properties_row(grid, row++, "Type:",
                     is_removable ? "Removable drive" : "Filesystem");

  const char *fstype =
      fs_info ? g_file_info_get_attribute_string(
                    fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE)
              : NULL;
  add_properties_row(grid, row++, "Filesystem:",
                     (fstype && *fstype) ? fstype : "Unknown");

  if (fs_info &&
      g_file_info_has_attribute(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) &&
      g_file_info_has_attribute(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
    guint64 total_bytes = g_file_info_get_attribute_uint64(
        fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    guint64 free_bytes = g_file_info_get_attribute_uint64(
        fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    guint64 used_bytes =
        total_bytes > free_bytes ? total_bytes - free_bytes : 0;

    char *total_str = g_format_size_full(total_bytes, G_FORMAT_SIZE_IEC_UNITS);
    char *used_str = g_format_size_full(used_bytes, G_FORMAT_SIZE_IEC_UNITS);
    char *free_str = g_format_size_full(free_bytes, G_FORMAT_SIZE_IEC_UNITS);

    add_properties_row(grid, row++, "Total space:", total_str);
    add_properties_row(grid, row++, "Used space:", used_str);
    add_properties_row(grid, row++, "Free space:", free_str);

    g_free(total_str);
    g_free(used_str);
    g_free(free_str);
  } else {
    add_properties_row(grid, row++, "Total space:", "Unavailable");
  }

  add_properties_row(grid, row++, "Mount point:", path);

  if (fs_info)
    g_object_unref(fs_info);

  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  g_object_unref(f);
}

void show_properties_dialog_multi(AppWidgets *app, GList *paths) {
  guint n_selected = 0, n_dirs = 0, n_files = 0;
  char *common_parent = NULL;
  gboolean parent_differs = FALSE;
  GList *job_paths = NULL; /* strdup'd copy handed to the async size job */

  for (GList *l = paths; l; l = l->next) {
    const char *p = l->data;
    n_selected++;

    GFile *f = g_file_new_for_path(p);
    GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL);
    if (type == G_FILE_TYPE_DIRECTORY)
      n_dirs++;
    else
      n_files++;
    g_object_unref(f);

    job_paths = g_list_prepend(job_paths, g_strdup(p));

    char *parent = g_path_get_dirname(p);
    if (!common_parent) {
      common_parent = parent;
    } else {
      if (g_strcmp0(common_parent, parent) != 0)
        parent_differs = TRUE;
      g_free(parent);
    }
  }

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Properties", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "_Close",
      GTK_RESPONSE_CLOSE, NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_widget_set_size_request(grid, 360, -1);
  gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

  int row = 0;
  char *sel_desc =
      g_strdup_printf("%u item%s selected (%u folder%s, %u file%s)", n_selected,
                      n_selected == 1 ? "" : "s", n_dirs,
                      n_dirs == 1 ? "" : "s", n_files, n_files == 1 ? "" : "s");
  add_properties_row(grid, row++, "Selection:", sel_desc);
  g_free(sel_desc);

  GtkWidget *size_label =
      add_properties_row(grid, row++, "Total Size:", "Calculating...");

  add_properties_row(grid, row++, "Location:",
                     parent_differs ? "Multiple locations"
                                    : (common_parent ? common_parent : ""));

  start_prop_size_job(dialog, size_label, job_paths);

  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  g_free(common_parent);
}

void confirm_delete(AppWidgets *app, const char *path, const char *name) {
  (void)name;
  char *post_target = capture_post_delete_selection_target(app);
  GList *paths = g_list_append(NULL, g_strdup(path));
  deletejob_start_trash(app, paths, post_target);
}

static void show_folder_exists_popup(AppWidgets *app, GtkWidget *parent,
                                     const char *name) {
  char *msg = g_strdup_printf(
      "\"%s\" naam ka folder is location mein pehle se maujood hai.\n"
      "Koi doosra naam try karein.",
      name);
  GtkWidget *popup = gtk_message_dialog_new(
      GTK_WINDOW(parent), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "%s", msg);
  gtk_window_set_title(GTK_WINDOW(popup), "Folder pehle se maujood hai");
  gtk_dialog_run(GTK_DIALOG(popup));
  gtk_widget_destroy(popup);
  g_free(msg);
  set_status(app, "Folder create nahi kar saka -- naam pehle se maujood hai");
}

static void on_new_folder_response(GtkDialog *dialog, gint response,
                                   gpointer user_data) {
  struct {
    AppWidgets *app;
    GtkWidget *entry;
  } *ctx = user_data;
  if (response == GTK_RESPONSE_OK) {
    const char *name = gtk_entry_get_text(GTK_ENTRY(ctx->entry));
    if (name && *name) {
      char *new_path = g_build_filename(ctx->app->current_dir, name, NULL);
      GFile *f = g_file_new_for_path(new_path);
      GError *err = NULL;
      // if (!g_file_make_directory(f, NULL, &err)) {
      //   if (err && err->code == G_IO_ERROR_EXISTS) {
      //     g_object_unref(f);
      //     g_free(new_path);
      //     g_error_free(err);
      //     show_folder_exists_popup(ctx->app, GTK_WIDGET(dialog), name);
      //     gtk_editable_select_region(GTK_EDITABLE(ctx->entry), 0, -1);
      //     gtk_widget_grab_focus(ctx->entry);
      //     return;
      //   }
      if (!g_file_make_directory(f, NULL, &err)) {

        if (err && err->code == G_IO_ERROR_EXISTS) {

          g_error_free(err);
          err = NULL;

          /* Same naam already hai, number add karo */
          // int i = 1;
          // char *unique_path = NULL;
          //
          // while (TRUE) {
          //   // char *new_name = g_strdup_printf("%s %d", name, i);
          //   // unique_path = g_build_filename(ctx->app->current_dir,
          //   //                                new_name,
          //   //                                NULL);
          //   // g_free(new_name);
          //   char *new_name = g_strdup_printf("%s %d", name, i);
          //
          //   unique_path = g_build_filename(ctx->app->current_dir,
          //                                  new_name,
          //                                  NULL);
          //
          //   if (!g_file_test(unique_path, G_FILE_TEST_EXISTS)) {
          //     gtk_entry_set_text(GTK_ENTRY(ctx->entry), new_name);
          //     gtk_editable_select_region(GTK_EDITABLE(ctx->entry), 0, -1);
          //     gtk_widget_grab_focus(ctx->entry);
          //     g_free(new_name);
          //     break;
          //   }
          //
          //   g_free(new_name);
          //
          //   if (!g_file_test(unique_path, G_FILE_TEST_EXISTS))
          //     break;
          //
          //   gtk_entry_set_text(GTK_ENTRY(ctx->entry), new_name);
          //   gtk_editable_select_region(GTK_EDITABLE(ctx->entry), 0, -1);
          //   gtk_widget_grab_focus(ctx->entry);
          //
          //   g_free(unique_path);
          //   unique_path = NULL;
          //   i++;
          // }
          /* Same naam already hai, number add karo */
          int i = 1;
          char *unique_path = NULL;

          while (TRUE) {
            char *new_name = g_strdup_printf("%s %d", name, i);

            unique_path = g_build_filename(ctx->app->current_dir,
                                           new_name,
                                           NULL);

            if (!g_file_test(unique_path, G_FILE_TEST_EXISTS)) {

              gtk_entry_set_text(GTK_ENTRY(ctx->entry), new_name);
              gtk_editable_select_region(GTK_EDITABLE(ctx->entry), 0, -1);
              gtk_widget_grab_focus(ctx->entry);

              g_free(new_name);
              break;
            }

            g_free(new_name);
            g_free(unique_path);
            unique_path = NULL;
            i++;
          }

          GFile *unique_file = g_file_new_for_path(unique_path);

          if (g_file_make_directory(unique_file, NULL, &err)) {

            g_free(ctx->app->pending_select_path);
            ctx->app->pending_select_path = g_strdup(unique_path);
            fileview_refresh(ctx->app);

          } else {

            char *msg = g_strdup_printf("Folder create fail: %s",
                                        err ? err->message : "unknown");
            set_status(ctx->app, msg);
            g_free(msg);

            if (err)
              g_error_free(err);
          }

          g_object_unref(unique_file);
          g_free(unique_path);

          g_object_unref(f);
          g_free(new_path);

          gtk_widget_destroy(GTK_WIDGET(dialog));
          g_free(ctx);
          return;
        }

        char *msg = g_strdup_printf("Folder create fail: %s",
                                    err ? err->message : "unknown");
        set_status(ctx->app, msg);
        g_free(msg);
        if (err)
          g_error_free(err);
      } else {
        g_free(ctx->app->pending_select_path);
        ctx->app->pending_select_path = g_strdup(new_path);
        fileview_refresh(ctx->app);
      }
      g_object_unref(f);
      g_free(new_path);
    }
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(ctx);
}

// void new_folder_dialog(AppWidgets *app) {
//   GtkWidget *dialog = gtk_dialog_new_with_buttons(
//       "New Folder", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "_Cancel",
//       GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_OK, NULL);
//   GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
//   GtkWidget *entry = gtk_entry_new();
//   // gtk_entry_set_text(GTK_ENTRY(entry), "New Folder");
//   char *new_name = g_strdup_printf("%s %d", name, i);
//
//   gtk_container_set_border_width(GTK_CONTAINER(content), 10);
//   gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 6);
//   gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
//   gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
//   gtk_widget_show_all(dialog);
//
//   struct {
//     AppWidgets *app;
//     GtkWidget *entry;
//   } *ctx = g_new0(typeof(*ctx), 1);
//   ctx->app = app;
//   ctx->entry = entry;
//   g_signal_connect(dialog, "response", G_CALLBACK(on_new_folder_response), ctx);
// }
void new_folder_dialog(AppWidgets *app) {
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    "New Folder", GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                                  "_Cancel", GTK_RESPONSE_CANCEL,
                                                  "_Create", GTK_RESPONSE_OK,
                                                  NULL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *entry = gtk_entry_new();

  // gtk_entry_set_text(GTK_ENTRY(entry), "New Folder");
  char *default_name = g_strdup("New Folder");
  int i = 1;

  while (TRUE) {
    char *test_path = g_build_filename(app->current_dir,
                                       default_name,
                                       NULL);

    if (!g_file_test(test_path, G_FILE_TEST_EXISTS)) {
      g_free(test_path);
      break;
    }

    g_free(test_path);

    g_free(default_name);
    default_name = g_strdup_printf("New Folder %d", i);
    i++;
  }

  gtk_entry_set_text(GTK_ENTRY(entry), default_name);
  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  g_free(default_name);

  gtk_container_set_border_width(GTK_CONTAINER(content), 10);
  gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 6);

  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

  gtk_widget_show_all(dialog);

  struct {
    AppWidgets *app;
    GtkWidget *entry;
  } *ctx = g_new0(typeof(*ctx), 1);

  ctx->app = app;
  ctx->entry = entry;

  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_new_folder_response),
                   ctx);
}
void fileview_new_folder(AppWidgets *app) { new_folder_dialog(app); }

static void on_new_file_response(GtkDialog *dialog, gint response,
                                 gpointer user_data) {
  struct {
    AppWidgets *app;
    GtkWidget *entry;
  } *ctx = user_data;
  if (response == GTK_RESPONSE_OK) {
    const char *name = gtk_entry_get_text(GTK_ENTRY(ctx->entry));
    if (name && *name) {
      char *new_path = g_build_filename(ctx->app->current_dir, name, NULL);
      GFile *f = g_file_new_for_path(new_path);
      GError *err = NULL;
      GFileOutputStream *os = g_file_create(f, G_FILE_CREATE_NONE, NULL, &err);
      if (!os) {
        if (err) {
          char *msg = g_strdup_printf("File create fail: %s", err->message);
          set_status(ctx->app, msg);
          g_free(msg);
          g_error_free(err);
        }
      } else {
        g_object_unref(os);
        g_free(ctx->app->pending_select_path);
        ctx->app->pending_select_path = g_strdup(new_path);
        fileview_refresh(ctx->app);
      }
      g_object_unref(f);
      g_free(new_path);
    }
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(ctx);
}

static void new_file_dialog(AppWidgets *app, const char *default_name) {
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "New File", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_OK, NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *entry = gtk_entry_new();
  // gtk_entry_set_text(GTK_ENTRY(entry), default_name);
  char *name = g_strdup(default_name);
  int i = 1;

  while (TRUE) {
    char *test_path = g_build_filename(app->current_dir, name, NULL);

    if (!g_file_test(test_path, G_FILE_TEST_EXISTS)) {
      g_free(test_path);
      break;
    }

    g_free(test_path);
    g_free(name);

    char *base = g_path_get_basename(default_name);
    char *dot = strrchr(base, '.');

    if (dot) {
      char *ext = g_strdup(dot);
      *dot = '\0';

      name = g_strdup_printf("%s %d%s", base, i, ext);

      g_free(ext);
    } else {
      name = g_strdup_printf("%s %d", base, i);
    }

    g_free(base);
    i++;
  }

  gtk_entry_set_text(GTK_ENTRY(entry), name);
  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  g_free(name);
  gtk_container_set_border_width(GTK_CONTAINER(content), 10);
  gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 6);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  gtk_widget_show_all(dialog);

  const char *dot = strrchr(default_name, '.');
  if (dot && dot != default_name)
    gtk_editable_select_region(GTK_EDITABLE(entry), 0,
                               g_utf8_pointer_to_offset(default_name, dot));
  else
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);

  struct {
    AppWidgets *app;
    GtkWidget *entry;
  } *ctx = g_new0(typeof(*ctx), 1);
  ctx->app = app;
  ctx->entry = entry;
  g_signal_connect(dialog, "response", G_CALLBACK(on_new_file_response), ctx);
}

void fileview_new_file(AppWidgets *app) {
  new_file_dialog(app, "New File.txt");
}

void new_file_type_activate(GtkMenuItem *mi, gpointer user_data) {
  AppWidgets *app = user_data;
  const char *default_name = g_object_get_data(G_OBJECT(mi), "default_name");
  new_file_dialog(app, default_name ? default_name : "New File.txt");
}

gboolean has_archive_extension(const char *path) {
  static const char *exts[] = {".zip",     ".tar",  ".tar.gz", ".tgz",
                               ".tar.bz2", ".tbz2", ".tar.xz", ".txz",
                               ".7z",      ".rar",  NULL};
  for (int i = 0; exts[i]; i++)
    if (g_str_has_suffix(path, exts[i]))
      return TRUE;
  return FALSE;
}

static GList *list_dir_names(const char *dir_path) {
  GList *names = NULL;
  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (!dir)
    return NULL;
  const char *name;
  while ((name = g_dir_read_name(dir)) != NULL)
    names = g_list_prepend(names, g_strdup(name));
  g_dir_close(dir);
  return names;
}

typedef enum { ARCHIVE_ZIP, ARCHIVE_7Z, ARCHIVE_RAR, ARCHIVE_TAR } ArchiveKind;

static ArchiveKind archive_kind_for(const char *path) {
  if (g_str_has_suffix(path, ".zip"))
    return ARCHIVE_ZIP;
  if (g_str_has_suffix(path, ".7z"))
    return ARCHIVE_7Z;
  if (g_str_has_suffix(path, ".rar"))
    return ARCHIVE_RAR;
  return ARCHIVE_TAR; /* tar/.tar.gz/.tgz/.tar.bz2/.tbz2/.tar.xz/.txz --
                       * tar khud compression detect kar leta hai */
}

static const char *tool_name_for_kind(ArchiveKind kind) {
  switch (kind) {
  case ARCHIVE_ZIP:
    return "unzip";
  case ARCHIVE_7Z:
    return "7z";
  case ARCHIVE_RAR:
    return "unrar";
  default:
    return "tar";
  }
}

static void extract_free_secret(char *s) {
  if (!s)
    return;
  volatile char *p = (volatile char *)s;
  while (*p)
    *p++ = 0;
  g_free(s);
}

static void redirect_stdin_devnull(gpointer user_data) {
  (void)user_data;
  int fd = open("/dev/null", O_RDONLY);
  if (fd >= 0) {
    dup2(fd, STDIN_FILENO);
    close(fd);
  }
}

static GPtrArray *build_extract_argv(ArchiveKind kind, gboolean zip_use_7z,
                                     const char *tool, const char *archive_path,
                                     const char *dest_dir,
                                     const char *password) {
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(arr, g_strdup(tool));

  switch (kind) {
  case ARCHIVE_ZIP:
    if (zip_use_7z) {
      g_ptr_array_add(arr, g_strdup("x"));
      g_ptr_array_add(arr, g_strdup("-y"));
      g_ptr_array_add(arr, g_strdup_printf("-p%s", password ? password : ""));
      g_ptr_array_add(arr, g_strdup(archive_path));
      g_ptr_array_add(arr, g_strdup_printf("-o%s", dest_dir));
    } else {
      g_ptr_array_add(arr, g_strdup("-o")); /* overwrite without asking */
      if (password && *password)
        g_ptr_array_add(arr, g_strdup_printf("-P%s", password));
      g_ptr_array_add(arr, g_strdup(archive_path));
      g_ptr_array_add(arr, g_strdup("-d"));
      g_ptr_array_add(arr, g_strdup(dest_dir));
    }
    break;
  case ARCHIVE_7Z:
    g_ptr_array_add(arr, g_strdup("x"));
    g_ptr_array_add(arr, g_strdup("-y"));
    g_ptr_array_add(arr, g_strdup_printf("-p%s", password ? password : ""));
    g_ptr_array_add(arr, g_strdup(archive_path));
    g_ptr_array_add(arr, g_strdup_printf("-o%s", dest_dir)); /* no space */
    break;
  case ARCHIVE_RAR:
    g_ptr_array_add(arr, g_strdup("x"));
    g_ptr_array_add(arr, g_strdup("-o+"));
    if (password && *password)
      g_ptr_array_add(arr, g_strdup_printf("-p%s", password));
    else
      g_ptr_array_add(arr, g_strdup("-p-")); /* prompt na kare */
    g_ptr_array_add(arr, g_strdup(archive_path));
    g_ptr_array_add(arr, g_strdup_printf("%s%s", dest_dir, G_DIR_SEPARATOR_S));
    break;
  default:
    g_ptr_array_add(arr, g_strdup("xf"));
    g_ptr_array_add(arr, g_strdup(archive_path));
    g_ptr_array_add(arr, g_strdup("-C"));
    g_ptr_array_add(arr, g_strdup(dest_dir));
    break;
  }

  g_ptr_array_add(arr, NULL);
  return arr;
}

typedef struct {
  AppWidgets *app;
  char *archive_path;
  char *dest_dir;
  GList *before;
  ArchiveKind kind;
  gboolean zip_use_7z;
  char *tool;
  char *password;
  GPid pid;
  gint wait_status;
  gboolean child_exited;
  gboolean io_done;
  GString *stderr_buf;
} ExtractJob;

static void start_extract_attempt(ExtractJob *job);

static void extract_job_destroy(ExtractJob *job) {
  extract_free_secret(job->password);
  g_free(job->tool);
  g_free(job->archive_path);
  g_free(job->dest_dir);
  g_list_free_full(job->before, g_free);
  if (job->stderr_buf)
    g_string_free(job->stderr_buf, TRUE);
  g_free(job);
}

static void finish_extract_success(ExtractJob *job) {
  set_status(job->app, "Extract ho gaya");

  g_free(job->app->pending_select_path);
  job->app->pending_select_path = g_strdup(job->dest_dir);
  fileview_refresh(job->app);
}

static gboolean extract_stderr_suggests_password(ArchiveKind kind,
                                                 const char *stderr_text) {
  if (kind == ARCHIVE_TAR || !stderr_text || !*stderr_text)
    return FALSE;
  char *lower = g_ascii_strdown(stderr_text, -1);
  gboolean found = strstr(lower, "password") != NULL;
  g_free(lower);
  return found;
}

typedef struct {
  ExtractJob *job;
  GtkWidget *entry;
} ExtractPasswordCtx;

static void on_extract_password_response(GtkDialog *dialog, gint response,
                                         gpointer user_data) {
  ExtractPasswordCtx *ctx = user_data;
  ExtractJob *job = ctx->job;

  if (response == GTK_RESPONSE_OK) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(ctx->entry));
    extract_free_secret(job->password);
    job->password = g_strdup(text);
    gtk_widget_destroy(GTK_WIDGET(dialog));
    g_free(ctx);
    start_extract_attempt(job); /* dobara koshish, password ke saath */
  } else {
    gtk_widget_destroy(GTK_WIDGET(dialog));
    g_free(ctx);
    set_status(job->app, "Password nahi diya, extract cancel kar diya");
    g_rmdir(job->dest_dir); /* khali reh gaya folder hai to hata dete hain */
    fileview_refresh(job->app);
    extract_job_destroy(job);
  }
}

static void prompt_for_extract_password(ExtractJob *job,
                                        gboolean previous_was_wrong) {
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Password Chahiye", GTK_WINDOW(job->app->window), GTK_DIALOG_MODAL,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Extract", GTK_RESPONSE_OK, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 340, -1);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 10);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_pack_start(GTK_BOX(content), box, FALSE, FALSE, 0);

  char *base = g_path_get_basename(job->archive_path);
  char *msg = g_strdup_printf("\"%s\" password se protected hai.", base);
  g_free(base);
  GtkWidget *label = gtk_label_new(msg);
  g_free(msg);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  if (previous_was_wrong) {
    GtkWidget *err_label =
        gtk_label_new("Password ghalat tha, dobara koshish karein.");
    gtk_widget_set_halign(err_label, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(err_label),
                                "error");
    gtk_box_pack_start(GTK_BOX(box), err_label, FALSE, FALSE, 0);
  }

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
  gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Password");
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  gtk_widget_show_all(dialog);

  ExtractPasswordCtx *ctx = g_new0(ExtractPasswordCtx, 1);
  ctx->job = job;
  ctx->entry = entry;
  g_signal_connect(dialog, "response", G_CALLBACK(on_extract_password_response),
                   ctx);

  set_status(job->app, "Password chahiye...");
}

static void remove_path_recursive(const char *path) {
  GFile *f = g_file_new_for_path(path);
  GFileEnumerator *en = g_file_enumerate_children(
      f, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      NULL, NULL);
  if (en) {
    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
      const char *name = g_file_info_get_name(info);
      char *child_path = g_build_filename(path, name, NULL);
      remove_path_recursive(child_path);
      g_free(child_path);
      g_object_unref(info);
    }
    g_file_enumerator_close(en, NULL, NULL);
    g_object_unref(en);
  }
  g_file_delete(f, NULL, NULL);
  g_object_unref(f);
}

static void cleanup_partial_extract(ExtractJob *job) {
  GList *after = list_dir_names(job->dest_dir);
  for (GList *l = after; l; l = l->next) {
    const char *name = l->data;
    gboolean was_there = FALSE;
    for (GList *b = job->before; b; b = b->next) {
      if (g_strcmp0((const char *)b->data, name) == 0) {
        was_there = TRUE;
        break;
      }
    }
    if (!was_there) {
      char *p = g_build_filename(job->dest_dir, name, NULL);
      remove_path_recursive(p);
      g_free(p);
    }
  }
  g_list_free_full(after, g_free);
}

static void maybe_finalize_extract(ExtractJob *job) {
  if (!job->child_exited || !job->io_done)
    return;

  if (g_spawn_check_wait_status(job->wait_status, NULL)) {
    finish_extract_success(job);
    extract_job_destroy(job);
    return;
  }

  cleanup_partial_extract(job);

  if (extract_stderr_suggests_password(job->kind, job->stderr_buf->str)) {
    gboolean was_retry = job->password && *job->password;
    prompt_for_extract_password(job, was_retry);
    return;
  }

  set_status(
      job->app,
      "Extract karte waqt error aayi (tool ka exit status check karein)");
  g_rmdir(job->dest_dir);
  fileview_refresh(job->app);
  extract_job_destroy(job);
}

static void on_extract_process_exit(GPid pid, gint wait_status,
                                    gpointer user_data) {
  ExtractJob *job = user_data;
  g_spawn_close_pid(pid);
  job->wait_status = wait_status;
  job->child_exited = TRUE;
  maybe_finalize_extract(job);
}

static gboolean on_extract_stderr_io(GIOChannel *channel, GIOCondition cond,
                                     gpointer user_data) {
  ExtractJob *job = user_data;
  gchar buf[512];

  while (TRUE) {
    gsize bytes_read = 0;
    GError *err = NULL;
    GIOStatus st =
        g_io_channel_read_chars(channel, buf, sizeof(buf), &bytes_read, &err);
    if (err)
      g_error_free(err);
    if (st == G_IO_STATUS_NORMAL && bytes_read > 0) {
      g_string_append_len(job->stderr_buf, buf, (gssize)bytes_read);
      if (bytes_read < sizeof(buf))
        break;
      continue;
    }
    break;
  }

  if (cond & (G_IO_HUP | G_IO_ERR)) {
    job->io_done = TRUE;
    maybe_finalize_extract(job);
    return FALSE; /* watch hata do */
  }
  return TRUE;
}

static void start_extract_attempt(ExtractJob *job) {
  GPtrArray *arr =
      build_extract_argv(job->kind, job->zip_use_7z, job->tool,
                         job->archive_path, job->dest_dir, job->password);

  if (job->stderr_buf)
    g_string_free(job->stderr_buf, TRUE);
  job->stderr_buf = g_string_new(NULL);
  job->child_exited = FALSE;
  job->io_done = FALSE;

  gint stderr_fd = -1;
  GError *err = NULL;
  gboolean ok = g_spawn_async_with_pipes(
      job->dest_dir, (char **)arr->pdata, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
      redirect_stdin_devnull, NULL, &job->pid, NULL, NULL, &stderr_fd, &err);
  g_ptr_array_free(arr, TRUE);

  if (!ok) {
    char *msg =
        g_strdup_printf("Extract fail: %s", err ? err->message : "unknown");
    set_status(job->app, msg);
    g_free(msg);
    if (err)
      g_error_free(err);
    extract_job_destroy(job);
    return;
  }

  set_status(job->app, "Extract ho raha hai...");

  GIOChannel *channel = g_io_channel_unix_new(stderr_fd);
  g_io_channel_set_encoding(channel, NULL, NULL);
  g_io_channel_set_close_on_unref(channel, TRUE);
  g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL);
  g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_extract_stderr_io,
                 job);
  g_io_channel_unref(channel);

  g_child_watch_add(job->pid, on_extract_process_exit, job);
}

static char *strip_archive_extension(const char *base) {
  static const char *multi_part[] = {".tar.gz", ".tar.bz2", ".tar.xz", NULL};
  for (int i = 0; multi_part[i]; i++) {
    if (g_str_has_suffix(base, multi_part[i]))
      return g_strndup(base, strlen(base) - strlen(multi_part[i]));
  }
  static const char *single_part[] = {".zip",  ".7z",  ".rar", ".tgz",
                                      ".tbz2", ".txz", ".tar", NULL};
  for (int i = 0; single_part[i]; i++) {
    if (g_str_has_suffix(base, single_part[i]))
      return g_strndup(base, strlen(base) - strlen(single_part[i]));
  }
  return g_strdup(base);
}

static char *unique_extract_dir_path(const char *parent_dir, const char *stem) {
  char *candidate = g_build_filename(parent_dir, stem, NULL);
  if (!g_file_test(candidate, G_FILE_TEST_EXISTS))
    return candidate;
  g_free(candidate);

  for (int n = 1;; n++) {
    char *name = g_strdup_printf("%s(%d)", stem, n);
    candidate = g_build_filename(parent_dir, name, NULL);
    g_free(name);
    if (!g_file_test(candidate, G_FILE_TEST_EXISTS))
      return candidate;
    g_free(candidate);
  }
}

void do_extract_archive(AppWidgets *app, const char *archive_path) {
  ArchiveKind kind = archive_kind_for(archive_path);
  gboolean zip_use_7z = FALSE;
  char *tool = NULL;

  if (kind == ARCHIVE_ZIP) {
    tool = g_find_program_in_path("7z");
    if (tool) {
      zip_use_7z = TRUE;
    } else {
      tool = g_find_program_in_path("unzip");
    }
  } else {
    tool = g_find_program_in_path(tool_name_for_kind(kind));
  }

  if (!tool) {
    set_status(app, "Extract ke liye zaroori program nahi mila "
                    "(unzip/tar/7z/unrar install karein)");
    return;
  }

  char *parent_dir = g_path_get_dirname(archive_path);
  char *base_name = g_path_get_basename(archive_path);
  char *stem = strip_archive_extension(base_name);
  g_free(base_name);

  char *dest_dir = unique_extract_dir_path(parent_dir, stem);
  g_free(stem);
  g_free(parent_dir);

  if (g_mkdir_with_parents(dest_dir, 0755) != 0) {
    set_status(app, "Extract ke liye folder nahi ban saka");
    g_free(dest_dir);
    g_free(tool);
    return;
  }

  ExtractJob *job = g_new0(ExtractJob, 1);
  job->app = app;
  job->archive_path = g_strdup(archive_path);
  job->dest_dir = dest_dir; /* ownership job ko mil gaya */
  job->kind = kind;
  job->zip_use_7z = zip_use_7z;
  job->tool = tool;
  job->password = NULL;
  job->before = list_dir_names(job->dest_dir); /* naya folder hai, khali hogi */

  start_extract_attempt(job);
}

char *preferred_archive_extension(void) {
  char *zip = g_find_program_in_path("zip");
  if (zip) {
    g_free(zip);
    return g_strdup(".zip");
  }
  char *tar = g_find_program_in_path("tar");
  if (tar) {
    g_free(tar);
    return g_strdup(".tar.gz");
  }
  return g_strdup(".zip"); /* neither found; do_compress will report this */
}
