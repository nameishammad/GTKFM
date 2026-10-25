#include "copyjob.h"
#include "fileview.h"
#include "undo.h"
#include <gio/gio.h>
#include <string.h>

typedef enum { JOB_PENDING, JOB_RUNNING, JOB_PAUSED, JOB_DONE } JobState;

typedef struct {
  AppWidgets *app;
  GList *paths;
  char *dest_dir;
  gboolean is_move;
  GtkWidget *row;
  GtkWidget *progress_bar;
  GtkWidget *label_file;
  GtkWidget *label_size;
  GtkWidget *label_speed;
  GtkWidget *button_toggle;
  GtkWidget *button_close;

  JobState state;
  gboolean stop_requested;
  gboolean remove_requested;

  GCancellable *cancellable;
  GThread *thread;
  guint timeout_id;

  GMutex mutex;
  guint64 total_bytes;
  guint64 done_bytes;
  char *current_name;
  guint ok_count;
  guint fail_count;
  gboolean finished;
  gboolean sizing;
  GList *dest_names;
  /* dest_names ke saath 1:1 (usi order mein) parallel list -- har
   * successfully copy/move hue top-level item ka full source path. Undo
   * (Ctrl+Z) ke liye on_worker_finished mein consume hoti hai. */
  GList *ok_src_paths;
  GList *ok_dest_paths;
  GMutex conflict_mutex;
  GCond conflict_cond;
  gboolean conflict_ready;
  gint conflict_action;
  char *conflict_basename;
  char *conflict_new_name;
  gboolean skip_all;
  gboolean overwrite_all;
  gboolean rename_all;
  gint64 start_time;
  guint64 last_done_bytes;
  gint64 last_time;
  double speed_mbps;
  guint64 space_total;
  guint64 space_free;
  char *last_error_msg;
} CopyJob;

typedef enum {
  CONFLICT_SKIP,
  CONFLICT_SKIP_ALL,
  CONFLICT_OVERWRITE,
  CONFLICT_OVERWRITE_ALL,
  CONFLICT_RENAME,
  CONFLICT_RENAME_ALL,
  CONFLICT_CANCEL
} ConflictAction;

static GtkWidget *g_jobs_window = NULL;
static GtkWidget *g_jobs_box = NULL;
static GList *g_jobs = NULL; /* list of CopyJob* */

