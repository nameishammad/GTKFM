#include "terminal.h"
#include "fileview.h"
#include <pango/pango-font.h>
#include <pwd.h>
#include <unistd.h>
#include <vte/vte.h>
#define TERMINAL_HEIGHT 220

typedef struct {
  AppWidgets *app;
  GtkWidget *vte;
  GtkWidget *scrolled;
  gboolean spawned;
  gboolean exiting;
  gboolean reopen_pending;
} TermPanel;

typedef struct {
  GtkWidget *vte;
  AppWidgets *app;
} SpawnGuard;

GdkRGBA palette[16] = {
    {0.11, 0.11, 0.12, 1.0}, // Black
    {0.95, 0.29, 0.35, 1.0}, // Red
    {0.65, 0.89, 0.18, 1.0}, // Green
    {0.98, 0.83, 0.27, 1.0}, // Yellow
    {0.33, 0.67, 0.93, 1.0}, // Blue
    {0.78, 0.48, 0.87, 1.0}, // Magenta
    {0.33, 0.84, 0.83, 1.0}, // Cyan
    {0.80, 0.82, 0.85, 1.0}, // White

    {0.35, 0.38, 0.43, 1.0}, // Bright Black
    {1.00, 0.42, 0.46, 1.0}, // Bright Red
    {0.75, 0.95, 0.35, 1.0}, // Bright Green
    {1.00, 0.90, 0.45, 1.0}, // Bright Yellow
    {0.50, 0.78, 1.00, 1.0}, // Bright Blue
    {0.90, 0.65, 1.00, 1.0}, // Bright Magenta
    {0.50, 0.95, 0.95, 1.0}, // Bright Cyan
    {1.00, 1.00, 1.00, 1.0}  // Bright White
};

static void spawn_guard_vte_gone(gpointer data, GObject *where_the_object_was) {
  (void)where_the_object_was;
  SpawnGuard *guard = data;
  guard->vte = NULL;
}

static void term_panel_free(TermPanel *tp) { g_free(tp); }

static const char *default_shell_path(void) {
  const char *env_shell = g_getenv("SHELL");
  if (env_shell && *env_shell)
    return env_shell;

  struct passwd *pw = getpwuid(getuid());
  if (pw && pw->pw_shell && *pw->pw_shell)
    return pw->pw_shell;

  return "/bin/sh";
}

static void apply_theme_colors(GtkWidget *vte) {
  GtkStyleContext *ctx = gtk_widget_get_style_context(vte);

  GdkRGBA fg, bg;
  gboolean has_fg, has_bg;

  has_fg = gtk_style_context_lookup_color(ctx, "theme_fg_color", &fg);
  has_bg = gtk_style_context_lookup_color(ctx, "theme_base_color", &bg);

  if (!has_fg)
    gtk_style_context_get_color(ctx, GTK_STATE_FLAG_NORMAL, &fg);
  if (!has_bg) {
    GdkRGBA *bgp = NULL;
    gtk_style_context_get(ctx, GTK_STATE_FLAG_NORMAL,
                          GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &bgp, NULL);
    if (bgp) {
      bg = *bgp;
      gdk_rgba_free(bgp);
    } else {
      bg.red = bg.green = bg.blue = 0.0;
      bg.alpha = 1.0;
    }
  }
  vte_terminal_set_colors(VTE_TERMINAL(vte), &fg, &bg, palette, 16);
}

static void on_terminal_style_updated(GtkWidget *vte, gpointer user_data) {
  (void)user_data;
  apply_theme_colors(vte);
}

static void spawn_shell(AppWidgets *app, TermPanel *tp);
static void grab_focus_idle(GtkWidget *vte);

static void on_terminal_child_exited(VteTerminal *vte, gint status,
                                     gpointer user_data) {
  (void)vte;
  (void)status;
  TermPanel *tp = user_data;
  AppWidgets *app = tp->app;

  tp->spawned = FALSE;
  tp->exiting = FALSE;

  vte_terminal_reset(VTE_TERMINAL(tp->vte), TRUE, TRUE);

  if (app->terminal_panel_visible) {
    app->terminal_panel_visible = FALSE;
    gtk_widget_hide(app->terminal_panel);
    if (app->icon_view)
      gtk_widget_grab_focus(app->icon_view);
  }

  if (tp->reopen_pending) {
    tp->reopen_pending = FALSE;
    if (app->terminal_panel_visible) {
      spawn_shell(app, tp);
      grab_focus_idle(tp->vte);
    }
  }
}

