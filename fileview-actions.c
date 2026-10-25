#include "compressjob.h"
#include "fileview_internal.h"
#include "renamejob.h"

static int natural_compare(const char *a, const char *b) {
  while (*a && *b) {
    if (g_ascii_isdigit(*a) && g_ascii_isdigit(*b)) {
      while (*a == '0' && g_ascii_isdigit(*(a + 1)))
        a++;
      while (*b == '0' && g_ascii_isdigit(*(b + 1)))
        b++;

      const char *a_start = a;
      const char *b_start = b;
      while (g_ascii_isdigit(*a))
        a++;
      while (g_ascii_isdigit(*b))
        b++;

      size_t len_a = (size_t)(a - a_start);
      size_t len_b = (size_t)(b - b_start);
      if (len_a != len_b)
        return len_a < len_b ? -1 : 1;

      int cmp = strncmp(a_start, b_start, len_a);
      if (cmp != 0)
        return cmp < 0 ? -1 : 1;
      /* numeric value barabar hai (leading zeros ke bawajood) -- baaqi
       * naam par aage badho */
    } else {
      gunichar ca = g_unichar_tolower(g_utf8_get_char(a));
      gunichar cb = g_unichar_tolower(g_utf8_get_char(b));
      if (ca != cb)
        return ca < cb ? -1 : 1;
      a = g_utf8_next_char(a);
      b = g_utf8_next_char(b);
    }
  }
  if (*a == *b)
    return 0;
  return *a ? 1 : -1;
}

gint file_sort_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                    gpointer data) {
  AppWidgets *app = data;

  gchar *name1;
  gchar *name2;
  gboolean is_dir1 = FALSE;
  gboolean is_dir2 = FALSE;

  gtk_tree_model_get(model, a, COL_NAME, &name1, COL_IS_DIR, &is_dir1, -1);
  gtk_tree_model_get(model, b, COL_NAME, &name2, COL_IS_DIR, &is_dir2, -1);

  if (!name1 && !name2)
    return 0;

  if (!name1) {
    g_free(name2);
    return 1;
  }

  if (!name2) {
    g_free(name1);
    return -1;
  }

  if (app->current_dir && g_strcmp0(app->current_dir, COMPUTER_URI) == 0) {
    gboolean is_fs1 = g_strcmp0(name1, "Filesystem") == 0;
    gboolean is_fs2 = g_strcmp0(name2, "Filesystem") == 0;
    if (is_fs1 != is_fs2) {
      g_free(name1);
      g_free(name2);
      return is_fs1 ? -1 : 1;
    }
  }

  gboolean is_hidden1 = FALSE;
  gboolean is_hidden2 = FALSE;
  gtk_tree_model_get(model, a, COL_IS_HIDDEN, &is_hidden1, -1);
  gtk_tree_model_get(model, b, COL_IS_HIDDEN, &is_hidden2, -1);
  if (is_hidden1 != is_hidden2) {
    g_free(name1);
    g_free(name2);
    return is_hidden1 ? -1 : 1;
  }

  if (app->folders_first && is_dir1 != is_dir2) {
    g_free(name1);
    g_free(name2);
    return is_dir1 ? -1 : 1;
  }

  gint result = 0;

  switch (app->sort_mode) {
  case SORT_NAME_ASC:
    if (app->current_dir && g_strcmp0(app->current_dir, RECENT_URI) == 0) {
      /* Recent view ka "default" order recency hai (abhi-abhi use hui
       * files sabse pehle) -- Name A-Z explicitly chuna ho to bhi yehi
       * default state hai is liye recency hi dikhate hain. Date/Size/Type
       * jaisay dusre Sort By options neeche normal tareeqe se kaam karte
       * hain. */
      guint rank1 = 0, rank2 = 0;
      gtk_tree_model_get(model, a, COL_RECENT_RANK, &rank1, -1);
      gtk_tree_model_get(model, b, COL_RECENT_RANK, &rank2, -1);
      result = (rank1 > rank2) - (rank1 < rank2);
    } else {
      result = natural_compare(name1, name2);
    }
    break;

  case SORT_NAME_DESC:
    result = natural_compare(name2, name1);
    break;

  case SORT_DATE_NEWEST:
  case SORT_DATE_OLDEST: {
    gint64 t1 = 0;
    gint64 t2 = 0;

    GDateTime *date1 = NULL;
    GDateTime *date2 = NULL;

    gtk_tree_model_get(model, a, COL_DATE, &date1, -1);
    gtk_tree_model_get(model, b, COL_DATE, &date2, -1);

    if (date1)
      t1 = g_date_time_to_unix(date1);

    if (date2)
      t2 = g_date_time_to_unix(date2);

    if (app->sort_mode == SORT_DATE_NEWEST)
      result = (t2 > t1) - (t2 < t1);
    else
      result = (t1 > t2) - (t1 < t2);

    if (date1)
      g_date_time_unref(date1);
    if (date2)
      g_date_time_unref(date2);

    break;
  }

  case SORT_SIZE_LARGEST:
  case SORT_SIZE_SMALLEST: {
    guint64 size1 = 0;
    guint64 size2 = 0;

    gtk_tree_model_get(model, a, COL_SIZE, &size1, -1);
    gtk_tree_model_get(model, b, COL_SIZE, &size2, -1);

    if (app->sort_mode == SORT_SIZE_LARGEST) {
      if (size1 > size2)
        result = -1;
      else if (size1 < size2)
        result = 1;
      else
        result = 0;
    } else {
      if (size1 < size2)
        result = -1;
      else if (size1 > size2)
        result = 1;
      else
        result = 0;
    }

    break;
  }

  case SORT_TYPE_ASC:
  case SORT_TYPE_DESC: {
    gchar *type1;
    gchar *type2;

    gtk_tree_model_get(model, a, COL_TYPE, &type1, -1);
    gtk_tree_model_get(model, b, COL_TYPE, &type2, -1);

    if (!type1 && !type2)
      result = 0;
    else if (!type1)
      result = 1;
    else if (!type2)
      result = -1;
    else if (app->sort_mode == SORT_TYPE_ASC)
      result = g_utf8_collate(type1, type2);
    else
      result = g_utf8_collate(type2, type1);

    g_free(type1);
    g_free(type2);

    break;
  }
  }

  g_free(name1);
  g_free(name2);

  return result;
}