/* ---- size calculation (runs on worker thread) ------------------------- */
static char *format_size(guint64 bytes) {
  if (bytes < 1024)
    return g_strdup_printf("%lu B", bytes);

  if (bytes < 1024ULL * 1024) {
    return g_strdup_printf("%.2f KB", bytes / 1024.0);
  }

  if (bytes < 1024ULL * 1024 * 1024) {
    return g_strdup_printf("%.2f MB", bytes / (1024.0 * 1024.0));
  }

  if (bytes < 1024ULL * 1024 * 1024 * 1024) {
    return g_strdup_printf("%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
  }

  return g_strdup_printf("%.2f TB",
                         bytes / (1024.0 * 1024.0 * 1024.0 * 1024.0));
}

static guint64 compute_size(GFile *f, GCancellable *cancellable) {
  GFileInfo *info = g_file_query_info(
      f, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
  if (!info)
    return 0;

  GFileType type = g_file_info_get_file_type(info);
  if (type != G_FILE_TYPE_DIRECTORY) {
    guint64 size = (guint64)g_file_info_get_size(info);
    g_object_unref(info);
    return size;
  }
  g_object_unref(info);

  guint64 total = 0;
  GFileEnumerator *en =
      g_file_enumerate_children(f, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                G_FILE_QUERY_INFO_NONE, cancellable, NULL);
  if (!en)
    return 0;

  GFileInfo *child_info;
  while (!g_cancellable_is_cancelled(cancellable) &&
         (child_info = g_file_enumerator_next_file(en, cancellable, NULL)) !=
             NULL) {
    GFile *child = g_file_enumerator_get_child(en, child_info);
    total += compute_size(child, cancellable);
    g_object_unref(child);
    g_object_unref(child_info);
  }
  g_file_enumerator_close(en, NULL, NULL);
  g_object_unref(en);
  return total;
}

/* ---- recursive copy with progress tracking (runs on worker thread) ---- */

typedef struct {
  CopyJob *job;
  guint64 base;
} ProgressCtx;

static void file_progress_cb(goffset current_num_bytes, goffset total_num_bytes,
                             gpointer user_data) {
  (void)total_num_bytes;
  ProgressCtx *ctx = user_data;
  g_mutex_lock(&ctx->job->mutex);
  ctx->job->done_bytes = ctx->base + (guint64)current_num_bytes;
  g_mutex_unlock(&ctx->job->mutex);
}

static char *unique_dest_path(const char *dir, const char *basename) {
  char *candidate = g_build_filename(dir, basename, NULL);
  if (!g_file_test(candidate, G_FILE_TEST_EXISTS))
    return candidate;
  g_free(candidate);

  char *stem = g_strdup(basename);
  char *dot = strrchr(stem, '.');
  char *ext = NULL;
  if (dot && dot != stem) {
    ext = g_strdup(dot);
    *dot = '\0';
  }

  char *result = NULL;
  for (int n = 1;; n++) {
    result = g_strdup_printf("%s/%s (copy %d)%s", dir, stem, n, ext ? ext : "");
    if (!g_file_test(result, G_FILE_TEST_EXISTS))
      break;
    g_free(result);
  }
  g_free(stem);
  g_free(ext);
  return result;
}

static gboolean show_conflict_dialog_idle(gpointer data) {
  CopyJob *job = data;

  g_mutex_lock(&job->conflict_mutex);
  char *basename = g_strdup(job->conflict_basename);
  g_mutex_unlock(&job->conflict_mutex);

  GtkWidget *dialog = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "File pehle se maujood hai");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(g_jobs_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);

  gtk_dialog_add_button(GTK_DIALOG(dialog), "Skip", CONFLICT_SKIP);
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Skip All", CONFLICT_SKIP_ALL);
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Overwrite", CONFLICT_OVERWRITE);
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Overwrite All",
                        CONFLICT_OVERWRITE_ALL);
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Rename", CONFLICT_RENAME);
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Rename All", CONFLICT_RENAME_ALL);
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", CONFLICT_CANCEL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), CONFLICT_SKIP);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_add(GTK_CONTAINER(content), box);

  char *msg = g_strdup_printf(
      "\"%s\" is pehle se destination folder mein maujood hai.\n"
      "Aap kya karna chahenge?",
      basename);
  GtkWidget *label = gtk_label_new(msg);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
  g_free(msg);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), basename);
  gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

  gtk_widget_show_all(dialog);
  gint response = gtk_dialog_run(GTK_DIALOG(dialog));

  g_mutex_lock(&job->conflict_mutex);
  job->conflict_action =
      (response >= CONFLICT_SKIP && response <= CONFLICT_CANCEL)
          ? response
          : CONFLICT_CANCEL;
  g_free(job->conflict_new_name);
  job->conflict_new_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
  job->conflict_ready = TRUE;
  g_cond_signal(&job->conflict_cond);
  g_mutex_unlock(&job->conflict_mutex);

  gtk_widget_destroy(dialog);
  g_free(basename);
  return G_SOURCE_REMOVE;
}

static gboolean resolve_conflict(CopyJob *job, const char *dest_dir,
                                 const char *basename, char **out_path) {
  if (job->overwrite_all) {
    *out_path = g_build_filename(dest_dir, basename, NULL);
    return TRUE;
  }

  char *candidate = g_build_filename(dest_dir, basename, NULL);

  if (!g_file_test(candidate, G_FILE_TEST_EXISTS)) {
    *out_path = candidate;
    return TRUE;
  }

  g_free(candidate);

  if (job->skip_all)
    return FALSE;

  if (job->rename_all) {
    *out_path = unique_dest_path(dest_dir, basename);
    return TRUE;
  }

  if (g_cancellable_is_cancelled(job->cancellable))
    return FALSE;

  g_mutex_lock(&job->conflict_mutex);
  g_free(job->conflict_basename);
  job->conflict_basename = g_strdup(basename);
  job->conflict_ready = FALSE;
  g_mutex_unlock(&job->conflict_mutex);

  g_idle_add(show_conflict_dialog_idle, job);

  g_mutex_lock(&job->conflict_mutex);
  while (!job->conflict_ready)
    g_cond_wait(&job->conflict_cond, &job->conflict_mutex);
  ConflictAction action = (ConflictAction)job->conflict_action;
  char *new_name = job->conflict_new_name;
  job->conflict_new_name = NULL;
  g_mutex_unlock(&job->conflict_mutex);

  switch (action) {
  case CONFLICT_SKIP:
    g_free(new_name);
    return FALSE;
  case CONFLICT_SKIP_ALL:
    job->skip_all = TRUE;
    g_free(new_name);
    return FALSE;
  case CONFLICT_OVERWRITE:
    g_free(new_name);
    *out_path = g_build_filename(dest_dir, basename, NULL);
    return TRUE;
  case CONFLICT_OVERWRITE_ALL:
    job->overwrite_all = TRUE;
    g_free(new_name);
    *out_path = g_build_filename(dest_dir, basename, NULL);
    return TRUE;
  case CONFLICT_RENAME: {
    const char *use_name = (new_name && *new_name) ? new_name : basename;
    char *path = g_build_filename(dest_dir, use_name, NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
      g_free(path);
      path = unique_dest_path(dest_dir, use_name);
    }
    g_free(new_name);
    *out_path = path;
    return TRUE;
  }
  case CONFLICT_RENAME_ALL: {
    job->rename_all = TRUE;
    g_free(new_name);
    *out_path = unique_dest_path(dest_dir, basename);
    return TRUE;
  }
  case CONFLICT_CANCEL:
  default:
    g_cancellable_cancel(job->cancellable);
    g_free(new_name);
    return FALSE;
  }
}