static void on_spawn_done(VteTerminal *vte, GPid pid, GError *error,
                          gpointer user_data) {
  (void)vte;
  (void)pid;
  SpawnGuard *guard = user_data;
  gboolean still_alive = (guard->vte != NULL);

  if (still_alive)
    g_object_weak_unref(G_OBJECT(guard->vte), spawn_guard_vte_gone, guard);

  if (still_alive && error) {
    AppWidgets *app = guard->app;
    char *msg =
        g_strdup_printf("Terminal shuru nahi ho saka: %s", error->message);
    fileview_set_status(app, msg);
    g_free(msg);

    app->terminal_panel_visible = FALSE;
    gtk_widget_hide(app->terminal_panel);
  }

  g_free(guard);
}

static void spawn_shell(AppWidgets *app, TermPanel *tp) {
  const char *shell = default_shell_path();
  char *argv[2] = {(char *)shell, NULL};

  const char *cwd = (app->current_dir && *app->current_dir) ? app->current_dir
                                                            : g_get_home_dir();

  SpawnGuard *guard = g_new0(SpawnGuard, 1);
  guard->vte = tp->vte;
  guard->app = app;
  g_object_weak_ref(G_OBJECT(tp->vte), spawn_guard_vte_gone, guard);

  vte_terminal_spawn_async(VTE_TERMINAL(tp->vte), VTE_PTY_DEFAULT, cwd, argv,
                           NULL, /* envp: parent env inherit */
                           G_SPAWN_SEARCH_PATH, NULL, NULL, /* child setup */
                           NULL, /* child setup destroy */
                           -1,   /* default timeout */
                           NULL, /* cancellable */
                           on_spawn_done, guard);
  tp->spawned = TRUE;
  tp->exiting = FALSE;
}

typedef struct {
  GtkWidget *vte; /* weak-tracked */
} FocusIdleCtx;

static void focus_idle_vte_gone(gpointer data, GObject *where_the_object_was) {
  (void)where_the_object_was;
  FocusIdleCtx *ctx = data;
  ctx->vte = NULL;
}

static gboolean focus_idle_cb(gpointer data) {
  FocusIdleCtx *ctx = data;
  if (ctx->vte) {
    gtk_widget_grab_focus(ctx->vte);
    g_object_weak_unref(G_OBJECT(ctx->vte), focus_idle_vte_gone, ctx);
  }
  g_free(ctx);
  return G_SOURCE_REMOVE;
}

static void grab_focus_idle(GtkWidget *vte) {
  FocusIdleCtx *ctx = g_new0(FocusIdleCtx, 1);
  ctx->vte = vte;
  g_object_weak_ref(G_OBJECT(vte), focus_idle_vte_gone, ctx);
  g_idle_add(focus_idle_cb, ctx);
}

void terminal_panel_sync_cwd(AppWidgets *app) {
  if (!app || !app->terminal_panel || !app->terminal_panel_visible)
    return;

  TermPanel *tp = g_object_get_data(G_OBJECT(app->terminal_panel), "termpanel");
  if (!tp || !tp->spawned || tp->exiting)
    return;
  if (!app->current_dir || !*app->current_dir)
    return;

  char *quoted = g_shell_quote(app->current_dir);
  char *cmd = g_strdup_printf(" cd %s\n", quoted);
  vte_terminal_feed_child(VTE_TERMINAL(tp->vte), cmd, -1);
  g_free(cmd);
  g_free(quoted);
}