static void force_resort(AppWidgets *app) {
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->file_store),
                                       GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);
}

/* Path khud ek drive/mount ka root hai (jaise "/" ya kisi external
 * USB drive ka mount point) ya phir us ke andar ek normal folder --
 * ye is se decide hota hai ke Properties par poori-disk recursive
 * scan chalani hai ya fast filesystem-size query. */
static gboolean path_is_mount_root(const char *path) {
  if (!path || !*path)
    return FALSE;

  if (g_strcmp0(path, "/") == 0)
    return TRUE;

  GFile *f = g_file_new_for_path(path);
  GMount *mount = g_file_find_enclosing_mount(f, NULL, NULL);
  gboolean result = FALSE;
  if (mount) {
    GFile *root = g_mount_get_root(mount);
    result = g_file_equal(f, root);
    g_object_unref(root);
    g_object_unref(mount);
  }
  g_object_unref(f);
  return result;
}

static void generic_menuitem_activate(GtkMenuItem *mi, gpointer user_data) {
  AppWidgets *app = user_data;
  const char *label = gtk_menu_item_get_label(mi);
  const char *path = g_object_get_data(G_OBJECT(mi), "path");
  const char *name = g_object_get_data(G_OBJECT(mi), "name");
  const char *dir = g_object_get_data(G_OBJECT(mi), "dir");

  gpointer sort_data = g_object_get_data(G_OBJECT(mi), "sort_mode");
  if (sort_data) {
    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(mi))) {
      app->sort_mode = (SortMode)(GPOINTER_TO_INT(sort_data) - 1);
      force_resort(app);
      fileview_refresh(app);
      fileview_save_settings(app);
    }
    return;
  }

  if (g_strcmp0(label, "Open") == 0 && path) {
    gboolean is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
    open_path(app, path, is_dir);
  } else if (g_strcmp0(label, "Run") == 0 && path) {
    if (path_is_desktop_file(path))
      run_desktop_file(app, path);
    else
      run_executable_file(app, path);
  } else if (g_strcmp0(label, "Edit Launcher") == 0 && path) {
    show_edit_launcher_dialog(app, path);
  } else if (g_strcmp0(label, "Rename") == 0 && path) {
    GList *sel = get_selected_paths(app);
    if (g_list_length(sel) > 1) {
      /* renamejob_show_dialog `sel` ka ownership le leta hai. */
      renamejob_show_dialog(app, sel);
    } else {
      g_list_free_full(sel, g_free);
      show_rename_dialog(app, path, name ? name : "");
    }
  } else if (g_strcmp0(label, "Move to Trash") == 0 && path) {
    GList *sel = get_selected_paths(app);
    guint n_sel = g_list_length(sel);
    g_list_free_full(sel, g_free);
    if (n_sel > 1) {
      fileview_delete_selected(app, FALSE);
    } else {
      confirm_delete(app, path, name ? name : path);
    }
  /*} else if (g_strcmp0(label, "Remove from Recent") == 0 && path) {
    GFile *rf = g_file_new_for_path(path);
    char *uri = g_file_get_uri(rf);
    gtk_recent_manager_remove_item(gtk_recent_manager_get_default(), uri,
                                   NULL);
    g_free(uri);
    g_object_unref(rf);
    fileview_refresh(app);
  }*/
  } else if (g_strcmp0(label, "Remove from Recent") == 0 && path) {
    GList *sel = get_selected_paths(app);

    if (sel) {
      GtkRecentManager *rm = gtk_recent_manager_get_default();

      for (GList *l = sel; l; l = l->next) {
        const char *selected_path = l->data;

        GFile *rf = g_file_new_for_path(selected_path);
        char *uri = g_file_get_uri(rf);

        gtk_recent_manager_remove_item(rm, uri, NULL);

        g_free(uri);
        g_object_unref(rf);
      }

      g_list_free_full(sel, g_free);
    } else {
      GFile *rf = g_file_new_for_path(path);
      char *uri = g_file_get_uri(rf);

      gtk_recent_manager_remove_item(
        gtk_recent_manager_get_default(), uri, NULL);

      g_free(uri);
      g_object_unref(rf);
    }

    fileview_refresh(app);
  }else if (g_strcmp0(label, "Delete") == 0 && path) {
    GList *sel = get_selected_paths(app);
    guint n_sel = g_list_length(sel);
    g_list_free_full(sel, g_free);
    if (n_sel > 1) {
      fileview_delete_selected(app, TRUE);
    } else {
      confirm_delete_permanent(app, path, name ? name : path);
    }
  } else if (g_strcmp0(label, "Restore") == 0 && path) {
    GList *sel = get_selected_paths(app);
    if (g_list_length(sel) > 1) {
      do_restore_multiple(app, sel);
      g_list_free_full(sel, g_free);
    } else {
      g_list_free_full(sel, g_free);
      do_restore(app, path, name ? name : "");
    }
  } else if (g_strcmp0(label, "Delete Permanently") == 0 && path) {
    GList *sel = get_selected_paths(app);
    guint n_sel = g_list_length(sel);
    g_list_free_full(sel, g_free);
    if (n_sel > 1) {
      fileview_delete_selected(app, TRUE);
    } else {
      confirm_delete_permanent(app, path, name ? name : path);
    }
  } else if (g_strcmp0(label, "Empty Trash") == 0) {
    confirm_empty_trash(app);
  }
  // else if (g_strcmp0(label, "Properties") == 0 && path) {
  //   GList *sel = get_selected_paths(app);
  //   if (g_list_length(sel) > 1) {
  //     show_properties_dialog_multi(app, sel);
  //     g_list_free_full(sel, g_free);
  //   } else {
  //     g_list_free_full(sel, g_free);
  //     gboolean prop_is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
  //     show_properties_dialog(app, path, name ? name : path, prop_is_dir);
  //   }
  // }
  else if (g_strcmp0(label, "Properties") == 0) {

    GList *sel = get_selected_paths(app);

    if (sel) {
      if (g_list_length(sel) > 1) {
        show_properties_dialog_multi(app, sel);
      } else {
        char *p = sel->data;
        char *n = g_path_get_basename(p);
        gboolean is_dir = g_file_test(p, G_FILE_TEST_IS_DIR);

        if (is_dir && path_is_mount_root(p))
          show_drive_properties_dialog(app, p, n, g_strcmp0(p, "/") != 0);
        else
          show_properties_dialog(app, p, n, is_dir);

        g_free(n);
      }

      g_list_free_full(sel, g_free);
    }
    else {
      /* koi selection nahi -> current folder ki properties */

      if (app->current_dir) {
        char *n = g_path_get_basename(app->current_dir);

        gboolean is_dir =
        g_file_test(app->current_dir, G_FILE_TEST_IS_DIR);

        if (is_dir && path_is_mount_root(app->current_dir))
          show_drive_properties_dialog(
              app, app->current_dir, n,
              g_strcmp0(app->current_dir, "/") != 0);
        else
          show_properties_dialog(app,
                                 app->current_dir,
                                 n,
                                 is_dir);

        g_free(n);
      }
    }
  }

  else if (g_strcmp0(label, "New Folder") == 0) {
    new_folder_dialog(app);
  } else if (g_strcmp0(label, "Open Terminal Here") == 0 && dir) {
    open_terminal_here(dir);
  } else if (g_strcmp0(label, "Copy") == 0) {
    fileview_copy(app);
  } else if (g_strcmp0(label, "Cut") == 0) {
    fileview_cut(app);
  } else if (g_strcmp0(label, "Paste") == 0) {
    fileview_paste(app);
  } else if (g_strcmp0(label, "Other Application...") == 0 && path) {
    show_app_chooser(app, path, FALSE);
  } else if (g_strcmp0(label, "Set as Default Application") == 0 && path) {
    show_app_chooser(app, path, TRUE);
  } else if (g_strcmp0(label, "Folders First") == 0) {

    app->folders_first =
        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(mi));

    force_resort(app);
    fileview_save_settings(app);

  } else if (g_strcmp0(label, "Extract Here") == 0 && path) {
    do_extract_archive(app, path);
  } else if (g_strcmp0(label, "Compress...") == 0 && path) {
    GList *sel = get_selected_paths(app);
    if (g_list_length(sel) > 1) {
      compressjob_show_dialog(app, sel); /* dialog takes ownership */
    } else {
      g_list_free_full(sel, g_free);
      compressjob_show_dialog(app, g_list_append(NULL, g_strdup(path)));
    }
  } else if (g_strcmp0(label, "Open Item Location") == 0 && path) {
    char *parent = g_path_get_dirname(path);
    g_free(app->pending_select_path);
    app->pending_select_path = g_strdup(path);
    fileview_navigate(app, parent, TRUE);
    g_free(parent);
  } else if (g_strcmp0(label, "Copy Location") == 0 && path) {
    // GtkClipboard *cb =
    // gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    // gtk_clipboard_set_text(cb, path, -1);
    // gtk_clipboard_store(cb);
    fileview_copy_location(app);
  }
}