static gboolean show_space_warning_dialog_idle(gpointer data) {
  CopyJob *job = data;

  char *total_str = format_size(job->space_total);
  char *free_str = format_size(job->space_free);

  GtkWidget *dialog = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Free space kam hai");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(g_jobs_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);

  gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", CONFLICT_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Jari Rakhein", CONFLICT_OVERWRITE);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), CONFLICT_CANCEL);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);

  char *msg = g_strdup_printf(
      "Destination par sirf %s free space hai, lekin is copy/move ke liye "
      "taqreeban %s chahiye.\n"
      "Jari rakha to disk full ho sakti hai aur kuch files copy hone se "
      "reh sakti hain.\n\nJari rakhein?",
      free_str, total_str);
  GtkWidget *label = gtk_label_new(msg);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_container_add(GTK_CONTAINER(content), label);
  g_free(msg);
  g_free(total_str);
  g_free(free_str);

  gtk_widget_show_all(dialog);
  gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  g_mutex_lock(&job->conflict_mutex);
  job->conflict_action =
      (response == CONFLICT_OVERWRITE) ? CONFLICT_OVERWRITE : CONFLICT_CANCEL;
  job->conflict_ready = TRUE;
  g_cond_signal(&job->conflict_cond);
  g_mutex_unlock(&job->conflict_mutex);

  return G_SOURCE_REMOVE;
}

static gboolean confirm_low_space(CopyJob *job, guint64 total,
                                  guint64 free_bytes) {
  job->space_total = total;
  job->space_free = free_bytes;

  g_mutex_lock(&job->conflict_mutex);
  job->conflict_ready = FALSE;
  g_mutex_unlock(&job->conflict_mutex);

  g_idle_add(show_space_warning_dialog_idle, job);

  g_mutex_lock(&job->conflict_mutex);
  while (!job->conflict_ready)
    g_cond_wait(&job->conflict_cond, &job->conflict_mutex);
  gboolean cont = (job->conflict_action == CONFLICT_OVERWRITE);
  g_mutex_unlock(&job->conflict_mutex);

  return cont;
}

