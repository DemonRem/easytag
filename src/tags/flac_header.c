/* EasyTAG - Tag editor for audio files
 * Copyright (C) 2014  David King <amigadave@amigadave.com>
 * Copyright (C) 2000-2003  Jerome Couderc <easytag@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * Code taken from :
 * FLAC - Free Lossless Audio Codec - v1.0.3
 * Copyright (C) 2001  Josh Coalson
 *
 */

#include "config.h" /* For definition of ENABLE_FLAC. */

#ifdef ENABLE_FLAC

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <FLAC/all.h>
#include <errno.h>

#include "easytag.h"
#include "et_core.h"
#include "flac_header.h"
#include "misc.h"

typedef struct
{
    GFileInputStream *istream;
    gboolean eof;
    GError *error;
} EtFlacState;

/* FLAC__metadata_read_with_callbacks() IO callbacks. */
static size_t
et_flac_read_func (void *ptr,
                   size_t size,
                   size_t nmemb,
                   FLAC__IOHandle handle)
{
    EtFlacState *state;
    gssize bytes_read;

    state = (EtFlacState *)handle;
    state->eof = FALSE;

    bytes_read = g_input_stream_read (G_INPUT_STREAM (state->istream), ptr,
                                      size * nmemb, NULL, &state->error);

    if (bytes_read == -1)
    {
        errno = EIO;
        return 0;
    }
    else if (bytes_read == 0)
    {
        state->eof = TRUE;
    }

    return bytes_read;
}

static int
et_flac_seek_func (FLAC__IOHandle handle,
                   FLAC__int64 offset,
                   int whence)
{
    EtFlacState *state;
    GSeekable *seekable;
    GSeekType seektype;

    state = (EtFlacState *)handle;
    seekable = G_SEEKABLE (state->istream);

    if (!g_seekable_can_seek (seekable))
    {
        errno = EBADF;
        return -1;
    }
    else
    {
        switch (whence)
        {
            case SEEK_SET:
                seektype = G_SEEK_SET;
                break;
            case SEEK_CUR:
                seektype = G_SEEK_CUR;
                break;
            case SEEK_END:
                seektype = G_SEEK_END;
                break;
            default:
                errno = EINVAL;
                return -1;
        }

        if (!g_seekable_seek (seekable, offset, seektype, NULL, &state->error))
        {
            /* TODO: More suitable error. */
            errno = EINVAL;
            return -1;
        }
        else
        {
            return 0;
        }
    }
}

static FLAC__int64
et_flac_tell_func (FLAC__IOHandle handle)
{
    EtFlacState *state;
    GSeekable *seekable;

    state = (EtFlacState *)handle;
    seekable = G_SEEKABLE (state->istream);

    if (!g_seekable_can_seek (seekable))
    {
        errno = EBADF;
        return -1;
    }
    else
    {
        return g_seekable_tell (seekable);
    }
}

static int
et_flac_eof_func (FLAC__IOHandle handle)
{
    EtFlacState *state;

    state = (EtFlacState *)handle;

    /* EOF is not directly supported by GFileInputStream. */
    return state->eof ? 1 : 0;
}

static int
et_flac_close_func (FLAC__IOHandle handle)
{
    EtFlacState *state;

    state = (EtFlacState *)handle;

    g_clear_object (&state->istream);
    g_clear_error (&state->error);

    /* Always return success. */
    return 0;
}

/* Header info of FLAC file */

