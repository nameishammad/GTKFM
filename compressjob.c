#include "compressjob.h"
#include "fileview_internal.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <signal.h>

static guint64 total_size_of_paths(GList *paths) {
  guint64 total = 0;
  for (GList *l = paths; l; l = l->next) {
    const char *path = (const char *)l->data;
    GFile *f = g_file_new_for_path(path);
    if (g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL) ==
        G_FILE_TYPE_DIRECTORY) {
      guint count_ignore = 0;
      count_recursive(f, &total, &count_ignore);
    } else {
      GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                          G_FILE_QUERY_INFO_NONE, NULL, NULL);
      if (info) {
        total += (guint64)g_file_info_get_size(info);
        g_object_unref(info);
      }
    }
    g_object_unref(f);
  }
  return total;
}

typedef struct {
  AppWidgets *app;
  char *archive_name;
  char *archive_full_path;
  char *target_dir;
  guint64 original_size;
  gboolean is_zip_temp_file_format;
  GHashTable *pre_existing_names;
  char *zip_temp_name;
  GtkWidget *dialog;
  GtkWidget *progress_bar;
  GtkWidget *label;
  GtkWidget *cancel_button;

  GPid pid;
  GIOChannel *out_channel;
  guint io_watch_id;
  guint progress_timer_id;
  gboolean cancelled;
} CompressJob;

static void set_compress_progress_fraction(CompressJob *job, double frac) {
  if (frac > 0.99)
    frac = 0.99; /* poora 100% sirf process khatam hone par dikhate hain */
  if (frac < 0.0)
    frac = 0.0;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(job->progress_bar), frac);

  int pct_val = (int)(frac * 100.0 + 0.5);
  if (pct_val < 1)
    pct_val = 1;
  char *pct = g_strdup_printf("%d%%", pct_val);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(job->progress_bar), pct);
  g_free(pct);
}

static GHashTable *snapshot_dir_names(const char *dir) {
  GHashTable *set =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GDir *d = g_dir_open(dir, 0, NULL);
  if (!d)
    return set;
  const char *name;
  while ((name = g_dir_read_name(d)) != NULL)
    g_hash_table_add(set, g_strdup(name));
  g_dir_close(d);
  return set;
}

static char *find_zip_temp_file(CompressJob *job) {
  GDir *d = g_dir_open(job->target_dir, 0, NULL);
  if (!d)
    return NULL;
  const char *name;
  char *found = NULL;
  while ((name = g_dir_read_name(d)) != NULL) {
    if (g_strcmp0(name, job->archive_name) == 0)
      continue;
    if (g_hash_table_contains(job->pre_existing_names, name))
      continue;
    found = g_strdup(name);
    break;
  }
  g_dir_close(d);
  return found;
}

static gboolean on_compress_progress_tick(gpointer user_data) {
  CompressJob *job = user_data;

  if (job->original_size == 0) {
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(job->progress_bar));
    return G_SOURCE_CONTINUE;
  }

  const char *poll_path = job->archive_full_path;
  char *built_temp_path = NULL;

  if (job->is_zip_temp_file_format) {
    if (!job->zip_temp_name)
      job->zip_temp_name = find_zip_temp_file(job);
    if (job->zip_temp_name) {
      built_temp_path =
          g_build_filename(job->target_dir, job->zip_temp_name, NULL);
      poll_path = built_temp_path;
    } else {
      /* Temp file abhi tak nahi bana (process start ho raha hai) ya
       * pehle hi rename ho chuka -- doosri soorat mein final path khud
       * poll kar lenge, jo neeche wale g_file_query_info se ho jayega. */
    }
  }

  GFile *af = g_file_new_for_path(poll_path);
  GFileInfo *info = g_file_query_info(af, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);
  guint64 current_size = info ? (guint64)g_file_info_get_size(info) : 0;
  if (info)
    g_object_unref(info);
  g_object_unref(af);
  g_free(built_temp_path);

  double frac = (double)current_size / (double)job->original_size;
  set_compress_progress_fraction(job, frac);

  return G_SOURCE_CONTINUE;
}

