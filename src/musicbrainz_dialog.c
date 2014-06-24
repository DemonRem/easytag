/* musicbrainz_dialog.c - 2014/05/05 */
/*
 *  EasyTAG - Tag editor for MP3 and Ogg Vorbis files
 *  Copyright (C) 2000-2014  Abhinav Jangda <abhijangda@hotmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
 
#include "config.h"

#ifdef ENABLE_MUSICBRAINZ

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <discid/discid.h>

#include "easytag.h"
#include "log.h"
#include "musicbrainz_dialog.h"
#include "mbentityview.h"
#include "mb_search.h"
#include "browser.h"
#include "application_window.h"

/***************
 * Declaration *
 ***************/
//static GSimpleAsyncResult *async_result;

typedef struct
{
    GNode *mb_tree_root;
    GSimpleAsyncResult *async_result;
} MusicBrainzDialogPrivate;

static MusicBrainzDialogPrivate *mb_dialog_priv;

typedef struct
{
    GHashTable *hash_table;
} SelectedFindThreadData;
/*
 * ManualSearchThreadData:
 * @text_to_search: Manual Search Text
 * @type: Type of Entity
 *
 * Thread Data for storing Manual Search's data.
 */
typedef struct
{
    gchar *text_to_search;
    int type;
} ManualSearchThreadData;

/*************
 * Functions *
 *************/

/*
 * manual_search_callback:
 * @source: Source Object
 * @res: GAsyncResult
 * @user_data: User data
 *
 * Callback function for GAsyncResult for Manual Search.
 */
static void
manual_search_callback (GObject *source, GAsyncResult *res,
                        gpointer user_data)
{
    GtkComboBoxText *combo_box;

    if (!g_simple_async_result_get_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res)))
    {
        g_object_unref (res);
        g_free (user_data);
        free_mb_tree (mb_dialog_priv->mb_tree_root);
        mb_dialog_priv->mb_tree_root = g_node_new (NULL);
        return;
    }

    et_mb_entity_view_set_tree_root (ET_MB_ENTITY_VIEW (entityView),
                                     mb_dialog_priv->mb_tree_root);
    gtk_statusbar_push (GTK_STATUSBAR (gtk_builder_get_object (builder,
                        "statusbar")), 0, _("Searching Completed"));
    g_object_unref (res);
    g_free (user_data);

    combo_box = GTK_COMBO_BOX_TEXT (gtk_builder_get_object (builder,
                                                            "cbManualSearch"));
    gtk_combo_box_text_append_text (combo_box,
                                    gtk_combo_box_text_get_active_text (combo_box));
    et_music_brainz_dialog_stop_set_sensitive (FALSE);
}

/*
 * et_show_status_msg_in_idle_cb:
 * @obj: Source Object
 * @res: GAsyncResult
 * @user_data: User data
 *
 * Callback function for Displaying StatusBar Message.
 */
static void
et_show_status_msg_in_idle_cb (GObject *obj, GAsyncResult *res,
                               gpointer user_data)
{
    gtk_statusbar_push (GTK_STATUSBAR (gtk_builder_get_object (builder,
                        "statusbar")), 0, user_data);
    g_free (user_data);
    g_object_unref (res);
}

/*
 * et_show_status_msg_in_idle:
 * @obj: Source Object
 * @res: GAsyncResult
 * @user_data: User data
 *
 * Function to Display StatusBar Messages in Main Thread.
 */
void
et_show_status_msg_in_idle (gchar *message)
{
    GSimpleAsyncResult *async_res;

    async_res = g_simple_async_result_new (NULL,
                                           et_show_status_msg_in_idle_cb,
                                           g_strdup (message),
                                           et_show_status_msg_in_idle);
    g_simple_async_result_complete_in_idle (async_res);
}

/*
 * mb5_search_error_callback:
 * @obj: Source Object
 * @res: GAsyncResult
 * @user_data: User data
 *
 * Callback function for displaying errors.
 */