static void show_context_menu(AppWidgets *app, const char *item_path,
                              const char *item_name, gboolean is_dir,
                              GdkEventButton *event) {
  GtkWidget *menu = gtk_menu_new();
  GtkWidget *sort_item = gtk_menu_item_new_with_label("Sort By");
  GtkWidget *sort_menu = gtk_menu_new();
  GtkWidget *folders = gtk_check_menu_item_new_with_label("Folders First");

  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(folders),
                                 app->folders_first);

  g_signal_connect(folders, "activate", G_CALLBACK(generic_menuitem_activate),
                   app);

  gtk_menu_shell_append(GTK_MENU_SHELL(sort_menu), folders);

  gtk_menu_shell_append(GTK_MENU_SHELL(sort_menu),
                        gtk_separator_menu_item_new());

  GtkWidget *name_item = gtk_menu_item_new_with_label("Name");
  GtkWidget *name_menu = gtk_menu_new();

  GtkWidget *name_asc = gtk_check_menu_item_new_with_label("A to Z");
  GtkWidget *name_desc = gtk_check_menu_item_new_with_label("Z to A");
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(name_asc), TRUE);
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(name_desc), TRUE);

  g_object_set_data(G_OBJECT(name_asc), "sort_mode",
                    GINT_TO_POINTER(SORT_NAME_ASC + 1));
  g_object_set_data(G_OBJECT(name_desc), "sort_mode",
                    GINT_TO_POINTER(SORT_NAME_DESC + 1));

  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(name_asc),
                                 app->sort_mode == SORT_NAME_ASC);
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(name_desc),
                                 app->sort_mode == SORT_NAME_DESC);

  g_signal_connect(name_asc, "activate", G_CALLBACK(generic_menuitem_activate),
                   app);
  g_signal_connect(name_desc, "activate", G_CALLBACK(generic_menuitem_activate),
                   app);

  gtk_menu_shell_append(GTK_MENU_SHELL(name_menu), name_asc);
  gtk_menu_shell_append(GTK_MENU_SHELL(name_menu), name_desc);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(name_item), name_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(sort_menu), name_item);

  GtkWidget *date_item = gtk_menu_item_new_with_label("Date");
  GtkWidget *date_menu = gtk_menu_new();

  GtkWidget *date_newest = gtk_check_menu_item_new_with_label("Newest First");
  GtkWidget *date_oldest = gtk_check_menu_item_new_with_label("Oldest First");
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(date_newest), TRUE);
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(date_oldest), TRUE);

  g_object_set_data(G_OBJECT(date_newest), "sort_mode",
                    GINT_TO_POINTER(SORT_DATE_NEWEST + 1));
  g_object_set_data(G_OBJECT(date_oldest), "sort_mode",
                    GINT_TO_POINTER(SORT_DATE_OLDEST + 1));

  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(date_newest),
                                 app->sort_mode == SORT_DATE_NEWEST);
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(date_oldest),
                                 app->sort_mode == SORT_DATE_OLDEST);

  g_signal_connect(date_newest, "activate",
                   G_CALLBACK(generic_menuitem_activate), app);
  g_signal_connect(date_oldest, "activate",
                   G_CALLBACK(generic_menuitem_activate), app);

  gtk_menu_shell_append(GTK_MENU_SHELL(date_menu), date_newest);
  gtk_menu_shell_append(GTK_MENU_SHELL(date_menu), date_oldest);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(date_item), date_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(sort_menu), date_item);

  GtkWidget *size_item = gtk_menu_item_new_with_label("Size");
  GtkWidget *size_menu = gtk_menu_new();

  GtkWidget *size_largest = gtk_check_menu_item_new_with_label("Largest First");
  GtkWidget *size_smallest =
      gtk_check_menu_item_new_with_label("Smallest First");
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(size_largest),
                                        TRUE);
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(size_smallest),
                                        TRUE);

  g_object_set_data(G_OBJECT(size_largest), "sort_mode",
                    GINT_TO_POINTER(SORT_SIZE_LARGEST + 1));
  g_object_set_data(G_OBJECT(size_smallest), "sort_mode",
                    GINT_TO_POINTER(SORT_SIZE_SMALLEST + 1));

  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(size_largest),
                                 app->sort_mode == SORT_SIZE_LARGEST);
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(size_smallest),
                                 app->sort_mode == SORT_SIZE_SMALLEST);

  g_signal_connect(size_largest, "activate",
                   G_CALLBACK(generic_menuitem_activate), app);
  g_signal_connect(size_smallest, "activate",
                   G_CALLBACK(generic_menuitem_activate), app);

  gtk_menu_shell_append(GTK_MENU_SHELL(size_menu), size_largest);
  gtk_menu_shell_append(GTK_MENU_SHELL(size_menu), size_smallest);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(size_item), size_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(sort_menu), size_item);

  GtkWidget *type_item = gtk_menu_item_new_with_label("Type");
  GtkWidget *type_menu = gtk_menu_new();

  GtkWidget *type_asc = gtk_check_menu_item_new_with_label("A to Z");
  GtkWidget *type_desc = gtk_check_menu_item_new_with_label("Z to A");
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(type_asc), TRUE);
  gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(type_desc), TRUE);

  g_object_set_data(G_OBJECT(type_asc), "sort_mode",
                    GINT_TO_POINTER(SORT_TYPE_ASC + 1));
  g_object_set_data(G_OBJECT(type_desc), "sort_mode",
                    GINT_TO_POINTER(SORT_TYPE_DESC + 1));

  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(type_asc),
                                 app->sort_mode == SORT_TYPE_ASC);
  gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(type_desc),
                                 app->sort_mode == SORT_TYPE_DESC);

  g_signal_connect(type_asc, "activate", G_CALLBACK(generic_menuitem_activate),
                   app);
  g_signal_connect(type_desc, "activate", G_CALLBACK(generic_menuitem_activate),
                   app);

  gtk_menu_shell_append(GTK_MENU_SHELL(type_menu), type_asc);
  gtk_menu_shell_append(GTK_MENU_SHELL(type_menu), type_desc);
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(type_item), type_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(sort_menu), type_item);

  gtk_menu_item_set_submenu(GTK_MENU_ITEM(sort_item), sort_menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sort_item);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  GtkWidget *item;
  gboolean in_trash = is_trash_dir(app);

  if (item_path && in_trash) {
    item = gtk_menu_item_new_with_label("Restore");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    g_object_set_data_full(G_OBJECT(item), "name",
                           g_strdup(item_name ? item_name : ""), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Delete Permanently");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    g_object_set_data_full(G_OBJECT(item), "name",
                           g_strdup(item_name ? item_name : ""), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Copy");
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // item = gtk_menu_item_new_with_label("Properties");
    // g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    // g_object_set_data_full(G_OBJECT(item), "name",
    //                        g_strdup(item_name ? item_name : ""), g_free);
    // g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
    //                  app);
    // gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return;
  }

  if (!item_path && in_trash) {
    item = gtk_menu_item_new_with_label("Empty Trash");
    gtk_widget_set_sensitive(item, TRUE);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return;
  }

  if (item_path) {
    item = gtk_menu_item_new_with_label("Open");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    if (app->search_recursive_active ||
        g_strcmp0(app->current_dir, RECENT_URI) == 0) {
      item = gtk_menu_item_new_with_label("Open Item Location");
      g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path),
                             g_free);
      g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    if (!is_dir &&
        (path_is_desktop_file(item_path) || path_is_executable(item_path))) {
      item = gtk_menu_item_new_with_label("Run");
      g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path),
                             g_free);
      g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    if (!is_dir && path_is_desktop_file(item_path)) {
      item = gtk_menu_item_new_with_label("Edit Launcher");
      g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path),
                             g_free);
      g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    if (!is_dir && has_archive_extension(item_path)) {
      item = gtk_menu_item_new_with_label("Extract Here");
      g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path),
                             g_free);
      g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    if (!is_dir) {
      GtkWidget *with_item = gtk_menu_item_new_with_label("Open With");
      GtkWidget *with_menu = gtk_menu_new();

      gboolean uncertain;
      char *ctype = g_content_type_guess(item_path, NULL, 0, &uncertain);
      GList *apps = ctype ? g_app_info_get_all_for_type(ctype) : NULL;
      if (!apps)
        apps = g_app_info_get_all();

      for (GList *l = apps; l; l = l->next) {
        GAppInfo *ai = l->data;
        GtkWidget *sub = gtk_menu_item_new_with_label(g_app_info_get_name(ai));
        g_object_set_data_full(G_OBJECT(sub), "appinfo", g_object_ref(ai),
                               g_object_unref);
        char *path_copy = g_strdup(item_path);
        g_signal_connect(sub, "activate", G_CALLBACK(on_open_with_app),
                         path_copy);
        gtk_menu_shell_append(GTK_MENU_SHELL(with_menu), sub);
      }
      g_list_free_full(apps, g_object_unref);
      g_free(ctype);

      gtk_menu_shell_append(GTK_MENU_SHELL(with_menu),
                            gtk_separator_menu_item_new());

      GtkWidget *other = gtk_menu_item_new_with_label("Other Application...");
      g_object_set_data_full(G_OBJECT(other), "path", g_strdup(item_path),
                             g_free);
      g_signal_connect(other, "activate", G_CALLBACK(generic_menuitem_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(with_menu), other);

      gtk_menu_item_set_submenu(GTK_MENU_ITEM(with_item), with_menu);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), with_item);

      GtkWidget *set_default =
          gtk_menu_item_new_with_label("Set as Default Application");
      g_object_set_data_full(G_OBJECT(set_default), "path", g_strdup(item_path),
                             g_free);
      g_signal_connect(set_default, "activate",
                       G_CALLBACK(generic_menuitem_activate), app);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), set_default);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Rename");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    g_object_set_data_full(G_OBJECT(item), "name",
                           g_strdup(item_name ? item_name : ""), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Delete");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    g_object_set_data_full(G_OBJECT(item), "name",
                           g_strdup(item_name ? item_name : ""), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Move to Trash");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    g_object_set_data_full(G_OBJECT(item), "name",
                           g_strdup(item_name ? item_name : ""), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    if (g_strcmp0(app->current_dir, RECENT_URI) == 0) {
      item = gtk_menu_item_new_with_label("Remove from Recent");
      g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path),
                             g_free);
      g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Copy");
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Cut");
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Copy Location");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Compress...");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    // item = gtk_menu_item_new_with_label("Properties");
    // g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
    // g_object_set_data_full(G_OBJECT(item), "name",
    //                        g_strdup(item_name ? item_name : ""), g_free);
    // g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
    //                  app);
    // gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  } else {
    item = gtk_menu_item_new_with_label("New Folder");
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    {
      item = gtk_menu_item_new_with_label("New File");
      GtkWidget *newfile_menu = gtk_menu_new();
      GtkWidget *nf_item;

      nf_item = gtk_menu_item_new_with_label("Text File (.txt)");
      g_object_set_data_full(G_OBJECT(nf_item), "default_name",
                             g_strdup("New File.txt"), g_free);
      g_signal_connect(nf_item, "activate", G_CALLBACK(new_file_type_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(newfile_menu), nf_item);

      nf_item = gtk_menu_item_new_with_label("C Source File (.c)");
      g_object_set_data_full(G_OBJECT(nf_item), "default_name",
                             g_strdup("new_file.c"), g_free);
      g_signal_connect(nf_item, "activate", G_CALLBACK(new_file_type_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(newfile_menu), nf_item);

      nf_item = gtk_menu_item_new_with_label("Empty File");
      g_object_set_data_full(G_OBJECT(nf_item), "default_name",
                             g_strdup("New File"), g_free);
      g_signal_connect(nf_item, "activate", G_CALLBACK(new_file_type_activate),
                       app);
      gtk_menu_shell_append(GTK_MENU_SHELL(newfile_menu), nf_item);

      gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), newfile_menu);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }

    item = gtk_menu_item_new_with_label("Paste");
    gtk_widget_set_sensitive(item, g_clipboard_paths != NULL);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  }

  item = gtk_menu_item_new_with_label("Open Terminal Here");
  char *term_dir = g_strdup(item_path && is_dir ? item_path : app->current_dir);
  g_object_set_data_full(G_OBJECT(item), "dir", term_dir, g_free);
  g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                   app);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

  // item = gtk_menu_item_new_with_label("Properties");
  // g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path), g_free);
  // g_object_set_data_full(G_OBJECT(item), "name",
  //                        g_strdup(item_name ? item_name : ""), g_free);
  // g_signal_connect(item, "activate",
  //                  G_CALLBACK(generic_menuitem_activate), app);
  item = gtk_menu_item_new_with_label("Properties");

  if (item_path) {
    g_object_set_data_full(G_OBJECT(item),
                           "path",
                           g_strdup(item_path),
                           g_free);

    g_object_set_data_full(G_OBJECT(item),
                           "name",
                           g_strdup(item_name ? item_name : ""),
                           g_free);
  }

  g_signal_connect(item,
                   "activate",
                   G_CALLBACK(generic_menuitem_activate),
                   app);

  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  // gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static void show_computer_context_menu(AppWidgets *app, const char *item_path,
                                       const char *item_name, gboolean mounted,
                                       gboolean is_removable, GVolume *volume,
                                       GdkEventButton *event);