gboolean
flac_header_read_file_info (GFile *file,
                            ET_File_Info *ETFileInfo,
                            GError **error)
{
    GFileInfo *info;
    FLAC__Metadata_Chain *chain;
    EtFlacState state;
    GFileInputStream *istream;
    FLAC__IOCallbacks callbacks = { et_flac_read_func, NULL, et_flac_seek_func,
                                    et_flac_tell_func, et_flac_eof_func,
                                    et_flac_close_func };
    FLAC__Metadata_Iterator *iter;
    gsize metadata_len;

    g_return_val_if_fail (file != NULL && ETFileInfo != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    /* Decoding FLAC file */
    chain = FLAC__metadata_chain_new ();

    if (chain == NULL)
    {
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOMEM, "%s",
                     g_strerror (ENOMEM));
        return FALSE;
    }

    istream = g_file_read (file, NULL, &state.error);

    if (istream == NULL)
    {
        FLAC__metadata_chain_delete (chain);
        return FALSE;
    }

    state.eof = FALSE;
    state.error = NULL;
    state.istream = istream;

    if (!FLAC__metadata_chain_read_with_callbacks (chain, &state, callbacks))
    {
        const FLAC__Metadata_ChainStatus status = FLAC__metadata_chain_status (chain);

        g_debug ("Error reading FLAC metadata chain: %s:",
                 FLAC__Metadata_ChainStatusString[status]);
        FLAC__metadata_chain_delete (chain);
        /* TODO: Provide a dedicated error enum corresponding to status. */
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s",
                     _("Error opening FLAC file"));
        et_flac_close_func (&state);
        return FALSE;
    }

    iter = FLAC__metadata_iterator_new ();

    if (iter == NULL)
    {
        et_flac_close_func (&state);
        g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOMEM, "%s",
                     g_strerror (ENOMEM));
        return FALSE;
    }

    FLAC__metadata_iterator_init (iter, chain);
    metadata_len = 0;

    do
    {
        const FLAC__StreamMetadata *block;

        block = FLAC__metadata_iterator_get_block (iter);

        metadata_len += block->length;

        switch (block->type)
        {
            case FLAC__METADATA_TYPE_STREAMINFO:
                {
                    const FLAC__StreamMetadata_StreamInfo *stream_info = &block->data.stream_info;
                    ETFileInfo->duration = stream_info->total_samples
                                           / stream_info->sample_rate;
                    ETFileInfo->mode = stream_info->channels;
                    ETFileInfo->samplerate = stream_info->sample_rate;
                    ETFileInfo->version = 0; /* Not defined in FLAC file. */
                }
                break;
            default:
                /* Ignore all other metadata types. */
                break;
        }
    }
    while (FLAC__metadata_iterator_next (iter));

    FLAC__metadata_iterator_delete (iter);
    FLAC__metadata_chain_delete (chain);
    et_flac_close_func (&state);
    /* End of decoding FLAC file */

    info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                              G_FILE_QUERY_INFO_NONE, NULL, NULL);

    if (info)
    {
        ETFileInfo->size = g_file_info_get_size (info);
        g_object_unref (info);
    }
    else
    {
        ETFileInfo->size = 0;
    }

    if (ETFileInfo->duration > 0 && ETFileInfo->size > 0)
    {
        /* Ignore metadata blocks, and use the remainder to calculate the
         * average bitrate (including format overhead). */
        ETFileInfo->bitrate = (ETFileInfo->size - metadata_len) * 8 /
                              ETFileInfo->duration / 1000;
    }

    return TRUE;
}

EtFileHeaderFields *
et_flac_header_display_file_info_to_ui (const ET_File *ETFile)
{
    EtFileHeaderFields *fields;
    ET_File_Info *info;
    gchar *time = NULL;
    gchar *time1 = NULL;
    gchar *size = NULL;
    gchar *size1 = NULL;

    info = ETFile->ETFileInfo;
    fields = g_slice_new (EtFileHeaderFields);

    fields->description = _("FLAC File");

    /* Nothing to display */
    fields->version_label = _("Encoder:");
    fields->version = g_strdup ("flac");

    /* Bitrate */
    fields->bitrate = g_strdup_printf (_("%d kb/s"), info->bitrate);

    /* Samplerate */
    fields->samplerate = g_strdup_printf (_("%d Hz"), info->samplerate);

    /* Mode */
    fields->mode_label = _("Channels:");
    fields->mode = g_strdup_printf ("%d", info->mode);

    /* Size */
    size = g_format_size (info->size);
    size1 = g_format_size (ETCore->ETFileDisplayedList_TotalSize);
    fields->size = g_strdup_printf ("%s (%s)", size, size1);
    g_free (size);
    g_free (size1);

    /* Duration */
    time = Convert_Duration (info->duration);
    time1 = Convert_Duration (ETCore->ETFileDisplayedList_TotalDuration);
    fields->duration = g_strdup_printf ("%s (%s)", time, time1);
    g_free (time);
    g_free (time1);

    return fields;
}

void
et_flac_file_header_fields_free (EtFileHeaderFields *fields)
{
    g_return_if_fail (fields != NULL);

    g_free (fields->version);
    g_free (fields->bitrate);
    g_free (fields->samplerate);
    g_free (fields->mode);
    g_free (fields->size);
    g_free (fields->duration);
    g_slice_free (EtFileHeaderFields, fields);
}

#endif /* ENABLE_FLAC */