static gboolean copy_recursive_tracked(CopyJob *job, GFile *src,
                                       const char *dest_dir, const char *name,
                                       guint64 *running_base, GError **error,
                                       char **out_final_name) {
  if (g_cancellable_is_cancelled(job->cancellable)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    return FALSE;
  }

  GFileType type =
      g_file_query_file_type(src, G_FILE_QUERY_INFO_NONE, job->cancellable);

  if (type == G_FILE_TYPE_DIRECTORY) {
    char *dst_path = g_build_filename(dest_dir, name, NULL);
    GFile *dst = g_file_new_for_path(dst_path);
    if (!g_file_make_directory(dst, job->cancellable, error)) {
      if (*error && (*error)->code == G_IO_ERROR_EXISTS) {
        g_clear_error(error);
      } else {
        g_object_unref(dst);
        g_free(dst_path);
        return FALSE;
      }
    }
    GFileEnumerator *en = g_file_enumerate_children(
        src, "standard::name", G_FILE_QUERY_INFO_NONE, job->cancellable, error);
    if (!en) {
      g_object_unref(dst);
      g_free(dst_path);
      return FALSE;
    }

    gboolean ok = TRUE;
    GFileInfo *info;
    while (ok && (info = g_file_enumerator_next_file(en, job->cancellable,
                                                     NULL)) != NULL) {
      const char *child_name = g_file_info_get_name(info);
      GFile *child_src = g_file_get_child(src, child_name);
      ok = copy_recursive_tracked(job, child_src, dst_path, child_name,
                                  running_base, error, NULL);
      g_object_unref(child_src);
      g_object_unref(info);
    }
    g_file_enumerator_close(en, NULL, NULL);
    g_object_unref(en);
    // g_object_unref(dst);
    // g_free(dst_path);
    // return ok;
    g_object_unref(dst);

    if (ok && out_final_name)
      *out_final_name = g_strdup(name);
    // if (ok) {
    //   char *actual_name = g_strdup(name);
    //   job->dest_names = g_list_append(job->dest_names, actual_name);
    //   if (out_final_name)
    //     *out_final_name = g_strdup(name);
    // }

    g_free(dst_path);
    return ok;
  }

  char *final_path = NULL;
  if (!resolve_conflict(job, dest_dir, name, &final_path)) {
    GFileInfo *fi = g_file_query_info(src, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                      G_FILE_QUERY_INFO_NONE, NULL, NULL);
    guint64 fsize = fi ? (guint64)g_file_info_get_size(fi) : 0;
    if (fi)
      g_object_unref(fi);
    *running_base += fsize;
    g_mutex_lock(&job->mutex);
    job->done_bytes = *running_base;
    g_mutex_unlock(&job->mutex);
    return TRUE;
  }

  GFile *dst = g_file_new_for_path(final_path);
  ProgressCtx ctx = {job, *running_base};
  gboolean ok = g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, job->cancellable,
                            file_progress_cb, &ctx, error);

  GFileInfo *fi = g_file_query_info(src, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                    G_FILE_QUERY_INFO_NONE, NULL, NULL);
  guint64 fsize = fi ? (guint64)g_file_info_get_size(fi) : 0;
  if (fi)
    g_object_unref(fi);
  *running_base += fsize;

  // g_mutex_lock(&job->mutex);
  // job->done_bytes = *running_base;
  // g_mutex_unlock(&job->mutex);
  //
  // g_object_unref(dst);
  // g_free(final_path);
  // return ok;
  g_mutex_lock(&job->mutex);
  job->done_bytes = *running_base;
  g_mutex_unlock(&job->mutex);

  // if (ok) {
  //   char *actual_name = g_path_get_basename(final_path);
  //   job->dest_names = g_list_append(job->dest_names, actual_name);
  //   if (out_final_name)
  //     *out_final_name = g_path_get_basename(final_path);
  // }

  if (ok && out_final_name)
    *out_final_name = g_path_get_basename(final_path);

  g_object_unref(dst);
  g_free(final_path);
  return ok;
}

/* ---- worker thread ------------------------------------------------------*/

