#include "deletejob.h"
#include "fileview.h"
#include "fileview_internal.h"
#include "sidebar.h"
#include "undo.h"
#include <gio/gio.h>

typedef struct {
  AppWidgets *app;
  GList *paths;
  char *post_target;
  gboolean cleanup_trashinfo;
  gboolean is_empty_trash;
  gboolean is_trash; /* TRUE = "Move to Trash" (g_file_trash per top-level
                      * item) instead of permanent recursive delete */

  GtkWidget *dialog;
  GtkWidget *progress_bar;
  GtkWidget *label;
  GtkWidget *cancel_button;

  GCancellable *cancellable;
  GThread *thread;
  guint timeout_id;
  guint show_timeout_id;

  GMutex mutex;
  guint done; /* items deleted so far (for the label) */
  char *current_name;
  guint ok_count;
  guint fail_count;
  gboolean finished;
  char *error_msg;

  /* is_trash ke successful items ke liye UndoAction* (worker thread mein
   * banaye jate hain -- sirf malloc/strdup, koi GTK/app state nahi chhedte,
   * isliye thread-safe). Main thread (on_delete_finished) inhe join ke
   * baad app->undo_stack par push karta hai. */
  GList *undo_items;
} DeleteJob;

/* ---- worker thread ----------------------------------------------------- */

static gboolean delete_recursive_job(DeleteJob *job, GFile *f, GError **error) {
  if (g_cancellable_is_cancelled(job->cancellable))
    return TRUE; /* stop quietly, not a failure */

  GFileType type =
      g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, job->cancellable);
  if (type == G_FILE_TYPE_DIRECTORY) {
    GFileEnumerator *en = g_file_enumerate_children(
        f, "standard::name", G_FILE_QUERY_INFO_NONE, job->cancellable, error);
    if (!en)
      return FALSE;
    gboolean ok = TRUE;
    GFileInfo *info;
    while (ok && !g_cancellable_is_cancelled(job->cancellable) &&
           (info = g_file_enumerator_next_file(en, job->cancellable, NULL)) !=
               NULL) {
      GFile *child = g_file_get_child(f, g_file_info_get_name(info));
      ok = delete_recursive_job(job, child, error);
      g_object_unref(child);
      g_object_unref(info);
    }
    g_file_enumerator_close(en, NULL, NULL);
    g_object_unref(en);
    if (!ok)
      return FALSE;
  }

  if (g_cancellable_is_cancelled(job->cancellable))
    return TRUE;

  char *base = g_file_get_basename(f);
  g_mutex_lock(&job->mutex);
  g_free(job->current_name);
  job->current_name = base; /* ownership transferred */
  job->done++;
  g_mutex_unlock(&job->mutex);

  return g_file_delete(f, job->cancellable, error);
}

static gpointer worker_main(gpointer data) {
  DeleteJob *job = data;
  guint ok = 0, fail = 0;

  for (GList *l = job->paths;
       l && !g_cancellable_is_cancelled(job->cancellable); l = l->next) {
    const char *p = l->data;
    GFile *f = g_file_new_for_path(p);
    GError *err = NULL;
    gboolean item_ok;

    if (job->is_trash) {
      char *base = g_file_get_basename(f);
      g_mutex_lock(&job->mutex);
      g_free(job->current_name);
      job->current_name = base; /* ownership transferred */
      job->done++;
      g_mutex_unlock(&job->mutex);

      item_ok = g_file_trash(f, job->cancellable, &err);

      if (item_ok) {
        /* g_file_trash() apna target khud chunta hai (freedesktop trash
         * spec ke mutabiq -- normally files/<basename>, lekin naam takraye
         * to suffix laga sakta hai). Hum sirf "seedha" (no-collision) case
         * ke liye undo track karte hain -- agar wo jagah maujood nahi to
         * chup-chap is item ka undo skip kar dete hain (trash to phir bhi
         * ho chuka, sirf Ctrl+Z is ek item ko wapas nahi la sakega). */
        char *trash_dir = trash_files_dir();
        if (trash_dir) {
          char *expected = g_build_filename(trash_dir, base, NULL);
          if (g_file_test(expected, G_FILE_TEST_EXISTS)) {
            UndoAction *u = g_new0(UndoAction, 1);
            u->type = UNDO_DELETE;
            u->src = g_strdup(p);
            u->dest = g_strdup(expected);
            job->undo_items = g_list_prepend(job->undo_items, u);
          }
          g_free(expected);
          g_free(trash_dir);
        }
      }
    } else {
      item_ok = delete_recursive_job(job, f, &err);
    }

    if (item_ok) {
      ok++;
      if (job->cleanup_trashinfo) {
        char *base = g_path_get_basename(p);
        char *files_dir = g_path_get_dirname(p);
        char *info_dir = info_dir_for_files_dir(files_dir);
        if (info_dir) {
          char *info_path = g_strdup_printf("%s/%s.trashinfo", info_dir, base);
          g_remove(info_path);
          g_free(info_path);
          g_free(info_dir);
        }
        g_free(files_dir);
        g_free(base);
      }
    } else {
      fail++;
      g_mutex_lock(&job->mutex);
      if (!job->error_msg && err)
        job->error_msg = g_strdup(err->message);
      g_mutex_unlock(&job->mutex);
    }
    if (err)
      g_error_free(err);
    g_object_unref(f);
  }

  g_mutex_lock(&job->mutex);
  job->ok_count = ok;
  job->fail_count = fail;
  job->finished = TRUE;
  g_mutex_unlock(&job->mutex);
  return NULL;
}