void
mb5_search_error_callback (GObject *source, GAsyncResult *res,
                           gpointer user_data)
{
    GError *dest;
    dest = NULL;
    g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (res),
                                           &dest);
    Log_Print (LOG_ERROR,
               _("Error searching MusicBrainz Database '%s'"), dest->message);
    gtk_statusbar_push (GTK_STATUSBAR (gtk_builder_get_object (builder,
                                       "statusbar")), 0, dest->message);
    g_error_free (dest);
    et_music_brainz_dialog_stop_set_sensitive (FALSE);
}

/*
 * manual_search_thread_func:
 * @res: GSimpleAsyncResult
 * @obj: Source GObject
 * @cancellable: GCancellable to cancel the operation
 *
 * Thread func of GSimpleAsyncResult to do Manual Search in another thread.
 */
static void
manual_search_thread_func (GSimpleAsyncResult *res, GObject *obj,
                           GCancellable *cancellable)
{
    GError *error;
    ManualSearchThreadData *thread_data;
    gchar *status_msg;

    error = NULL;
    g_simple_async_result_set_op_res_gboolean (res, FALSE);

    if (g_cancellable_is_cancelled (cancellable))
    {
        g_set_error (&error, ET_MB5_SEARCH_ERROR,
                     ET_MB5_SEARCH_ERROR_CANCELLED,
                     _("Operation cancelled by user"));
        g_simple_async_report_gerror_in_idle (NULL,
                                              mb5_search_error_callback,
                                              NULL, error);
        return;
    }

    thread_data = (ManualSearchThreadData *)g_async_result_get_user_data (G_ASYNC_RESULT (res));
    status_msg = g_strdup_printf (_("Searching %s"), thread_data->text_to_search);
    et_show_status_msg_in_idle (status_msg);
    g_free (status_msg);

    if (g_cancellable_is_cancelled (cancellable))
    {
        g_set_error (&error, ET_MB5_SEARCH_ERROR,
                     ET_MB5_SEARCH_ERROR_CANCELLED,
                     _("Operation cancelled by user"));
        g_simple_async_report_gerror_in_idle (NULL,
                                              mb5_search_error_callback,
                                              thread_data, error);
        return;
    }

    if (!et_musicbrainz_search (thread_data->text_to_search,
                                thread_data->type, mb_dialog_priv->mb_tree_root, &error,
                                cancellable))
    {
        g_simple_async_report_gerror_in_idle (NULL,
                                              mb5_search_error_callback,
                                              thread_data, error);
        return;
    }

    if (g_cancellable_is_cancelled (cancellable))
    {
        g_set_error (&error, ET_MB5_SEARCH_ERROR,
                     ET_MB5_SEARCH_ERROR_CANCELLED,
                     _("Operation cancelled by user"));
        g_simple_async_report_gerror_in_idle (NULL,
                                              mb5_search_error_callback,
                                              thread_data, error);
        return;
    }

    g_simple_async_result_set_op_res_gboolean (res, TRUE);
}

/*
 * btn_manual_find_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of btnManualFind.
 */
static void
btn_manual_find_clicked (GtkWidget *btn, gpointer user_data)
{
    GtkWidget *cb_manual_search;
    GtkWidget *cb_manual_search_in;
    int type;
    ManualSearchThreadData *thread_data;

    cb_manual_search_in = GTK_WIDGET (gtk_builder_get_object (builder,
                                                              "cbManualSearchIn"));
    type = gtk_combo_box_get_active (GTK_COMBO_BOX (cb_manual_search_in));

    if (type == -1)
    {
        return;
    }

    if (g_node_first_child (mb_dialog_priv->mb_tree_root))
    {
        free_mb_tree (mb_dialog_priv->mb_tree_root);
        mb_dialog_priv->mb_tree_root = g_node_new (NULL);
    }

    et_mb_entity_view_clear_all (ET_MB_ENTITY_VIEW (entityView));
    cb_manual_search = GTK_WIDGET (gtk_builder_get_object (builder,
                                                           "cbManualSearch"));
    thread_data = g_malloc (sizeof (ManualSearchThreadData));
    thread_data->type = type;
    thread_data->text_to_search = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (cb_manual_search));
    mb5_search_cancellable = g_cancellable_new ();
    gtk_statusbar_push (GTK_STATUSBAR (gtk_builder_get_object (builder,
                        "statusbar")), 0, _("Starting MusicBrainz Search"));
    mb_dialog_priv->async_result = g_simple_async_result_new (NULL, manual_search_callback,
                                              thread_data,
                                              btn_manual_find_clicked);
    g_simple_async_result_run_in_thread (mb_dialog_priv->async_result,
                                         manual_search_thread_func, 0,
                                         mb5_search_cancellable);
    et_music_brainz_dialog_stop_set_sensitive (TRUE);
}