void fileview_show_context_menu_for_selection(AppWidgets *app) {
  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0) {
    GList *selected =
        app->icon_view
            ? gtk_icon_view_get_selected_items(GTK_ICON_VIEW(app->icon_view))
            : NULL;
    if (selected) {
      GtkTreeIter filter_iter, iter;
      if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter),
                                  &filter_iter, selected->data)) {
        gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter,
                                                         &iter, &filter_iter);
        char *c_name = NULL, *c_path = NULL;
        gboolean c_mounted = FALSE;
        gpointer c_volume_ptr = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_NAME,
                           &c_name, COL_PATH, &c_path, COL_IS_DIR, &c_mounted,
                           COL_VOLUME, &c_volume_ptr, -1);
        show_computer_context_menu(app, c_path, c_name, c_mounted,
                                   c_volume_ptr != NULL,
                                   c_volume_ptr ? G_VOLUME(c_volume_ptr)
                                                : NULL,
                                   NULL);
        g_free(c_name);
        g_free(c_path);
      }
    }
    g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    return;
  }

  GList *sel = get_selected_paths(app);
  if (sel) {
    const char *path = (const char *)sel->data;
    char *name = g_path_get_basename(path);
    gboolean is_dir = g_file_test(path, G_FILE_TEST_IS_DIR);
    show_context_menu(app, path, name, is_dir, NULL);
    g_free(name);
  } else {
    show_context_menu(app, NULL, NULL, FALSE, NULL);
  }
  g_list_free_full(sel, g_free);
}

