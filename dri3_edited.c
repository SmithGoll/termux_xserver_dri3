/*
 * Copyright © 2013 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "dri3_priv.h"

#include <drm_fourcc.h>

#include <sys/mman.h>
#include "fb.h"

#define unused __attribute__((unused))
#define dri3_wrap(priv, real, mem, func) { priv = real->mem; real->mem = func; }
#define dri3_unwrap(priv, real, mem) { real->mem = priv; }

static DestroyPixmapProcPtr destoryPixmap = NULL;
static DevPrivateKeyRec loriePixmapPrivateKeyRec;

static Bool FalseNoop() { return FALSE; }

static Bool
lorieDestroyPixmap(PixmapPtr pPixmap) {
    Bool ret;
    void *ptr = NULL;
    size_t size = 0;

    if (pPixmap->refcnt == 1) {
        ptr = dixLookupPrivate(&pPixmap->devPrivates, &loriePixmapPrivateKeyRec);
        size = pPixmap->devKind * pPixmap->drawable.height;
    }

    dri3_unwrap(destoryPixmap, pPixmap->drawable.pScreen, DestroyPixmap)
    ret = (*pPixmap->drawable.pScreen->DestroyPixmap) (pPixmap);
    dri3_wrap(destoryPixmap, pPixmap->drawable.pScreen, DestroyPixmap, lorieDestroyPixmap)

    if (ptr)
        munmap(ptr, size);
    return ret;
}

static PixmapPtr
loriePixmapFromFds(ScreenPtr screen, CARD8 num_fds, const int *fds, CARD16 width, CARD16 height,
                                    const CARD32 *strides, const CARD32 *offsets, CARD8 depth, unused CARD8 bpp, CARD64 modifier) {
    const CARD64 RAW_MMAPPABLE_FD = 1274;
    PixmapPtr pixmap;
    void *addr = NULL;
    if (num_fds != 1 || modifier != RAW_MMAPPABLE_FD) {
        ErrorF("DRI3: More than 1 fd or modifier is not RAW_MMAPPABLE_FD");
        return NULL;
    }

    addr = mmap(NULL, strides[0] * height, PROT_READ, MAP_SHARED, fds[0], offsets[0]);
    if (!addr || addr == MAP_FAILED) {
        ErrorF("DRI3: mmap failed");
        return NULL;
    }

    pixmap = fbCreatePixmap(screen, 0, 0, depth, 0);
    if (!pixmap) {
        ErrorF("DRI3: failed to create pixmap");
        munmap(addr, strides[0] * height);
        return NULL;
    }

    dixSetPrivate(&pixmap->devPrivates, &loriePixmapPrivateKeyRec, addr);
    screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, strides[0], addr);

    return pixmap;
}

static int
lorieGetFormats(unused ScreenPtr screen, CARD32 *num_formats, CARD32 **formats) {
    *num_formats = 0;
    *formats = NULL;
    return TRUE;
}

static int
lorieGetModifiers(unused ScreenPtr screen, unused uint32_t format, uint32_t *num_modifiers, uint64_t **modifiers) {
    *num_modifiers = 0;
    *modifiers = NULL;
    return TRUE;
}

static dri3_screen_info_rec dri3Info = {
            .version = 2,
            .fds_from_pixmap = FalseNoop,
            .pixmap_from_fds = loriePixmapFromFds,
            .get_formats = lorieGetFormats,
            .get_modifiers = lorieGetModifiers,
            .get_drawable_modifiers = FalseNoop
};

static Bool
init_dri3(ScreenPtr screen) {
    pScreenPtr = screen;
    dri3_wrap(destoryPixmap, screen, DestroyPixmap, lorieDestroyPixmap)
    return (!dri3_screen_init(screen, &dri3Info) ||
           !dixRegisterPrivateKey(&loriePixmapPrivateKeyRec, PRIVATE_PIXMAP, 0));
}


static int dri3_request;
DevPrivateKeyRec dri3_screen_private_key;

static int dri3_screen_generation;

static Bool
dri3_close_screen(ScreenPtr screen)
{
    dri3_screen_priv_ptr screen_priv = dri3_screen_priv(screen);

    unwrap(screen_priv, screen, CloseScreen);

    free(screen_priv);
    return (*screen->CloseScreen) (screen);
}

Bool
dri3_screen_init(ScreenPtr screen, const dri3_screen_info_rec *info)
{
    dri3_screen_generation = serverGeneration;

    if (!dixRegisterPrivateKey(&dri3_screen_private_key, PRIVATE_SCREEN, 0))
        return FALSE;

    if (!dri3_screen_priv(screen)) {
        dri3_screen_priv_ptr screen_priv = calloc(1, sizeof (dri3_screen_priv_rec));
        if (!screen_priv)
            return FALSE;

        wrap(screen_priv, screen, CloseScreen, dri3_close_screen);

        screen_priv->info = info;

        dixSetPrivate(&screen->devPrivates, &dri3_screen_private_key, screen_priv);
    }

    return TRUE;
}

void
dri3_extension_init(void)
{
    ExtensionEntry *extension;
    int i;

    dri3_screen_generation = serverGeneration;
    /* If no screens support DRI3, there's no point offering the
     * extension at all
     */
    /*if (dri3_screen_generation != serverGeneration)
    {
        ErrorF("[DRI3]Failed: if (dri3_screen_generation != serverGeneration)\n"
               "dri3_screen_generation: %d\n"
               "serverGeneration: %d\n",
               dri3_screen_generation,
               serverGeneration);
        //return;
    }*/

#ifdef PANORAMIX
    if (!noPanoramiXExtension)
        FatalError("[DRI3]Failed: if (!noPanoramiXExtension)\n");
#endif

    extension = AddExtension(DRI3_NAME, DRI3NumberEvents, DRI3NumberErrors,
                             proc_dri3_dispatch, sproc_dri3_dispatch,
                             NULL, StandardMinorOpcode);
    if (!extension)
        goto bail;

    dri3_request = extension->base;

    if (screenInfo.numScreens != 1)
    {
        ErrorF("[DRI3]Multi-screen isn't supported\n");
        goto bail;
    }
    if (init_dri3(screenInfo.screens[0]))
        goto bail;

    ErrorF("DRI3 enabled\n");
    return;

bail:
    FatalError("Cannot initialize DRI3 extension");
}

uint32_t
drm_format_for_depth(uint32_t depth, uint32_t bpp)
{
    switch (bpp) {
        case 16:
            return DRM_FORMAT_RGB565;
        case 24:
            return DRM_FORMAT_XRGB8888;
        case 30:
            return DRM_FORMAT_XRGB2101010;
        case 32:
            return DRM_FORMAT_ARGB8888;
        default:
            return 0;
    }
}
