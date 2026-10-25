#include "renamejob.h"
#include "fileview_internal.h"
#include "undo.h"
#include <gio/gio.h>
#include <string.h>

typedef enum { RENAME_MODE_FIND_REPLACE, RENAME_MODE_NUMBERING } RenameMode;

enum {
  RCOL_OLD_NAME,
  RCOL_NEW_NAME,
  RCOL_OLD_PATH,
  RCOL_FG,
  RCOL_HAS_CONFLICT,
  RN_COLS
};

typedef struct {
  AppWidgets *app;
  GList *paths;

  GtkWidget *dialog;
  GtkListStore *store;

  RenameMode mode;

  GtkWidget *mode_find_radio;
  GtkWidget *mode_numbering_radio;

  /* "Find and Replace" ke widgets */
  GtkWidget *find_replace_box;
  GtkWidget *find_entry;
  GtkWidget *replace_entry;
  GtkWidget *regex_check;
  GtkWidget *case_check;

  /* "Numbering" ke widgets */
  GtkWidget *numbering_box;
  GtkWidget *name_entry;
  GtkWidget *sep_combo;
  GtkWidget *start_spin;
  GtkWidget *digits_spin;
  GtkWidget *keep_ext_check;

  GtkWidget *rename_button;
  GtkWidget *status_label;
} RenameJobUI;

/* ---- naye naam banane wale helpers -------------------------------------- */

static char *find_replace_name(RenameJobUI *ui, const char *basename) {
  const char *find_text = gtk_entry_get_text(GTK_ENTRY(ui->find_entry));
  const char *replace_text = gtk_entry_get_text(GTK_ENTRY(ui->replace_entry));
  gboolean use_regex =
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->regex_check));
  gboolean match_case =
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->case_check));

  if (!find_text || !*find_text)
    return g_strdup(basename);

  if (use_regex) {
    GRegexCompileFlags flags = match_case ? 0 : G_REGEX_CASELESS;
    GError *err = NULL;
    GRegex *re = g_regex_new(find_text, flags, 0, &err);
    if (!re) {
      if (err)
        g_error_free(err);
      return g_strdup(basename);
    }
    GError *err2 = NULL;
    char *result = g_regex_replace(re, basename, -1, 0, replace_text, 0, &err2);
    g_regex_unref(re);
    if (!result) {
      if (err2)
        g_error_free(err2);
      return g_strdup(basename);
    }
    return result;
  }

  GString *out = g_string_new(NULL);
  size_t flen = strlen(find_text);
  const char *p = basename;
  while (*p) {
    gboolean matches = match_case
                           ? (strncmp(p, find_text, flen) == 0)
                           : (g_ascii_strncasecmp(p, find_text, flen) == 0);
    if (matches) {
      g_string_append(out, replace_text);
      p += flen;
    } else {
      g_string_append_c(out, *p);
      p++;
    }
  }
  return g_string_free(out, FALSE);
}

static char *numbering_name(RenameJobUI *ui, const char *basename, int index) {
  const char *base = gtk_entry_get_text(GTK_ENTRY(ui->name_entry));
  int start = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui->start_spin));
  int digits =
      gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui->digits_spin));
  gboolean keep_ext =
      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->keep_ext_check));

  char *num_str = g_strdup_printf("%0*d", digits, start + index);

  char *ext = NULL;
  if (keep_ext) {
    const char *dot = strrchr(basename, '.');
    if (dot && dot != basename)
      ext = g_strdup(dot);
  }

  char *result;
  if (base && *base) {
    const char *sep = "";
    if (ui->sep_combo) {
      const char *active_id =
          gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui->sep_combo));
      if (g_strcmp0(active_id, "space") == 0)
        sep = " ";
      else if (g_strcmp0(active_id, "underscore") == 0)
        sep = "_";
      else if (g_strcmp0(active_id, "dash") == 0)
        sep = "-";
    }
    result = g_strdup_printf("%s%s%s%s", base, sep, num_str, ext ? ext : "");
  } else {
    result = g_strdup_printf("%s%s", num_str, ext ? ext : "");
  }
  g_free(num_str);
  g_free(ext);
  return result;
}

static char *generate_new_name(RenameJobUI *ui, const char *basename,
                               int index) {
  if (ui->mode == RENAME_MODE_NUMBERING)
    return numbering_name(ui, basename, index);
  return find_replace_name(ui, basename);
}

