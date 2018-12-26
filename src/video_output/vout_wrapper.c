/*****************************************************************************
 * vout_wrapper.c: "vout display" -> "video output" wrapper
 *****************************************************************************
 * Copyright (C) 2009 Laurent Aimar
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout.h>
#include <assert.h>
#include "vout_internal.h"
#include "display.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
#ifdef _WIN32
static int  Forward(vlc_object_t *, char const *,
                    vlc_value_t, vlc_value_t, void *);
#endif

/* Minimum number of display picture */
#define DISPLAY_PICTURE_COUNT (1)

/*****************************************************************************
 *
 *****************************************************************************/
int vout_OpenWrapper(vout_thread_t *vout,
                     const char *splitter_name, vout_display_cfg_t *cfg)
{
    vout_thread_sys_t *sys = vout->p;
    vout_display_t *vd;

    msg_Dbg(vout, "Opening vout display wrapper");

    /* */
    char *modlist = var_InheritString(vout, "vout");

    if (splitter_name)
        vd = vout_NewSplitter(vout, &vout->p->original, cfg, modlist, splitter_name);
    else
        vd = vout_NewDisplay(vout, &vout->p->original, cfg, modlist);
    free(modlist);

    if (vd == NULL)
        return VLC_EGENERIC;

    sys->decoder_pool = NULL;

    const bool use_dr = !vout_IsDisplayFiltered(vd);
    const bool allow_dr = !vd->info.has_pictures_invalid && !vd->info.is_slow && use_dr;
    const unsigned private_picture  = 4; /* XXX 3 for filter, 1 for SPU */
    const unsigned decoder_picture  = 1 + sys->dpb_size;
    const unsigned kept_picture     = 1; /* last displayed picture */
    const unsigned reserved_picture = DISPLAY_PICTURE_COUNT +
                                      private_picture +
                                      kept_picture;
    const unsigned display_pool_size = allow_dr ? __MAX(VOUT_MAX_PICTURES,
                                                        reserved_picture + decoder_picture) : 3;
    picture_pool_t *display_pool = vout_GetPool(vd, display_pool_size);
    if (display_pool == NULL)
        goto error;

#ifndef NDEBUG
    if ( picture_pool_GetSize(display_pool) < display_pool_size )
        msg_Warn(vout, "Not enough display buffers in the pool, requested %u got %u",
                 display_pool_size, picture_pool_GetSize(display_pool));
#endif

    if (allow_dr &&
        picture_pool_GetSize(display_pool) >= reserved_picture + decoder_picture) {
        sys->display.use_copy = false;
        sys->dpb_size     = picture_pool_GetSize(display_pool) - reserved_picture;
        sys->decoder_pool = display_pool;
        sys->display_pool = display_pool;
    } else {
        sys->display.use_copy = use_dr;
        sys->decoder_pool =
            picture_pool_NewFromFormat(&vd->source,
                                       __MAX(VOUT_MAX_PICTURES,
                                             reserved_picture + decoder_picture - DISPLAY_PICTURE_COUNT));
        if (!sys->decoder_pool)
            goto error;
        if (allow_dr) {
            msg_Warn(vout, "Not enough direct buffers, using system memory");
            sys->dpb_size = 0;
        } else {
            sys->dpb_size = picture_pool_GetSize(sys->decoder_pool) - reserved_picture;
        }
        if (use_dr)
            sys->display_pool = vout_GetPool(vd, 3);
        else
            sys->display_pool = NULL;
    }
    sys->private_pool = picture_pool_Reserve(sys->decoder_pool, private_picture);
    if (!sys->private_pool)
    {
        if (sys->decoder_pool != sys->display_pool)
            picture_pool_Release(sys->decoder_pool);
        sys->display_pool = sys->decoder_pool = NULL;
        goto error;
    }

    sys->display.vd = vd;

#ifdef _WIN32
    var_Create(vout, "video-wallpaper", VLC_VAR_BOOL|VLC_VAR_DOINHERIT);
    var_AddCallback(vout, "video-wallpaper", Forward, NULL);
#endif
    return VLC_SUCCESS;

error:
    vout_DeleteDisplay(vd, cfg);
    return VLC_EGENERIC;
}

/*****************************************************************************
 *
 *****************************************************************************/
void vout_CloseWrapper(vout_thread_t *vout, vout_display_cfg_t *cfg)
{
    vout_thread_sys_t *sys = vout->p;

    assert(vout->p->decoder_pool && vout->p->private_pool);

    picture_pool_Release(sys->private_pool);

    if (sys->decoder_pool != sys->display_pool)
        picture_pool_Release(sys->decoder_pool);

#ifdef _WIN32
    var_DelCallback(vout, "video-wallpaper", Forward, NULL);
#endif
    sys->decoder_pool = NULL; /* FIXME remove */

    vout_DeleteDisplay(sys->display.vd, cfg);
}

/*****************************************************************************
 *
 *****************************************************************************/
void vout_ManageWrapper(vout_thread_t *vout)
{
    vout_thread_sys_t *sys = vout->p;
    vout_display_t *vd = sys->display.vd;

    if (vout_ManageDisplay(vd)) {
        assert(vd->info.has_pictures_invalid);

        if (!vout_IsDisplayFiltered(vd))
            sys->display_pool = vout_GetPool(vd, 3);
        else
            sys->display_pool = NULL;
    }
}

#ifdef _WIN32
static int Forward(vlc_object_t *object, char const *var,
                   vlc_value_t oldval, vlc_value_t newval, void *data)
{
    vout_thread_t *vout = (vout_thread_t*)object;

    VLC_UNUSED(oldval);
    VLC_UNUSED(data);
    return var_Set(vout->p->display.vd, var, newval);
}
#endif

