#ifndef UNDO_H
#define UNDO_H

#include "app.h"

/* Ek successful operation (delete/rename/move/copy) ke baad undo stack mein
 * push karo. `action` ka ownership undo.c le leta hai (undo hone ya app
 * band hone par g_free ho jayega). */
void undo_push(AppWidgets *app, UndoAction *action);

/* Undo stack ka sab se upar wala (sab se recent) action wapas (reverse)
 * kar deta hai. Stack khali ho to kuch nahi hota. */
void undo_last(AppWidgets *app);

#endif /* UNDO_H */