/*
 * btn_manual_stop_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of toolbtnToggleRedLines.
 */
static void
tool_btn_toggle_red_lines_clicked (GtkWidget *btn, gpointer user_data)
{
    et_mb_entity_view_toggle_red_lines (ET_MB_ENTITY_VIEW (entityView));
}

/*
 * btn_manual_stop_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of toolbtnUp.
 */
static void
tool_btn_up_clicked (GtkWidget *btn, gpointer user_data)
{
    et_mb_entity_view_select_up (ET_MB_ENTITY_VIEW (entityView));
}

/*
 * btn_manual_stop_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of toolbtnDown.
 */
static void
tool_btn_down_clicked (GtkWidget *btn, gpointer user_data)
{
    et_mb_entity_view_select_down (ET_MB_ENTITY_VIEW (entityView));
}

/*
 * btn_manual_stop_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of toolbtnInvertSelection.
 */
static void
tool_btn_invert_selection_clicked (GtkWidget *btn, gpointer user_data)
{
    et_mb_entity_view_invert_selection (ET_MB_ENTITY_VIEW (entityView));
}

/*
 * btn_manual_stop_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of toolbtnSelectAll.
 */
static void
tool_btn_select_all_clicked (GtkWidget *btn, gpointer user_data)
{
    et_mb_entity_view_select_all (ET_MB_ENTITY_VIEW (entityView));
}

/*
 * btn_manual_stop_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of toolbtnUnselectAll.
 */
static void
tool_btn_unselect_all_clicked (GtkWidget *btn, gpointer user_data)
{
    et_mb_entity_view_unselect_all (ET_MB_ENTITY_VIEW (entityView));
}

/*
 * btn_manual_stop_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of btnManualStop.
 */
static void
tool_btn_refresh_clicked (GtkWidget *btn, gpointer user_data)
{
    /* TODO: Implement Refresh Operation */
    if (et_mb_entity_view_get_current_level (ET_MB_ENTITY_VIEW (entityView)) >
        1)
    {
        /* Current level is more than 1, refereshing means downloading an */
        /* entity's children */
    }
}

/*
 * btn_manual_stop_clicked:
 * @btn: GtkButton
 * @user_data: User data
 *
 * Signal Handler for "clicked" signal of btnManualStop.
 */
static void
btn_manual_stop_clicked (GtkWidget *btn, gpointer user_data)
{
    if (G_IS_CANCELLABLE (mb5_search_cancellable))
    {
        g_cancellable_cancel (mb5_search_cancellable);
    }
}

/*
 * entry_tree_view_search_changed:
 * @editable: GtkEditable for which handler is called
 * @user_data: User data
 *
 * Signal Handler for "changed" signal of entryTreeViewSearch.
 */
static void
entry_tree_view_search_changed (GtkEditable *editable, gpointer user_data)
{
    et_mb_entity_view_search_in_results (ET_MB_ENTITY_VIEW (entityView),
                                         gtk_entry_get_text (GTK_ENTRY (gtk_builder_get_object (builder,
                                                                        "entryTreeViewSearch"))));
}

static void
selected_find_callback (GObject *source, GAsyncResult *res,
                        gpointer user_data)
{
    if (!g_simple_async_result_get_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res)))
    {
        g_object_unref (res);
        g_object_unref (((SelectedFindThreadData *)user_data)->hash_table);
        g_free (user_data);
        free_mb_tree (mb_dialog_priv->mb_tree_root);
        mb_dialog_priv->mb_tree_root = g_node_new (NULL);
        return;
    }

    et_mb_entity_view_set_tree_root (ET_MB_ENTITY_VIEW (entityView),
                                     mb_dialog_priv->mb_tree_root);
    gtk_statusbar_push (GTK_STATUSBAR (gtk_builder_get_object (builder, "statusbar")),
                        0, _("Searching Completed"));
    g_object_unref (res);
    g_object_unref (((SelectedFindThreadData *)user_data)->hash_table);
    g_free (user_data);
    et_music_brainz_dialog_stop_set_sensitive (FALSE);
}