/* ---- live preview + conflict detection ---------------------------------- */

static void regenerate_preview(RenameJobUI *ui) {
  guint count = g_list_length(ui->paths);
  if (count == 0)
    return;

  char **new_fulls = g_new0(char *, count);
  GHashTable *count_tbl = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTable *old_to_new = g_hash_table_new(g_str_hash, g_str_equal);

  int i = 0;
  for (GList *l = ui->paths; l; l = l->next, i++) {
    const char *old_path = l->data;
    char *basename = g_path_get_basename(old_path);
    char *new_name = generate_new_name(ui, basename, i);
    char *dir = g_path_get_dirname(old_path);
    new_fulls[i] = g_build_filename(dir, new_name, NULL);
    g_free(dir);
    g_free(basename);
    g_free(new_name);

    g_hash_table_insert(old_to_new, (gpointer)old_path, new_fulls[i]);

    guint c = GPOINTER_TO_UINT(g_hash_table_lookup(count_tbl, new_fulls[i]));
    g_hash_table_insert(count_tbl, new_fulls[i], GUINT_TO_POINTER(c + 1));
  }

  gboolean any_conflict = FALSE;
  GtkTreeIter iter;
  gboolean valid =
      gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->store), &iter);
  i = 0;
  for (GList *l = ui->paths; l && valid; l = l->next, i++,
             valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->store),
                                              &iter)) {
    const char *old_path = l->data;
    const char *new_full = new_fulls[i];
    char *new_name = g_path_get_basename(new_full);

    gboolean conflict = FALSE;
    guint cnt = GPOINTER_TO_UINT(g_hash_table_lookup(count_tbl, new_full));
    if (cnt > 1) {
      conflict = TRUE;
    } else if (strcmp(new_full, old_path) != 0 &&
               g_file_test(new_full, G_FILE_TEST_EXISTS)) {
      const char *other_new = g_hash_table_lookup(old_to_new, new_full);
      if (!other_new || strcmp(other_new, new_full) == 0)
        conflict = TRUE;
    }
    if (conflict)
      any_conflict = TRUE;

    gtk_list_store_set(ui->store, &iter, RCOL_NEW_NAME, new_name, RCOL_FG,
                       conflict ? "#cc0000" : NULL, RCOL_HAS_CONFLICT, conflict,
                       -1);
    g_free(new_name);
  }

  g_hash_table_destroy(count_tbl);
  g_hash_table_destroy(old_to_new);
  for (guint j = 0; j < count; j++)
    g_free(new_fulls[j]);
  g_free(new_fulls);

  gtk_widget_set_sensitive(ui->rename_button, !any_conflict);
  gtk_label_set_text(GTK_LABEL(ui->status_label),
                     any_conflict ? "Kuch naye naam aapas mein ya mojooda file "
                                    "se takra rahe hain (surkh)"
                                  : "");
}

static void on_option_changed(GtkWidget *w, gpointer user_data) {
  (void)w;
  regenerate_preview((RenameJobUI *)user_data);
}

static void on_mode_toggled(GtkToggleButton *btn, gpointer user_data) {
  RenameJobUI *ui = user_data;
  if (!gtk_toggle_button_get_active(btn))
    return; /* sirf abhi active hua radio hi react kare */
  ui->mode = (RenameMode)GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(btn), "rename-mode"));
  gtk_widget_set_visible(ui->find_replace_box,
                         ui->mode == RENAME_MODE_FIND_REPLACE);
  gtk_widget_set_visible(ui->numbering_box, ui->mode == RENAME_MODE_NUMBERING);
  regenerate_preview(ui);
}