static gpointer worker_main(gpointer data) {
  CopyJob *job = data;

  guint64 total = 0;
  for (GList *l = job->paths;
       l && !g_cancellable_is_cancelled(job->cancellable); l = l->next) {
    GFile *f = g_file_new_for_path((const char *)l->data);
    total += compute_size(f, job->cancellable);
    g_object_unref(f);
  }
  g_mutex_lock(&job->mutex);
  job->total_bytes = total;
  job->sizing = FALSE;
  g_mutex_unlock(&job->mutex);

  if (!g_cancellable_is_cancelled(job->cancellable) && total > 0) {
    GFile *dest_gfile = g_file_new_for_path(job->dest_dir);
    GFileInfo *fs_info = g_file_query_filesystem_info(
        dest_gfile, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, job->cancellable, NULL);
    if (fs_info) {
      if (g_file_info_has_attribute(fs_info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
        guint64 free_bytes = g_file_info_get_attribute_uint64(
            fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        if (total > free_bytes) {
          if (!confirm_low_space(job, total, free_bytes)) {
            job->last_error_msg =
                g_strdup("Free space kam hai -- user ne cancel kar diya");
            g_cancellable_cancel(job->cancellable);
          }
        }
      }
      g_object_unref(fs_info);
    }
    g_object_unref(dest_gfile);
  }

  guint64 running_base = 0;
  for (GList *l = job->paths; l; l = l->next) {
    if (g_cancellable_is_cancelled(job->cancellable))
      break;

    const char *src_path = (const char *)l->data;
    char *base = g_path_get_basename(src_path);

    g_mutex_lock(&job->mutex);
    g_free(job->current_name);
    job->current_name = g_strdup(base);
    g_mutex_unlock(&job->mutex);

    GFile *src = g_file_new_for_path(src_path);
    GError *err = NULL;
    gboolean ok;

    if (job->is_move) {

      GFileInfo *fi = g_file_query_info(src, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        G_FILE_QUERY_INFO_NONE, NULL, NULL);
      guint64 fsize = fi ? (guint64)g_file_info_get_size(fi) : 0;
      if (fi)
        g_object_unref(fi);

      char *dst_path = g_build_filename(job->dest_dir, base, NULL);
      GFile *dst = g_file_new_for_path(dst_path);
      ProgressCtx ctx = {job, running_base};
      ok = g_file_move(src, dst, G_FILE_COPY_NONE, job->cancellable,
                       file_progress_cb, &ctx, &err);
      if (!ok && err &&
          (err->code == G_IO_ERROR_WOULD_MERGE ||
           err->code == G_IO_ERROR_EXISTS)) {
        g_clear_error(&err);
        char *top_final_name = NULL;
        ok = copy_recursive_tracked(job, src, job->dest_dir, base,
                                    &running_base, &err, &top_final_name);
        // if (ok && top_final_name) {
        //   job->ok_src_paths =
        //       g_list_append(job->ok_src_paths, g_strdup(src_path));
        //   job->ok_dest_paths = g_list_append(
        //       job->ok_dest_paths,
        //       g_build_filename(job->dest_dir, top_final_name, NULL));
        // }
        if (ok && top_final_name) {

          job->dest_names =
          g_list_append(job->dest_names, g_strdup(top_final_name));

          job->ok_src_paths =
          g_list_append(job->ok_src_paths, g_strdup(src_path));

          job->ok_dest_paths =
          g_list_append(job->ok_dest_paths,
                        g_build_filename(job->dest_dir,
                                         top_final_name,
                                         NULL));
        }

        g_free(top_final_name);
      } else if (ok) {
        running_base += fsize;

        g_mutex_lock(&job->mutex);
        job->done_bytes = running_base;
        g_mutex_unlock(&job->mutex);

        char *actual = g_path_get_basename(dst_path);
        job->dest_names = g_list_append(job->dest_names, actual);
        job->ok_src_paths =
            g_list_append(job->ok_src_paths, g_strdup(src_path));
        job->ok_dest_paths =
            g_list_append(job->ok_dest_paths, g_strdup(dst_path));
      }
      /*else if (ok) {
        running_base += fsize;
        g_mutex_lock(&job->mutex);
        job->done_bytes = running_base;
        g_mutex_unlock(&job->mutex);
      }*/
      g_object_unref(dst);
      g_free(dst_path);
    } else {
      char *top_final_name = NULL;

      ok = copy_recursive_tracked(job, src, job->dest_dir, base,
                                  &running_base, &err,
                                  &top_final_name);

      if (ok && top_final_name) {

        /* sirf top level copied item select hoga */
        job->dest_names =
        g_list_append(job->dest_names,
                      g_strdup(top_final_name));

        job->ok_src_paths =
        g_list_append(job->ok_src_paths,
                      g_strdup(src_path));

        job->ok_dest_paths =
        g_list_append(job->ok_dest_paths,
                      g_build_filename(job->dest_dir,
                                       top_final_name,
                                       NULL));
      }

      g_free(top_final_name);
    }
    // else {
    //   char *top_final_name = NULL;
    //   ok = copy_recursive_tracked(job, src, job->dest_dir, base, &running_base,
    //                               &err, &top_final_name);
    //   if (ok && top_final_name) {
    //     job->ok_src_paths =
    //         g_list_append(job->ok_src_paths, g_strdup(src_path));
    //     job->ok_dest_paths = g_list_append(
    //         job->ok_dest_paths,
    //         g_build_filename(job->dest_dir, top_final_name, NULL));
    //   }
    //   g_free(top_final_name);
    // }

    g_mutex_lock(&job->mutex);
    if (ok)
      job->ok_count++;
    else
      job->fail_count++;
    g_mutex_unlock(&job->mutex);

    // if (ok)
    //   job->dest_names = g_list_append(job->dest_names, g_strdup(base));
    // if (ok) {
    //   char *actual = g_path_get_basename(dst_path);
    //   job->dest_names = g_list_append(job->dest_names, actual);
    // }

    if (err) {
      if (!job->last_error_msg)
        job->last_error_msg = g_strdup(err->message);
      g_error_free(err);
    }
    g_object_unref(src);
    g_free(base);
  }

  g_mutex_lock(&job->mutex);
  job->finished = TRUE;
  g_mutex_unlock(&job->mutex);
  return NULL;
}

/* ---- UI: shared jobs window + one row per job --------------------------*/

static gboolean any_job_running(void) {
  for (GList *l = g_jobs; l; l = l->next) {
    CopyJob *j = l->data;
    if (j->state == JOB_RUNNING)
      return TRUE;
  }
  return FALSE;
}

static gboolean on_jobs_window_delete(GtkWidget *w, GdkEvent *event,
                                      gpointer data) {
  (void)event;
  (void)data;
  gtk_widget_hide(w);
  return TRUE;
}

static void ensure_jobs_window(AppWidgets *app) {
  if (g_jobs_window)
    return;

  g_jobs_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(g_jobs_window), "Copy / Move Jobs");
  gtk_window_set_transient_for(GTK_WINDOW(g_jobs_window),
                               GTK_WINDOW(app->window));
  gtk_window_set_type_hint(GTK_WINDOW(g_jobs_window),
                           GDK_WINDOW_TYPE_HINT_UTILITY);
  gtk_window_set_default_size(GTK_WINDOW(g_jobs_window), 440, -1);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(g_jobs_window), outer);

  GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scroller, -1, 320);
  gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);

  g_jobs_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(g_jobs_box), 6);
  gtk_container_add(GTK_CONTAINER(scroller), g_jobs_box);

  gtk_widget_show_all(outer);

  g_signal_connect(g_jobs_window, "delete-event",
                   G_CALLBACK(on_jobs_window_delete), NULL);
}

