/*
 * This code is modified on the basis of vncDRI3, so its license is retained.
 *
 * Copyright 2024 Pierre Ossman for Cendio AB
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "lorieDRI3.h"
#include <dri3.h>
#include <fb.h>
#include <sys/mman.h>

#ifdef FB_ACCESS_WRAPPER
#error "This code is not compatible with accessors"
#endif

static DevPrivateKeyRec lorieDRI3ScreenPrivateKey;
static DevPrivateKeyRec lorieDRI3PixmapPrivateKey;

typedef struct lorieDRI3ScreenPrivate {
    DestroyPixmapProcPtr DestroyPixmap;
} lorieDRI3ScreenPrivateRec, *lorieDRI3ScreenPrivatePtr;

typedef void *lorieDRI3PixmapPrivatePtr;

#define wrap(priv, real, mem, func) {\
  priv->mem = real->mem; \
  real->mem = func; \
}

#define unwrap(priv, real, mem) {\
  real->mem = priv->mem; \
}

static inline lorieDRI3ScreenPrivatePtr lorieDRI3ScreenPrivate(ScreenPtr screen)
{
    return (lorieDRI3ScreenPrivatePtr)dixLookupPrivate(&screen->devPrivates, &lorieDRI3ScreenPrivateKey);
}

static inline lorieDRI3PixmapPrivatePtr lorieDRI3PixmapPrivate(PixmapPtr pixmap)
{
    return (lorieDRI3PixmapPrivatePtr)dixLookupPrivate(&pixmap->devPrivates, &lorieDRI3PixmapPrivateKey);
}

static Bool FalseNoop() { return FALSE; }

static Bool lorieDestroyPixmap(PixmapPtr pixmap)
{
    Bool ret;
    void *ptr = NULL;
    size_t size = 0;
    ScreenPtr screen = pixmap->drawable.pScreen;
    lorieDRI3ScreenPrivatePtr screenPriv = lorieDRI3ScreenPrivate(screen);

    if (pixmap->refcnt == 1) {
        ptr = lorieDRI3PixmapPrivate(pixmap);
        size = pixmap->devKind * pixmap->drawable.height;
    }

    unwrap(screenPriv, screen, DestroyPixmap);
    ret = screen->DestroyPixmap(pixmap);
    wrap(screenPriv, screen, DestroyPixmap, lorieDestroyPixmap);

    if (ptr) munmap(ptr, size);
    return ret;
}

static PixmapPtr loriePixmapFromFds(ScreenPtr screen, CARD8 num_fds, const int *fds, CARD16 width, CARD16 height,
                                        const CARD32 *strides, const CARD32 *offsets, CARD8 depth, CARD8 bpp, CARD64 modifier)
{
    void *addr;
    PixmapPtr pixmap;
    const CARD64 RAW_MMAPPABLE_FD = 1274;

    (void)bpp;

    if (num_fds != 1 || modifier != RAW_MMAPPABLE_FD) {
        ErrorF("DRI3: More than 1 fd or modifier is not RAW_MMAPPABLE_FD");
        return NULL;
    }

    addr = mmap(NULL, strides[0] * height, PROT_READ, MAP_SHARED, fds[0], offsets[0]);
    if (!addr || addr == MAP_FAILED) {
        ErrorF("DRI3: mmap failed");
        return NULL;
    }

    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
    if (!pixmap) {
        ErrorF("DRI3: failed to create pixmap");
        munmap(addr, strides[0] * height);
        return NULL;
    }

    dixSetPrivate(&pixmap->devPrivates, &lorieDRI3PixmapPrivateKey, addr);
    screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, strides[0], addr);
    return pixmap;
}

static int lorieGetFormats(ScreenPtr screen, CARD32 *num_formats, CARD32 **formats)
{
    (void)screen;
    *num_formats = 0;
    *formats = NULL;
    return TRUE;
}

static int lorieGetModifiers(ScreenPtr screen, uint32_t format, uint32_t *num_modifiers, uint64_t **modifiers)
{
    (void)screen;
    (void)format;
    *num_modifiers = 0;
    *modifiers = NULL;
    return TRUE;
}

static dri3_screen_info_rec lorieDRI3Info = {
    .version = 2,
    .fds_from_pixmap = (dri3_fds_from_pixmap_proc)FalseNoop,
    .pixmap_from_fds = loriePixmapFromFds,
    .get_formats = lorieGetFormats,
    .get_modifiers = lorieGetModifiers,
    .get_drawable_modifiers = (dri3_get_drawable_modifiers_proc)FalseNoop
};

Bool lorieDRI3Init(ScreenPtr screen)
{
    lorieDRI3ScreenPrivatePtr screenPriv;

    if (!dixRegisterPrivateKey(&lorieDRI3ScreenPrivateKey, PRIVATE_SCREEN, sizeof(lorieDRI3ScreenPrivateRec)) ||
        !dixRegisterPrivateKey(&lorieDRI3PixmapPrivateKey, PRIVATE_PIXMAP, 0)) {
        return FALSE;
    }

    screenPriv = lorieDRI3ScreenPrivate(screen);
    wrap(screenPriv, screen, DestroyPixmap, lorieDestroyPixmap);
    return dri3_screen_init(screen, &lorieDRI3Info);
}