static void do_apply_rename(RenameJobUI *ui) {
  guint count = g_list_length(ui->paths);
  if (count == 0)
    return;

  char **old_arr = g_new0(char *, count); /* borrowed, ui->paths ke andar */
  char **new_arr = g_new0(char *, count); /* owned */
  char **tmp_arr = g_new0(char *, count); /* owned */
  gboolean *needs_rename = g_new0(gboolean, count);

  GtkTreeIter iter;
  gboolean valid =
      gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->store), &iter);
  int idx = 0;
  for (GList *l = ui->paths; l && valid; l = l->next, idx++,
             valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->store),
                                              &iter)) {
    old_arr[idx] = l->data;
    char *new_name = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, RCOL_NEW_NAME,
                       &new_name, -1);
    char *dir = g_path_get_dirname(old_arr[idx]);
    new_arr[idx] = g_build_filename(dir, new_name, NULL);
    g_free(dir);
    g_free(new_name);

    if (strcmp(new_arr[idx], old_arr[idx]) != 0) {
      needs_rename[idx] = TRUE;
      tmp_arr[idx] =
          g_strdup_printf("%s.__gtkfm_rename_tmp_%d", old_arr[idx], idx);
    }
  }

  guint ok = 0, fail = 0;
  char *first_error = NULL;
  char *last_renamed = NULL;
  GList *undo_items = NULL;

  /* Phase 1: old -> temp */
  gboolean *phase1_ok = g_new0(gboolean, count);
  for (guint i = 0; i < count; i++) {
    if (!needs_rename[i])
      continue;
    GFile *src = g_file_new_for_path(old_arr[i]);
    GFile *dst = g_file_new_for_path(tmp_arr[i]);
    GError *err = NULL;
    gboolean moved =
        g_file_move(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
    g_object_unref(src);
    g_object_unref(dst);
    if (moved) {
      phase1_ok[i] = TRUE;
    } else {
      fail++;
      if (!first_error && err)
        first_error = g_strdup(err->message);
    }
    if (err)
      g_error_free(err);
  }

  /* Phase 2: temp -> naya asal naam */
  for (guint i = 0; i < count; i++) {
    if (!needs_rename[i] || !phase1_ok[i])
      continue;
    GFile *src = g_file_new_for_path(tmp_arr[i]);
    GFile *dst = g_file_new_for_path(new_arr[i]);
    GError *err = NULL;
    gboolean moved =
        g_file_move(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
    g_object_unref(src);
    g_object_unref(dst);
    if (moved) {
      ok++;
      g_free(last_renamed);
      last_renamed = g_strdup(new_arr[i]);

      UndoAction *u = g_new0(UndoAction, 1);
      u->type = UNDO_RENAME;
      u->src = g_strdup(old_arr[i]);
      u->dest = g_strdup(new_arr[i]);
      undo_items = g_list_prepend(undo_items, u);
    } else {
      fail++;
      if (!first_error && err)
        first_error = g_strdup(err->message);
    }
    if (err)
      g_error_free(err);
  }

  char *msg;
  if (fail > 0) {
    msg = first_error
              ? g_strdup_printf("%u file rename ho gayi, %u fail (%s)", ok,
                                fail, first_error)
              : g_strdup_printf("%u file rename ho gayi, %u fail", ok, fail);
  } else if (ok == 0) {
    msg = g_strdup("Koi naam badla nahi gaya");
  } else {
    msg = g_strdup_printf("%u file rename ho gayi", ok);
  }
  fileview_set_status(ui->app, msg);
  g_free(msg);
  g_free(first_error);

  if (undo_items) {
    undo_items = g_list_reverse(undo_items);
    if (undo_items->next) {
      UndoAction *batch = g_new0(UndoAction, 1);
      batch->type = UNDO_BATCH;
      batch->batch = undo_items;
      undo_push(ui->app, batch);
    } else {
      undo_push(ui->app, (UndoAction *)undo_items->data);
      g_list_free(undo_items);
    }
  }

  g_free(ui->app->pending_select_path);
  ui->app->pending_select_path =
      last_renamed; /* ownership move, NULL ho sakta hai */

  fileview_refresh(ui->app);

  for (guint i = 0; i < count; i++) {
    g_free(new_arr[i]);
    g_free(tmp_arr[i]);
  }
  g_free(old_arr);
  g_free(new_arr);
  g_free(tmp_arr);
  g_free(needs_rename);
  g_free(phase1_ok);
}

static GList *reorder_paths_like_view(AppWidgets *app, GList *paths) {
  if (!paths || !paths->next)
    return paths; /* 0 ya 1 item -- order ka koi matlab nahi */

  GHashTable *set = g_hash_table_new(g_str_hash, g_str_equal);
  for (GList *l = paths; l; l = l->next)
    g_hash_table_add(set, l->data);

  GList *ordered = NULL;
  GtkTreeIter iter;
  gboolean valid =
      gtk_tree_model_get_iter_first(GTK_TREE_MODEL(app->file_store), &iter);
  while (valid) {
    char *path = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_PATH, &path,
                       -1);
    if (path && g_hash_table_contains(set, path))
      ordered = g_list_prepend(ordered, path); /* fresh copy, ownership yahan */
    else
      g_free(path);
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(app->file_store), &iter);
  }
  ordered = g_list_reverse(ordered);

  g_hash_table_destroy(set);
  g_list_free_full(paths, g_free); /* purani list + uske strings ab fazool */
  return ordered;
}

