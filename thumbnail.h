#ifndef THUMBNAIL_H
#define THUMBNAIL_H

#include <gtk/gtk.h>

void thumbnail_init(void);

void thumbnail_shutdown(void);

/* Video files ke thumbnails ke around Thunar/Nautilus jaisa halka
 * "photo frame" border chahiye ya nahi -- global preference hai (saari
 * split-view panes/thumbnail requests isay share karti hain), app.c/main.c
 * settings load/toggle hote hi isay update karta hai. */
void thumbnail_set_video_frame_enabled(gboolean enabled);

typedef void (*ThumbnailReadyFunc)(const char *filepath, GdkPixbuf *pixbuf,
                                   gpointer user_data);

void thumbnail_get_async(const char *filepath, int size,
                         ThumbnailReadyFunc callback, gpointer user_data);

void folder_preview_get_async(const char *dirpath, int size,
                              GdkPixbuf *base_icon, ThumbnailReadyFunc callback,
                              gpointer user_data);

#endif /* THUMBNAIL_H */