void fileview_rename_selected(AppWidgets *app) {
  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0)
    return;

  GList *sel = get_selected_paths(app);
  if (!sel)
    return;

  if (sel->next) {
    /* renamejob_show_dialog `sel` ka ownership le leta hai. */
    renamejob_show_dialog(app, sel);
  } else {
    const char *path = (const char *)sel->data;
    char *name = g_path_get_basename(path);
    show_rename_dialog(app, path, name);
    g_free(name);
    g_list_free_full(sel, g_free);
  }
}

void on_item_activated(GtkIconView *iv, GtkTreePath *path, gpointer user_data) {
  AppWidgets *app = user_data;
  GtkTreeIter filter_iter, iter;
  if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                               path))
    return;
  gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter, &iter,
                                                   &filter_iter);

  char *item_path;
  gboolean is_dir;
  gpointer volume_ptr = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_PATH,
                     &item_path, COL_IS_DIR, &is_dir, COL_VOLUME, &volume_ptr,
                     -1);
  if (volume_ptr) {
    mount_computer_volume(app, G_VOLUME(volume_ptr));
  } else {
    open_path(app, item_path, is_dir);
  }
  g_free(item_path);
  (void)iv;
}

static gint iconview_columns_count(AppWidgets *app) {
  GtkIconView *iv = GTK_ICON_VIEW(app->icon_view);
  gint n =
      gtk_tree_model_iter_n_children(GTK_TREE_MODEL(app->file_filter), NULL);
  gint cols = 0;
  for (gint i = 0; i < n; i++) {
    GtkTreePath *p = gtk_tree_path_new_from_indices(i, -1);
    gint row = gtk_icon_view_get_item_row(iv, p);
    gtk_tree_path_free(p);
    if (row != 0)
      break;
    cols++;
  }
  return cols > 0 ? cols : 1;
}

