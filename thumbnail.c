#include "thumbnail.h"
#include <cairo.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define THUMB_W 70
#define THUMB_H 96
/* ---- job bookkeeping ------------------------------------------------- */

typedef struct {
  char *filepath;
  int size;
  ThumbnailReadyFunc callback;
  gpointer user_data;
} ThumbJob;

typedef struct {
  char *filepath;
  GdkPixbuf *pixbuf;
  ThumbnailReadyFunc callback;
  gpointer user_data;
} ThumbResult;

static GThreadPool *pool = NULL;
static gboolean g_video_frame_enabled = TRUE;
static GdkPixbuf *apply_video_frame(GdkPixbuf *src);

/* ---- in-memory pixbuf cache -------------------------------------------- */
#define THUMB_MEM_CACHE_MAX 512

static GHashTable *mem_cache = NULL; /* key: char* cache_path -> GdkPixbuf* */
static GQueue mem_cache_order =
    G_QUEUE_INIT; /* keys, oldest first, for eviction */
static GMutex mem_cache_lock;

static GdkPixbuf *mem_cache_lookup(const char *key) {
  GdkPixbuf *hit = NULL;
  g_mutex_lock(&mem_cache_lock);
  if (mem_cache) {
    GdkPixbuf *found = g_hash_table_lookup(mem_cache, key);
    if (found)
      hit = g_object_ref(found);
  }
  g_mutex_unlock(&mem_cache_lock);
  return hit;
}

static void mem_cache_store(const char *key, GdkPixbuf *pixbuf) {
  if (!pixbuf)
    return;
  g_mutex_lock(&mem_cache_lock);
  if (!mem_cache)
    mem_cache =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

  if (!g_hash_table_contains(mem_cache, key))
    g_queue_push_tail(&mem_cache_order, g_strdup(key));

  g_hash_table_insert(mem_cache, g_strdup(key), g_object_ref(pixbuf));

  while (g_queue_get_length(&mem_cache_order) > THUMB_MEM_CACHE_MAX) {
    char *oldest = g_queue_pop_head(&mem_cache_order);
    g_hash_table_remove(mem_cache, oldest);
    g_free(oldest);
  }
  g_mutex_unlock(&mem_cache_lock);
}

/* ---- helpers ----------------------------------------------------------*/

static char *cache_dir_path(void) {
  char *dir =
      g_build_filename(g_get_user_cache_dir(), "gtkfm", "thumbnails-v3", NULL);
  g_mkdir_with_parents(dir, 0700);
  return dir;
}

static char *cache_path_for(const char *filepath, int size) {
  struct stat st;
  if (stat(filepath, &st) != 0)
    return NULL;

  char *key = g_strdup_printf("%s:%ld:%d", filepath, (long)st.st_mtime, size);
  char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
  g_free(key);

  char *dir = cache_dir_path();
  char *full = g_strdup_printf("%s/%s.png", dir, hash);
  g_free(dir);
  g_free(hash);
  return full;
}

static gboolean run_cmd(char **argv) {
  GError *err = NULL;
  gint exit_status = -1;
  gboolean ok = g_spawn_sync(NULL, argv, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                                 G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, NULL, NULL, &exit_status, &err);
  if (!ok) {
    if (err)
      g_error_free(err);
    return FALSE;
  }
  return exit_status == 0;
}

typedef enum { CAT_IMAGE, CAT_VIDEO, CAT_AUDIO, CAT_PDF, CAT_OTHER } FileCat;

static gboolean path_has_ext(const char *path, const char *ext) {
  const char *dot = strrchr(path, '.');
  if (!dot)
    return FALSE;

  return g_ascii_strcasecmp(dot, ext) == 0;
}