void terminal_panel_toggle(AppWidgets *app) {
  if (!app || !app->terminal_panel)
    return;

  TermPanel *tp = g_object_get_data(G_OBJECT(app->terminal_panel), "termpanel");
  if (!tp)
    return;

  app->terminal_panel_visible = !app->terminal_panel_visible;

  if (app->terminal_panel_visible) {
    gtk_widget_show(app->terminal_panel);
    if (app->root_paned) {
      GtkAllocation alloc;
      gtk_widget_get_allocation(app->root_paned, &alloc);
      if (app->terminal_position > 0 && app->terminal_position < alloc.height) {
        gtk_paned_set_position(GTK_PANED(app->root_paned),
                               app->terminal_position);
      } else if (alloc.height > TERMINAL_HEIGHT + 150) {
        gtk_paned_set_position(GTK_PANED(app->root_paned),
                               alloc.height - TERMINAL_HEIGHT);
      }
    }

    if (app->dir_monitor_debounce_id) {
      g_source_remove(app->dir_monitor_debounce_id);
      app->dir_monitor_debounce_id = 0;
    }

    if (tp->exiting) {
      tp->reopen_pending = TRUE;
    } else if (!tp->spawned) {
      spawn_shell(app, tp);
      grab_focus_idle(tp->vte);
    } else {
      terminal_panel_sync_cwd(app);
      grab_focus_idle(tp->vte);
    }
  } else {
    gtk_widget_hide(app->terminal_panel);

    if (tp->spawned && !tp->exiting) {
      tp->exiting = TRUE;
      vte_terminal_feed_child(VTE_TERMINAL(tp->vte), "exit\n", -1);
    }

    if (app->icon_view)
      gtk_widget_grab_focus(app->icon_view);
  }
}

gboolean terminal_panel_has_focus(AppWidgets *app) {
  if (!app || !app->terminal_panel || !app->terminal_panel_visible)
    return FALSE;

  TermPanel *tp = g_object_get_data(G_OBJECT(app->terminal_panel), "termpanel");
  if (!tp || !tp->vte)
    return FALSE;

  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(app->window));
  return focus == tp->vte;
}

GtkWidget *terminal_panel_create(AppWidgets *app) {
  TermPanel *tp = g_new0(TermPanel, 1);
  tp->app = app;

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(root, -1, TERMINAL_HEIGHT);

  gtk_box_pack_start(GTK_BOX(root),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                     FALSE, 0);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(root), scrolled, TRUE, TRUE, 0);

  tp->vte = vte_terminal_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(tp->vte),
                              "gtkfm-terminal");

  apply_theme_colors(tp->vte);
  g_signal_connect(tp->vte, "style-updated",
                   G_CALLBACK(on_terminal_style_updated), NULL);

  g_signal_connect(tp->vte, "child-exited",
                   G_CALLBACK(on_terminal_child_exited), tp);
  /* GTK theme ke colors use karega, sirf font customize karte hain */
  PangoFontDescription *font =
      pango_font_description_from_string("JetBrainsMono Nerd Font 8");

  vte_terminal_set_font(VTE_TERMINAL(tp->vte), font);
  pango_font_description_free(font);

  /* Cursor */
  vte_terminal_set_cursor_shape(VTE_TERMINAL(tp->vte), VTE_CURSOR_SHAPE_BLOCK);
  vte_terminal_set_cursor_blink_mode(VTE_TERMINAL(tp->vte),
                                     VTE_CURSOR_BLINK_OFF);

  /* Smooth scrolling */
  vte_terminal_set_scroll_on_output(VTE_TERMINAL(tp->vte), FALSE);
  vte_terminal_set_scroll_on_keystroke(VTE_TERMINAL(tp->vte), TRUE);

  /* Large scrollback */
  vte_terminal_set_scrollback_lines(VTE_TERMINAL(tp->vte), 10000);

  vte_terminal_set_cell_height_scale(VTE_TERMINAL(tp->vte), 1.0);
  vte_terminal_set_cell_width_scale(VTE_TERMINAL(tp->vte), 1.0);

  vte_terminal_set_scrollback_lines(VTE_TERMINAL(tp->vte), 4096);
  vte_terminal_set_cursor_blink_mode(VTE_TERMINAL(tp->vte),
                                     VTE_CURSOR_BLINK_OFF);
  gtk_container_add(GTK_CONTAINER(scrolled), tp->vte);

  g_signal_connect(tp->vte, "child-exited",
                   G_CALLBACK(on_terminal_child_exited), tp);

  g_object_set_data_full(G_OBJECT(root), "termpanel", tp,
                         (GDestroyNotify)term_panel_free);

  gtk_widget_show_all(root);
  gtk_widget_set_no_show_all(root, TRUE);
  gtk_widget_hide(root);

  return root;
}
