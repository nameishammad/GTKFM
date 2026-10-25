#include "undo.h"
#include "fileview_internal.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>


void undo_push(AppWidgets *app, UndoAction *action)
{
    if (!app || !action)
        return;

    app->undo_stack =
        g_list_prepend(app->undo_stack, action);
}


static void undo_free_action(UndoAction *a)
{
    if (!a)
        return;

    if (a->type == UNDO_BATCH) {
        for (GList *l = a->batch; l; l = l->next)
            undo_free_action((UndoAction *)l->data);
        g_list_free(a->batch);
    }

    g_free(a->src);
    g_free(a->dest);
    g_free(a->old_name);
    g_free(a->new_name);

    g_free(a);
}


/* Simple src/dest wapas move -- RENAME aur MOVE undo dono is se guzarte
 * hain. Agar `src` (jahan cheez wapas jani hai) par pehle se koi doosri
 * file/folder maujood ho (mislan undo se pehle wahan naye naam ki koi
 * cheez bana di gayi ho) to hum chup-chap overwrite nahi karte -- undo
 * cancel karke error bata dete hain, taake koi data accidentally na kho
 * jaye. */
static gboolean undo_move_back(UndoAction *a, char **out_err)
{
    if (!a->src || !a->dest)
        return TRUE; /* kuch karne ko nahi -- fail nahi maante */

    if (g_file_test(a->src, G_FILE_TEST_EXISTS)) {
        *out_err = g_strdup_printf(
            "'%s' par pehle se kuch maujood hai", a->src);
        return FALSE;
    }

    GFile *src = g_file_new_for_path(a->dest);
    GFile *dst = g_file_new_for_path(a->src);
    GError *err = NULL;
    gboolean ok = g_file_move(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &err);
    if (!ok)
        *out_err = g_strdup(err ? err->message : "unknown");
    if (err)
        g_error_free(err);
    g_object_unref(src);
    g_object_unref(dst);
    return ok;
}


/* Ek (non-batch) UndoAction ko reverse karta hai. AppWidgets *app sirf
 * UNDO_DELETE (do_restore_one -- overwrite popup ke liye parent window)
 * ke liye chahiye hota hai. Success/fail wapas karta hai, error text
 * *out_err mein (caller ko g_free karna hai). */
static gboolean undo_apply_one(AppWidgets *app, UndoAction *a, char **out_err)
{
    switch (a->type) {

    case UNDO_DELETE:
        /* Trash mein gayi cheez wapas uski asal jagah par -- do_restore_one
         * hi sab kuch (trashinfo cleanup, aur agar asal jagah par ab kuch
         * aur maujood ho to overwrite confirm popup) sambhal leta hai. */
        if (!a->dest)
            return TRUE;
        return do_restore_one(a->dest, out_err);

    case UNDO_RENAME:
    case UNDO_MOVE:
        return undo_move_back(a, out_err);

    case UNDO_COPY: {
        /* Copy undo = jo naya file/folder bana tha use hata do (original
         * source ko haath nahi lagate). Folder ho to recursively delete
         * karna padta hai (g_remove sirf khaali folder par chalta hai). */
        if (!a->dest)
            return TRUE;
        GFile *f = g_file_new_for_path(a->dest);
        GFileType type =
            g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL);
        GError *err = NULL;
        gboolean ok = (type == G_FILE_TYPE_DIRECTORY)
                          ? delete_recursive(f, &err)
                          : g_file_delete(f, NULL, &err);
        if (!ok)
            *out_err = g_strdup(err ? err->message : "unknown");
        if (err)
            g_error_free(err);
        g_object_unref(f);
        return ok;
    }

    case UNDO_BATCH:
        /* Nested batch nahi hoti -- yahan tak pahunchna nahi chahiye. */
        return TRUE;
    }

    return TRUE;
}


void undo_last(AppWidgets *app)
{
    if (!app || !app->undo_stack)
        return;


    GList *node = app->undo_stack;

    UndoAction *a = node->data;


    if (a->type == UNDO_BATCH) {

        /* Batch ke items ko ULTA order (jo sab se aakhir mein bana tha use
         * pehle undo karo) mein reverse karte hain, aur ek hi status
         * message mein result batate hain -- taake user ko ek Ctrl+Z se
         * pura operation undo hota mehsoos ho, file-by-file nahi. */
        guint ok = 0, fail = 0;
        char *first_err = NULL;

        for (GList *l = g_list_last(a->batch); l; l = l->prev) {
            char *err_msg = NULL;
            if (undo_apply_one(app, (UndoAction *)l->data, &err_msg))
                ok++;
            else {
                fail++;
                if (!first_err)
                    first_err = g_strdup(err_msg);
            }
            g_free(err_msg);
        }

        if (fail == 0) {
            char *msg = g_strdup_printf("%u item undo ho gaye", ok);
            set_status(app, msg);
            g_free(msg);
        } else {
            char *msg = g_strdup_printf(
                "%u item undo ho gaye, %u fail%s%s", ok, fail,
                first_err ? ": " : "", first_err ? first_err : "");
            set_status(app, msg);
            g_free(msg);
        }
        g_free(first_err);

    } else {

        // char *err_msg = NULL;
        // gboolean ok = undo_apply_one(app, a, &err_msg);
        // if (ok)
        //     set_status(app, "Undo ho gaya");
        // else {
        //     char *msg = g_strdup_printf("Undo fail: %s",
        //                                 err_msg ? err_msg : "unknown");
        //     set_status(app, msg);
        //     g_free(msg);
        // }
        // g_free(err_msg);
        char *err_msg = NULL;
        gboolean ok = undo_apply_one(app, a, &err_msg);

        if (ok) {
            /* Refresh ke baad isi item ko dobara select karo */
            if (a->type == UNDO_RENAME ||
                a->type == UNDO_MOVE ||
                a->type == UNDO_DELETE) {

                g_free(app->pending_select_path);
            app->pending_select_path = g_strdup(a->src);
                }

                set_status(app, "Undo ho gaya");
        } else {
            char *msg = g_strdup_printf("Undo fail: %s",
                                        err_msg ? err_msg : "unknown");
            set_status(app, msg);
            g_free(msg);
        }

        g_free(err_msg);
    }


    app->undo_stack =
        g_list_delete_link(app->undo_stack,node);


    undo_free_action(a);


    fileview_refresh(app);
}