static FileCat detect_kind_by_magic(const guchar *b, gsize len) {
  if (len >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3)
    return CAT_VIDEO;

  if (len >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
    return CAT_IMAGE;

  if (len >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    return CAT_IMAGE;

  if (len >= 6 && memcmp(b, "GIF87a", 6) == 0)
    return CAT_IMAGE;

  if (len >= 6 && memcmp(b, "GIF89a", 6) == 0)
    return CAT_IMAGE;

  if (len >= 4 && memcmp(b, "%PDF", 4) == 0)
    return CAT_PDF;

  if (len >= 12 && memcmp(b, "RIFF", 4) == 0) {
    if (memcmp(b + 8, "WAVE", 4) == 0)
      return CAT_AUDIO;
    if (memcmp(b + 8, "AVI ", 4) == 0)
      return CAT_VIDEO;
    if (memcmp(b + 8, "WEBP", 4) == 0)
      return CAT_IMAGE;
  }

  if (len >= 3 && memcmp(b, "ID3", 3) == 0)
    return CAT_AUDIO;

  if (len >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0)
    return CAT_AUDIO;

  if (len >= 12 && memcmp(b + 4, "ftyp", 4) == 0)
    return CAT_VIDEO;

  return CAT_OTHER;
}

static FileCat guess_category(const char *filepath) {
  static const char *image_exts[] = {".png",  ".jpg", ".jpeg", ".gif", ".bmp",
                                     ".webp", ".tif", ".tiff", NULL};

  static const char *video_exts[] = {".mp4",  ".mkv", ".avi", ".mov",
                                     ".webm", ".flv", ".wmv", ".mpeg",
                                     ".mpg",  ".m4v", NULL};

  static const char *audio_exts[] = {".mp3", ".flac", ".ogg",  ".wav",
                                     ".aac", ".m4a",  ".opus", NULL};

  for (int i = 0; image_exts[i]; i++)
    if (path_has_ext(filepath, image_exts[i]))
      return CAT_IMAGE;

  for (int i = 0; video_exts[i]; i++)
    if (path_has_ext(filepath, video_exts[i]))
      return CAT_VIDEO;

  for (int i = 0; audio_exts[i]; i++)
    if (path_has_ext(filepath, audio_exts[i]))
      return CAT_AUDIO;

  if (path_has_ext(filepath, ".pdf"))
    return CAT_PDF;

  gboolean uncertain = FALSE;
  guchar buffer[8192];
  gsize len = 0;

  FILE *fp = fopen(filepath, "rb");
  if (fp) {
    len = fread(buffer, 1, sizeof(buffer), fp);
    fclose(fp);
  }

  char *ctype = g_content_type_guess(NULL, buffer, len, &uncertain);

  FileCat cat = CAT_OTHER;

  if (ctype) {
    if (g_str_has_prefix(ctype, "image/"))
      cat = CAT_IMAGE;
    else if (g_str_has_prefix(ctype, "video/"))
      cat = CAT_VIDEO;
    else if (g_str_has_prefix(ctype, "audio/"))
      cat = CAT_AUDIO;
    else if (g_strcmp0(ctype, "application/pdf") == 0)
      cat = CAT_PDF;

    g_free(ctype);
  }

  if (cat == CAT_OTHER)
    cat = detect_kind_by_magic(buffer, len);

  return cat;
}

static gboolean generate_thumbnail(const char *filepath, const char *out_path,
                                   int size) {
  FileCat cat = guess_category(filepath);
  char size_str[16];
  g_snprintf(size_str, sizeof(size_str), "%d", size);

  char *scale_filter = g_strdup_printf(
      "scale='min(%d,iw)':'min(%d,ih)':force_original_aspect_ratio=decrease",
      size, size);
  gboolean ok = FALSE;

  switch (cat) {
  case CAT_VIDEO: {
    char *argv[] = {"ffmpeg",         "-y",       "-ss",
                    "00:00:01",       "-i",       (char *)filepath,
                    "-frames:v",      "1",        "-vf",
                    scale_filter,     "-pix_fmt", "rgba",
                    (char *)out_path, NULL};
    ok = run_cmd(argv);
    if (!ok) {
      char *argv2[] = {"ffmpeg",
                       "-y",
                       "-i",
                       (char *)filepath,
                       "-frames:v",
                       "1",
                       "-vf",
                       scale_filter,
                       "-pix_fmt",
                       "rgba",
                       (char *)out_path,
                       NULL};
      ok = run_cmd(argv2);
    }
    break;
  }
  case CAT_AUDIO: {
    char *argv[] = {"ffmpeg", "-y",       "-i",         (char *)filepath,
                    "-an",    "-vf",      scale_filter, "-frames:v",
                    "1",      "-pix_fmt", "rgba",       (char *)out_path,
                    NULL};
    ok = run_cmd(argv);
    break;
  }
  case CAT_IMAGE: {
    GError *gerr = NULL;
    GdkPixbuf *src =
        gdk_pixbuf_new_from_file_at_scale(filepath, size, size, TRUE, &gerr);
    if (gerr) {
      g_error_free(gerr);
      gerr = NULL;
    }

    if (src) {
      ok = gdk_pixbuf_save(src, out_path, "png", NULL, NULL);
      g_object_unref(src);
    }

    if (!ok) {
      char *argv[] = {"ffmpeg",     "-y",       "-i",   (char *)filepath, "-vf",
                      scale_filter, "-pix_fmt", "rgba", (char *)out_path, NULL};
      ok = run_cmd(argv);
    }
    break;
  }
  case CAT_PDF: {
    char *prefix = g_strdup(out_path);
    char *dot = strrchr(prefix, '.');
    if (dot)
      *dot = '\0';
    char *argv[] = {"pdftoppm",    "-png",      "-f",     "1",
                    "-singlefile", "-scale-to", size_str, (char *)filepath,
                    prefix,        NULL};
    ok = run_cmd(argv);
    g_free(prefix);
    break;
  }
  default:
    ok = FALSE;
    break;
  }

  g_free(scale_filter);
  return ok && g_file_test(out_path, G_FILE_TEST_EXISTS);
}

/* ---- worker thread ------------------------------------------------------*/

static gboolean deliver_result_idle(gpointer data) {
  ThumbResult *res = data;
  res->callback(res->filepath, res->pixbuf, res->user_data);
  g_free(res->filepath);
  g_free(res);
  return G_SOURCE_REMOVE;
}

static void worker_func(gpointer data, gpointer user_data) {
  (void)user_data;
  ThumbJob *job = data;

  GdkPixbuf *pixbuf = NULL;
  char *cache_path = cache_path_for(job->filepath, job->size);

  if (cache_path) {
    pixbuf = mem_cache_lookup(cache_path);

    if (!pixbuf) {
      if (!g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
        generate_thumbnail(job->filepath, cache_path, job->size);
      }
      if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
        GError *err = NULL;
        pixbuf = gdk_pixbuf_new_from_file_at_scale(cache_path, job->size,
                                                   job->size, TRUE, &err);
        if (err)
          g_error_free(err);

        if (pixbuf && g_video_frame_enabled) {
          GdkPixbuf *framed = apply_video_frame(pixbuf);
          if (framed) {
            g_object_unref(pixbuf);
            pixbuf = framed;
          }
        }

        if (pixbuf)
          mem_cache_store(cache_path, pixbuf);
      }
    }
    g_free(cache_path);
  }

  ThumbResult *res = g_new0(ThumbResult, 1);
  res->filepath = job->filepath; /* transfer ownership */
  res->pixbuf = pixbuf;
  res->callback = job->callback;
  res->user_data = job->user_data;

  g_idle_add(deliver_result_idle, res);

  g_free(job);
}

/* =======================================================================
 *                      FOLDER THUMBS PREVIEW
 * ======================================================================= */

#define FOLDER_PREVIEW_MAX_ITEMS 4
#define FOLDER_PREVIEW_SCAN_LIMIT                                              \
  30 /* ek folder mein max itni entries scan karte hain */

typedef struct {
  char *dirpath;
  int size;
  GdkPixbuf *base_icon;
  ThumbnailReadyFunc callback;
  gpointer user_data;
} FolderPreviewJob;

static GThreadPool *preview_pool = NULL;

static GdkPixbuf *draw_generic_file_tile(int size) {
  if (size < 8)
    size = 8;
  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(surface);

  double fold = size * 0.28;

  /* Kaagaz jaisi shape, upar-right corner fold ke sath. */
  cairo_move_to(cr, size * 0.10, 0);
  cairo_line_to(cr, size - fold, 0);
  cairo_line_to(cr, size, fold);
  cairo_line_to(cr, size, size);
  cairo_line_to(cr, size * 0.10, size);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, 0.97, 0.97, 0.97, 1.0);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.62, 0.62, 0.62, 1.0);
  cairo_set_line_width(cr, size * 0.045);
  cairo_stroke(cr);

  /* Fold ka chhota triangle. */
  cairo_move_to(cr, size - fold, 0);
  cairo_line_to(cr, size - fold, fold);
  cairo_line_to(cr, size, fold);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, 0.82, 0.82, 0.82, 1.0);
  cairo_fill(cr);

  /* Chand halki "text lines". */
  cairo_set_source_rgba(cr, 0.68, 0.68, 0.68, 1.0);
  cairo_set_line_width(cr, size * 0.07);
  double lx = size * 0.24, rx = size * 0.80;
  for (int i = 0; i < 3; i++) {
    double ly = size * 0.42 + i * size * 0.17;
    if (ly > size * 0.92)
      break;
    cairo_move_to(cr, lx, ly);
    cairo_line_to(cr, rx, ly);
    cairo_stroke(cr);
  }

  cairo_destroy(cr);
  GdkPixbuf *pb = gdk_pixbuf_get_from_surface(surface, 0, 0, size, size);
  cairo_surface_destroy(surface);
  return pb;
}