static void remove_job_completely(CopyJob *job) {
  if (job->timeout_id) {
    g_source_remove(job->timeout_id);
    job->timeout_id = 0;
  }

  g_jobs = g_list_remove(g_jobs, job);
  if (job->row)
    gtk_widget_destroy(job->row);

  g_list_free_full(job->paths, g_free);
  g_free(job->dest_dir);
  g_free(job->current_name);
  g_list_free_full(job->dest_names, g_free);
  g_list_free_full(job->ok_src_paths, g_free);
  g_list_free_full(job->ok_dest_paths, g_free);
  g_free(job->conflict_basename);
  g_free(job->conflict_new_name);
  g_free(job->last_error_msg);
  g_mutex_clear(&job->mutex);
  g_mutex_clear(&job->conflict_mutex);
  g_cond_clear(&job->conflict_cond);
  if (job->cancellable)
    g_object_unref(job->cancellable);
  g_free(job);

  if (!g_jobs && g_jobs_window)
    gtk_widget_hide(g_jobs_window);
}

static void start_job(CopyJob *job) {
  job->cancellable = g_cancellable_new();
  job->total_bytes = 0;
  job->done_bytes = 0;
  g_free(job->current_name);
  job->current_name = g_strdup("");
  job->ok_count = 0;
  job->fail_count = 0;
  job->finished = FALSE;
  job->sizing = TRUE;
  job->skip_all = FALSE;
  job->overwrite_all = FALSE;
  job->rename_all = FALSE;
  g_list_free_full(job->dest_names, g_free);
  job->dest_names = NULL;
  g_list_free_full(job->ok_src_paths, g_free);
  job->ok_src_paths = NULL;
  g_list_free_full(job->ok_dest_paths, g_free);
  job->ok_dest_paths = NULL;
  job->stop_requested = FALSE;
  job->remove_requested = FALSE;
  g_free(job->last_error_msg);
  job->last_error_msg = NULL;
  job->start_time = g_get_monotonic_time();
  job->last_time = job->start_time;
  job->last_done_bytes = 0;
  job->speed_mbps = 0.0;
  job->state = JOB_RUNNING;

  gtk_button_set_label(GTK_BUTTON(job->button_toggle), "Stop");
  gtk_widget_set_sensitive(job->button_toggle, TRUE);
  gtk_widget_set_sensitive(job->button_close, TRUE);
  gtk_label_set_text(GTK_LABEL(job->label_file), "Shuru ho raha hai...");
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(job->progress_bar), 0.0);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(job->progress_bar), "0%");

  job->thread = g_thread_new("copyjob", worker_main, job);
}

static gboolean update_ui_cb(gpointer data);

static void start_job_real(CopyJob *job) {
  start_job(job);
  job->timeout_id = g_timeout_add(120, update_ui_cb, job);
}

static gboolean auto_remove_job_cb(gpointer data) {
  CopyJob *job = data;
  job->timeout_id = 0;
  remove_job_completely(job);
  return G_SOURCE_REMOVE;
}