static void
selected_find_thread_func (GSimpleAsyncResult *res, GObject *obj,
                           GCancellable *cancellable)
{
    GList *list_keys;
    GList *iter;
    SelectedFindThreadData *thread_data;
    GError *error;

    g_simple_async_result_set_op_res_gboolean (res, FALSE);
    error = NULL;
    thread_data = (SelectedFindThreadData *)g_async_result_get_user_data (G_ASYNC_RESULT (res));
    list_keys = g_hash_table_get_keys (thread_data->hash_table);
    iter = g_list_first (list_keys);

    while (iter)
    {
        if (!et_musicbrainz_search ((gchar *)iter->data, MB_ENTITY_KIND_ALBUM,
                                    mb_dialog_priv->mb_tree_root, &error, cancellable))
        {
            g_simple_async_report_gerror_in_idle (NULL,
                                                  mb5_search_error_callback,
                                                  thread_data, error);
            g_list_free (list_keys);
            return;
        }

        if (g_cancellable_is_cancelled (cancellable))
        {
            g_set_error (&error, ET_MB5_SEARCH_ERROR,
                         ET_MB5_SEARCH_ERROR_CANCELLED,
                         _("Operation cancelled by user"));
            g_simple_async_report_gerror_in_idle (NULL,
                                                  mb5_search_error_callback,
                                                  thread_data, error);
            g_list_free (list_keys);
            return;
        }

        iter = g_list_next (iter);
    }

    g_list_free (list_keys);
    g_simple_async_result_set_op_res_gboolean (res, TRUE);
}

static void
bt_selected_find_clicked (GtkWidget *widget, gpointer user_data)
{
    GtkListStore *tree_model;
    GtkTreeSelection *selection;
    int count;
    GList *iter_list;
    GList *l;
    GHashTable *hash_table;
    SelectedFindThreadData *thread_data;
    GtkTreeView *browser_list;

    selection = et_application_window_browser_get_selection (ET_APPLICATION_WINDOW (MainWindow));
    browser_list = gtk_tree_selection_get_tree_view (selection);
    iter_list = NULL;
    tree_model = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(browser_list)));
    count = gtk_tree_selection_count_selected_rows(selection);

    if (count > 0)
    {
        GList* list_sel_rows;

        list_sel_rows = gtk_tree_selection_get_selected_rows(selection, NULL);

        for (l = list_sel_rows; l != NULL; l = g_list_next (l))
        {
            GtkTreeIter iter;

            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(tree_model),
                                        &iter,
                                        (GtkTreePath *) l->data))
            {
                iter_list = g_list_prepend (iter_list,
                                            gtk_tree_iter_copy (&iter));
            }
        }

        g_list_free_full (list_sel_rows,
                          (GDestroyNotify)gtk_tree_path_free);

    }
    else /* No rows selected, use the whole list */
    {
        GtkTreeIter current_iter;

        if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(tree_model),
                                           &current_iter))
        {
            /* No row is present, return */
            return;
        }

        do
        {
            iter_list = g_list_prepend (iter_list,
                                        gtk_tree_iter_copy (&current_iter));
        }
        while (gtk_tree_model_iter_next(GTK_TREE_MODEL(tree_model),
               &current_iter));

        count = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(tree_model),
                                               NULL);
    }

    hash_table = g_hash_table_new (g_str_hash,
                                   g_str_equal);

    for (l = iter_list; l != NULL; l = g_list_next (l))
    {
        ET_File *etfile;
        File_Tag *file_tag;

        etfile = et_application_window_browser_get_et_file_from_iter (ET_APPLICATION_WINDOW (MainWindow),
                                                                      (GtkTreeIter *)l->data);
        file_tag = (File_Tag *)etfile->FileTag->data;

        if (file_tag->album != NULL)
        {
            g_hash_table_add (hash_table, file_tag->album);
        }
    }

    g_list_free_full (iter_list, (GDestroyNotify)gtk_tree_iter_free);
    thread_data = g_malloc (sizeof (SelectedFindThreadData));
    thread_data->hash_table = hash_table;
    mb5_search_cancellable = g_cancellable_new ();
    mb_dialog_priv->async_result = g_simple_async_result_new (NULL,
                                                              selected_find_callback,
                                                              thread_data,
                                                              bt_selected_find_clicked);
    g_simple_async_result_run_in_thread (mb_dialog_priv->async_result,
                                         selected_find_thread_func, 0,
                                         mb5_search_cancellable);
    gtk_statusbar_push (GTK_STATUSBAR (gtk_builder_get_object (builder, "statusbar")),
                        0, _("Starting Selected Files Search"));
    et_music_brainz_dialog_stop_set_sensitive (TRUE);
}

