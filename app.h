#ifndef APP_H
#define APP_H

#include <gtk/gtk.h>

enum {
    COL_PIXBUF,
    COL_NAME,
    COL_PATH,
    COL_IS_DIR,
    COL_VOLUME,
    COL_DATE,
    COL_SIZE,
    COL_TYPE,
    COL_CUT,
    COL_IS_SYMLINK,
    COL_THEMED_ICON,
    COL_GICON,
    COL_IS_HIDDEN,
    COL_IS_READONLY,
    COL_RECENT_RANK,
    N_COLS
};

enum {
    SB_COL_ICON = 0,
    SB_COL_NAME,
    SB_COL_PATH,
    SB_COL_IS_DEVICE,
    SB_COL_MOUNTED,
    SB_COL_VOLUME,
    SB_COL_MOUNT,
    SB_COL_IS_BOOKMARK,
    SB_N_COLS
};

typedef enum {
    SORT_NAME_ASC,
    SORT_NAME_DESC,
    SORT_DATE_NEWEST,
    SORT_DATE_OLDEST,
    SORT_SIZE_LARGEST,
    SORT_SIZE_SMALLEST,
    SORT_TYPE_ASC,
    SORT_TYPE_DESC
} SortMode;

typedef enum {
    UNDO_DELETE,
    UNDO_RENAME,
    UNDO_MOVE,
    UNDO_COPY,
    /* Ek hi operation (ek saath kai files trash/copy/move) ko ek hi
     * Ctrl+Z se undo karne ke liye -- `batch` GList mein har file ka apna
     * (non-batch) UndoAction hota hai. */
    UNDO_BATCH
} UndoType;


typedef struct {
    UndoType type;

    char *src;
    char *dest;

    char *old_name;
    char *new_name;

    /* Sirf UNDO_BATCH ke liye: sub-items ki list (har element ek
     * UndoAction*, type kabhi UNDO_BATCH nahi hota). */
    GList *batch;

} UndoAction;

#define COMPUTER_URI "computer://"
#define RECENT_URI "recent://"

