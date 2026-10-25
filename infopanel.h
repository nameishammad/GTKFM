#ifndef INFOPANEL_H
#define INFOPANEL_H

#include "app.h"

/* Dolphin jaisa right-side "Information" panel banata hai jo current
 * selection (ya, agar kuch select nahi hai, current folder) ki
 * name/type/size/modified/location dikhata hai, saath hi ek badi preview
 * icon/thumbnail bhi. Shuru mein hidden hota hai -- F11 se dikhta/chupta
 * hai (main.c dekhein). Caller ko is widget ko ek GtkPaned ke 2nd child
 * ki tarah pack karna chahiye. */
GtkWidget *infopanel_create(AppWidgets *app);

/* F11 shortcut: panel show/hide karta hai (aur pehli baar show karte
 * waqt content ko turant refresh kar deta hai). */
void infopanel_toggle(AppWidgets *app);

/* Selection ya current folder badalne par panel ka content update karta
 * hai. fileview.c is ko selection-changed, navigate aur refresh ke baad
 * call karta hai. Panel hidden ho to ye no-op hai (perf ke liye). */
void infopanel_refresh(AppWidgets *app);

#endif /* INFOPANEL_H */