static void select_index_range(AppWidgets *app, gint from_idx, gint to_idx) {
  GtkIconView *iv = GTK_ICON_VIEW(app->icon_view);
  gint lo = MIN(from_idx, to_idx);
  gint hi = MAX(from_idx, to_idx);

  gtk_icon_view_unselect_all(iv);
  for (gint i = lo; i <= hi; i++) {
    GtkTreePath *p = gtk_tree_path_new_from_indices(i, -1);
    gtk_icon_view_select_path(iv, p);
    gtk_tree_path_free(p);
  }
}

static gboolean move_iconview_cursor(AppWidgets *app, GdkEventKey *event) {
  GtkIconView *iv = GTK_ICON_VIEW(app->icon_view);
  gint n =
      gtk_tree_model_iter_n_children(GTK_TREE_MODEL(app->file_filter), NULL);
  if (n <= 0)
    return TRUE;

  GtkTreePath *cursor_path = NULL;
  gtk_icon_view_get_cursor(iv, &cursor_path, NULL);

  gint cur_idx = 0;
  if (cursor_path) {
    cur_idx = gtk_tree_path_get_indices(cursor_path)[0];
    gtk_tree_path_free(cursor_path);
  }

  gint target_idx;
  switch (event->keyval) {
  case GDK_KEY_Left:
    target_idx = cur_idx - 1;
    break;
  case GDK_KEY_Right:
    target_idx = cur_idx + 1;
    break;
  case GDK_KEY_Up:
    target_idx = cur_idx - iconview_columns_count(app);
    break;
  case GDK_KEY_Down:
    target_idx = cur_idx + iconview_columns_count(app);
    break;
  case GDK_KEY_Home:
    target_idx = 0;
    break;
  case GDK_KEY_End:
    target_idx = n - 1;
    break;
  default:
    return FALSE;
  }

  if (target_idx < 0 || target_idx >= n)
    target_idx = cur_idx;

  gboolean shift = (event->state & GDK_SHIFT_MASK) != 0;

  if (shift) {
    if (app->select_anchor_idx < 0)
      app->select_anchor_idx = cur_idx;
    gint anchor = app->select_anchor_idx;
    select_index_range(app, anchor, target_idx);
    app->select_anchor_idx = anchor;
  } else {
    app->select_anchor_idx = target_idx;
    gtk_icon_view_unselect_all(iv);
    GtkTreePath *only = gtk_tree_path_new_from_indices(target_idx, -1);
    gtk_icon_view_select_path(iv, only);
    gtk_tree_path_free(only);
  }

  GtkTreePath *target = gtk_tree_path_new_from_indices(target_idx, -1);
  gtk_icon_view_set_cursor(iv, target, NULL, FALSE);
  gtk_icon_view_scroll_to_path(iv, target, FALSE, 0, 0);
  gtk_tree_path_free(target);
  return TRUE;
}

static void typeahead_popup_position(AppWidgets *app) {
  if (!app->typeahead_popup || !app->icon_view)
    return;

  GdkWindow *gdkwin = gtk_widget_get_window(app->icon_view);
  if (!gdkwin)
    return;

  GtkAllocation alloc;
  gtk_widget_get_allocation(app->icon_view, &alloc);

  gint origin_x = 0, origin_y = 0;
  gdk_window_get_origin(gdkwin, &origin_x, &origin_y);

  GtkRequisition req;
  gtk_widget_get_preferred_size(app->typeahead_popup, NULL, &req);

  const gint margin = 14;
  gint x = origin_x + alloc.width - req.width - margin;
  gint y = origin_y + alloc.height - req.height - margin;
  if (x < origin_x)
    x = origin_x;
  if (y < origin_y)
    y = origin_y;

  gtk_window_move(GTK_WINDOW(app->typeahead_popup), x, y);
}

static void typeahead_ensure_popup(AppWidgets *app) {
  if (app->typeahead_popup)
    return;

  GtkWidget *popup = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_window_set_transient_for(GTK_WINDOW(popup), GTK_WINDOW(app->window));
  gtk_window_set_type_hint(GTK_WINDOW(popup), GDK_WINDOW_TYPE_HINT_UTILITY);
  gtk_window_set_resizable(GTK_WINDOW(popup), FALSE);
  gtk_window_set_accept_focus(GTK_WINDOW(popup),
                              FALSE); /* focus icon view par hi rahay */

  GtkWidget *frame = gtk_frame_new(NULL);
  gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
  gtk_container_add(GTK_CONTAINER(popup), frame);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(hbox), 6);
  gtk_container_add(GTK_CONTAINER(frame), hbox);

  GtkWidget *icon =
      gtk_image_new_from_icon_name("edit-find-symbolic", GTK_ICON_SIZE_MENU);
  gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);

  GtkWidget *label = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  app->typeahead_popup = popup;
  app->typeahead_label = label;
}

static gboolean typeahead_select_match(AppWidgets *app,
                                       const char *buffer_folded) {
  GtkTreeModel *model = GTK_TREE_MODEL(app->file_filter);
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter_first(model, &iter))
    return FALSE;

  do {
    char *name = NULL;
    gtk_tree_model_get(model, &iter, COL_NAME, &name, -1);
    if (name) {
      char *folded = g_utf8_casefold(name, -1);
      gboolean match = g_str_has_prefix(folded, buffer_folded);
      g_free(folded);
      g_free(name);
      if (match) {
        GtkTreePath *tpath = gtk_tree_model_get_path(model, &iter);
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(app->icon_view));
        gtk_icon_view_select_path(GTK_ICON_VIEW(app->icon_view), tpath);
        gtk_icon_view_set_cursor(GTK_ICON_VIEW(app->icon_view), tpath, NULL,
                                 FALSE);
        gtk_icon_view_scroll_to_path(GTK_ICON_VIEW(app->icon_view), tpath,
                                     FALSE, 0, 0);
        gtk_tree_path_free(tpath);
        return TRUE;
      }
    }
  } while (gtk_tree_model_iter_next(model, &iter));

  return FALSE;
}