/* ---- GUI ----------------------------------------------------------------*/

static void delete_job_free(DeleteJob *job) {
  g_list_free_full(job->paths, g_free);
  g_free(job->post_target);
  g_free(job->current_name);
  g_free(job->error_msg);
  if (job->cancellable)
    g_object_unref(job->cancellable);
  g_mutex_clear(&job->mutex);
  g_free(job);
}

static void on_delete_finished(DeleteJob *job) {
  g_thread_join(job->thread);
  job->thread = NULL;

  gboolean cancelled = g_cancellable_is_cancelled(job->cancellable);

  char *msg;
  if (job->is_empty_trash) {
    msg = cancelled ? g_strdup("Trash khali karna cancel kiya")
                    : g_strdup("Trash khali kar diya");
  } else if (cancelled) {
    msg = g_strdup_printf("%s cancel kiya (%u item ho chuke the)",
                          job->is_trash ? "Trash mein bhejna" : "Delete",
                          job->ok_count);
  } else if (job->fail_count > 0) {
    msg = job->error_msg
              ? g_strdup_printf("%u item %s, %u fail (%s)", job->ok_count,
                                job->is_trash ? "trash mein bhej diye"
                                              : "delete ho gaye",
                                job->fail_count, job->error_msg)
              : g_strdup_printf("%u item %s, %u fail", job->ok_count,
                                job->is_trash ? "trash mein bhej diye"
                                              : "delete ho gaye",
                                job->fail_count);
  } else {
    msg = g_strdup_printf("%u item %s", job->ok_count,
                          job->is_trash ? "trash mein bhej diye"
                                        : "delete ho gaye");
  }
  fileview_set_status(job->app, msg);
  g_free(msg);

  if (!job->is_empty_trash) {
    g_free(job->app->pending_select_path);
    job->app->pending_select_path = job->post_target;
    job->post_target = NULL; /* ownership moved */
  }

  if (job->undo_items) {
    /* worker ne prepend kiya tha (reverse order mein) -- seedha karo taake
     * batch andar UNDO_BATCH ka "aakhri pehle undo" logic sahi order mein
     * chale. */
    job->undo_items = g_list_reverse(job->undo_items);
    if (job->undo_items->next) {
      UndoAction *batch = g_new0(UndoAction, 1);
      batch->type = UNDO_BATCH;
      batch->batch = job->undo_items;
      undo_push(job->app, batch);
    } else {
      /* Ek hi item -- batch mein lapetne ki zaroorat nahi, isse undo ke
       * baad wo item dobara select bhi ho jata hai (batch is behavior
       * nahi deta). */
      undo_push(job->app, (UndoAction *)job->undo_items->data);
      g_list_free(job->undo_items);
    }
    job->undo_items = NULL;
  }

  fileview_refresh(job->app);
  sidebar_update_trash_icon(job->app);

  if (job->show_timeout_id) {
    g_source_remove(job->show_timeout_id);
    job->show_timeout_id = 0;
  }
  if (job->dialog)
    gtk_widget_destroy(job->dialog);

  delete_job_free(job);
}