static GdkPixbuf *pixbuf_trim_transparent(GdkPixbuf *src) {
  if (!src)
    return NULL;
  if (!gdk_pixbuf_get_has_alpha(src))
    return g_object_ref(src);

  int w = gdk_pixbuf_get_width(src);
  int h = gdk_pixbuf_get_height(src);
  int rowstride = gdk_pixbuf_get_rowstride(src);
  int n_channels = gdk_pixbuf_get_n_channels(src);
  guchar *pixels = gdk_pixbuf_get_pixels(src);

  int min_x = w, min_y = h, max_x = -1, max_y = -1;
  for (int y = 0; y < h; y++) {
    guchar *row = pixels + (size_t)y * rowstride;
    for (int x = 0; x < w; x++) {
      if (row[x * n_channels + 3] > 12) {
        if (x < min_x)
          min_x = x;
        if (x > max_x)
          max_x = x;
        if (y < min_y)
          min_y = y;
        if (y > max_y)
          max_y = y;
      }
    }
  }

  if (max_x < min_x || max_y < min_y)
    return g_object_ref(src); /* poora transparent -- normally nahi hoga */

  int cw = max_x - min_x + 1;
  int ch = max_y - min_y + 1;
  if (cw == w && ch == h)
    return g_object_ref(src);

  return gdk_pixbuf_new_subpixbuf(src, min_x, min_y, cw, ch);
}