static void
discid_search_callback (GObject *source, GAsyncResult *res,
                        gpointer user_data)
{
    if (!g_simple_async_result_get_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res)))
    {
        g_object_unref (res);
        free_mb_tree (mb_dialog_priv->mb_tree_root);
        mb_dialog_priv->mb_tree_root = g_node_new (NULL);
        return;
    }

    et_mb_entity_view_set_tree_root (ET_MB_ENTITY_VIEW (entityView),
                                     mb_dialog_priv->mb_tree_root);
    gtk_statusbar_push (GTK_STATUSBAR (gtk_builder_get_object (builder, "statusbar")),
                        0, _("Searching Completed"));
    g_object_unref (res);
    g_free (user_data);
    et_music_brainz_dialog_stop_set_sensitive (FALSE);
}

static void
discid_search_thread_func (GSimpleAsyncResult *res, GObject *obj,
                           GCancellable *cancellable)
{
    GError *error;
    DiscId *disc;
    gchar *discid;

    error = NULL;
    disc = discid_new ();
    g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res),
                                               FALSE);

    if (!discid_read_sparse (disc, discid_get_default_device (), 0))
    {
        /* Error reading disc */
        g_set_error (&error, ET_MB5_SEARCH_ERROR,
                     ET_MB5_SEARCH_ERROR_DISCID,
                     discid_get_error_msg (disc));
        g_simple_async_report_gerror_in_idle (NULL,
                                              mb5_search_error_callback,
                                              NULL, error);
        return;
    }

    discid = discid_get_id (disc);

    if (g_cancellable_is_cancelled (cancellable))
    {
        g_set_error (&error, ET_MB5_SEARCH_ERROR,
                     ET_MB5_SEARCH_ERROR_CANCELLED,
                     _("Operation cancelled by user"));
        g_simple_async_report_gerror_in_idle (NULL,
                                              mb5_search_error_callback,
                                              NULL, error);
        discid_free (disc);
        return;
    }

    if (!et_musicbrainz_search (discid, MB_ENTITY_KIND_DISCID, mb_dialog_priv->mb_tree_root,
                                &error, cancellable))
    {
        g_simple_async_report_gerror_in_idle (NULL,
                                              mb5_search_error_callback,
                                              NULL, error);
        discid_free (disc);
        return;
    }

    et_mb_entity_view_set_tree_root (ET_MB_ENTITY_VIEW (entityView),
                                     mb_dialog_priv->mb_tree_root);
    discid_free (disc);
    g_simple_async_result_set_op_res_gboolean (G_SIMPLE_ASYNC_RESULT (res),
                                               TRUE);
}

static void
btn_discid_search (GtkWidget *button, gpointer data)
{
    mb5_search_cancellable = g_cancellable_new ();
    gtk_statusbar_push (GTK_STATUSBAR (gtk_builder_get_object (builder, "statusbar")),
                        0, _("Starting MusicBrainz Search"));
    mb_dialog_priv->async_result = g_simple_async_result_new (NULL,
                                                              discid_search_callback,
                                                              NULL,
                                                              btn_manual_find_clicked);
    g_simple_async_result_run_in_thread (mb_dialog_priv->async_result,
                                         discid_search_thread_func, 0,
                                         mb5_search_cancellable);
    et_music_brainz_dialog_stop_set_sensitive (TRUE);
}

static void
btn_close_clicked (GtkWidget *button, gpointer data)
{
    gtk_dialog_response (GTK_DIALOG (mbDialog), GTK_RESPONSE_DELETE_EVENT);
}