static gboolean typeahead_timeout_cb(gpointer data) {
  AppWidgets *app = data;
  app->typeahead_timeout_id = 0;
  fileview_typeahead_cancel(app);
  return G_SOURCE_REMOVE;
}

static void typeahead_update(AppWidgets *app) {
  typeahead_ensure_popup(app);

  gtk_label_set_text(GTK_LABEL(app->typeahead_label),
                     app->typeahead_buffer->str);

  if (!gtk_widget_get_visible(app->typeahead_popup))
    gtk_widget_show_all(app->typeahead_popup);
  typeahead_popup_position(app);

  char *folded = g_utf8_casefold(app->typeahead_buffer->str, -1);
  typeahead_select_match(app, folded);
  g_free(folded);

  if (app->typeahead_timeout_id)
    g_source_remove(app->typeahead_timeout_id);
  app->typeahead_timeout_id = g_timeout_add(3000, typeahead_timeout_cb, app);
}

gboolean fileview_typeahead_is_active(AppWidgets *app) {
  return app && app->typeahead_popup &&
         gtk_widget_get_visible(app->typeahead_popup);
}

void fileview_typeahead_cancel(AppWidgets *app) {
  if (!app)
    return;
  if (app->typeahead_timeout_id) {
    g_source_remove(app->typeahead_timeout_id);
    app->typeahead_timeout_id = 0;
  }
  if (app->typeahead_buffer) {
    g_string_free(app->typeahead_buffer, TRUE);
    app->typeahead_buffer = NULL;
  }
  if (app->typeahead_popup)
    gtk_widget_hide(app->typeahead_popup);
}

static gboolean typeahead_handle_key(AppWidgets *app, GdkEventKey *event) {
  if (event->keyval == GDK_KEY_BackSpace) {
    if (!app->typeahead_buffer || app->typeahead_buffer->len == 0)
      return FALSE;
    g_string_truncate(app->typeahead_buffer, app->typeahead_buffer->len - 1);
    if (app->typeahead_buffer->len == 0) {
      fileview_typeahead_cancel(app);
      return TRUE;
    }
    typeahead_update(app);
    return TRUE;
  }

  gunichar ch = gdk_keyval_to_unicode(event->keyval);
  if (ch == 0 || !g_unichar_isprint(ch) || g_unichar_isspace(ch))
    return FALSE;

  if (!app->typeahead_buffer)
    app->typeahead_buffer = g_string_new(NULL);
  g_string_append_unichar(app->typeahead_buffer, ch);

  typeahead_update(app);
  return TRUE;
}

gboolean on_iconview_key_press(GtkWidget *widget, GdkEventKey *event,
                               gpointer user_data) {
  AppWidgets *app = user_data;
  (void)widget;

  if (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))
    return FALSE;

  switch (event->keyval) {
  case GDK_KEY_Up:
  case GDK_KEY_Down:
  case GDK_KEY_Left:
  case GDK_KEY_Right:
  case GDK_KEY_Home:
  case GDK_KEY_End:
    return move_iconview_cursor(app, event);
  default:
    return typeahead_handle_key(app, event);
  }
}

static gboolean on_iconview_ctrl_press(AppWidgets *app, GtkWidget *widget,
                                       GdkEventButton *event) {
  GtkTreePath *tpath = gtk_icon_view_get_path_at_pos(
      GTK_ICON_VIEW(widget), (gint)event->x, (gint)event->y);
  if (!tpath)
    return FALSE;

  gboolean already_selected =
      gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), tpath);
  GList *sel = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(widget));
  guint n_selected = g_list_length(sel);
  g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);

  if (already_selected && n_selected > 1) {
    if (app->ctrl_drag_pending_path)
      gtk_tree_path_free(app->ctrl_drag_pending_path);
    app->ctrl_drag_pending_path = tpath; /* ownership yahan aa gayi */
    app->ctrl_drag_press_x = event->x;
    app->ctrl_drag_press_y = event->y;
    return TRUE; /* selection ko abhi mat chhedo */
  }

  gtk_tree_path_free(tpath);
  return FALSE;
}

gboolean on_iconview_motion(GtkWidget *widget, GdkEventMotion *event,
                            gpointer user_data) {
  AppWidgets *app = user_data;
  if (!app->ctrl_drag_pending_path)
    return FALSE;

  if (!(event->state & GDK_BUTTON1_MASK)) {
    gtk_tree_path_free(app->ctrl_drag_pending_path);
    app->ctrl_drag_pending_path = NULL;
    return FALSE;
  }

  if (!gtk_drag_check_threshold(widget, (gint)app->ctrl_drag_press_x,
                                (gint)app->ctrl_drag_press_y, (gint)event->x,
                                (gint)event->y))
    return FALSE;

  GtkTargetList *tlist =
      gtk_target_list_new(dnd_targets, G_N_ELEMENTS(dnd_targets));
  gtk_drag_begin_with_coordinates(widget, tlist, GDK_ACTION_COPY, 1,
                                  (GdkEvent *)event, (gint)event->x,
                                  (gint)event->y);
  gtk_target_list_unref(tlist);

  gtk_tree_path_free(app->ctrl_drag_pending_path);
  app->ctrl_drag_pending_path = NULL;
  return TRUE;
}

gboolean on_iconview_button_release(GtkWidget *widget, GdkEventButton *event,
                                    gpointer user_data) {
  AppWidgets *app = user_data;
  (void)event;
  if (!app->ctrl_drag_pending_path)
    return FALSE;

  gtk_icon_view_unselect_path(GTK_ICON_VIEW(widget),
                              app->ctrl_drag_pending_path);
  gtk_tree_path_free(app->ctrl_drag_pending_path);
  app->ctrl_drag_pending_path = NULL;
  return TRUE;
}

static void on_computer_menu_mount(GtkMenuItem *mi, gpointer user_data) {
  AppWidgets *app = user_data;
  GVolume *volume = g_object_get_data(G_OBJECT(mi), "volume");
  if (volume)
    mount_computer_volume(app, volume);
}