static void on_worker_finished(CopyJob *job) {
  g_thread_join(job->thread);
  job->thread = NULL;
  job->timeout_id = 0;

  char *msg;
  if (job->fail_count > 0 && job->last_error_msg) {
    msg = g_strdup_printf("%s: %u ho gaye, %u fail (%s)",
                          job->is_move ? "Move" : "Paste", job->ok_count,
                          job->fail_count, job->last_error_msg);
  } else {
    msg = g_strdup_printf("%s: %u ho gaye, %u fail",
                          job->is_move ? "Move" : "Paste", job->ok_count,
                          job->fail_count);
  }
  fileview_set_status(job->app, msg);
  g_free(msg);

  {
    GList *ls = job->ok_src_paths, *ld = job->ok_dest_paths;
    GList *batch = NULL;
    for (; ls && ld; ls = ls->next, ld = ld->next) {
      UndoAction *sub = g_new0(UndoAction, 1);
      sub->type = job->is_move ? UNDO_MOVE : UNDO_COPY;
      sub->src = g_strdup((const char *)ls->data);
      sub->dest = g_strdup((const char *)ld->data);
      batch = g_list_append(batch, sub);
    }
    if (batch) {
      if (batch->next) {
        UndoAction *action = g_new0(UndoAction, 1);
        action->type = UNDO_BATCH;
        action->batch = batch;
        undo_push(job->app, action);
      } else {
        /* Sirf ek item -- batch wrapper ki zaroorat nahi. */
        undo_push(job->app, (UndoAction *)batch->data);
        g_list_free(batch);
      }
    }
  }

  if (job->dest_names) {
    GList *dest_paths = NULL;
    for (GList *l = job->dest_names; l; l = l->next) {
      g_print("DEST SELECT: %s\n", (char *)l->data);
    }
    for (GList *l = job->dest_names; l; l = l->next)
      dest_paths = g_list_append(
          dest_paths,
          g_build_filename(job->dest_dir, (const char *)l->data, NULL));
    g_list_free_full(job->app->pending_select_paths, g_free);
    g_free(job->app->pending_select_path);
    job->app->pending_select_path = NULL;
    job->app->pending_select_paths = dest_paths; /* transfers ownership */
  }
  fileview_refresh(job->app);

  if (job->remove_requested) {
    remove_job_completely(job);
    return;
  }

  if (job->stop_requested) {
    job->state = JOB_PAUSED;
    job->stop_requested = FALSE;
    if (job->cancellable) {
      g_object_unref(job->cancellable);
      job->cancellable = NULL;
    }
    gtk_button_set_label(GTK_BUTTON(job->button_toggle), "Start");
    gtk_widget_set_sensitive(job->button_toggle, TRUE);
    gtk_label_set_text(GTK_LABEL(job->label_file),
                       "Roka gaya -- jaari rakhne ke liye Start dabayen");
    return;
  }

  job->state = JOB_DONE;
  if (job->cancellable) {
    g_object_unref(job->cancellable);
    job->cancellable = NULL;
  }
  gtk_button_set_label(GTK_BUTTON(job->button_toggle), "Done");
  gtk_widget_set_sensitive(job->button_toggle, FALSE);
  g_list_free_full(job->paths, g_free);
  job->paths = NULL;

  job->timeout_id = g_timeout_add(1500, auto_remove_job_cb, job);
}