static GdkPixbuf *pixbuf_scale_cover(GdkPixbuf *src, int target_w,
                                     int target_h) {
  if (!src || target_w < 1 || target_h < 1)
    return NULL;

  int sw = gdk_pixbuf_get_width(src);
  int sh = gdk_pixbuf_get_height(src);
  if (sw < 1 || sh < 1)
    return NULL;

  double scale = MAX((double)target_w / sw, (double)target_h / sh);
  int new_w = MAX(1, (int)(sw * scale + 0.5));
  int new_h = MAX(1, (int)(sh * scale + 0.5));

  GdkPixbuf *scaled =
      gdk_pixbuf_scale_simple(src, new_w, new_h, GDK_INTERP_HYPER);
  if (!scaled)
    return NULL;

  int off_x = MAX(0, (new_w - target_w) / 2);
  int off_y = MAX(0, (new_h - target_h) / 2);
  int crop_w = MIN(target_w, new_w - off_x);
  int crop_h = MIN(target_h, new_h - off_y);

  GdkPixbuf *cropped =
      gdk_pixbuf_new_subpixbuf(scaled, off_x, off_y, crop_w, crop_h);
  g_object_unref(scaled);
  return cropped;
}

static GdkPixbuf *load_child_preview(const char *filepath, int target_w,
                                     int target_h) {
  FileCat cat = guess_category(filepath);
  int fetch_size = (int)(MAX(target_w, target_h) * 3.0 + 0.5);
  if (fetch_size < 32)
    fetch_size = 32;

  GdkPixbuf *result = NULL;

  if (cat == CAT_IMAGE || cat == CAT_VIDEO || cat == CAT_PDF ||
      cat == CAT_AUDIO) {
    char *cache_path = cache_path_for(filepath, fetch_size);
    if (cache_path) {
      /* Tile size (target_w/target_h) can vary a bit between calls even for
       * the same fetch_size, so key the mem cache on the final tile, not
       * just the underlying PNG. */
      char *tile_key =
          g_strdup_printf("%s|tile:%dx%d", cache_path, target_w, target_h);
      result = mem_cache_lookup(tile_key);

      if (!result) {
        if (!g_file_test(cache_path, G_FILE_TEST_EXISTS))
          generate_thumbnail(filepath, cache_path, fetch_size);

        if (g_file_test(cache_path, G_FILE_TEST_EXISTS)) {
          GError *err = NULL;
          GdkPixbuf *raw = gdk_pixbuf_new_from_file_at_scale(
              cache_path, fetch_size, fetch_size, TRUE, &err);
          if (err)
            g_error_free(err);
          if (raw) {
            GdkPixbuf *trimmed = pixbuf_trim_transparent(raw);
            g_object_unref(raw);
            if (trimmed) {
              result = pixbuf_scale_cover(trimmed, target_w, target_h);
              g_object_unref(trimmed);
            }
          }
        }

        if (result)
          mem_cache_store(tile_key, result);
      }
      g_free(tile_key);
      g_free(cache_path);
    }
  }

  if (!result) {
    GdkPixbuf *generic = draw_generic_file_tile(MAX(target_w, target_h));
    if (generic) {
      result = pixbuf_scale_cover(generic, target_w, target_h);
      g_object_unref(generic);
    }
  }

  return result;
}