void
et_music_brainz_dialog_stop_set_sensitive (gboolean sensitive)
{
    gtk_widget_set_sensitive (GTK_WIDGET (gtk_builder_get_object (builder, "btnStop")),
                              sensitive);
}

/*
 * et_open_musicbrainz_dialog:
 *
 * This function will open the musicbrainz dialog.
 */
void
et_open_musicbrainz_dialog ()
{
    GtkWidget *cb_manual_search_in;
    GtkWidget *cb_search;
    GError *error;

    builder = gtk_builder_new ();
    error = NULL;

    if (!gtk_builder_add_from_resource (builder,
                                        "/org/gnome/EasyTAG/musicbrainz_dialog.ui",
                                        &error))
    {
        Log_Print (LOG_ERROR,
                   _("Error loading MusicBrainz Search Dialog '%s'"),
                   error->message);
        g_error_free (error);
        g_object_unref (G_OBJECT (builder));
        return;
    }

    mb_dialog_priv = g_malloc (sizeof (MusicBrainzDialogPrivate));
    mb_dialog_priv->mb_tree_root = g_node_new (NULL);
    entityView = et_mb_entity_view_new ();
    mbDialog = GTK_WIDGET (gtk_builder_get_object (builder, "mbDialog"));
    gtk_widget_set_size_request (mbDialog, 660, 500);
    gtk_box_pack_start (GTK_BOX (gtk_builder_get_object (builder, "centralBox")),
                        entityView, TRUE, TRUE, 2);

    cb_search = GTK_WIDGET (gtk_builder_get_object (builder, "cbManualSearch"));
    g_signal_connect (gtk_bin_get_child (GTK_BIN (cb_search)), "activate",
                      G_CALLBACK (btn_manual_find_clicked), NULL);
    g_signal_connect (gtk_builder_get_object (builder, "btnManualFind"),
                      "clicked", G_CALLBACK (btn_manual_find_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "toolbtnUp"),
                      "clicked", G_CALLBACK (tool_btn_up_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "toolbtnDown"),
                      "clicked", G_CALLBACK (tool_btn_down_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "toolbtnSelectAll"),
                      "clicked", G_CALLBACK (tool_btn_select_all_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "toolbtnUnselectAll"),
                      "clicked", G_CALLBACK (tool_btn_unselect_all_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "toolbtnInvertSelection"),
                      "clicked", G_CALLBACK (tool_btn_invert_selection_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "toolbtnToggleRedLines"),
                      "clicked", G_CALLBACK (tool_btn_toggle_red_lines_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "toolbtnRefresh"),
                      "clicked", G_CALLBACK (tool_btn_refresh_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "btnSelectedFind"),
                      "clicked", G_CALLBACK (bt_selected_find_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "btnDiscFind"),
                      "clicked", G_CALLBACK (btn_discid_search),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "btnStop"),
                      "clicked", G_CALLBACK (btn_manual_stop_clicked),
                      NULL);
    g_signal_connect (gtk_builder_get_object (builder, "btnClose"),
                      "clicked", G_CALLBACK (btn_close_clicked),
                      NULL);
    g_signal_connect_after (gtk_builder_get_object (builder, "entryTreeViewSearch"),
                            "changed",
                            G_CALLBACK (entry_tree_view_search_changed),
                            NULL);

    /* Fill Values in cb_manual_search_in */
    cb_manual_search_in = GTK_WIDGET (gtk_builder_get_object (builder,
                                                              "cbManualSearchIn"));

    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (cb_manual_search_in), "Artist");
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (cb_manual_search_in), "Album");
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (cb_manual_search_in), "Track");

    gtk_combo_box_set_active (GTK_COMBO_BOX (cb_manual_search_in), 1);

    gtk_widget_show_all (mbDialog);
    gtk_dialog_run (GTK_DIALOG (mbDialog));
    gtk_widget_destroy (mbDialog);
    g_object_unref (G_OBJECT (builder));
    free_mb_tree (mb_dialog_priv->mb_tree_root);
    g_free (mb_dialog_priv);
}
#endif /* ENABLE_MUSICBRAINZ */