/* ---- dialog lifecycle ----------------------------------------------------*/

static void renamejob_ui_free(RenameJobUI *ui) {
  g_list_free_full(ui->paths, g_free);
  g_free(ui);
}

static void on_dialog_response(GtkDialog *dialog, gint response,
                               gpointer user_data) {
  RenameJobUI *ui = user_data;
  if (response == GTK_RESPONSE_OK)
    do_apply_rename(ui);
  gtk_widget_destroy(GTK_WIDGET(dialog));
  renamejob_ui_free(ui);
}

/* ---- UI banana ------------------------------------------------------------*/

static GtkWidget *labeled_row(GtkWidget *grid, int row, const char *label_text,
                              GtkWidget *widget) {
  GtkWidget *label = gtk_label_new(label_text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
  gtk_widget_set_hexpand(widget, TRUE);
  gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
  return widget;
}

void renamejob_show_dialog(AppWidgets *app, GList *paths) {
  if (!paths)
    return;

  paths = reorder_paths_like_view(app, paths);

  if (!paths->next) {
    char *name = g_path_get_basename((const char *)paths->data);
    show_rename_dialog(app, (const char *)paths->data, name);
    g_free(name);
    g_list_free_full(paths, g_free);
    return;
  }

  RenameJobUI *ui = g_new0(RenameJobUI, 1);
  ui->app = app;
  ui->paths = paths;
  ui->mode = RENAME_MODE_FIND_REPLACE;

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      "Rename Multiple Files", GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Rename", GTK_RESPONSE_OK, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 480);
  ui->dialog = dialog;

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 10);
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_box_pack_start(GTK_BOX(content), vbox, TRUE, TRUE, 0);

  /* Mode selector */
  GtkWidget *mode_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  ui->mode_find_radio =
      gtk_radio_button_new_with_label(NULL, "Find and Replace");
  ui->mode_numbering_radio = gtk_radio_button_new_with_label_from_widget(
      GTK_RADIO_BUTTON(ui->mode_find_radio), "Numbering");
  g_object_set_data(G_OBJECT(ui->mode_find_radio), "rename-mode",
                    GINT_TO_POINTER(RENAME_MODE_FIND_REPLACE));
  g_object_set_data(G_OBJECT(ui->mode_numbering_radio), "rename-mode",
                    GINT_TO_POINTER(RENAME_MODE_NUMBERING));
  gtk_box_pack_start(GTK_BOX(mode_box), ui->mode_find_radio, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(mode_box), ui->mode_numbering_radio, FALSE, FALSE,
                     0);
  gtk_box_pack_start(GTK_BOX(vbox), mode_box, FALSE, FALSE, 0);

  /* "Find and Replace" options */
  GtkWidget *fr_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(fr_grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(fr_grid), 8);
  ui->find_replace_box = fr_grid;

  ui->find_entry = gtk_entry_new();
  labeled_row(fr_grid, 0, "Find:", ui->find_entry);

  ui->replace_entry = gtk_entry_new();
  labeled_row(fr_grid, 1, "Replace with:", ui->replace_entry);

  GtkWidget *fr_checks = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  ui->regex_check = gtk_check_button_new_with_label("Regular Expression");
  ui->case_check = gtk_check_button_new_with_label("Match Case");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->case_check), TRUE);
  gtk_box_pack_start(GTK_BOX(fr_checks), ui->regex_check, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(fr_checks), ui->case_check, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(fr_grid), fr_checks, 0, 2, 2, 1);

  gtk_box_pack_start(GTK_BOX(vbox), fr_grid, FALSE, FALSE, 0);

  /* "Numbering" options */
  GtkWidget *num_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(num_grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(num_grid), 8);
  ui->numbering_box = num_grid;

  ui->name_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(ui->name_entry), "Name");
  labeled_row(num_grid, 0, "Name:", ui->name_entry);
  gtk_widget_set_tooltip_text(
      ui->name_entry,
      "Khali chhod dein sirf number se naam banane ke liye (1.png, 2.png, "
      "...). Kuch likhein to us ke baad number lagega (mytext1.png, "
      "mytext2.png, ...).");

  ui->sep_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->sep_combo), "none",
                            "(None)");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->sep_combo), "space",
                            "Space");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->sep_combo), "underscore",
                            "Underscore ( _ )");
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->sep_combo), "dash",
                            "Dash ( - )");
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->sep_combo), "none");
  labeled_row(num_grid, 1, "Separator:", ui->sep_combo);

  ui->start_spin = gtk_spin_button_new_with_range(0, 999999, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->start_spin), 0);
  labeled_row(num_grid, 2, "Start At:", ui->start_spin);

  ui->digits_spin = gtk_spin_button_new_with_range(1, 6, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->digits_spin), 1);
  labeled_row(num_grid, 3, "Digits:", ui->digits_spin);
  gtk_widget_set_tooltip_text(
      ui->digits_spin,
      "1 = 1,2,3... 2 = 01,02,03... waghera (leading zeros se padding).");

  ui->keep_ext_check =
      gtk_check_button_new_with_label("Keep Original Extension");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->keep_ext_check), TRUE);
  gtk_grid_attach(GTK_GRID(num_grid), ui->keep_ext_check, 0, 4, 2, 1);

  gtk_box_pack_start(GTK_BOX(vbox), num_grid, FALSE, FALSE, 0);

  /* Preview list */
  ui->store = gtk_list_store_new(RN_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
  for (GList *l = paths; l; l = l->next) {
    const char *path = l->data;
    char *name = g_path_get_basename(path);
    GtkTreeIter iter;
    gtk_list_store_append(ui->store, &iter);
    gtk_list_store_set(ui->store, &iter, RCOL_OLD_NAME, name, RCOL_NEW_NAME,
                       name, RCOL_OLD_PATH, path, RCOL_FG, NULL,
                       RCOL_HAS_CONFLICT, FALSE, -1);
    g_free(name);
  }

  GtkWidget *tree_view =
      gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->store));
  g_object_unref(ui->store);

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col_old = gtk_tree_view_column_new_with_attributes(
      "Old Name", renderer, "text", RCOL_OLD_NAME, NULL);
  gtk_tree_view_column_set_expand(col_old, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col_old);

  GtkCellRenderer *renderer2 = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col_new = gtk_tree_view_column_new_with_attributes(
      "New Name", renderer2, "text", RCOL_NEW_NAME, "foreground", RCOL_FG,
      NULL);
  gtk_tree_view_column_set_expand(col_new, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col_new);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), tree_view);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

  ui->status_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(ui->status_label), 0.0);
  gtk_box_pack_start(GTK_BOX(vbox), ui->status_label, FALSE, FALSE, 0);

  ui->rename_button =
      gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

  /* Signals: koi bhi option badle to preview turant refresh ho. */
  g_signal_connect(ui->mode_find_radio, "toggled", G_CALLBACK(on_mode_toggled),
                   ui);
  g_signal_connect(ui->mode_numbering_radio, "toggled",
                   G_CALLBACK(on_mode_toggled), ui);
  g_signal_connect(ui->find_entry, "changed", G_CALLBACK(on_option_changed),
                   ui);
  g_signal_connect(ui->replace_entry, "changed", G_CALLBACK(on_option_changed),
                   ui);
  g_signal_connect(ui->regex_check, "toggled", G_CALLBACK(on_option_changed),
                   ui);
  g_signal_connect(ui->case_check, "toggled", G_CALLBACK(on_option_changed),
                   ui);
  g_signal_connect(ui->name_entry, "changed", G_CALLBACK(on_option_changed),
                   ui);
  g_signal_connect(ui->sep_combo, "changed", G_CALLBACK(on_option_changed), ui);
  g_signal_connect(ui->start_spin, "value-changed",
                   G_CALLBACK(on_option_changed), ui);
  g_signal_connect(ui->digits_spin, "value-changed",
                   G_CALLBACK(on_option_changed), ui);
  g_signal_connect(ui->keep_ext_check, "toggled", G_CALLBACK(on_option_changed),
                   ui);

  g_signal_connect(dialog, "response", G_CALLBACK(on_dialog_response), ui);

  gtk_widget_show_all(dialog);
  gtk_widget_hide(num_grid); /* mode default Find & Replace hai */

  regenerate_preview(ui);
}