static gboolean has_known_non_media_extension(const char *name) {
  static const char *const skip_exts[] = {
      ".so",   ".o",   ".a",   ".la",   ".txt",  ".md",  ".log", ".csv",
      ".tsv",  ".c",   ".h",   ".hpp",  ".cpp",  ".cc",  ".rs",  ".go",
      ".py",   ".pyc", ".js",  ".ts",   ".java", ".rb",  ".pl",  ".sh",
      ".json", ".xml", ".yml", ".yaml", ".toml", ".ini", ".cfg", ".conf",
      ".html", ".htm", ".css", ".zip",  ".tar",  ".gz",  ".bz2", ".xz",
      ".deb",  ".rpm", NULL};

  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;

  const char *dot = base;
  while ((dot = strchr(dot, '.')) != NULL) {
    for (int i = 0; skip_exts[i]; i++) {
      size_t elen = strlen(skip_exts[i]);
      if (g_ascii_strncasecmp(dot, skip_exts[i], elen) == 0 &&
          (dot[elen] == '\0' || dot[elen] == '.'))
        return TRUE;
    }
    dot++;
  }
  return FALSE;
}

static GPtrArray *pick_preview_candidates(const char *dirpath) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  GDir *dir = g_dir_open(dirpath, 0, NULL);
  if (!dir)
    return out;

  const char *name;
  int scanned = 0;
  while (out->len < FOLDER_PREVIEW_MAX_ITEMS &&
         scanned < FOLDER_PREVIEW_SCAN_LIMIT &&
         (name = g_dir_read_name(dir)) != NULL) {
    scanned++;
    if (name[0] == '.')
      continue; /* hidden file/dir */

    char *child = g_build_filename(dirpath, name, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
      g_free(child);
      continue;
    }

    if (has_known_non_media_extension(name)) {
      g_free(child);
      continue;
    }

    gboolean ext_uncertain = TRUE;
    char *ext_guess = g_content_type_guess(child, NULL, 0, &ext_uncertain);
    gboolean quick_reject = FALSE;
    if (ext_guess && !ext_uncertain) {
      if (!g_str_has_prefix(ext_guess, "image/") &&
          !g_str_has_prefix(ext_guess, "video/") &&
          !g_str_has_prefix(ext_guess, "audio/"))
        quick_reject = TRUE;
    }
    g_free(ext_guess);

    if (quick_reject) {
      g_free(child);
      continue;
    }

    FileCat cat = guess_category(child);
    if (cat != CAT_IMAGE && cat != CAT_VIDEO && cat != CAT_AUDIO) {
      g_free(child);
      continue;
    }

    g_ptr_array_add(out, child);
  }
  g_dir_close(dir);
  return out;
}