static void on_computer_menu_properties(GtkMenuItem *mi, gpointer user_data) {
  AppWidgets *app = user_data;
  const char *path = g_object_get_data(G_OBJECT(mi), "path");
  const char *name = g_object_get_data(G_OBJECT(mi), "name");
  gboolean is_removable =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "removable"));
  if (path && *path)
    show_drive_properties_dialog(app, path, name ? name : path, is_removable);
}

/* "Computer" (drives) page par right-click -- ab tak yahan kuch hota
 * hi nahi tha, is liye "Properties" tak access hi nahi tha. Mounted
 * drive (internal ya external/USB) ke liye Open + Properties, aur
 * unmounted volume ke liye Mount dikhate hain. */
static void show_computer_context_menu(AppWidgets *app, const char *item_path,
                                       const char *item_name, gboolean mounted,
                                       gboolean is_removable, GVolume *volume,
                                       GdkEventButton *event) {
  GtkWidget *menu = gtk_menu_new();
  GtkWidget *item;

  if (!mounted && volume) {
    item = gtk_menu_item_new_with_label("Mount");
    g_object_set_data_full(G_OBJECT(item), "volume", g_object_ref(volume),
                           g_object_unref);
    g_signal_connect(item, "activate", G_CALLBACK(on_computer_menu_mount),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  }

  if (mounted && item_path && *item_path) {
    item = gtk_menu_item_new_with_label("Open");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path),
                           g_free);
    g_signal_connect(item, "activate", G_CALLBACK(generic_menuitem_activate),
                     app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    item = gtk_menu_item_new_with_label("Properties");
    g_object_set_data_full(G_OBJECT(item), "path", g_strdup(item_path),
                           g_free);
    g_object_set_data_full(G_OBJECT(item), "name",
                           g_strdup(item_name ? item_name : item_path), g_free);
    g_object_set_data(G_OBJECT(item), "removable",
                      GINT_TO_POINTER(is_removable));
    g_signal_connect(item, "activate",
                     G_CALLBACK(on_computer_menu_properties), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  }

  if (!gtk_container_get_children(GTK_CONTAINER(menu))) {
    gtk_widget_destroy(menu);
    return;
  }

  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

gboolean on_button_press(GtkWidget *widget, GdkEventButton *event,
                         gpointer user_data) {
  AppWidgets *app = user_data;

  if (event->button == 1 && event->type == GDK_BUTTON_PRESS &&
      (event->state & GDK_CONTROL_MASK))
    return on_iconview_ctrl_press(app, widget, event);

  if (event->button != 3)
    return FALSE;

  if (g_strcmp0(app->current_dir, COMPUTER_URI) == 0) {
    GtkTreePath *ctpath = gtk_icon_view_get_path_at_pos(
        GTK_ICON_VIEW(widget), (gint)event->x, (gint)event->y);
    if (!ctpath)
      return TRUE;

    GtkTreeIter filter_iter, iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                                ctpath)) {
      gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter, &iter,
                                                       &filter_iter);

      char *c_name = NULL, *c_path = NULL;
      gboolean c_mounted = FALSE;
      gpointer c_volume_ptr = NULL;
      gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_NAME,
                         &c_name, COL_PATH, &c_path, COL_IS_DIR, &c_mounted,
                         COL_VOLUME, &c_volume_ptr, -1);

      if (!gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), ctpath)) {
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
        gtk_icon_view_select_path(GTK_ICON_VIEW(widget), ctpath);
      }

      show_computer_context_menu(app, c_path, c_name, c_mounted,
                                 c_volume_ptr != NULL,
                                 c_volume_ptr ? G_VOLUME(c_volume_ptr) : NULL,
                                 event);

      g_free(c_name);
      g_free(c_path);
    }
    gtk_tree_path_free(ctpath);
    return TRUE;
  }

  GtkTreePath *tpath = gtk_icon_view_get_path_at_pos(
      GTK_ICON_VIEW(widget), (gint)event->x, (gint)event->y);
  char *item_path = NULL, *item_name = NULL;
  gboolean is_dir = FALSE;

  if (tpath) {
    GtkTreeIter filter_iter, iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->file_filter), &filter_iter,
                                tpath)) {
      gtk_tree_model_filter_convert_iter_to_child_iter(app->file_filter, &iter,
                                                       &filter_iter);
      gtk_tree_model_get(GTK_TREE_MODEL(app->file_store), &iter, COL_PATH,
                         &item_path, COL_NAME, &item_name, COL_IS_DIR, &is_dir,
                         -1);
    }
    if (!gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), tpath)) {
      gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
      gtk_icon_view_select_path(GTK_ICON_VIEW(widget), tpath);
    }
    gtk_tree_path_free(tpath);
  }

  show_context_menu(app, item_path, item_name, is_dir, event);

  g_free(item_path);
  g_free(item_name);
  return TRUE;
}

static gboolean G_GNUC_UNUSED copy_recursive(GFile *src, GFile *dst,
                                             GError **error) {
  GFileType type = g_file_query_file_type(src, G_FILE_QUERY_INFO_NONE, NULL);
  if (type == G_FILE_TYPE_DIRECTORY) {
    if (!g_file_make_directory(dst, NULL, error)) {
      if (*error && (*error)->code == G_IO_ERROR_EXISTS) {
        g_clear_error(error);
      } else {
        return FALSE;
      }
    }
    GFileEnumerator *en = g_file_enumerate_children(
        src, "standard::name", G_FILE_QUERY_INFO_NONE, NULL, error);
    if (!en)
      return FALSE;

    gboolean ok = TRUE;
    GFileInfo *info;
    while (ok && (info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
      const char *name = g_file_info_get_name(info);
      GFile *child_src = g_file_get_child(src, name);
      GFile *child_dst = g_file_get_child(dst, name);
      ok = copy_recursive(child_src, child_dst, error);
      g_object_unref(child_src);
      g_object_unref(child_dst);
      g_object_unref(info);
    }
    g_file_enumerator_close(en, NULL, NULL);
    g_object_unref(en);
    return ok;
  }
  return g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, error);
}

__attribute__((unused)) static char *unique_dest_path(const char *dir,
                                                      const char *basename) {
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

void fileview_copy_location(AppWidgets *app) {
  char *path = fileview_get_selected_path(app);
  if (!path)
    return;

  GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);

  gtk_clipboard_set_text(cb, path, -1);
  gtk_clipboard_store(cb);

  g_free(path);
}
