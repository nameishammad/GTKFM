#ifndef ICONCHOOSER_H
#define ICONCHOOSER_H

/* Ek reusable, grid-based icon-picker dialog (jaisa Nautilus/Nemo/Thunar
 * ka "select icon" dialog) -- Properties (folder icon) aur Edit Launcher
 * (.desktop icon) dono isi ek module ko use karte hain, taake behaviour
 * aur look consistent rahe aur code duplicate na ho.
 *
 * Speed & crash-safety: grid sirf ek chhoti, fixed candidate_names list
 * ke against banti hai (poore icon theme ko enumerate nahi karte) --
 * isliye yeh hamesha fast khulta hai aur missing/corrupt icons par kabhi
 * crash nahi karta (har icon load GError ke saath fail-safe hai, missing
 * icon sirf grid se skip ho jata hai). */

#include <gtk/gtk.h>

typedef enum {
  ICON_CHOOSER_CANCEL = 0, /* User ne Cancel kiya ya dialog band kar diya */
  ICON_CHOOSER_NAME,       /* Grid se koi themed icon chuna -- *out_value =
                              icon-theme naam */
  ICON_CHOOSER_IMAGE,      /* "Image..." se custom file chuni -- *out_value =
                              absolute image path */
  ICON_CHOOSER_RESET       /* "Reset to Default" click kiya */
} IconChooserResult;

/* Modal icon-picker dialog dikhata hai.
 *
 * parent            - dialog ka parent window
 * title             - dialog ka title
 * candidate_names   - grid mein dikhane wale icon-theme names ki list
 *                      (sirf wahi jo installed theme mein maujood hon wo
 *                      dikhte hain, baaki chupke se skip ho jate hain)
 * n_candidates      - candidate_names mein entries ki tadaad
 * current_icon      - abhi wala icon (naam ya path); grid mein match milte
 *                      hi wahi item pehle se select/scrolled-to hota hai
 *                      (NULL ho to koi preselection nahi)
 * show_reset        - TRUE ho to "Reset to Default" button bhi dikhega
 * out_value         - (nullable) result NAME/IMAGE hone par yahan
 *                      newly-allocated string set hoti hai (caller
 *                      g_free() kare). CANCEL/RESET par NULL set hoti hai.
 *
 * Return value bataata hai user ne kya action liya. */
IconChooserResult icon_chooser_run(GtkWindow *parent, const char *title,
                                    const char **candidate_names,
                                    guint n_candidates,
                                    const char *current_icon,
                                    gboolean show_reset, char **out_value);

/* Icon-theme ka raw naam ("folder-pictures", "applications-graphics") ek
 * parhne laayak label ("Pictures", "Graphics") mein badalta hai. */
char *icon_chooser_label_for(const char *name);

/* Folder icon-theme names ki fixed list (Properties -> folder "Change
 * Icon..." ke liye). *n_out mein entries ki tadaad milti hai. */
const char **icon_chooser_folder_icon_names(guint *n_out);

/* Application/launcher icon-theme names ki fixed list (.desktop launcher
 * ke "Change Icon..." / "Choose Icon..." ke liye). *n_out mein entries ki
 * tadaad milti hai. */
const char **icon_chooser_app_icon_names(guint *n_out);

#endif /* ICONCHOOSER_H */