static gboolean on_compress_output(GIOChannel *channel, GIOCondition condition,
                                   gpointer user_data) {
  CompressJob *job = user_data;

  if (condition & G_IO_IN) {
    gchar *line;
    gsize len;
    GIOStatus status;
    while ((status = g_io_channel_read_line(channel, &line, &len, NULL,
                                            NULL)) == G_IO_STATUS_NORMAL) {
      g_free(line);
    }
    if (status == G_IO_STATUS_EOF) {
      job->io_watch_id = 0;
      return G_SOURCE_REMOVE;
    }
  }

  if (condition & (G_IO_HUP | G_IO_ERR)) {
    job->io_watch_id = 0;
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

static void on_compress_dialog_cancel_response(GtkDialog *dialog, gint response,
                                               gpointer user_data) {
  (void)dialog;
  (void)response;
  CompressJob *job = user_data;
  if (job->cancelled)
    return;
  job->cancelled = TRUE;
  gtk_widget_set_sensitive(job->cancel_button, FALSE);
  gtk_label_set_text(GTK_LABEL(job->label), "Cancel ho raha hai...");
  if (job->pid > 0)
    kill(job->pid, SIGTERM);
}

static void build_compress_progress_dialog(CompressJob *job) {
  GtkWidget *dialog = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Compress ho raha hai");
  gtk_window_set_transient_for(GTK_WINDOW(dialog),
                               GTK_WINDOW(job->app->window));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 360, -1);
  gtk_window_set_deletable(GTK_WINDOW(dialog), FALSE);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_add(GTK_CONTAINER(content), box);

  char *label_text = g_strdup_printf("'%s' ban raha hai...", job->archive_name);
  GtkWidget *label = gtk_label_new(label_text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
  g_free(label_text);

  GtkWidget *progress = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress), TRUE);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), "1%");
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 0.01);
  gtk_box_pack_start(GTK_BOX(box), progress, FALSE, FALSE, 0);

  GtkWidget *cancel_btn =
      gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);

  job->dialog = dialog;
  job->label = label;
  job->progress_bar = progress;
  job->cancel_button = cancel_btn;

  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_compress_dialog_cancel_response), job);

  gtk_widget_show_all(dialog);
}

static void on_compress_child_exit(GPid pid, gint wait_status,
                                   gpointer user_data) {
  CompressJob *job = user_data;
  g_spawn_close_pid(pid);

  if (job->progress_timer_id) {
    g_source_remove(job->progress_timer_id);
    job->progress_timer_id = 0;
  }
  if (job->io_watch_id) {
    g_source_remove(job->io_watch_id);
    job->io_watch_id = 0;
  }
  if (job->out_channel) {
    g_io_channel_shutdown(job->out_channel, FALSE, NULL);
    g_io_channel_unref(job->out_channel);
    job->out_channel = NULL;
  }

  gboolean success =
      !job->cancelled && g_spawn_check_wait_status(wait_status, NULL);

  if (success) {
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(job->progress_bar), 1.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(job->progress_bar), "100%");

    GFile *af = g_file_new_for_path(job->archive_full_path);
    GFileInfo *info = g_file_query_info(af, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    guint64 archive_size = info ? (guint64)g_file_info_get_size(info) : 0;
    if (info)
      g_object_unref(info);
    g_object_unref(af);

    char *msg;
    if (job->original_size > 0) {
      double pct = 100.0 * (double)archive_size / (double)job->original_size;
      msg = g_strdup_printf("'%s' ban gaya (%.0f%% original size)",
                            job->archive_name, pct);
    } else {
      msg = g_strdup_printf("'%s' ban gaya", job->archive_name);
    }
    set_status(job->app, msg);
    g_free(msg);

    g_free(job->app->pending_select_path);
    job->app->pending_select_path = g_strdup(job->archive_full_path);
    fileview_refresh(job->app);
  } else if (job->cancelled) {
    set_status(job->app, "Compress cancel kar diya gaya");
    g_unlink(job->archive_full_path); /* adhoori archive delete kar do */
    if (job->zip_temp_name) {
      char *temp_path =
          g_build_filename(job->target_dir, job->zip_temp_name, NULL);
      g_unlink(temp_path);
      g_free(temp_path);
    }
  } else {
    set_status(job->app, "Archive banate waqt error aayi");
  }

  gtk_widget_destroy(job->dialog);
  g_free(job->archive_name);
  g_free(job->archive_full_path);
  g_free(job->target_dir);
  g_free(job->zip_temp_name);
  if (job->pre_existing_names)
    g_hash_table_destroy(job->pre_existing_names);
  g_free(job);
}

