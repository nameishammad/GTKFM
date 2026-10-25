#ifndef COMPRESSJOB_H
#define COMPRESSJOB_H

#include "app.h"

/* "Compress..." context-menu action ke liye Thunar/Ark jaisa dialog kholta
 * hai: naam field, ek format-selector (ZIP, TAR.GZ, TAR.BZ2, TAR.XZ,
 * 7-Zip -- sirf wo formats dikhte hain jinka program system mein mojood
 * ho) aur ek optional password field (sirf ZIP aur 7-Zip encryption
 * support karte hain, isliye tar.* format select hone par password field
 * disable ho jata hai). "Create" dabane par background process (zip -r /
 * tar -cv*f / 7z a) chala kar ek progress dialog (Cancel button ke saath)
 * dikhata hai -- isi tarah jaise copyjob/deletejob bade operations ke
 * liye UI freeze nahi hone dete.
 *
 * Note: password command-line argument ke tor par pass hota hai, is liye
 * process chalne ke doran isi machine ke doosre users ise 'ps' se dekh
 * sakte hain (zip/7z ki apni limitation hai) -- .zip ka encryption
 * (ZipCrypto) bhi cryptographically kamzor hai; zyada mehfooz ke liye
 * .7z (AES-256 + encrypted headers) behtar hai.
 *
 * Takes ownership of `paths` (frees it once the dialog closes, chahe
 * user Cancel kare ya Create). */
void compressjob_show_dialog(AppWidgets *app, GList *paths);

#endif /* COMPRESSJOB_H */
