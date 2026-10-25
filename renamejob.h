#ifndef RENAMEJOB_H
#define RENAMEJOB_H

#include "app.h"

/* Thunar jaisa "Rename Multiple Files" dialog kholta hai: jab user ek se
 * zyada files/folders select karke Rename kare (ya F2 dabaye), to har
 * ek ke liye alag-alag chhota dialog kholne ki bajaye, ek hi dialog mein
 * sab ke naye naam ek live preview list mein dikhaye jaate hain.
 *
 * Do modes milte hain (Thunar ki tarah):
 *   - "Find and Replace": text (ya regular expression) dhoond kar
 *     replace karta hai.
 *   - "Numbering": ek naya base naam + running counter (jaise "Photo 01",
 *     "Photo 02", ...), extension chahe to as-is rakhi ja sakti hai.
 *
 * Naye naam list mein hi edit ki taraf ishara (conflict highlight) ho
 * jata hai agar do items ka naya naam takra jaye ya pehle se koi file
 * usi naam se mojood ho -- aisi soorat mein "Rename" button disable ho
 * jata hai jab tak conflict theek na ho.
 *
 * Takes ownership of `paths` (frees it when dialog closes). `paths` ki
 * order hi list mein numbering/preview order hoti hai. */
void renamejob_show_dialog(AppWidgets *app, GList *paths);

#endif /* RENAMEJOB_H */