static void rounded_rect_path(cairo_t *cr, double x, double y, double w,
                              double h, double r) {
  if (r > w / 2.0)
    r = w / 2.0;
  if (r > h / 2.0)
    r = h / 2.0;
  if (r < 0)
    r = 0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -G_PI_2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI_2);
  cairo_arc(cr, x + r, y + h - r, r, G_PI_2, G_PI);
  cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI_2);
  cairo_close_path(cr);
}

static GdkPixbuf *apply_video_frame(GdkPixbuf *src) {
  if (!src)
    return NULL;

  int w = gdk_pixbuf_get_width(src);
  int h = gdk_pixbuf_get_height(src);
  if (w < 8 || h < 8)
    return NULL;

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(surface);

  gdk_cairo_set_source_pixbuf(cr, src, 0, 0);
  cairo_paint(cr);

  double radius = MIN(w, h) * 0.05;

  /* Bahar wali halki white line. */
  cairo_save(cr);
  rounded_rect_path(cr, 0.75, 0.75, w - 1.5, h - 1.5, radius);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
  cairo_set_line_width(cr, 1.5);
  cairo_stroke(cr);
  cairo_restore(cr);

  /* Thodi andar ek halki dark line -- isi se frame ko thoda depth
   * milta hai, bilkul Nautilus/Thunar ke thumbnail frame jaisa. */
  cairo_save(cr);
  rounded_rect_path(cr, 2.25, 2.25, w - 4.5, h - 4.5, radius * 0.75);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.18);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);
  cairo_restore(cr);

  GdkPixbuf *result = gdk_pixbuf_get_from_surface(surface, 0, 0, w, h);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  return result;
}

static GdkPixbuf *compose_folder_preview(GdkPixbuf *base_icon,
                                         GPtrArray *children, int size) {
  if (!base_icon || children->len == 0)
    return NULL;

  guint n = MIN(children->len, (guint)FOLDER_PREVIEW_MAX_ITEMS);

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(surface);

  /* Folder ka plain themed icon sab se peeche base ki tarah. */
  gdk_cairo_set_source_pixbuf(cr, base_icon, 0, 0);
  cairo_paint(cr);

  /* Icon ke center-bottom hisse mein ek chowkor grid area. */
  double area = size * 0.92;
  double area_x = (size - area) / 2.0;
  double area_y = size - area - size * -0.25;
  if (area_x < 0)
    area_x = 0;
  if (area_y < 0)
    area_y = 0;

  double gap = MAX(1.0, size * 0.05);
  double radius = size * 0.02;
  double shadow_offset = MAX(1.0, size * 0.015);

  static const double tile_angles_deg[4] = {-2.5, 2.0, -1.5, 3.0};

  guint placed = 0;
  for (guint i = 0; i < n; i++) {
    double cell_w;
    double cell_h;

    if (n == 1) {
      cell_w = area;
      cell_h = area * 9.0 / 16.0;
    } else {
      cell_w = area / 2.0;
      cell_h = cell_w * 9.0 / 16.0;
    }
    int col = (n == 1) ? 0 : (int)(placed % 2);
    int row = (n <= 2) ? 0 : (int)(placed / 2);

    double gx = area_x + col * cell_w + gap / 2.0;
    double gy = area_y + row * cell_h + gap / 2.0;
    int tile_w = (int)(cell_w - gap + 0.5);
    int tile_h = (int)(cell_h - gap + 0.5);
    if (tile_w < 4 || tile_h < 4)
      continue;

    GdkPixbuf *thumb =
        load_child_preview((const char *)children->pdata[i], tile_w, tile_h);
    if (!thumb)
      continue;

    cairo_save(cr);
    double angle = tile_angles_deg[placed % 4] * G_PI / 180.0;
    double ccx = gx + tile_w / 2.0;
    double ccy = gy + tile_h / 2.0;
    cairo_translate(cr, ccx, ccy);
    cairo_rotate(cr, angle);
    cairo_translate(cr, -ccx, -ccy);

    cairo_save(cr);
    rounded_rect_path(cr, gx + shadow_offset, gy + shadow_offset * 1.5, tile_w,
                      tile_h, radius);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.30);
    cairo_fill(cr);
    cairo_restore(cr);

    cairo_save(cr);
    rounded_rect_path(cr, gx, gy, tile_w, tile_h, radius);
    cairo_clip(cr);
    gdk_cairo_set_source_pixbuf(cr, thumb, gx, gy);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_save(cr);
    rounded_rect_path(cr, gx + 0.5, gy + 0.5, tile_w - 1, tile_h - 1, radius);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.75);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_restore(cr);

    cairo_restore(cr); /* tilt transform pop */

    g_object_unref(thumb);
    placed++;
  }

  GdkPixbuf *canvas = NULL;
  if (placed > 0)
    canvas = gdk_pixbuf_get_from_surface(surface, 0, 0, size, size);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  return canvas;
}