static gboolean update_ui_cb(gpointer data) {
  DeleteJob *job = data;

  g_mutex_lock(&job->mutex);
  guint done = job->done;
  gboolean finished = job->finished;
  char *name = g_strdup(job->current_name);
  g_mutex_unlock(&job->mutex);

  if (job->progress_bar)
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(job->progress_bar));

  if (job->label) {
    char *text =
        name && *name
            ? g_strdup_printf("Delete ho raha hai... (%u)\n%s", done, name)
            : g_strdup_printf("Delete ho raha hai... (%u)", done);
    gtk_label_set_text(GTK_LABEL(job->label), text);
    g_free(text);
  }
  g_free(name);

  if (finished) {
    job->timeout_id = 0;
    on_delete_finished(job);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void on_cancel_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  DeleteJob *job = user_data;
  g_cancellable_cancel(job->cancellable);
  gtk_widget_set_sensitive(job->cancel_button, FALSE);
  if (job->label)
    gtk_label_set_text(GTK_LABEL(job->label), "Rok raha hai...");
}

static void build_progress_dialog(DeleteJob *job) {
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Delete ho raha hai", GTK_WINDOW(job->app->window),
      GTK_DIALOG_DESTROY_WITH_PARENT, NULL, NULL);
  gtk_window_set_deletable(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 360, -1);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 12);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_add(GTK_CONTAINER(content), box);

  GtkWidget *label = gtk_label_new("Delete ho raha hai...");
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  GtkWidget *progress = gtk_progress_bar_new();
  gtk_box_pack_start(GTK_BOX(box), progress, FALSE, FALSE, 0);

  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
  gtk_widget_set_halign(cancel_btn, GTK_ALIGN_END);
  gtk_box_pack_start(GTK_BOX(box), cancel_btn, FALSE, FALSE, 0);
  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), job);

  job->dialog = dialog;
  job->progress_bar = progress;
  job->label = label;
  job->cancel_button = cancel_btn;

  gtk_widget_show_all(dialog);
}

/* Chote deletes (thodi si files) ke liye dialog flash na ho isliye, ise
 * sirf tab dikhate hain jab operation 250ms mein khatam na ho paye. */
static gboolean show_dialog_if_still_running(gpointer data) {
  DeleteJob *job = data;
  job->show_timeout_id = 0;
  g_mutex_lock(&job->mutex);
  gboolean finished = job->finished;
  g_mutex_unlock(&job->mutex);
  if (!finished)
    build_progress_dialog(job);
  return G_SOURCE_REMOVE;
}

static void start_delete_job(DeleteJob *job) {
  job->cancellable = g_cancellable_new();
  g_mutex_init(&job->mutex);
  job->current_name = g_strdup("");

  job->thread = g_thread_new("deletejob", worker_main, job);
  job->timeout_id = g_timeout_add(80, update_ui_cb, job);
  job->show_timeout_id = g_timeout_add(250, show_dialog_if_still_running, job);
}

/* ---- public API ---------------------------------------------------------*/

void deletejob_start_permanent(AppWidgets *app, GList *paths,
                               char *post_target) {
  if (!paths) {
    g_free(post_target);
    return;
  }

  DeleteJob *job = g_new0(DeleteJob, 1);
  job->app = app;
  job->paths = paths;
  job->post_target = post_target;
  job->cleanup_trashinfo = TRUE;
  job->is_empty_trash = FALSE;

  start_delete_job(job);
}

void deletejob_start_trash(AppWidgets *app, GList *paths, char *post_target) {
  if (!paths) {
    g_free(post_target);
    return;
  }

  DeleteJob *job = g_new0(DeleteJob, 1);
  job->app = app;
  job->paths = paths;
  job->post_target = post_target;
  job->cleanup_trashinfo = FALSE;
  job->is_empty_trash = FALSE;
  job->is_trash = TRUE;

  start_delete_job(job);
}

void deletejob_start_empty_trash(AppWidgets *app, GList *paths) {
  if (!paths)
    return;

  DeleteJob *job = g_new0(DeleteJob, 1);
  job->app = app;
  job->paths = paths;
  job->post_target = NULL;
  job->cleanup_trashinfo = FALSE;
  job->is_empty_trash = TRUE;

  start_delete_job(job);
}