typedef enum {
  FMT_ZIP,
  FMT_TAR,
  FMT_TAR_GZ,
  FMT_TAR_BZ2,
  FMT_TAR_XZ,
  FMT_7Z,
} CompressFormat;

typedef enum {
  ENC_ZIPCRYPTO,
  ENC_AES128,
  ENC_AES192,
  ENC_AES256,
} EncryptionMethod;

static gboolean encryption_is_aes(EncryptionMethod enc) {
  return enc == ENC_AES128 || enc == ENC_AES192 || enc == ENC_AES256;
}

static const char *aes_mem_flag_for(EncryptionMethod enc) {
  switch (enc) {
  case ENC_AES128:
    return "-mem=AES128";
  case ENC_AES192:
    return "-mem=AES192";
  case ENC_AES256:
  default:
    return "-mem=AES256";
  }
}

static const char *ext_for_format(CompressFormat fmt) {
  switch (fmt) {
  case FMT_ZIP:
    return ".zip";
  case FMT_TAR:
    return ".tar";
  case FMT_TAR_GZ:
    return ".tar.gz";
  case FMT_TAR_BZ2:
    return ".tar.bz2";
  case FMT_TAR_XZ:
    return ".tar.xz";
  case FMT_7Z:
    return ".7z";
  }
  return ".zip";
}

static gboolean format_supports_password(CompressFormat fmt) {
  return fmt == FMT_ZIP || fmt == FMT_7Z;
}

static gboolean format_has_encryption_choice(CompressFormat fmt) {
  return fmt == FMT_ZIP;
}

static const char *tool_name_for(CompressFormat fmt, gboolean use_password,
                                 EncryptionMethod enc) {
  if (fmt == FMT_ZIP && use_password && encryption_is_aes(enc))
    return "7z";
  switch (fmt) {
  case FMT_ZIP:
    return "zip";
  case FMT_TAR:
  case FMT_TAR_GZ:
  case FMT_TAR_BZ2:
  case FMT_TAR_XZ:
    return "tar";
  case FMT_7Z:
    return "7z";
  }
  return "zip";
}

static void free_secret(char *s) {
  if (!s)
    return;
  size_t len = strlen(s);
  if (len)
    memset(s, 0, len);
  g_free(s);
}