typedef struct {
    GtkWidget   *window;
    GtkWidget   *paned;
    gboolean sidebar_visible;
    gint sidebar_position;
    /* F3 se split-view (do panes) active thi ya nahi -- sirf owner pane
     * (panes[0], jis ke paas apna "asal" paned/sidebar hota hai) ye
     * save/load karta hai, taake agli baar app khulne par split khud-ba-
     * khud wapas ban jaye. */
    gboolean split_view_active;
    GtkWidget   *content_paned;    /* fileview + right info panel wala horizontal paned */
    GtkWidget   *info_panel;       /* Dolphin jaisa right-side "Information" panel (F11) */
    gboolean     info_panel_visible;
    GtkWidget   *terminal_panel;   /* Dolphin jaisa neeche wala embedded terminal (F4) */
    gboolean     terminal_panel_visible;
    /* User ne mouse se root_paned (terminal) aur paned (sidebar) ke handle
     * jahan chhoray hain wahi position -- settings file mein save hoti hai
     * taake agli baar app khulne par bhi wahi jagah yaad rahe. 0 (ya kam)
     * ka matlab "kabhi save nahi hui", is soorat mein purana default
     * (sensible auto) behavior chalta hai. */
    // gint         sidebar_position;
    gint         terminal_position;
    gint         content_paned_position; /* info panel (F11) ka divider */
    guint        layout_save_debounce_id;

    /* Window maximized thi ya nahi, aur (agar maximized nahi thi to)
     * kitni width/height par thi -- taake agli baar app khulne par
     * bilkul wahi jagah/size wapas mil jaye. */
    gboolean     window_maximized;
    gint         window_width;
    gint         window_height;
    /* Vertical GtkPaned jis ke pack1 mein file-view+statusbar aur pack2
     * mein terminal_panel hai -- isi ki wajah se terminal ko mouse se
     * upar-neeche resize kiya ja sakta hai (Dolphin jaisa). main.c isay
     * banata hai, terminal.c isay F4 par khulte waqt initial height dene
     * ke liye use karta hai. */
    GtkWidget   *root_paned;
    GtkWidget   *sidebar_tree;
    GtkTreeStore *sidebar_store;
    GtkWidget   *icon_view;
    GtkListStore *file_store;
    GtkTreeModelFilter *file_filter;
    char        *search_text;
    GtkWidget   *search_entry;

    /* Toolbar wali search bar -- current folder + tamam sub-folders mein
     * (recursive) dhoondti hai. */
    gboolean      search_recursive_active;
    GCancellable *search_cancellable;
    guint         search_generation;

    /* Thunar jaisa "type-ahead" quick search: search bar se bahar, seedha
     * icon view par type karne se bottom-right corner mein ek chhota box
     * dikhta hai aur matching file select ho jati hai. */
    GString   *typeahead_buffer;
    GtkWidget *typeahead_popup;
    GtkWidget *typeahead_label;
    guint      typeahead_timeout_id;
    GtkWidget   *path_entry;
    /* Dolphin jaisi "breadcrumb" location bar: default mein path ka har
     * folder alag clickable button hota hai; click karte hi seedha us
     * folder mein chala jata hai. Ek chota pencil button dabane se ya
     * Ctrl+L se ye edit mode (path_entry, seedha type karne wala) mein
     * badal jati hai. main.c banata/manage karta hai; fileview-core.c
     * navigate hone ke baad fileview_refresh_location_bar() se ise
     * refresh kar deta hai. */
    GtkWidget   *location_breadcrumb_item;  /* toolbar item: breadcrumb buttons wala box */
    GtkWidget   *location_breadcrumb_box;   /* horizontal box jis mein per-folder buttons hain */
    GtkWidget   *location_entry_item;       /* toolbar item: path_entry (edit mode mein hi visible) */
    GtkWidget   *location_go_item;          /* toolbar item: "Go" button (edit mode mein hi visible) */
    /* User ki pasand: TRUE ho to location bar hamesha edit (text entry)
     * style mein rehti hai (har navigate ke baad bhi), FALSE ho to
     * default Dolphin jaisa breadcrumb behavior. settings file mein save
     * hota hai, is liye agli baar app khulne par wahi style yaad rehti
     * hai. Three-dots overflow menu mein iska checkbox hai. */
    gboolean     location_bar_always_edit;
    GtkWidget   *location_style_menu_item;
    GtkWidget   *statusbar;
    guint        statusbar_ctx;
    GtkWidget   *freespace_label;
    char        *default_status;
    GtkWidget   *btn_back;
    GtkWidget   *btn_forward;
    GtkWidget   *btn_up;
    GtkWidget   *preview_toggle;  /* three-dots overflow menu ka "Folder Previews" checkbox */
    GtkWidget   *hidden_menu_item;
    GtkWidget   *video_frame_menu_item;

    char  *current_dir;
    GList *history_back;
    GList *history_forward;
    char  *pending_select_path;
    /* Copy/move/paste ke baad "Thunar jaisa" multi-select ke liye: in mein
     * saare naye/pasted top-level items ke (owned, strdup'd) full paths
     * hote hain -- list order = copy order. Consumer (populate()) sab ko
     * select karta hai lekin keyboard cursor/focus sirf AAKHRI (list ke
     * aakhri) item par rakhta hai. Agar ye set ho to pending_select_path
     * (single-item wala) ignore ho jata hai. */
    GList *pending_select_paths;

    GVolumeMonitor *volume_monitor;

    /* Current folder par live GFileMonitor -- taake bahar se (gnome-files/
     * Nautilus se archive extract karna, koi doosra terminal/app files
     * badalna, device nikal jaana waghera) hone wale changes khud-ba-khud
     * (bina manual Refresh click kiye) nazar aa jayein. */
    GFileMonitor *dir_monitor;
    guint         dir_monitor_debounce_id;
    /* F4 se terminal khulte hi shell startup (rc files, history, cache
     * waghera) kabhi kabhi current folder mein filesystem activity paida
     * kar deti hai, jis se dir monitor ek fazool refresh chala deta hai
     * aur terminal se focus wapas file list par chala jata hai. Isliye
     * terminal spawn/cd ke turant baad thodi der (grace period) ke liye
     * dir-monitor events ignore kar dete hain -- terminal.c isay set karta
     * hai, fileview-core.c ka on_dir_monitor_event() isay check karta hai. */
    gint64        dir_monitor_suppress_until;
    /* User ki pasand: TRUE (default) ho to bahar se hone wale changes
     * (archive extract, terminal cp/mv waghera) khud-ba-khud dikhte hain
     * (dir_monitor ke zariye). FALSE ho to sirf manual Refresh (ya F5)
     * se update hoga. settings file mein save hota hai, three-dots
     * overflow menu mein iska checkbox hai (auto_refresh_menu_item). */
    gboolean      auto_refresh_enabled;
    GtkWidget    *auto_refresh_menu_item;

    gboolean folders_first;
    gboolean folder_previews;
    gboolean show_hidden;
    /* Video files ke thumbnails ke around Thunar/Nautilus jaisa halka
     * "photo frame" border. settings file mein save hota hai, three-dots
     * overflow menu mein iska checkbox hai (video_frame_menu_item). */
    gboolean video_thumbnail_frame;
    /* TRUE ho to files/folders ek column mein, har item apni row par
     * (icon+naam saath saath) dikhte hain -- grid ki bajaye list jaisa.
     * settings file mein save hota hai, three-dots overflow menu mein
     * iska checkbox hai (list_view_menu_item). */
    gboolean list_view;
    GtkWidget *list_view_menu_item;
    SortMode sort_mode;
    int      icon_size;
    GCancellable *populate_cancellable;
    guint populate_generation;
    GtkTreePath *ctrl_drag_pending_path;
    gdouble      ctrl_drag_press_x;
    gdouble      ctrl_drag_press_y;
    gint         select_anchor_idx; /* shift+arrow/home/end range selection ka anchor; -1 = unset */

    /* Ctrl+Z undo stack -- head = sab se recent action (UndoAction*, ownership
     * yahan). undo_push()/undo_last() (undo.c) hi ise chhedte hain. */
    GList *undo_stack;
} AppWidgets;

#endif /* APP_H */