static gboolean update_ui_cb(gpointer data) {
  CopyJob *job = data;

  g_mutex_lock(&job->mutex);
  guint64 done = job->done_bytes;
  guint64 total = job->total_bytes;
  gint64 now = g_get_monotonic_time();

  if (now - job->last_time >= G_USEC_PER_SEC) {
    guint64 diff = done - job->last_done_bytes;
    double seconds = (now - job->last_time) / 1000000.0;
    job->speed_mbps = (diff / 1024.0 / 1024.0) / seconds;
    job->last_done_bytes = done;
    job->last_time = now;
  }
  gboolean finished = job->finished;
  gboolean sizing = job->sizing;
  char *name = g_strdup(job->current_name);
  g_mutex_unlock(&job->mutex);

  if (sizing) {
    /* Total size not known yet -- pulse instead of sitting at 0%, so
     * a big (e.g. 1TB+) selection doesn't look frozen while it scans. */
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(job->progress_bar));
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(job->progress_bar),
                              "Size calculate ho raha hai...");
    gtk_label_set_text(GTK_LABEL(job->label_file),
                       "Size calculate ho raha hai...");
    gtk_label_set_text(GTK_LABEL(job->label_size), "");
    gtk_label_set_text(GTK_LABEL(job->label_speed), "");
    g_free(name);
    return G_SOURCE_CONTINUE;
  }

  double frac =
      total > 0 ? (double)done / (double)total : (finished ? 1.0 : 0.0);
  if (frac > 1.0)
    frac = 1.0;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(job->progress_bar), frac);

  char *pct = g_strdup_printf("%d%%", (int)(frac * 100.0 + 0.5));
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(job->progress_bar), pct);
  g_free(pct);

  char *done_str = format_size(done);
  char *total_str = format_size(total);
  char *size_text =
      g_strdup_printf("%s / %s copy ho gaya", done_str, total_str);
  gtk_label_set_text(GTK_LABEL(job->label_size), size_text);
  g_free(done_str);
  g_free(total_str);
  g_free(size_text);

  char *speed_text = g_strdup_printf("Speed: %.2f MB/s", job->speed_mbps);
  gtk_label_set_text(GTK_LABEL(job->label_speed), speed_text);
  g_free(speed_text);

  if (!finished) {
    char *file_text = g_strdup_printf("Copy ho raha hai: %s", name);
    gtk_label_set_text(GTK_LABEL(job->label_file), file_text);
    g_free(file_text);
  }
  g_free(name);

  if (finished) {
    on_worker_finished(job);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void on_toggle_clicked(GtkButton *btn, gpointer data) {
  (void)btn;
  CopyJob *job = data;

  if (job->state == JOB_RUNNING) {
    job->stop_requested = TRUE;
    g_cancellable_cancel(job->cancellable);
    gtk_widget_set_sensitive(job->button_toggle, FALSE);
    gtk_label_set_text(GTK_LABEL(job->label_file), "Rok raha hai...");
  } else if (job->state == JOB_PENDING || job->state == JOB_PAUSED) {
    start_job_real(job);
  }
}

static void on_close_clicked(GtkButton *btn, gpointer data) {
  (void)btn;
  CopyJob *job = data;

  if (job->state == JOB_RUNNING) {
    job->remove_requested = TRUE;
    g_cancellable_cancel(job->cancellable);
    gtk_widget_set_sensitive(job->button_toggle, FALSE);
    gtk_widget_set_sensitive(job->button_close, FALSE);
    gtk_label_set_text(GTK_LABEL(job->label_file), "Cancel ho raha hai...");
  } else {
    /* PENDING, PAUSED or DONE: no worker thread to wait for. */
    remove_job_completely(job);
  }
}

static void build_job_row(CopyJob *job, gboolean will_autostart) {
  GtkWidget *frame = gtk_frame_new(NULL);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
  gtk_container_add(GTK_CONTAINER(frame), outer);

  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  GtkWidget *label_file = gtk_label_new(
      will_autostart ? "Shuru ho raha hai..."
                     : (job->is_move ? "Move: queue mein (Start dabayen)"
                                     : "Copy: queue mein (Start dabayen)"));
  gtk_label_set_xalign(GTK_LABEL(label_file), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(label_file), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(label_file, TRUE);
  gtk_box_pack_start(GTK_BOX(top), label_file, TRUE, TRUE, 0);

  GtkWidget *button_toggle =
      gtk_button_new_with_label(will_autostart ? "Stop" : "Start");
  GtkWidget *button_close = gtk_button_new_from_icon_name(
      "window-close-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(button_close, "Hatayen");
  gtk_box_pack_start(GTK_BOX(top), button_toggle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(top), button_close, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(outer), top, FALSE, FALSE, 0);

  GtkWidget *progress = gtk_progress_bar_new();
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress), TRUE);
  gtk_box_pack_start(GTK_BOX(outer), progress, FALSE, FALSE, 0);

  GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *label_size = gtk_label_new("0 B / 0 B");
  gtk_label_set_xalign(GTK_LABEL(label_size), 0.0);
  gtk_widget_set_hexpand(label_size, TRUE);
  GtkWidget *label_speed = gtk_label_new("Speed: 0 MB/s");
  gtk_label_set_xalign(GTK_LABEL(label_speed), 1.0);
  gtk_box_pack_start(GTK_BOX(bottom), label_size, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bottom), label_speed, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(outer), bottom, FALSE, FALSE, 0);

  job->row = frame;
  job->label_file = label_file;
  job->progress_bar = progress;
  job->label_size = label_size;
  job->label_speed = label_speed;
  job->button_toggle = button_toggle;
  job->button_close = button_close;

  g_signal_connect(button_toggle, "clicked", G_CALLBACK(on_toggle_clicked),
                   job);
  g_signal_connect(button_close, "clicked", G_CALLBACK(on_close_clicked), job);
}

/* ---- public API ---------------------------------------------------------*/

void copyjob_start(AppWidgets *app, GList *src_paths, const char *dest_dir,
                   gboolean is_move) {
  if (!src_paths)
    return;

  ensure_jobs_window(app);

  CopyJob *job = g_new0(CopyJob, 1);
  job->app = app;
  job->paths = src_paths;
  job->dest_dir = g_strdup(dest_dir);
  job->is_move = is_move;
  g_mutex_init(&job->mutex);
  job->current_name = g_strdup("");
  g_mutex_init(&job->conflict_mutex);
  g_cond_init(&job->conflict_cond);
  job->state = JOB_PENDING;

  gboolean will_autostart = !any_job_running();

  build_job_row(job, will_autostart);
  gtk_box_pack_start(GTK_BOX(g_jobs_box), job->row, FALSE, FALSE, 0);
  gtk_widget_show_all(job->row);
  g_jobs = g_list_append(g_jobs, job);

  gtk_widget_show(g_jobs_window);
  gtk_window_present(GTK_WINDOW(g_jobs_window));

  if (will_autostart) {
    start_job_real(job);
  } else {
    fileview_set_status(
        app, is_move
                 ? "Move queue mein daal diya -- iski row par Start dabayen"
                 : "Copy queue mein daal diya -- iski row par Start dabayen");
  }
}