static void do_compress(AppWidgets *app, GList *paths, const char *base_name,
                        CompressFormat format, const char *password,
                        EncryptionMethod enc) {
  if (!paths || !base_name || !*base_name)
    return;

  char *archive_name =
      g_strdup_printf("%s%s", base_name, ext_for_format(format));

  {
    char *candidate_path =
        g_build_filename(app->current_dir, archive_name, NULL);
    for (int n = 1; g_file_test(candidate_path, G_FILE_TEST_EXISTS); n++) {
      g_free(candidate_path);
      g_free(archive_name);
      archive_name =
          g_strdup_printf("%s(%d)%s", base_name, n, ext_for_format(format));
      candidate_path = g_build_filename(app->current_dir, archive_name, NULL);
    }
    g_free(candidate_path);
  }

  gboolean use_password =
      format_supports_password(format) && password && *password;
  gboolean zip_via_7z_aes =
      format == FMT_ZIP && use_password && encryption_is_aes(enc);

  char *tool = g_find_program_in_path(tool_name_for(format, use_password, enc));
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);

  gboolean uses_7z = zip_via_7z_aes || format == FMT_7Z;
  char *stdbuf_tool =
      (tool && uses_7z) ? g_find_program_in_path("stdbuf") : NULL;

  if (tool) {
    if (stdbuf_tool) {
      g_ptr_array_add(arr, stdbuf_tool);
      g_ptr_array_add(arr, g_strdup("-oL"));
    }
    g_ptr_array_add(arr, tool);
    if (zip_via_7z_aes) {
      g_ptr_array_add(arr, g_strdup("a"));
      g_ptr_array_add(arr, g_strdup("-tzip"));
      g_ptr_array_add(arr, g_strdup(aes_mem_flag_for(enc)));
      g_ptr_array_add(arr, g_strdup_printf("-p%s", password));
      g_ptr_array_add(arr, g_strdup("-bb1"));
      g_ptr_array_add(arr, g_strdup(archive_name));
      for (GList *l = paths; l; l = l->next)
        g_ptr_array_add(arr, g_path_get_basename((const char *)l->data));
    } else {
      switch (format) {
      case FMT_ZIP:
        g_ptr_array_add(arr, g_strdup("-r"));
        if (use_password) {
          g_ptr_array_add(arr, g_strdup("-P"));
          g_ptr_array_add(arr, g_strdup(password));
        }
        g_ptr_array_add(arr, g_strdup(archive_name));
        for (GList *l = paths; l; l = l->next)
          g_ptr_array_add(arr, g_path_get_basename((const char *)l->data));
        break;
      case FMT_TAR:
        g_ptr_array_add(arr, g_strdup("-cvf"));
        g_ptr_array_add(arr, g_strdup(archive_name));
        for (GList *l = paths; l; l = l->next)
          g_ptr_array_add(arr, g_path_get_basename((const char *)l->data));
        break;
      case FMT_TAR_GZ:
        g_ptr_array_add(arr, g_strdup("-cvzf"));
        g_ptr_array_add(arr, g_strdup(archive_name));
        for (GList *l = paths; l; l = l->next)
          g_ptr_array_add(arr, g_path_get_basename((const char *)l->data));
        break;
      case FMT_TAR_BZ2:
        g_ptr_array_add(arr, g_strdup("-cvjf"));
        g_ptr_array_add(arr, g_strdup(archive_name));
        for (GList *l = paths; l; l = l->next)
          g_ptr_array_add(arr, g_path_get_basename((const char *)l->data));
        break;
      case FMT_TAR_XZ:
        g_ptr_array_add(arr, g_strdup("-cvJf"));
        g_ptr_array_add(arr, g_strdup(archive_name));
        for (GList *l = paths; l; l = l->next)
          g_ptr_array_add(arr, g_path_get_basename((const char *)l->data));
        break;
      case FMT_7Z:
        g_ptr_array_add(arr, g_strdup("a"));
        g_ptr_array_add(arr, g_strdup("-bb1"));
        if (use_password) {
          g_ptr_array_add(arr, g_strdup_printf("-p%s", password));
          g_ptr_array_add(arr, g_strdup("-mhe=on"));
        }
        g_ptr_array_add(arr, g_strdup(archive_name));
        for (GList *l = paths; l; l = l->next)
          g_ptr_array_add(arr, g_path_get_basename((const char *)l->data));
        break;
      }
    }
  }

  if (!tool) {
    char *msg = g_strdup_printf(
        "Compress ke liye zaroori program nahi mila -- '%s' install karein",
        tool_name_for(format, use_password, enc));
    set_status(app, msg);
    g_free(msg);
    g_ptr_array_free(arr, TRUE);
    g_free(archive_name);
    return;
  }

  g_ptr_array_add(arr, NULL);

  CompressJob *job = g_new0(CompressJob, 1);
  job->app = app;
  job->archive_name = archive_name; /* ownership moves to job */
  job->archive_full_path =
      g_build_filename(app->current_dir, archive_name, NULL);
  job->target_dir = g_strdup(app->current_dir);
  job->original_size = total_size_of_paths(paths);

  job->is_zip_temp_file_format = (format == FMT_ZIP && !zip_via_7z_aes);
  if (job->is_zip_temp_file_format)
    job->pre_existing_names = snapshot_dir_names(app->current_dir);

  gint stdout_fd = -1;
  gint stderr_fd = -1;
  GPid pid;
  GError *err = NULL;

  gboolean ok =
      g_spawn_async_with_pipes(app->current_dir, (char **)arr->pdata, NULL,
                               G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                   G_SPAWN_STDERR_TO_DEV_NULL,
                               NULL, NULL, &pid, NULL, &stdout_fd, NULL, &err);

  g_ptr_array_free(arr, TRUE);

  if (!ok) {
    char *msg =
        g_strdup_printf("Compress fail: %s", err ? err->message : "unknown");
    set_status(app, msg);
    g_free(msg);
    if (err)
      g_error_free(err);
    g_free(job->archive_name);
    g_free(job->archive_full_path);
    g_free(job->target_dir);
    if (job->pre_existing_names)
      g_hash_table_destroy(job->pre_existing_names);
    g_free(job);
    return;
  }

  job->pid = pid;
  job->out_channel = g_io_channel_unix_new(stdout_fd);
  g_io_channel_set_close_on_unref(job->out_channel, TRUE);
  g_io_channel_set_encoding(job->out_channel, NULL, NULL);
  g_io_channel_set_flags(job->out_channel, G_IO_FLAG_NONBLOCK, NULL);

  build_compress_progress_dialog(job);

  job->io_watch_id = g_io_add_watch(
      job->out_channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_compress_output, job);

  job->progress_timer_id = g_timeout_add(200, on_compress_progress_tick, job);

  set_status(app, "Archive ban raha hai...");
  g_child_watch_add(pid, on_compress_child_exit, job);
}

