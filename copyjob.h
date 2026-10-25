#ifndef COPYJOB_H
#define COPYJOB_H

#include "app.h"

void copyjob_start(AppWidgets *app, GList *src_paths, const char *dest_dir, gboolean is_move);

#endif /* COPYJOB_H */