static void folder_preview_worker_func(gpointer data, gpointer user_data) {
  (void)user_data;
  FolderPreviewJob *job = data;

  GPtrArray *candidates = pick_preview_candidates(job->dirpath);
  GdkPixbuf *result =
      compose_folder_preview(job->base_icon, candidates, job->size);
  g_ptr_array_free(candidates, TRUE);

  ThumbResult *res = g_new0(ThumbResult, 1);
  res->filepath = job->dirpath; /* transfer ownership, same as worker_func() */
  res->pixbuf = result;
  res->callback = job->callback;
  res->user_data = job->user_data;

  g_idle_add(deliver_result_idle, res);

  g_object_unref(job->base_icon);
  g_free(job);
}

/* ---- public API ---------------------------------------------------------*/

void thumbnail_init(void) {
  if (pool)
    return;
  GError *err = NULL;
  /* Thumbnail generation is mostly waiting on ffmpeg/pdftoppm child
   * processes, not CPU-bound in this process, so it's fine to run more of
   * them in parallel than we have cores -- a fixed pool of 3 was the main
   * reason a folder full of un-cached videos/images crawled in one by one. */
  int n = (int)g_get_num_processors();
  int workers = CLAMP(n * 2, 6, 12);
  pool = g_thread_pool_new(worker_func, NULL, workers, FALSE, &err);
  if (err) {
    g_warning("thumbnail_init: %s", err->message);
    g_error_free(err);
  }
}

void thumbnail_shutdown(void) {
  if (pool) {
    g_thread_pool_free(pool, TRUE, FALSE);
    pool = NULL;
  }
  if (preview_pool) {
    g_thread_pool_free(preview_pool, TRUE, FALSE);
    preview_pool = NULL;
  }
}

void thumbnail_set_video_frame_enabled(gboolean enabled) {
  g_video_frame_enabled = enabled;
}

void thumbnail_get_async(const char *filepath, int size,
                         ThumbnailReadyFunc callback, gpointer user_data) {
  if (!pool)
    thumbnail_init();

  ThumbJob *job = g_new0(ThumbJob, 1);
  job->filepath = g_strdup(filepath);
  job->size = size;
  job->callback = callback;
  job->user_data = user_data;

  GError *err = NULL;
  if (!g_thread_pool_push(pool, job, &err)) {
    if (err)
      g_error_free(err);
    g_free(job->filepath);
    g_free(job);
  }
}

void folder_preview_get_async(const char *dirpath, int size,
                              GdkPixbuf *base_icon, ThumbnailReadyFunc callback,
                              gpointer user_data) {
  if (!base_icon)
    return;

  if (!preview_pool) {
    GError *perr = NULL;
    preview_pool =
        g_thread_pool_new(folder_preview_worker_func, NULL, 3, FALSE, &perr);
    if (perr) {
      g_warning("folder_preview_get_async: %s", perr->message);
      g_error_free(perr);
    }
  }
  if (!preview_pool)
    return;

  FolderPreviewJob *job = g_new0(FolderPreviewJob, 1);
  job->dirpath = g_strdup(dirpath);
  job->size = size;
  job->base_icon = g_object_ref(base_icon);
  job->callback = callback;
  job->user_data = user_data;

  GError *err = NULL;
  if (!g_thread_pool_push(preview_pool, job, &err)) {
    if (err)
      g_error_free(err);
    g_object_unref(job->base_icon);
    g_free(job->dirpath);
    g_free(job);
  }
}