static char *suggest_archive_base_name(AppWidgets *app, GList *paths) {
  char *base;

  if (g_list_length(paths) == 1)
    base = g_path_get_basename((const char *)paths->data);
  else
    base = g_path_get_basename(app->current_dir);

  char *candidate = g_strdup(base);

  for (int i = 1; ; i++) {
    char *test = g_strdup_printf("%s%s", candidate, ext_for_format(FMT_ZIP));

    char *full = g_build_filename(app->current_dir, test, NULL);

    if (!g_file_test(full, G_FILE_TEST_EXISTS)) {
      g_free(test);
      g_free(full);
      break;
    }

    g_free(test);
    g_free(full);

    g_free(candidate);
    candidate = g_strdup_printf("%s %d", base, i);
  }

  g_free(base);
  return candidate;
}
// static char *suggest_archive_base_name(AppWidgets *app, GList *paths) {
//   if (g_list_length(paths) == 1)
//     return g_path_get_basename((const char *)paths->data);
//   return g_path_get_basename(app->current_dir);
// }

typedef struct {
  AppWidgets *app;
  GtkWidget *name_entry;
  GtkWidget *format_combo;
  GtkWidget *password_entry;
  GtkWidget *encryption_combo;
  GtkWidget *show_password_check;
  GtkWidget *preview_label;
  gboolean have_7z; /* AES-256 option ke liye 7z zaroori hai */
  GList *paths;     /* owned */
} CompressDialogCtx;

static CompressFormat selected_format(CompressDialogCtx *ctx) {
  const char *id =
      gtk_combo_box_get_active_id(GTK_COMBO_BOX(ctx->format_combo));
  return id ? (CompressFormat)atoi(id) : FMT_ZIP;
}

static EncryptionMethod selected_encryption(CompressDialogCtx *ctx) {
  const char *id =
      gtk_combo_box_get_active_id(GTK_COMBO_BOX(ctx->encryption_combo));
  return id ? (EncryptionMethod)atoi(id) : ENC_ZIPCRYPTO;
}

static void update_preview_label(CompressDialogCtx *ctx) {
  const char *name = gtk_entry_get_text(GTK_ENTRY(ctx->name_entry));
  CompressFormat fmt = selected_format(ctx);
  char *text = g_strdup_printf("Archive: %s%s", name, ext_for_format(fmt));
  gtk_label_set_text(GTK_LABEL(ctx->preview_label), text);
  g_free(text);
}

static void on_format_changed(GtkComboBox *combo, gpointer user_data) {
  (void)combo;
  CompressDialogCtx *ctx = user_data;
  CompressFormat fmt = selected_format(ctx);
  gboolean can_password = format_supports_password(fmt);
  gtk_widget_set_sensitive(ctx->password_entry, can_password);
  gtk_widget_set_sensitive(ctx->show_password_check, can_password);
  if (!can_password)
    gtk_entry_set_text(GTK_ENTRY(ctx->password_entry), "");

  gboolean can_choose_enc = format_has_encryption_choice(fmt);
  gtk_widget_set_sensitive(ctx->encryption_combo, can_choose_enc);
  update_preview_label(ctx);
}

static void on_name_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  update_preview_label(user_data);
}

static void on_show_password_toggled(GtkToggleButton *btn, gpointer user_data) {
  CompressDialogCtx *ctx = user_data;
  gtk_entry_set_visibility(GTK_ENTRY(ctx->password_entry),
                           gtk_toggle_button_get_active(btn));
}

static void on_compress_dialog_response(GtkDialog *dialog, gint response,
                                        gpointer user_data) {
  CompressDialogCtx *ctx = user_data;

  if (response == GTK_RESPONSE_OK) {
    const char *name = gtk_entry_get_text(GTK_ENTRY(ctx->name_entry));
    if (name && *name) {
      CompressFormat fmt = selected_format(ctx);
      EncryptionMethod enc = selected_encryption(ctx);
      char *password =
          g_strdup(gtk_entry_get_text(GTK_ENTRY(ctx->password_entry)));
      do_compress(ctx->app, ctx->paths, name, fmt, password, enc);
      free_secret(password);
    }
  }

  g_list_free_full(ctx->paths, g_free);
  gtk_widget_destroy(GTK_WIDGET(dialog));
  g_free(ctx);
}

/* paths ownership moves to this dialog (freed once the dialog closes). */
void compressjob_show_dialog(AppWidgets *app, GList *paths) {
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Compress", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Create", GTK_RESPONSE_OK, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 380, -1);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 10);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 4);

  CompressDialogCtx *ctx = g_new0(CompressDialogCtx, 1);
  ctx->app = app;
  ctx->paths = paths;

  /* --- Name --- */
  GtkWidget *name_label = gtk_label_new("Name:");
  gtk_widget_set_halign(name_label, GTK_ALIGN_START);
  char *suggested_base = suggest_archive_base_name(app, paths);
  GtkWidget *name_entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(name_entry), suggested_base);
  gtk_entry_set_activates_default(GTK_ENTRY(name_entry), TRUE);
  g_free(suggested_base);
  gtk_grid_attach(GTK_GRID(grid), name_label, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), name_entry, 1, 0, 1, 1);
  gtk_widget_set_hexpand(name_entry, TRUE);
  ctx->name_entry = name_entry;

  /* --- Format --- */
  GtkWidget *format_label = gtk_label_new("Format:");
  gtk_widget_set_halign(format_label, GTK_ALIGN_START);
  GtkWidget *format_combo = gtk_combo_box_text_new();
  gtk_grid_attach(GTK_GRID(grid), format_label, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), format_combo, 1, 1, 1, 1);
  ctx->format_combo = format_combo;

  gboolean any_format = FALSE;
  char *zip_tool = g_find_program_in_path("zip");
  if (zip_tool) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "0",
                              "ZIP (.zip)");
    any_format = TRUE;
    g_free(zip_tool);
  }
  char *tar_tool = g_find_program_in_path("tar");
  if (tar_tool) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "1",
                              "TAR -- uncompressed (.tar)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "2",
                              "TAR.GZ (.tar.gz)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "3",
                              "TAR.BZ2 (.tar.bz2)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "4",
                              "TAR.XZ (.tar.xz)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "1",
                              "TAR (.tar)");
    any_format = TRUE;
    g_free(tar_tool);
  }
  char *sevenzip_tool = g_find_program_in_path("7z");
  gboolean have_7z = sevenzip_tool != NULL;
  ctx->have_7z = have_7z;
  if (have_7z) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(format_combo), "5",
                              "7-Zip (.7z)");
    any_format = TRUE;
    g_free(sevenzip_tool);
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(format_combo), 0);

  /* --- Password --- */
  GtkWidget *password_label = gtk_label_new("Password:");
  gtk_widget_set_halign(password_label, GTK_ALIGN_START);
  GtkWidget *password_entry = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(password_entry), FALSE);
  gtk_entry_set_input_purpose(GTK_ENTRY(password_entry),
                              GTK_INPUT_PURPOSE_PASSWORD);
  gtk_entry_set_placeholder_text(GTK_ENTRY(password_entry),
                                 "(optional -- ZIP/7-Zip only)");
  gtk_entry_set_activates_default(GTK_ENTRY(password_entry), TRUE);
  gtk_grid_attach(GTK_GRID(grid), password_label, 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), password_entry, 1, 2, 1, 1);
  ctx->password_entry = password_entry;

  GtkWidget *show_password_check =
      gtk_check_button_new_with_label("Password dikhayen");
  gtk_grid_attach(GTK_GRID(grid), show_password_check, 1, 3, 1, 1);
  ctx->show_password_check = show_password_check;

  GtkWidget *encryption_label = gtk_label_new("Encryption:");
  gtk_widget_set_halign(encryption_label, GTK_ALIGN_START);
  GtkWidget *encryption_combo = gtk_combo_box_text_new();
  if (have_7z) {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(encryption_combo), "1",
                              "AES-128");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(encryption_combo), "2",
                              "AES-192");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(encryption_combo), "3",
                              "AES-256");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(encryption_combo), "3");
  } else {
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(encryption_combo), "0",
                              "ZipCrypto (7z na mila, AES nahi ban sakta)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(encryption_combo), 0);
  }
  gtk_grid_attach(GTK_GRID(grid), encryption_label, 0, 4, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), encryption_combo, 1, 4, 1, 1);
  ctx->encryption_combo = encryption_combo;

  /* --- Live "Archive: name.ext" preview --- */
  GtkWidget *preview_label = gtk_label_new("");
  gtk_widget_set_halign(preview_label, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(preview_label),
                              "dim-label");
  gtk_grid_attach(GTK_GRID(grid), preview_label, 0, 5, 2, 1);
  ctx->preview_label = preview_label;

  gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_OK,
                                    any_format);
  if (!any_format) {
    gtk_entry_set_text(GTK_ENTRY(name_entry), "(zip/tar/7z install karein)");
    gtk_widget_set_sensitive(name_entry, FALSE);
    gtk_widget_set_sensitive(format_combo, FALSE);
    gtk_widget_set_sensitive(password_entry, FALSE);
    gtk_widget_set_sensitive(show_password_check, FALSE);
    gtk_widget_set_sensitive(encryption_combo, FALSE);
  }

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  gtk_widget_show_all(dialog);

  update_preview_label(ctx);
  on_format_changed(GTK_COMBO_BOX(format_combo), ctx);

  g_signal_connect(format_combo, "changed", G_CALLBACK(on_format_changed), ctx);
  g_signal_connect(name_entry, "changed", G_CALLBACK(on_name_changed), ctx);
  g_signal_connect(show_password_check, "toggled",
                   G_CALLBACK(on_show_password_toggled), ctx);
  g_signal_connect(dialog, "response", G_CALLBACK(on_compress_dialog_response),
                   ctx);
}
