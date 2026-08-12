/*
 * video.c
 *
 * Written by
 *  Mathias Roslund <vice.emu@amidog.se>
 *  Marco van den Heuvel <blackystardust68@yahoo.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "videoarch.h"
#include "palette.h"
#include "video.h"
#include "viewport.h"
#include "kbd.h"
#include "keyboard.h"
#include "lib.h"
#include "fullscreenarch.h"
#include "pointer.h"

#define __USE_INLINE__
#define MUIPROG_H

#undef BYTE
#undef WORD
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/memory.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/asl.h>
#include <graphics/displayinfo.h>
#include <libraries/asl.h>

#ifdef AMIGA_M68K
#define _INLINE_MUIMASTER_H
#endif

#include <proto/muimaster.h>

#ifdef AMIGA_M68K
#include <libraries/mui.h>
#endif

#include <proto/graphics.h>

#ifdef HAVE_PROTO_CYBERGRAPHICS_H
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

#ifdef AMIGA_MORPHOS
#include <exec/execbase.h>
#endif
#if !defined(AMIGA_MORPHOS) && !defined(AMIGA_AROS)
#include <inline/cybergraphics.h>
#endif
#else
/*
 * FRF_P96_INLINE_P96BASE_OS3:
 * Correct OS3/m68k route.  Calls are generated through P96Base,
 * which loadlibs.c opens as Picasso96API.library.
 * No external libPicasso96API.a is linked.
 */
#include <proto/Picasso96.h>
#endif

#ifndef HAVE_PROTO_CYBERGRAPHICS_H
extern struct Library *P96Base;
#endif

#include "private.h"
#include "statusbar.h"
#include "mui/mui.h"

#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
#include <cybergraphx/cgxvideo.h>
#include <proto/cgxvideo.h>
#include "video/renderyuv.h"
#endif


/* FRF_PAL_RGB_NATIVE_V17
 * E-UAE-style AmigaOS fullscreen split:
 *
 *   Cyber/P96 true-colour mode
 *       Existing accelerated VICE RTG path.
 *
 *   Native PAL/NTSC or any <=8-bit mode
 *       graphics.library custom screen, interleaved bitplanes, indexed CPU
 *       buffer, LoadRGB32() palette and WriteChunkyPixels() publication.
 *
 * Native screens never enter p96LockBitMap(), LockBitMapTags(),
 * p96RectFill() or FillPixelArray().
 */
static int frf_pal_asl_request = 0;
static int frf_pal_asl_valid = 0;
static ULONG frf_pal_displayid = INVALID_ID;
static LONG frf_pal_width = 0;
static LONG frf_pal_height = 0;
static LONG frf_pal_depth = 0;
static UWORD frf_pal_overscan = OSCAN_STANDARD;
static BOOL frf_pal_autoscroll = TRUE;
static int frf_pal_native = 0;

static int frf_pal_is_native_mode(ULONG displayid, LONG depth)
{
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
    if (displayid != INVALID_ID && IsCyberModeID(displayid) && depth > 8) {
        return 0;
    }
    return 1;
#else
    return depth <= 8;
#endif
}

static int frf_pal_pick_asl_mode(int want_width, int want_height)
{
    struct ScreenModeRequester *request;

    request = (struct ScreenModeRequester *)
        AllocAslRequest(ASL_ScreenModeRequest, NULL);
    if (request == NULL) {
        return 0;
    }

    if (!AslRequestTags(request,
                        ASLSM_TitleText,
                        (ULONG)"VICE fullscreen - PAL RGB / RTG",
                        ASLSM_InitialDisplayID, 0,
                        ASLSM_InitialDisplayWidth, want_width,
                        ASLSM_InitialDisplayHeight, want_height,
                        ASLSM_InitialDisplayDepth, 4,
                        ASLSM_MinWidth, 320,
                        ASLSM_MinHeight, 200,
                        ASLSM_DoWidth, TRUE,
                        ASLSM_DoHeight, TRUE,
                        ASLSM_DoDepth, TRUE,
                        ASLSM_DoOverscanType, TRUE,
                        TAG_DONE)) {
        FreeAslRequest(request);
        return 0;
    }

    frf_pal_displayid = request->sm_DisplayID;
    frf_pal_width = request->sm_DisplayWidth;
    frf_pal_height = request->sm_DisplayHeight;
    frf_pal_depth = request->sm_DisplayDepth;
    frf_pal_overscan = request->sm_OverscanType;
    frf_pal_autoscroll = request->sm_AutoScroll;
    FreeAslRequest(request);

    if (frf_pal_displayid == INVALID_ID) {
        return 0;
    }
    if (frf_pal_width <= 0) {
        frf_pal_width = want_width;
    }
    if (frf_pal_height <= 0) {
        frf_pal_height = want_height;
    }
    if (frf_pal_depth <= 0) {
        frf_pal_depth = 4;
    }

    frf_pal_native =
        frf_pal_is_native_mode(frf_pal_displayid, frf_pal_depth);

    /* A native/planar screen cannot use the true-colour renderer. */
    if (frf_pal_native && frf_pal_depth > 8) {
        frf_pal_depth = 8;
    }

    frf_pal_asl_valid = 1;
    return 1;
}

static void frf_pal_free_native_buffer(struct video_canvas_s *canvas)
{
    if (canvas != NULL && canvas->os != NULL
        && canvas->os->frf_pal_native_chunky != NULL) {
        lib_free(canvas->os->frf_pal_native_chunky);
        canvas->os->frf_pal_native_chunky = NULL;
    }

    if (canvas != NULL && canvas->os != NULL) {
        canvas->os->frf_pal_native_bpr = 0;
        canvas->os->frf_pal_native_height = 0;
        canvas->os->frf_pal_native_mode = 0;
    }
}









/* FRF_PAL_RGB_V25_STAGED_BLIT
 *
 * V17 dirty rendering is retained. The complete persistent indexed frame is
 * cropped to 320x256, converted into a private four-plane staging bitmap, then
 * copied to the visible fullscreen Window with one BltBitMapRastPort().
 *
 * window->WScreen is authoritative for the palette and bitmap friendship.
 */
#define FRF_PAL_STAGE_WIDTH 320
#define FRF_PAL_STAGE_HEIGHT 256


/* FRF_PAL_RGB_V26_PIPELINE_PROBE
 * First 150 frames use a synthetic indexed pattern through the exact V25
 * conversion and final Window blit. Then the path switches to real VICE data.
 * Runtime log: PROGDIR:vice-pal-rgb-v26.log
 */
#define FRF_PAL_PROBE_FRAMES 150
static UBYTE frf_pal_probe_pixels[FRF_PAL_STAGE_WIDTH * FRF_PAL_STAGE_HEIGHT];
static int frf_pal_probe_ready = 0;
static ULONG frf_pal_probe_frame = 0;
static const UWORD frf_pal_probe_palette[16] = {
  0x000,0xfff,0xf00,0x0ff,0xf0f,0x0f0,0x00f,0xff0,
  0xf80,0x840,0xf88,0x444,0x888,0x8f8,0x88f,0xccc
};
static void frf_pal_probe_init(void)
{
    int x,y;
    if (frf_pal_probe_ready) return;
    for (y=0;y<FRF_PAL_STAGE_HEIGHT;y++) {
        for (x=0;x<FRF_PAL_STAGE_WIDTH;x++) {
            UBYTE pen=(UBYTE)((x*16)/FRF_PAL_STAGE_WIDTH);
            if (y<8) pen=1;
            else if (y>=FRF_PAL_STAGE_HEIGHT-8) pen=2;
            else if ((y>=120 && y<128) || (x>=156 && x<164)) pen=7;
            frf_pal_probe_pixels[y*FRF_PAL_STAGE_WIDTH+x]=pen;
        }
    }
    frf_pal_probe_ready=1;
}
static void frf_pal_probe_force_display(struct Screen *screen, struct Window *window)
{
    if (!screen || !window) return;
    LoadRGB4(&screen->ViewPort,(UWORD *)frf_pal_probe_palette,16);
    MakeScreen(screen);
    RethinkDisplay();
    ScreenToFront(screen);
    WindowToFront(window);
    ActivateWindow(window);
}
static ULONG frf_pal_probe_checksum(const UBYTE *pixels, ULONG bpr, int sx, int sy)
{
    ULONG hash=2166136261UL;
    int x,y;
    if (!pixels || !bpr) return 0;
    for (y=0;y<FRF_PAL_STAGE_HEIGHT;y+=8) {
        const UBYTE *row=pixels+((ULONG)(sy+y)*bpr)+sx;
        for (x=0;x<FRF_PAL_STAGE_WIDTH;x+=8) {
            hash^=row[x]; hash*=16777619UL;
        }
    }
    return hash;
}
static void frf_pal_probe_log(struct video_canvas_s *canvas, struct Window *window,
 int dx,int dy,int stage_pen,int window_pen,ULONG hash,int synthetic)
{
    FILE *f;
    struct Screen *screen;
    struct BitMap *wbm,*sbm;
    if (!window) return;
    screen=window->WScreen;
    wbm=window->RPort ? window->RPort->BitMap : NULL;
    sbm=screen ? screen->RastPort.BitMap : NULL;
    f=fopen("PROGDIR:vice-pal-rgb-v26.log","a");
    if (!f) f=fopen("vice-pal-rgb-v26.log","a");
    if (!f) return;
    fprintf(f,"frame=%lu source=%s hash=%08lx canvas_screen=%p window=%p wscreen=%p window_bm=%p screen_bm=%p same_screen=%d same_bitmap=%d screen=%dx%dx%d window=%dx%d borders=%d,%d,%d,%d native=%lux%lu canvas_depth=%d bpl=%u dest=%d,%d stage_pen=%d window_pen=%d\n",
      (unsigned long)frf_pal_probe_frame, synthetic?"synthetic":"vice",
      (unsigned long)hash,
      canvas&&canvas->os?(void *)canvas->os->screen:NULL,(void *)window,(void *)screen,
      (void *)wbm,(void *)sbm,canvas&&canvas->os&&canvas->os->screen==screen,
      wbm&&wbm==sbm,screen?screen->Width:-1,screen?screen->Height:-1,
      sbm?sbm->Depth:-1,window->Width,window->Height,window->BorderLeft,
      window->BorderTop,window->BorderRight,window->BorderBottom,
      canvas&&canvas->os?(unsigned long)canvas->os->frf_pal_native_bpr:0,
      canvas&&canvas->os?(unsigned long)canvas->os->frf_pal_native_height:0,
      canvas?canvas->depth:-1,canvas?canvas->bytes_per_line:0,dx,dy,stage_pen,window_pen);
    fclose(f);
}

static struct BitMap *frf_pal_stage_bitmap = NULL;
static struct BitMap *frf_pal_stage_line_bitmap = NULL;
static struct BitMap *frf_pal_stage_friend_bitmap = NULL;
static struct RastPort frf_pal_stage_rp;
static struct RastPort frf_pal_stage_line_rp;
static struct Screen *frf_pal_stage_screen = NULL;

static void frf_pal_stage_release(void)
{
    if (frf_pal_stage_bitmap != NULL || frf_pal_stage_line_bitmap != NULL) {
        WaitBlit();
    }
    if (frf_pal_stage_line_bitmap != NULL) {
        FreeBitMap(frf_pal_stage_line_bitmap);
    }
    if (frf_pal_stage_bitmap != NULL) {
        FreeBitMap(frf_pal_stage_bitmap);
    }
    frf_pal_stage_bitmap = NULL;
    frf_pal_stage_line_bitmap = NULL;
    frf_pal_stage_friend_bitmap = NULL;
    frf_pal_stage_screen = NULL;
}

static void frf_pal_stage_load_palette(
    struct video_canvas_s *canvas,
    struct Screen *screen)
{
    ULONG rgbtab[1 + (16 * 3) + 1];
    unsigned int index;
    unsigned int count;

    if (canvas == NULL || canvas->palette == NULL || screen == NULL) {
        return;
    }

    count = canvas->palette->num_entries;
    if (count > 16) {
        count = 16;
    }

    rgbtab[0] = ((ULONG)count << 16) | 0;
    for (index = 0; index < count; index++) {
        rgbtab[1 + index * 3 + 0] =
            ((ULONG)canvas->palette->entries[index].red) * 0x01010101UL;
        rgbtab[1 + index * 3 + 1] =
            ((ULONG)canvas->palette->entries[index].green) * 0x01010101UL;
        rgbtab[1 + index * 3 + 2] =
            ((ULONG)canvas->palette->entries[index].blue) * 0x01010101UL;
    }
    rgbtab[1 + count * 3] = 0;
    LoadRGB32(&screen->ViewPort, rgbtab);
}


/* FRF_PAL_RGB_V27_FORCED_NATIVE_GATE
 *
 * If the fullscreen Window is currently backed by a four-plane bitmap, force
 * the native RGB refresh path even when V17's frf_pal_native_mode flag/buffer
 * was not set during reopen(). This is a gate/activation fix, not another
 * publisher experiment.
 */
static int frf_pal_forced_native_gate(struct video_canvas_s *canvas)
{
    struct Window *window;
    struct BitMap *bitmap;
    ULONG need_bpr;
    ULONG need_height;
    ULONG need_size;
    int allocated;

    if (canvas == NULL || canvas->os == NULL
        || canvas->os->window == NULL
        || canvas->draw_buffer == NULL) {
        return 0;
    }

    window = canvas->os->window;
    if (window->RPort == NULL || window->RPort->BitMap == NULL) {
        return 0;
    }

    bitmap = window->RPort->BitMap;
    if (bitmap->Depth != 4) {
        return canvas->os->frf_pal_native_mode
            && canvas->os->frf_pal_native_chunky != NULL;
    }

    need_bpr = canvas->draw_buffer->canvas_physical_width;
    need_height = canvas->draw_buffer->canvas_physical_height;

    if (need_bpr < FRF_PAL_STAGE_WIDTH) {
        need_bpr = FRF_PAL_STAGE_WIDTH;
    }
    if (need_height < FRF_PAL_STAGE_HEIGHT) {
        need_height = FRF_PAL_STAGE_HEIGHT;
    }

    allocated = 0;
    if (canvas->os->frf_pal_native_chunky == NULL
        || canvas->os->frf_pal_native_bpr < need_bpr
        || canvas->os->frf_pal_native_height < need_height) {
        if (canvas->os->frf_pal_native_chunky != NULL) {
            lib_free(canvas->os->frf_pal_native_chunky);
            canvas->os->frf_pal_native_chunky = NULL;
        }

        need_size = need_bpr * need_height;
        canvas->os->frf_pal_native_chunky = lib_malloc(need_size);
        if (canvas->os->frf_pal_native_chunky == NULL) {
            canvas->os->frf_pal_native_mode = 0;
            canvas->os->frf_pal_native_bpr = 0;
            canvas->os->frf_pal_native_height = 0;
            return 0;
        }

        memset(canvas->os->frf_pal_native_chunky, 0, need_size);
        canvas->os->frf_pal_native_bpr = need_bpr;
        canvas->os->frf_pal_native_height = need_height;
        allocated = 1;
    }

    canvas->os->frf_pal_native_mode = 1;
    canvas->os->window_bitmap = NULL;
    canvas->os->pixfmt = 0;
    canvas->os->bpr = canvas->os->frf_pal_native_bpr;
    canvas->os->bpp = 1;

    if (frf_pal_probe_frame < 4 || allocated) {
        FILE *file;

        file = fopen("PROGDIR:vice-pal-rgb-v27.log", "a");
        if (file == NULL) {
            file = fopen("vice-pal-rgb-v27.log", "a");
        }
        if (file != NULL) {
            fprintf(
                file,
                "gate frame=%lu allocated=%d window=%p wscreen=%p "
                "bitmap=%p depth=%d need=%lux%lu bpr=%lu height=%lu "
                "native_mode=%d chunky=%p\\n",
                (unsigned long)frf_pal_probe_frame,
                allocated,
                (void *)window,
                (void *)window->WScreen,
                (void *)bitmap,
                bitmap->Depth,
                (unsigned long)need_bpr,
                (unsigned long)need_height,
                (unsigned long)canvas->os->frf_pal_native_bpr,
                (unsigned long)canvas->os->frf_pal_native_height,
                canvas->os->frf_pal_native_mode,
                (void *)canvas->os->frf_pal_native_chunky);
            fclose(file);
        }
    }

    return canvas->os->frf_pal_native_chunky != NULL;
}

static int frf_pal_stage_prepare(struct video_canvas_s *canvas)
{
    struct Window *window;
    struct Screen *screen;
    struct BitMap *friend_bitmap;

    if (canvas == NULL || canvas->os == NULL
        || canvas->os->window == NULL) {
        return 0;
    }

    window=canvas->os->window;
    screen=window->WScreen;
    if (screen == NULL || window->RPort == NULL
        || window->RPort->BitMap == NULL) {
        return 0;
    }

    friend_bitmap=window->RPort->BitMap;
    if (friend_bitmap->Depth != 4) {
        return 0;
    }

    if (frf_pal_stage_bitmap != NULL
        && frf_pal_stage_line_bitmap != NULL
        && frf_pal_stage_friend_bitmap == friend_bitmap
        && frf_pal_stage_screen == screen) {
        frf_pal_stage_load_palette(canvas,screen);
        return 1;
    }

    frf_pal_stage_release();

    frf_pal_stage_bitmap=AllocBitMap(
        FRF_PAL_STAGE_WIDTH,FRF_PAL_STAGE_HEIGHT,4,BMF_CLEAR,friend_bitmap);
    if (frf_pal_stage_bitmap == NULL) {
        return 0;
    }

    frf_pal_stage_line_bitmap=AllocBitMap(
        FRF_PAL_STAGE_WIDTH,1,4,BMF_CLEAR,friend_bitmap);
    if (frf_pal_stage_line_bitmap == NULL) {
        frf_pal_stage_release();
        return 0;
    }

    InitRastPort(&frf_pal_stage_rp);
    frf_pal_stage_rp.Layer=NULL;
    frf_pal_stage_rp.BitMap=frf_pal_stage_bitmap;

    frf_pal_stage_line_rp=frf_pal_stage_rp;
    frf_pal_stage_line_rp.Layer=NULL;
    frf_pal_stage_line_rp.BitMap=frf_pal_stage_line_bitmap;

    frf_pal_stage_friend_bitmap=friend_bitmap;
    frf_pal_stage_screen=screen;

    frf_pal_stage_load_palette(canvas,screen);
    ScreenToFront(screen);
    ActivateWindow(window);
    return 1;
}

static int frf_pal_stage_source_origin(
    struct video_canvas_s *canvas,
    int *source_x,
    int *source_y)
{
    int source_width;
    int source_height;

    if (canvas == NULL || canvas->os == NULL
        || source_x == NULL || source_y == NULL) {
        return 0;
    }

    source_width=(int)canvas->os->frf_pal_native_bpr;
    source_height=(int)canvas->os->frf_pal_native_height;

    if (source_width < FRF_PAL_STAGE_WIDTH
        || source_height < FRF_PAL_STAGE_HEIGHT) {
        return 0;
    }

    *source_x=(source_width-FRF_PAL_STAGE_WIDTH)/2;
    *source_y=(source_height-FRF_PAL_STAGE_HEIGHT)/2;
    return 1;
}

static int frf_pal_stage_publish(struct video_canvas_s *canvas)
{
    struct Window *window;
    int source_x;
    int source_y;
    int destination_x;
    int destination_y;
    int row;
    int synthetic;
    ULONG source_hash;
    int stage_pen;
    int window_pen;

    if (canvas == NULL || canvas->os == NULL
        || canvas->os->window == NULL
        || canvas->os->frf_pal_native_chunky == NULL
        || !frf_pal_stage_prepare(canvas)
        || !frf_pal_stage_source_origin(canvas,&source_x,&source_y)) {
        return 0;
    }

    window=canvas->os->window;
    frf_pal_probe_init();
    synthetic=frf_pal_probe_frame<FRF_PAL_PROBE_FRAMES;
    if (synthetic) frf_pal_probe_force_display(window->WScreen,window);
    else frf_pal_stage_load_palette(canvas,window->WScreen);
    destination_x=(window->Width-FRF_PAL_STAGE_WIDTH)/2;
    destination_y=(window->Height-FRF_PAL_STAGE_HEIGHT)/2;
    if (destination_x < 0 || destination_y < 0) {
        return 0;
    }

    for (row=0;row<FRF_PAL_STAGE_HEIGHT;row++) {
        UBYTE *source;
        LONG plotted;

        if (synthetic) {
            source=frf_pal_probe_pixels+(row*FRF_PAL_STAGE_WIDTH);
        } else {
            source=canvas->os->frf_pal_native_chunky
                + ((ULONG)(source_y+row)*canvas->os->frf_pal_native_bpr)
                + source_x;
        }

        plotted=WritePixelLine8(
            &frf_pal_stage_rp,0,(UWORD)row,FRF_PAL_STAGE_WIDTH,
            source,&frf_pal_stage_line_rp);

        if (plotted != FRF_PAL_STAGE_WIDTH) {
            return 0;
        }
    }

    WaitBlit();
    BltBitMapRastPort(
        frf_pal_stage_bitmap,0,0,
        window->RPort,destination_x,destination_y,
        FRF_PAL_STAGE_WIDTH,FRF_PAL_STAGE_HEIGHT,0xc0);
    WaitBlit();
    if (synthetic) source_hash=frf_pal_probe_checksum(frf_pal_probe_pixels,FRF_PAL_STAGE_WIDTH,0,0);
    else source_hash=frf_pal_probe_checksum(canvas->os->frf_pal_native_chunky,canvas->os->frf_pal_native_bpr,source_x,source_y);
    stage_pen=ReadPixel(&frf_pal_stage_rp,FRF_PAL_STAGE_WIDTH/2,FRF_PAL_STAGE_HEIGHT/2);
    window_pen=ReadPixel(window->RPort,destination_x+FRF_PAL_STAGE_WIDTH/2,destination_y+FRF_PAL_STAGE_HEIGHT/2);
    if (frf_pal_probe_frame==0 || frf_pal_probe_frame==1 || frf_pal_probe_frame==10 || frf_pal_probe_frame==FRF_PAL_PROBE_FRAMES-1 || frf_pal_probe_frame==FRF_PAL_PROBE_FRAMES || frf_pal_probe_frame==FRF_PAL_PROBE_FRAMES+10)
        frf_pal_probe_log(canvas,window,destination_x,destination_y,stage_pen,window_pen,source_hash,synthetic);
    frf_pal_probe_frame++;
    return 1;
}

static void frf_pal_stage_fallback(struct video_canvas_s *canvas)
{
    struct Window *window;
    int source_x;
    int source_y;
    int destination_x;
    int destination_y;

    if (canvas == NULL || canvas->os == NULL
        || canvas->os->window == NULL
        || canvas->os->frf_pal_native_chunky == NULL
        || !frf_pal_stage_source_origin(canvas,&source_x,&source_y)) {
        return;
    }

    window=canvas->os->window;
    destination_x=(window->Width-FRF_PAL_STAGE_WIDTH)/2;
    destination_y=(window->Height-FRF_PAL_STAGE_HEIGHT)/2;
    if (destination_x < 0 || destination_y < 0) {
        return;
    }

    WriteChunkyPixels(
        window->RPort,
        destination_x,destination_y,
        destination_x+FRF_PAL_STAGE_WIDTH-1,
        destination_y+FRF_PAL_STAGE_HEIGHT-1,
        canvas->os->frf_pal_native_chunky
            + ((ULONG)source_y*canvas->os->frf_pal_native_bpr)
            + source_x,
        canvas->os->frf_pal_native_bpr);
}

video_canvas_t *canvaslist = NULL;

#ifndef HAVE_PROTO_CYBERGRAPHICS_H
/* FRF_MAY17_NATIVE_DIRTY_FASTPATH_V2 */
static UBYTE *frf_may17_p96_buffer = NULL;
static ULONG frf_may17_p96_buffer_size = 0;
static const char frf_may17_native_dirty_marker[] =
    "FRF_P96_SAFE_RENDERINFO_PATH";
#endif

#ifndef HAVE_PROTO_CYBERGRAPHICS_H
/* FRF_MAY17_P96_NATIVE_BUFFER */
static UBYTE *frf_p96_cpu_buffer = NULL;
static ULONG frf_p96_cpu_buffer_size = 0;
const char frf_p96_safe_renderinfo_marker[] = "FRF_P96_SAFE_RENDERINFO_PATH";
#endif

int video_arch_cmdline_options_init(void)
{
    return 0;
}

#ifndef AMIGA_AROS
struct RastPort *CreateRastPort(void)
{
    return lib_AllocVec(sizeof(struct RastPort), MEMF_ANY | MEMF_PUBLIC);
}

struct RastPort *CloneRastPort(struct RastPort *friend_rastport)
{
    struct RastPort *tmpRPort = CreateRastPort();

    if (tmpRPort != NULL) {
        CopyMem(friend_rastport, tmpRPort, sizeof(struct RastPort));
        return tmpRPort;
    }
    return NULL;
}
#endif

#ifdef AMIGA_AROS
/* Use these on ALL amiga platforms not just AROS */
UBYTE *unlockable_buffer = NULL;            /* Used to render the vice-buffer so we can WPA it into our backbuffer if we cant lock a bitmap! */

static struct RastPort *renderRPort = NULL; /* Clone of the windows rastport used to visibly output  */
static struct RastPort *backRPort = NULL;   /* RastPort for our backbuffer (canvas->os->window_bitmap) */
#endif

static struct Process *self;
static struct Window *orig_windowptr;

int video_init(void)
{
    self = (APTR)FindTask(NULL);
    orig_windowptr = self->pr_WindowPtr;
    if (mui_init() == 0) {
        return 0;
    }
    return -1;
}

void video_shutdown(void)
{
    struct video_canvas_s *nextcanvas, *canvas;

    mui_exit();

    /* make sure the process window ref won't be bad */
    self->pr_WindowPtr = orig_windowptr;

    /* close any possibly open canvas */
    nextcanvas = canvaslist;
    while ((canvas = nextcanvas)) {
        nextcanvas = canvas->next;

        video_canvas_destroy(canvas);
    }
}

static int IsFullscreenEnabled(void)
{
    int b;
    resources_get_value("FullscreenEnabled", (void *)&b);
    return b;
}

static int IsFullscreenStatusbarEnabled(void)
{
    int b;
    resources_get_value("StatusBarEnabled", (void *)&b);
    return b;
}

#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
static int IsVideoOverlayEnabled(void)
{
    int b;
    resources_get_value("VideoOverlayEnabled", (void *)&b);
    return b;
}
#endif

static void closecanvaswindow(struct video_canvas_s *canvas)
{
    struct Window *window = canvas->os->window;
    if (window != NULL) {
        canvas->os->window = NULL;
        if (canvas->current_fullscreen == 0) {
            canvas->window_left = window->LeftEdge;
            canvas->window_top = window->TopEdge;
        }
        /* make sure the process window ref won't be bad */
        if (self->pr_WindowPtr == window) {
            self->pr_WindowPtr = orig_windowptr;
        }
        CloseWindow(window);
    }
}



/* FRF_PAL_RGB_V29_OPEN_SCREEN_PROBE
 *
 * Runs inside reopen(), immediately after the fullscreen Screen and Window
 * have been successfully opened and before ordinary refresh begins.
 *
 * It is deliberately independent of:
 *   frf_pal_native_mode
 *   frf_pal_native_chunky
 *   waiting_for_resize
 *   V25/V26/V27 publication
 *
 * The probe prints and writes the selected DisplayID classification, actual
 * bitmap depths and pointer relationships, then holds a 16-colour pattern on
 * the opened fullscreen Window for roughly three seconds.
 */
static const UWORD frf_pal_v29_palette[16] = {
    0x000, 0xfff, 0xf00, 0x0ff,
    0xf0f, 0x0f0, 0x00f, 0xff0,
    0xf80, 0x840, 0xf88, 0x444,
    0x888, 0x8f8, 0x88f, 0xccc
};

static void frf_pal_v29_write_report(
    const char *path,
    ULONG displayid,
    int is_cyber,
    int requested_width,
    int requested_height,
    int requested_depth,
    struct video_canvas_s *canvas)
{
    FILE *file;
    struct Window *window;
    struct Screen *screen;
    struct BitMap *window_bitmap;
    struct BitMap *screen_bitmap;

    if (canvas == NULL || canvas->os == NULL) {
        return;
    }

    window = canvas->os->window;
    screen = window != NULL ? window->WScreen : canvas->os->screen;
    window_bitmap = window != NULL && window->RPort != NULL
        ? window->RPort->BitMap : NULL;
    screen_bitmap = screen != NULL
        ? screen->RastPort.BitMap : NULL;

    file = fopen(path, "a");
    if (file == NULL) {
        return;
    }

    fprintf(
        file,
        "displayid=%08lx is_cyber=%d requested=%dx%dx%d "
        "frf_native=%d asl_valid=%d "
        "canvas_screen=%p window=%p wscreen=%p "
        "window_rp=%p window_bitmap=%p screen_bitmap=%p "
        "window_depth=%d screen_depth=%d same_screen=%d same_bitmap=%d "
        "screen_size=%dx%d window_size=%dx%d "
        "borders=%d,%d,%d,%d native_mode=%d chunky=%p "
        "native_bpr=%lu native_height=%lu waiting=%d\n",
        (unsigned long)displayid,
        is_cyber,
        requested_width,
        requested_height,
        requested_depth,
        frf_pal_native,
        frf_pal_asl_valid,
        (void *)canvas->os->screen,
        (void *)window,
        (void *)screen,
        window != NULL ? (void *)window->RPort : NULL,
        (void *)window_bitmap,
        (void *)screen_bitmap,
        window_bitmap != NULL ? window_bitmap->Depth : -1,
        screen_bitmap != NULL ? screen_bitmap->Depth : -1,
        canvas->os->screen == screen,
        window_bitmap != NULL && window_bitmap == screen_bitmap,
        screen != NULL ? screen->Width : -1,
        screen != NULL ? screen->Height : -1,
        window != NULL ? window->Width : -1,
        window != NULL ? window->Height : -1,
        window != NULL ? window->BorderLeft : -1,
        window != NULL ? window->BorderTop : -1,
        window != NULL ? window->BorderRight : -1,
        window != NULL ? window->BorderBottom : -1,
        canvas->os->frf_pal_native_mode,
        (void *)canvas->os->frf_pal_native_chunky,
        (unsigned long)canvas->os->frf_pal_native_bpr,
        (unsigned long)canvas->os->frf_pal_native_height,
        canvas->waiting_for_resize);

    fclose(file);
}

static void frf_pal_v29_open_screen_probe(
    struct video_canvas_s *canvas,
    ULONG displayid,
    int requested_width,
    int requested_height,
    int requested_depth)
{
    struct Window *window;
    struct Screen *screen;
    struct RastPort *rp;
    struct BitMap *bitmap;
    int is_cyber;
    int bar;
    int x0;
    int x1;
    int frame;

    if (canvas == NULL || canvas->os == NULL
        || canvas->os->window == NULL) {
        return;
    }

    window = canvas->os->window;
    screen = window->WScreen;
    rp = window->RPort;
    bitmap = rp != NULL ? rp->BitMap : NULL;

#ifdef HAVE_PROTO_CYBERGRAPHICS_H
    is_cyber = displayid != INVALID_ID && IsCyberModeID(displayid);
#else
    is_cyber = 0;
#endif

    printf(
        "FRF V29 OPEN: id=%08lx cyber=%d req=%dx%dx%d "
        "screen=%p window=%p wscreen=%p bitmap=%p depth=%d "
        "frf_native=%d native_mode=%d waiting=%d\n",
        (unsigned long)displayid,
        is_cyber,
        requested_width,
        requested_height,
        requested_depth,
        (void *)canvas->os->screen,
        (void *)window,
        (void *)screen,
        (void *)bitmap,
        bitmap != NULL ? bitmap->Depth : -1,
        frf_pal_native,
        canvas->os->frf_pal_native_mode,
        canvas->waiting_for_resize);
    fflush(stdout);

    frf_pal_v29_write_report(
        "RAM:vice-pal-rgb-v29.log",
        displayid,
        is_cyber,
        requested_width,
        requested_height,
        requested_depth,
        canvas);
    frf_pal_v29_write_report(
        "T:vice-pal-rgb-v29.log",
        displayid,
        is_cyber,
        requested_width,
        requested_height,
        requested_depth,
        canvas);

    if (screen == NULL || rp == NULL || bitmap == NULL) {
        return;
    }

    LoadRGB4(
        &screen->ViewPort,
        (UWORD *)frf_pal_v29_palette,
        16);
    MakeScreen(screen);
    RethinkDisplay();
    ScreenToFront(screen);
    WindowToFront(window);
    ActivateWindow(window);

    for (bar = 0; bar < 16; bar++) {
        x0 = (bar * window->Width) / 16;
        x1 = (((bar + 1) * window->Width) / 16) - 1;
        if (x1 < x0) {
            x1 = x0;
        }
        SetAPen(rp, (ULONG)bar);
        RectFill(rp, x0, 0, x1, window->Height - 1);
    }

    SetAPen(rp, 1);
    RectFill(rp, 0, 0, window->Width - 1, 7);
    SetAPen(rp, 2);
    RectFill(
        rp,
        0,
        window->Height - 8,
        window->Width - 1,
        window->Height - 1);
    SetAPen(rp, 7);
    RectFill(
        rp,
        (window->Width / 2) - 4,
        0,
        (window->Width / 2) + 3,
        window->Height - 1);

    WaitBlit();

    /* Hold before any refresh code can overwrite the pattern. */
    for (frame = 0; frame < 150; frame++) {
        WaitTOF();
    }
}




/* FRF_PAL_RGB_V37_MAME_BULK_C2P
 *
 * V36 fixed ownership and proved a correct moving native image, but its
 * SetAPen()/RectFill() colour-run painter performs thousands of graphics.library
 * calls per frame.
 *
 * V37 retains V36's first-line native refresh ownership and indexed VICE render,
 * then applies the fast native model used by the MAME/E-UAE paths:
 *
 *   - align the dirty rectangle to 16 chunky pixels;
 *   - convert all four bitplanes together in Fast RAM;
 *   - keep a planar shadow for the visible BitMap;
 *   - copy only changed contiguous UWORD runs into Chip RAM;
 *   - support both standard planar and SA_Interleaved BitMaps;
 *   - retain the V36 run painter only as a safety fallback.
 *
 * This is a C bulkplane implementation so it does not add a new assembler or
 * Automake dependency. The data flow and direct-planar output are the same
 * principles as MAME's aligned native chunky-to-planar path.
 */
#define FRF_PAL_V37_MAX_PENS 16
#define FRF_PAL_V37_PLANES 4
#define FRF_PAL_V37_MAX_WORDS 256

static UBYTE *frf_pal_v37_buffer = NULL;
static ULONG frf_pal_v37_buffer_size = 0;
static ULONG frf_pal_v37_bpr = 0;
static ULONG frf_pal_v37_height = 0;

static UWORD *frf_pal_v37_planar_shadow = NULL;
static ULONG frf_pal_v37_shadow_words = 0;
static struct BitMap *frf_pal_v37_shadow_bitmap = NULL;
static ULONG frf_pal_v37_shadow_row_stride = 0;
static ULONG frf_pal_v37_shadow_words_per_row = 0;
static ULONG frf_pal_v37_shadow_rows = 0;
static int frf_pal_v37_shadow_interleaved = 0;

static UWORD frf_pal_v37_packed
    [FRF_PAL_V37_PLANES][FRF_PAL_V37_MAX_WORDS];

static struct Window *frf_pal_v37_palette_window = NULL;
static struct Screen *frf_pal_v37_palette_screen = NULL;
static struct palette_s *frf_pal_v37_palette = NULL;
/* FRF_PAL_RGB_V39_MENU_SAFE
 *
 * Direct writes into BitMap->Planes bypass Intuition layer clipping.
 * IDCMP_MENUVERIFY therefore freezes native publication before Intuition
 * renders a menu. IDCMP_MENUPICK resumes it after the menu session.
 */
static volatile int frf_pal_v39_menu_active = 0;

void video_arch_native_menu_begin(void)
{
    frf_pal_v39_menu_active = 1;
}

void video_arch_native_menu_end(void)
{
    ULONG index;

    frf_pal_v39_menu_active = 0;

    /*
     * Intuition temporarily modifies the visible planar bitmap while menus
     * are active. Invert the Fast-RAM shadow so the next touched words are
     * guaranteed to be republished instead of incorrectly treated as equal.
     */
    if (frf_pal_v37_planar_shadow != NULL) {
        for (index = 0;
             index < frf_pal_v37_shadow_words;
             index++) {
            frf_pal_v37_planar_shadow[index] ^= (UWORD)0xffffU;
        }
    }
}


static int frf_pal_v37_should_own_refresh(
    struct video_canvas_s *canvas)
{
    struct Window *window;
    struct BitMap *bitmap;
    ULONG depth;

    if (canvas == NULL
        || canvas->os == NULL
        || canvas->os->screen == NULL
        || canvas->os->window == NULL) {
        return 0;
    }

    window = canvas->os->window;

    if (window->WScreen != canvas->os->screen
        || window->RPort == NULL
        || window->RPort->BitMap == NULL) {
        return 0;
    }

    bitmap = window->RPort->BitMap;
    depth = GetBitMapAttr(bitmap, BMA_DEPTH);

    /*
     * Preserve the V36 ownership fix: the visible low-depth custom Screen
     * owns refresh even if an old RTG window_bitmap remains allocated.
     */
    return depth > 0 && depth <= 8;
}

static int frf_pal_v37_prepare_buffer(
    struct video_canvas_s *canvas)
{
    ULONG width;
    ULONG height;
    ULONG size;

    if (canvas == NULL || canvas->draw_buffer == NULL) {
        return 0;
    }

    width = canvas->draw_buffer->canvas_physical_width;
    height = canvas->draw_buffer->canvas_physical_height;

    if (width == 0 || height == 0) {
        return 0;
    }

    size = width * height;
    if (size == 0) {
        return 0;
    }

    if (frf_pal_v37_buffer == NULL
        || frf_pal_v37_buffer_size < size
        || frf_pal_v37_bpr != width
        || frf_pal_v37_height != height) {
        if (frf_pal_v37_buffer != NULL) {
            lib_free(frf_pal_v37_buffer);
            frf_pal_v37_buffer = NULL;
        }

        frf_pal_v37_buffer = (UBYTE *)lib_malloc(size);
        if (frf_pal_v37_buffer == NULL) {
            frf_pal_v37_buffer_size = 0;
            frf_pal_v37_bpr = 0;
            frf_pal_v37_height = 0;
            return 0;
        }

        memset(frf_pal_v37_buffer, 0, size);
        frf_pal_v37_buffer_size = size;
        frf_pal_v37_bpr = width;
        frf_pal_v37_height = height;
    }

    return 1;
}

static void frf_pal_v37_apply_palette(
    struct video_canvas_s *canvas)
{
    struct Window *window;
    struct Screen *screen;
    ULONG rgbtab[1 + (FRF_PAL_V37_MAX_PENS * 3) + 1];
    unsigned int count;
    unsigned int index;
    unsigned int source_index;

    if (canvas == NULL
        || canvas->os == NULL
        || canvas->os->window == NULL
        || canvas->palette == NULL
        || canvas->videoconfig == NULL) {
        return;
    }

    window = canvas->os->window;
    screen = window->WScreen;

    if (screen == NULL) {
        return;
    }

    count = canvas->palette->num_entries;
    if (count > FRF_PAL_V37_MAX_PENS) {
        count = FRF_PAL_V37_MAX_PENS;
    }

    for (index = 0; index < FRF_PAL_V37_MAX_PENS; index++) {
        video_render_setphysicalcolor(
            canvas->videoconfig,
            index,
            index,
            8);
    }

    rgbtab[0] = ((ULONG)FRF_PAL_V37_MAX_PENS << 16) | 0;

    for (index = 0; index < FRF_PAL_V37_MAX_PENS; index++) {
        if (count == 0) {
            rgbtab[1 + index * 3 + 0] = 0;
            rgbtab[1 + index * 3 + 1] = 0;
            rgbtab[1 + index * 3 + 2] = 0;
            continue;
        }

        source_index = index;
        if (source_index >= count) {
            source_index = count - 1;
        }

        rgbtab[1 + index * 3 + 0] =
            ((ULONG)canvas->palette->entries[source_index].red)
            * 0x01010101UL;
        rgbtab[1 + index * 3 + 1] =
            ((ULONG)canvas->palette->entries[source_index].green)
            * 0x01010101UL;
        rgbtab[1 + index * 3 + 2] =
            ((ULONG)canvas->palette->entries[source_index].blue)
            * 0x01010101UL;
    }

    rgbtab[1 + (FRF_PAL_V37_MAX_PENS * 3)] = 0;
    LoadRGB32(&screen->ViewPort, rgbtab);

    if (frf_pal_v37_palette_window != window
        || frf_pal_v37_palette_screen != screen
        || frf_pal_v37_palette != canvas->palette) {
        MakeScreen(screen);
        RethinkDisplay();
        ScreenToFront(screen);
        WindowToFront(window);
        ActivateWindow(window);

        frf_pal_v37_palette_window = window;
        frf_pal_v37_palette_screen = screen;
        frf_pal_v37_palette = canvas->palette;
    }
}

static int frf_pal_v37_bitmap_layout(
    struct BitMap *bitmap,
    ULONG *row_stride,
    int *is_interleaved)
{
    ULONG bytes_per_row;
    ULONG plane_span;
    int plane;
    int other;
    int exact_interleaved;

    if (bitmap == NULL
        || row_stride == NULL
        || is_interleaved == NULL
        || bitmap->Depth != FRF_PAL_V37_PLANES
        || bitmap->Rows == 0
        || bitmap->BytesPerRow < 2) {
        return 0;
    }

    for (plane = 0; plane < FRF_PAL_V37_PLANES; plane++) {
        if (bitmap->Planes[plane] == NULL) {
            return 0;
        }
    }

    bytes_per_row = (ULONG)bitmap->BytesPerRow;
    exact_interleaved = 1;

    for (plane = 1; plane < FRF_PAL_V37_PLANES; plane++) {
        if ((ULONG)bitmap->Planes[plane]
            != (ULONG)bitmap->Planes[0]
                + ((ULONG)plane * bytes_per_row)) {
            exact_interleaved = 0;
            break;
        }
    }

    if (exact_interleaved) {
        *row_stride =
            bytes_per_row * (ULONG)bitmap->Depth;
        *is_interleaved = 1;
        return 1;
    }

    /*
     * Standard planar BitMaps use independent non-overlapping allocations.
     * Reject unknown overlapping layouts rather than corrupting display RAM.
     */
    plane_span = bytes_per_row * (ULONG)bitmap->Rows;

    for (plane = 0; plane < FRF_PAL_V37_PLANES; plane++) {
        ULONG plane_address;

        plane_address = (ULONG)bitmap->Planes[plane];

        for (other = plane + 1;
             other < FRF_PAL_V37_PLANES;
             other++) {
            ULONG other_address;

            other_address = (ULONG)bitmap->Planes[other];

            if (plane_address < other_address + plane_span
                && other_address < plane_address + plane_span) {
                return 0;
            }
        }
    }

    *row_stride = bytes_per_row;
    *is_interleaved = 0;
    return 1;
}

static int frf_pal_v37_prepare_shadow(
    struct BitMap *bitmap,
    ULONG row_stride,
    int is_interleaved)
{
    ULONG words_per_row;
    ULONG rows;
    ULONG required_words;

    if (bitmap == NULL || bitmap->Depth != FRF_PAL_V37_PLANES) {
        return 0;
    }

    words_per_row = ((ULONG)bitmap->BytesPerRow) >> 1;
    rows = (ULONG)bitmap->Rows;

    if (words_per_row == 0
        || rows == 0
        || words_per_row > FRF_PAL_V37_MAX_WORDS) {
        return 0;
    }

    required_words =
        words_per_row * rows * FRF_PAL_V37_PLANES;

    if (frf_pal_v37_planar_shadow == NULL
        || frf_pal_v37_shadow_words < required_words
        || frf_pal_v37_shadow_bitmap != bitmap
        || frf_pal_v37_shadow_row_stride != row_stride
        || frf_pal_v37_shadow_words_per_row != words_per_row
        || frf_pal_v37_shadow_rows != rows
        || frf_pal_v37_shadow_interleaved != is_interleaved) {
        if (frf_pal_v37_planar_shadow != NULL) {
            lib_free(frf_pal_v37_planar_shadow);
            frf_pal_v37_planar_shadow = NULL;
        }

        frf_pal_v37_planar_shadow =
            (UWORD *)lib_malloc(required_words * sizeof(UWORD));

        if (frf_pal_v37_planar_shadow == NULL) {
            frf_pal_v37_shadow_words = 0;
            frf_pal_v37_shadow_bitmap = NULL;
            return 0;
        }

        memset(
            frf_pal_v37_planar_shadow,
            0,
            required_words * sizeof(UWORD));

        frf_pal_v37_shadow_words = required_words;
        frf_pal_v37_shadow_bitmap = bitmap;
        frf_pal_v37_shadow_row_stride = row_stride;
        frf_pal_v37_shadow_words_per_row = words_per_row;
        frf_pal_v37_shadow_rows = rows;
        frf_pal_v37_shadow_interleaved = is_interleaved;
    }

    return 1;
}

static void frf_pal_v37_pack_words(
    const UBYTE *source,
    int words)
{
    int word_index;

    for (word_index = 0; word_index < words; word_index++) {
        const UBYTE *pixels;
        UWORD word0;
        UWORD word1;
        UWORD word2;
        UWORD word3;
        int pair;

        pixels = source + (word_index << 4);
        word0 = 0;
        word1 = 0;
        word2 = 0;
        word3 = 0;

        /*
         * Two pixels are merged per iteration. After eight shifts each UWORD
         * contains the Amiga high-bit-first representation of 16 pixels.
         */
        for (pair = 0; pair < 8; pair++) {
            UBYTE colour0;
            UBYTE colour1;

            colour0 = pixels[pair << 1] & 0x0f;
            colour1 = pixels[(pair << 1) + 1] & 0x0f;

            word0 = (UWORD)(
                (word0 << 2)
                | ((colour0 & 0x01) << 1)
                | (colour1 & 0x01));

            word1 = (UWORD)(
                (word1 << 2)
                | (colour0 & 0x02)
                | ((colour1 & 0x02) >> 1));

            word2 = (UWORD)(
                (word2 << 2)
                | ((colour0 & 0x04) >> 1)
                | ((colour1 & 0x04) >> 2));

            word3 = (UWORD)(
                (word3 << 2)
                | ((colour0 & 0x08) >> 2)
                | ((colour1 & 0x08) >> 3));
        }

        frf_pal_v37_packed[0][word_index] = word0;
        frf_pal_v37_packed[1][word_index] = word1;
        frf_pal_v37_packed[2][word_index] = word2;
        frf_pal_v37_packed[3][word_index] = word3;
    }
}

static void frf_pal_v37_copy_word_run(
    UWORD *destination,
    const UWORD *source,
    int words)
{
    if (words <= 0) {
        return;
    }

    if (words == 1) {
        destination[0] = source[0];
    } else if (words == 2) {
        destination[0] = source[0];
        destination[1] = source[1];
    } else {
        CopyMem(
            (APTR)source,
            (APTR)destination,
            (ULONG)words * sizeof(UWORD));
    }
}

static int frf_pal_v37_publish_bulk(
    struct Window *window,
    int source_x,
    int source_y,
    int destination_x,
    int destination_y,
    int width,
    int height)
{
    struct BitMap *bitmap;
    ULONG row_stride;
    ULONG words_per_row;
    int is_interleaved;
    int base_delta;
    int left_extra;
    int aligned_source_x;
    int aligned_destination_x;
    int aligned_right;
    int aligned_width;
    int word_count;
    int destination_word;
    int line;
    int plane;

    /*
     * A successful no-op prevents the graphics.library run fallback from
     * drawing through an active Intuition menu.
     */
    if (frf_pal_v39_menu_active) {
        return 1;
    }

    if (window == NULL
        || window->RPort == NULL
        || window->RPort->BitMap == NULL
        || frf_pal_v37_buffer == NULL
        || width <= 0
        || height <= 0) {
        return 0;
    }

    bitmap = window->RPort->BitMap;

    if (!frf_pal_v37_bitmap_layout(
            bitmap,
            &row_stride,
            &is_interleaved)) {
        return 0;
    }

    /*
     * MAME's native path aligns the destination before C2P. Here the physical
     * image base must be word aligned; each dirty update is expanded to the
     * surrounding 16-pixel words using the persistent indexed render buffer.
     */
    base_delta = destination_x - source_x;
    if ((base_delta & 15) != 0) {
        return 0;
    }

    left_extra = source_x & 15;
    aligned_source_x = source_x - left_extra;
    aligned_destination_x = destination_x - left_extra;

    aligned_right = (source_x + width + 15) & ~15;

    if (aligned_source_x < 0
        || aligned_destination_x < 0
        || aligned_right > (int)frf_pal_v37_bpr) {
        return 0;
    }

    aligned_width = aligned_right - aligned_source_x;
    word_count = aligned_width >> 4;

    if (word_count <= 0
        || word_count > FRF_PAL_V37_MAX_WORDS
        || source_y < 0
        || destination_y < 0
        || (ULONG)(source_y + height) > frf_pal_v37_height
        || destination_y + height > (int)bitmap->Rows) {
        return 0;
    }

    words_per_row = ((ULONG)bitmap->BytesPerRow) >> 1;
    destination_word = aligned_destination_x >> 4;

    if (destination_word < 0
        || (ULONG)(destination_word + word_count)
            > words_per_row) {
        return 0;
    }

    if (!frf_pal_v37_prepare_shadow(
            bitmap,
            row_stride,
            is_interleaved)) {
        return 0;
    }

    /*
     * Finish any graphics.library blit before direct CPU writes touch the
     * visible planar BitMap.
     */
    WaitBlit();

    for (line = 0; line < height; line++) {
        const UBYTE *source_line;

        source_line = frf_pal_v37_buffer
            + ((ULONG)(source_y + line) * frf_pal_v37_bpr)
            + aligned_source_x;

        frf_pal_v37_pack_words(source_line, word_count);

        for (plane = 0;
             plane < FRF_PAL_V37_PLANES;
             plane++) {
            UWORD *shadow;
            UWORD *destination;
            int word_index;
            int run_start;

            shadow = frf_pal_v37_planar_shadow
                + (
                    (
                        (ULONG)plane
                        * frf_pal_v37_shadow_rows
                        + (ULONG)(destination_y + line)
                    )
                    * frf_pal_v37_shadow_words_per_row
                )
                + destination_word;

            destination =
                (UWORD *)(
                    (UBYTE *)bitmap->Planes[plane]
                    + ((ULONG)(destination_y + line) * row_stride)
                )
                + destination_word;

            run_start = -1;

            for (word_index = 0;
                 word_index < word_count;
                 word_index++) {
                UWORD new_word;

                new_word =
                    frf_pal_v37_packed[plane][word_index];

                if (shadow[word_index] != new_word) {
                    if (run_start < 0) {
                        run_start = word_index;
                    }
                    shadow[word_index] = new_word;
                } else if (run_start >= 0) {
                    frf_pal_v37_copy_word_run(
                        destination + run_start,
                        shadow + run_start,
                        word_index - run_start);
                    run_start = -1;
                }
            }

            if (run_start >= 0) {
                frf_pal_v37_copy_word_run(
                    destination + run_start,
                    shadow + run_start,
                    word_count - run_start);
            }
        }
    }

    return 1;
}

static void frf_pal_v37_draw_runs_fallback(
    struct Window *window,
    int source_x,
    int source_y,
    int destination_x,
    int destination_y,
    int width,
    int height)
{
    struct RastPort *rastport;
    int line;
    int pixel;

    if (window == NULL
        || window->RPort == NULL
        || frf_pal_v37_buffer == NULL
        || width <= 0
        || height <= 0) {
        return;
    }

    rastport = window->RPort;

    for (line = 0; line < height; line++) {
        UBYTE *source;
        int run_start;
        UBYTE run_pen;

        source = frf_pal_v37_buffer
            + ((ULONG)(source_y + line) * frf_pal_v37_bpr)
            + source_x;

        run_start = 0;
        run_pen = source[0] & 0x0f;

        for (pixel = 1; pixel <= width; pixel++) {
            UBYTE pen;

            pen = pixel < width
                ? (source[pixel] & 0x0f)
                : (UBYTE)0xff;

            if (pixel == width || pen != run_pen) {
                SetAPen(rastport, (ULONG)run_pen);
                RectFill(
                    rastport,
                    destination_x + run_start,
                    destination_y + line,
                    destination_x + pixel - 1,
                    destination_y + line);

                if (pixel < width) {
                    run_start = pixel;
                    run_pen = pen;
                }
            }
        }
    }
}

static int frf_pal_v37_refresh(
    struct video_canvas_s *canvas,
    unsigned int xs,
    unsigned int ys,
    unsigned int xi,
    unsigned int yi,
    unsigned int w,
    unsigned int h)
{
    struct Window *window;
    ULONG physical_width;
    ULONG physical_height;
    int scale_x;
    int scale_y;
    int client_width;
    int client_height;
    int source_x;
    int source_y;
    int destination_x;
    int destination_y;
    int copy_width;
    int copy_height;

    /*
     * MENUVERIFY is replied only after this flag is set, so Intuition cannot
     * begin menu rendering while native direct-planar writes are still active.
     */
    if (frf_pal_v39_menu_active) {
        return 1;
    }

    if (!frf_pal_v37_should_own_refresh(canvas)
        || canvas->draw_buffer == NULL
        || canvas->videoconfig == NULL) {
        return 0;
    }

    window = canvas->os->window;

    /*
     * Preserve V36 native ownership while retaining the stale RTG allocation
     * for a safe return to the working RTG window.
     */
    canvas->os->frf_pal_native_mode = 1;
    canvas->os->pixfmt = 0;
    canvas->os->bpp = 1;
    canvas->waiting_for_resize = 0;
    canvas->depth = 8;
    canvas->use_triple_buffering = 0;

    if (!frf_pal_v37_prepare_buffer(canvas)) {
        return 0;
    }

    canvas->os->bpr = frf_pal_v37_bpr;
    canvas->bytes_per_line = frf_pal_v37_bpr;

    frf_pal_v37_apply_palette(canvas);

    scale_x = canvas->videoconfig->scalex;
    scale_y = canvas->videoconfig->scaley;

    if (scale_x < 1) {
        scale_x = 1;
    }
    if (scale_y < 1) {
        scale_y = 1;
    }

    /*
     * V37 is dispatched before stock video_canvas_refresh() scaling.
     * Apply exactly one local scaling pass, as in V36.
     */
    xi *= scale_x;
    w *= scale_x;
    yi *= scale_y;
    h *= scale_y;

    physical_width =
        canvas->draw_buffer->canvas_physical_width;
    physical_height =
        canvas->draw_buffer->canvas_physical_height;

    if (w == 0
        || h == 0
        || xi >= physical_width
        || yi >= physical_height) {
        return 0;
    }

    if (w > physical_width - xi) {
        w = physical_width - xi;
    }
    if (h > physical_height - yi) {
        h = physical_height - yi;
    }

    video_canvas_render(
        canvas,
        frf_pal_v37_buffer,
        w,
        h,
        xs,
        ys,
        xi,
        yi,
        frf_pal_v37_bpr,
        8);

    source_x = (int)xi;
    source_y = (int)yi;
    copy_width = (int)w;
    copy_height = (int)h;

    client_width =
        window->Width
        - window->BorderLeft
        - window->BorderRight;
    client_height =
        window->Height
        - window->BorderTop
        - window->BorderBottom;

    destination_x =
        window->BorderLeft
        + (int)xi
        + ((client_width - (int)physical_width) / 2);
    destination_y =
        window->BorderTop
        + (int)yi
        + ((client_height - (int)physical_height) / 2);

    if (destination_x < window->BorderLeft) {
        int cut;

        cut = window->BorderLeft - destination_x;
        source_x += cut;
        copy_width -= cut;
        destination_x = window->BorderLeft;
    }

    if (destination_y < window->BorderTop) {
        int cut;

        cut = window->BorderTop - destination_y;
        source_y += cut;
        copy_height -= cut;
        destination_y = window->BorderTop;
    }

    if (destination_x + copy_width
        > window->BorderLeft + client_width) {
        copy_width =
            window->BorderLeft
            + client_width
            - destination_x;
    }

    if (destination_y + copy_height
        > window->BorderTop + client_height) {
        copy_height =
            window->BorderTop
            + client_height
            - destination_y;
    }

    if (copy_width <= 0
        || copy_height <= 0
        || source_x < 0
        || source_y < 0
        || (ULONG)(source_x + copy_width)
            > frf_pal_v37_bpr
        || (ULONG)(source_y + copy_height)
            > frf_pal_v37_height) {
        return 0;
    }

    if (!frf_pal_v37_publish_bulk(
            window,
            source_x,
            source_y,
            destination_x,
            destination_y,
            copy_width,
            copy_height)) {
        frf_pal_v37_draw_runs_fallback(
            window,
            source_x,
            source_y,
            destination_x,
            destination_y,
            copy_width,
            copy_height);
    }

    return 1;
}

static struct video_canvas_s *reopen(struct video_canvas_s *canvas, int width, int height)
{
    int amiga_width, amiga_height, fullscreen, fullscreenstatusbar, overlay;
    ULONG dispid = INVALID_ID; /* stfu compiler */
    int nofullscreen = 0;
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
    int nooverlay = 0;
#endif

    if (canvas == NULL) {
        return NULL;
    }

    fullscreen = IsFullscreenEnabled();
    fullscreenstatusbar = IsFullscreenStatusbarEnabled();
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
    overlay = IsVideoOverlayEnabled();
#else
    overlay = 0;
#endif

    /* Request the new mode before closing the working RTG/window output. */
    if (fullscreen && frf_pal_asl_request) {
        frf_pal_asl_request = 0;
        frf_pal_asl_valid = 0;

        if (!frf_pal_pick_asl_mode(
                width,
                height + (fullscreenstatusbar
                          ? statusbar_get_status_height() : 0))) {
            resources_set_value("FullscreenEnabled", (resource_value_t)0);
            fullscreen = 0;
        }
    }

    /* Do not carry a previously selected native/ASL mode into windowed output. */
    if (!fullscreen) {
        frf_pal_asl_request = 0;
        frf_pal_asl_valid = 0;
        frf_pal_native = 0;
    }

    /* if there is no change, don't bother with anything else */
    if ((canvas->current_fullscreen == fullscreen || width == 0 || height == 0) &&
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
        canvas->current_overlay == overlay &&
#endif
        canvas->os->visible_width == width && canvas->os->visible_height == height) {
            return canvas;
    }

    /* if changing to/from fullscreen, close screen and window */
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
    if ((canvas->current_fullscreen != fullscreen) || fullscreen || canvas->current_overlay != overlay || overlay) {
#else
    if ((canvas->current_fullscreen != fullscreen) || fullscreen) {
#endif
        pointer_show();
        ui_menu_destroy(canvas);
        statusbar_destroy(canvas);

#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
        if (canvas->os->vlayer_handle) {
            DetachVLayer(canvas->os->vlayer_handle);
            DeleteVLayerHandle(canvas->os->vlayer_handle);
            canvas->os->vlayer_handle = NULL;
        }
#endif
        closecanvaswindow(canvas);
        if (canvas->os->screen != NULL) {
            CloseScreen(canvas->os->screen);
            canvas->os->screen = NULL;
        }
    }

    /* free bitmap */
    if (canvas->os->window_bitmap != NULL) {
        FreeBitMap(canvas->os->window_bitmap);
        canvas->os->window_bitmap = NULL;
    }
    frf_pal_free_native_buffer(canvas);

#ifdef AMIGA_AROS
    if (unlockable_buffer != NULL) {
        lib_free(unlockable_buffer);
        unlockable_buffer = NULL;
    }

    if (renderRPort != NULL) {
        FreeRastPort(renderRPort);
        renderRPort = NULL;
    }
    if (backRPort != NULL) {
        FreeRastPort(backRPort);
        backRPort = NULL;
    }
#endif

    /* try to get screenmode to use */
    if (fullscreen && frf_pal_asl_valid) {
        dispid = frf_pal_displayid;
    } else if (fullscreen) {
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
        static const UBYTE depths_lowend[] = { 16, 15, 0 };
#ifdef AMIGA_MORPHOS
        static const UBYTE depths_highend[] = { 32, 24, 16, 15, 0 };
#endif
        const UBYTE *depths;
        int i;

        depths = depths_lowend;
#ifdef AMIGA_MORPHOS
        if (SysBase->MaxLocMem == NULL) {
            depths = depths_highend;
        }
#endif

        for (i = 0; depths[i]; i++) {
            dispid = BestCModeIDTags(CYBRBIDTG_Depth, depths[i],
                                     CYBRBIDTG_NominalWidth, width,
                                     CYBRBIDTG_NominalHeight, height + (fullscreenstatusbar ? statusbar_get_status_height() : 0),
                                     TAG_DONE);
            if (dispid != INVALID_ID) {
                break;
            }
        }
#else

        unsigned long cmodels = RGBFF_R5G5B5 | RGBFF_R5G6B5 | RGBFF_R5G5B5PC | RGBFF_R5G6B5PC;
        dispid = p96BestModeIDTags(P96BIDTAG_NominalWidth, width,
        /* FIXME: only ask for statusbar height if it should be shown */
                                   P96BIDTAG_NominalHeight, height + (fullscreenstatusbar ? statusbar_get_status_height() : 0),
                                   P96BIDTAG_FormatsAllowed, cmodels,
                                   TAG_DONE);
#endif

        if (dispid == INVALID_ID) {
            nofullscreen = 1;
        }
    }

    /* if fullscreen, open the screen */
    if (fullscreen && nofullscreen == 0) {
        static const UWORD penarray[1] = { ~0 };
        int amiga_depth;
        if (frf_pal_asl_valid) {
            amiga_width = frf_pal_width;
            amiga_height = frf_pal_height;
            amiga_depth = frf_pal_depth;
        } else {
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
            amiga_width = GetCyberIDAttr(CYBRIDATTR_WIDTH, dispid);
            amiga_height = GetCyberIDAttr(CYBRIDATTR_HEIGHT, dispid);
            amiga_depth = GetCyberIDAttr(CYBRIDATTR_DEPTH, dispid);
#else
            amiga_width = p96GetModeIDAttr(dispid, P96IDA_WIDTH);
            amiga_height = p96GetModeIDAttr(dispid, P96IDA_HEIGHT);
#ifdef AMIGA_OS4
            amiga_depth = p96GetModeIDAttr(dispid, P96IDA_DEPTH);
#else
            amiga_depth = 8;
#endif
#endif
        }

        /* open screen */
        if (frf_pal_asl_valid && frf_pal_native) {
            canvas->os->screen = OpenScreenTags(NULL,
                                                SA_Type, CUSTOMSCREEN,
                                                SA_DisplayID, dispid,
                                                SA_Width, amiga_width,
                                                SA_Height, amiga_height,
                                                SA_Depth, amiga_depth,
                                                SA_Overscan, frf_pal_overscan,
                                                SA_AutoScroll, frf_pal_autoscroll,
                                                SA_Quiet, TRUE,
                                                SA_ShowTitle, FALSE,
                                                SA_Title,
                                                (ULONG)"VICE Native PAL/RGB",
                                                SA_Draggable, TRUE,
                                                SA_Interleaved, TRUE,
                                                TAG_DONE);
        } else {
            canvas->os->screen = OpenScreenTags(NULL,
                                                SA_Width, amiga_width,
                                                SA_Height, amiga_height,
                                                SA_Depth, amiga_depth,
                                                SA_Quiet, TRUE,
                                                SA_ShowTitle, FALSE,
                                                SA_Type, CUSTOMSCREEN,
                                                SA_DisplayID, dispid,
                                                SA_Title, (ULONG)"VICE",
                                                SA_Pens, (ULONG)penarray,
                                                SA_SharePens, TRUE,
                                                SA_FullPalette, TRUE,
                                                TAG_DONE);
        }

        /* could the screen be opened? */
        if (canvas->os->screen == NULL) {
            return NULL;
        }

        /* open window */
        canvas->os->window = OpenWindowTags(NULL,
                                            WA_CustomScreen, (ULONG)canvas->os->screen,
                                            WA_Width, canvas->os->screen->Width,
                                            WA_Height, canvas->os->screen->Height,
                                            WA_IDCMP, IDCMP_RAWKEY | IDCMP_MENUPICK | IDCMP_MENUVERIFY,
                                            WA_Backdrop, (frf_pal_asl_valid && frf_pal_native) ? TRUE : FALSE,
                                            WA_Borderless, TRUE,
                                            WA_Activate, TRUE,
                                            WA_NewLookMenus, TRUE,
                                            TAG_DONE);

        if (canvas->os->window == NULL) {
            CloseScreen(canvas->os->screen);
            canvas->os->screen = NULL;
            return NULL;
        }

        if (frf_pal_asl_valid && frf_pal_native) {
            SetAPen(&canvas->os->screen->RastPort, 0);
            RectFill(&canvas->os->screen->RastPort,
                     0, 0,
                     canvas->os->screen->Width - 1,
                     canvas->os->screen->Height - 1);
        } else {
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
            FillPixelArray(&canvas->os->screen->RastPort, 0, 0,
                           canvas->os->screen->Width,
                           canvas->os->screen->Height, 0);
#else
            p96RectFill(&canvas->os->screen->RastPort, 0, 0,
                        canvas->os->screen->Width,
                        canvas->os->screen->Height, 0);
#endif
        }

        /* V30: V29 colour-bar hold removed; real frame handoff owns output. */
        pointer_set_default(POINTER_HIDE);
        pointer_hide();

        canvas->os->visible_width = canvas->os->screen->Width;
        canvas->os->visible_height = canvas->os->screen->Height;
        if (fullscreenstatusbar) {
            canvas->os->visible_height -= statusbar_get_status_height();
        }
    } else {
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
reopenwindow:
#endif
        /* if window already is open, just resize it, otherwise, open it */
        if (canvas->os->window != NULL) {
            ChangeWindowBox(canvas->os->window,
                            canvas->os->window->LeftEdge,
                            canvas->os->window->TopEdge,
                            canvas->os->window->BorderLeft + width + canvas->os->window->BorderRight,
                            canvas->os->window->BorderTop + height + statusbar_get_status_height() + canvas->os->window->BorderBottom);
            canvas->waiting_for_resize = 1;
        } else {
            int statusheight = statusbar_get_status_height();
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
            int have_resize = overlay && CGXVideoBase && nooverlay == 0;
#else
            const int have_resize = FALSE;
#endif
            canvas->os->window = OpenWindowTags(NULL,
                                                WA_Title, (ULONG)canvas->os->window_name,
                                                WA_Flags, WFLG_NOCAREREFRESH | WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET,
                                                WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_RAWKEY | IDCMP_SIZEVERIFY | IDCMP_CHANGEWINDOW | IDCMP_MENUPICK | IDCMP_MENUVERIFY,
                                                WA_Left, canvas->window_left,
                                                WA_Top, canvas->window_top,
                                                WA_InnerWidth, width,
                                                WA_InnerHeight, height + statusheight,
                                                WA_Activate, TRUE,
                                                WA_NewLookMenus, TRUE,
                                                WA_AutoAdjust, TRUE,
                                                have_resize ? WA_MinWidth : TAG_IGNORE, 64 * 2,
                                                have_resize ? WA_MinHeight : TAG_IGNORE, 48 * 2 + statusheight,
                                                have_resize ? WA_MaxWidth : TAG_IGNORE, ~0,
                                                have_resize ? WA_MaxHeight : TAG_IGNORE, ~0,
                                                have_resize ? WA_SizeGadget : TAG_IGNORE, TRUE,
                                                TAG_DONE);

            if (canvas->os->window == NULL) {
                return NULL;
            }
        }

        pointer_set_default(POINTER_SHOW);
        pointer_show();

        canvas->os->visible_width = width;
        canvas->os->visible_height = height;
    }

#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)

    if (overlay && CGXVideoBase && nooverlay == 0) {
        static const struct {
            int      srcfmt;
            int      pixfmt;
            fourcc_t yuvfmt;
        } vlayer_formats[] = {
            { SRCFMT_YCbCr16, 0, {FOURCC_YUY2} },
            { SRCFMT_RGB16, PIXFMT_RGB16PC, {0} },
            { SRCFMT_RGB15, PIXFMT_RGB15PC, {0} },
            { -1, }
        };

        int i;

        for (i = 0; vlayer_formats[i].srcfmt != -1; i++) {
            canvas->os->vlayer_handle = CreateVLayerHandleTags(canvas->os->window->WScreen,
                                                               VOA_SrcType, vlayer_formats[i].srcfmt,
                                                               VOA_SrcWidth, width,
                                                               VOA_SrcHeight, height,
                                                               VOA_UseColorKey, TRUE,
                                                               TAG_DONE);
            if (canvas->os->vlayer_handle) {
                break;
            }
        }

        if (canvas->os->vlayer_handle != NULL) {
            int statusheight = canvas->os->screen == NULL || fullscreenstatusbar ? statusbar_get_status_height() : 0;

            canvas->os->pixfmt = vlayer_formats[i].pixfmt;
            canvas->vlayer_yuvfmt.id = vlayer_formats[i].yuvfmt.id;

            if (AttachVLayerTags(canvas->os->vlayer_handle, canvas->os->window, VOA_BottomIndent, statusheight, TAG_DONE) == 0) {
                struct Window *window = canvas->os->window;

                canvas->vlayer_image.width = width * 2;
                canvas->vlayer_image.height = height;
                canvas->vlayer_image.data_size = width * 2 * height * 2;
                canvas->vlayer_image.num_planes = 0;
                canvas->vlayer_image.pitches = canvas->vlayer_pitches;
                canvas->vlayer_pitches[0] = width * 2;
                canvas->vlayer_image.offsets = canvas->vlayer_offsets;
                canvas->vlayer_offsets[0] = 0;
                canvas->vlayer_image.data = NULL;

                canvas->os->bpr = GetVLayerAttr(canvas->os->vlayer_handle, VOA_Modulo);
                canvas->os->bpp = 2;

                canvas->os->vlayer_colorkey = GetVLayerAttr(canvas->os->vlayer_handle, VOA_ColorKey);
                if ((LONG)canvas->os->vlayer_colorkey != -1) {
                    FillPixelArray(window->RPort,
                                   window->BorderLeft, window->BorderTop,
                                   window->Width - window->BorderLeft - window->BorderRight,
                                   window->Height - window->BorderTop - window->BorderBottom - statusheight,
                                   canvas->os->vlayer_colorkey);
                }
            } else {
                DeleteVLayerHandle(canvas->os->vlayer_handle);
                canvas->os->vlayer_handle = NULL;
            }
        }

        if (canvas->os->vlayer_handle == NULL && canvas->os->screen == NULL) {
            /* overlay creation failed for some reason, close window and reopen without resize */
            closecanvaswindow(canvas);
            nooverlay = 1;
            goto reopenwindow;
        }
    }
#endif

    if (canvas->os->screen == NULL || fullscreenstatusbar) {
        statusbar_create(canvas);
    }

#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
    if (canvas->os->vlayer_handle == NULL) {
#endif
        if (fullscreen && frf_pal_asl_valid && frf_pal_native) {
            /* FRF native PAL/RGB CPU buffer. Native screens never enter the
             * P96/Cyber bitmap-lock path. */
            canvas->os->frf_pal_native_mode = 1;
            canvas->os->frf_pal_native_bpr = (ULONG)width;
            canvas->os->frf_pal_native_height = (ULONG)height;
            canvas->os->frf_pal_native_chunky =
                lib_malloc((ULONG)width * (ULONG)height);

            if (canvas->os->frf_pal_native_chunky == NULL) {
                closecanvaswindow(canvas);
                if (canvas->os->screen != NULL) {
                    CloseScreen(canvas->os->screen);
                    canvas->os->screen = NULL;
                }
                return NULL;
            }

            memset(canvas->os->frf_pal_native_chunky, 0,
                   (ULONG)width * (ULONG)height);
            canvas->os->window_bitmap = NULL;
            canvas->os->pixfmt = 0;
            canvas->os->bpr = (ULONG)width;
            canvas->os->bpp = 1;
        } else {
            canvas->os->window_bitmap = AllocBitMap(width, height,
                                                    GetBitMapAttr(canvas->os->window->RPort->BitMap, BMA_DEPTH),
                                                    BMF_CLEAR | BMF_INTERLEAVED | BMF_MINPLANES,
                                                    canvas->os->window->RPort->BitMap);
            if (canvas->os->window_bitmap == NULL) {
                closecanvaswindow(canvas);
                if (canvas->os->screen != NULL) {
                    CloseScreen(canvas->os->screen);
                    canvas->os->screen = NULL;
                }
                return NULL;
            }

    #ifdef HAVE_PROTO_CYBERGRAPHICS_H
            /* make sure we didn't get some incompatible bitmap */
            if (GetCyberMapAttr(canvas->os->window_bitmap, CYBRMATTR_ISCYBERGFX) == 0) {
                FreeBitMap(canvas->os->window_bitmap);
                canvas->os->window_bitmap = NULL;
                closecanvaswindow(canvas);
                if (canvas->os->screen != NULL) {
                    CloseScreen(canvas->os->screen);
                    canvas->os->screen = NULL;
                }
                return NULL;
            }

    #ifdef AMIGA_AROS
            unlockable_buffer = lib_malloc(width * 2 * height * 2 * 4);

            renderRPort = CloneRastPort(canvas->os->window->RPort);

            backRPort = CreateRastPort();
            backRPort->BitMap = canvas->os->window_bitmap;
    #endif

            canvas->os->pixfmt = GetCyberMapAttr(canvas->os->window_bitmap, CYBRMATTR_PIXFMT);
            canvas->os->bpr = GetCyberMapAttr(canvas->os->window_bitmap, CYBRMATTR_XMOD);
            canvas->os->bpp = GetCyberMapAttr(canvas->os->window_bitmap, CYBRMATTR_BPPIX);
    #else
            canvas->os->pixfmt = p96GetBitMapAttr(canvas->os->window_bitmap, P96BMA_RGBFORMAT);
            canvas->os->bpr = p96GetBitMapAttr(canvas->os->window_bitmap, P96BMA_BYTESPERROW);
            canvas->os->bpp = p96GetBitMapAttr(canvas->os->window_bitmap, P96BMA_BYTESPERPIXEL);
    #endif
        }
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
    }
#endif

    canvas->draw_buffer->canvas_physical_width = width;
    canvas->draw_buffer->canvas_physical_height = height;
    canvas->depth = (canvas->os->bpp * 8);
    canvas->bytes_per_line = canvas->os->bpr;
    canvas->use_triple_buffering = 0;

    video_canvas_set_palette(canvas, canvas->palette);

    /* refresh */
    video_canvas_refresh_all(canvas);

    /* remember previous state */
    canvas->current_fullscreen = fullscreen;
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
    canvas->current_overlay = overlay;
#endif
    /* make sure we get the possible requesters */
    if (self->pr_WindowPtr == orig_windowptr) {
      self->pr_WindowPtr = canvas->os->window;
    }

    return canvas;
}

struct video_canvas_s *video_canvas_create(struct video_canvas_s *canvas, unsigned int *width, unsigned int *height, int mapped)
{
    int i;

    canvas->next = NULL;
    canvas->os = lib_malloc(sizeof(struct os_s));
    if (canvas->os == NULL) {
      return NULL;
    }
    memset(canvas->os, 0, sizeof(struct os_s));
    for (i = 0; i < 16; i++) {
        canvas->os->pens[i] = -1;
    }

    canvas->os->window_name = lib_stralloc(canvas->viewport->title);
    if (canvas->os->window_name == NULL) {
        lib_free(canvas->os);
        canvas->os = NULL;
        return NULL;
    }

    if (reopen(canvas, *width, *height) == NULL) {
        lib_free(canvas->os->window_name);
        lib_free(canvas->os);
        canvas->os = NULL;
        return NULL;
    }

    if (canvaslist == NULL) {
        canvaslist = canvas;
    } else {
        video_canvas_t *node = canvaslist;
        while (node->next != NULL) {
            node = node->next;
        }
        node->next = canvas;
    }

    return canvas;
}

void video_arch_canvas_init(struct video_canvas_s *canvas)
{
    canvas->os = NULL;
    canvas->video_draw_buffer_callback = NULL;
    canvas->window_left = 100;
    canvas->window_top = 100;
    canvas->current_fullscreen = 0;
    canvas->waiting_for_resize = 0;
#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
    canvas->current_overlay = 0;
#endif
#ifdef AMIGA_AROS
    canvas->videoconfig->readable = 1; /* it's not direct rendering */
#endif
}


#ifndef HAVE_PROTO_CYBERGRAPHICS_H
/* FRF_WORKING_P96_CPU_BUFFER
 * Launch-freeze fix used by the previously working build:
 * never lock and write directly into the P96 destination bitmap.
 * Render a complete 16-bit RGB565 frame into ordinary public CPU memory,
 * then publish it with p96WritePixelArray().
 */
char frf_working_p96_cpu_buffer_marker[] = "FRF_WORKING_P96_CPU_BUFFER";
static UBYTE *frf_p96_render_buffer = NULL;
static ULONG frf_p96_render_buffer_size = 0;

static int frf_p96_prepare_cpu_render(struct video_canvas_s *canvas,
                                      struct RenderInfo *ri)
{
    ULONG width;
    ULONG height;
    ULONG bytes_per_row;
    ULONG needed;

    if (canvas == NULL || canvas->draw_buffer == NULL || ri == NULL) {
        return 0;
    }

    width = (ULONG)canvas->draw_buffer->canvas_physical_width;
    height = (ULONG)canvas->draw_buffer->canvas_physical_height;
    if (width == 0 || height == 0) {
        return 0;
    }

    /*
     * FRF_P96_FIXED_RGB565_SOURCE:
     * VICE renders a fixed 16-bit big-endian RGB565 source image.
     * p96WritePixelArray converts it to the real RTG destination format.
     */
    bytes_per_row = width * 2UL;
    needed = bytes_per_row * height;

    if (needed == 0 || bytes_per_row > 32767UL) {
        return 0;
    }

    if (frf_p96_render_buffer == NULL ||
        frf_p96_render_buffer_size < needed) {
        if (frf_p96_render_buffer != NULL) {
            FreeVec(frf_p96_render_buffer);
            frf_p96_render_buffer = NULL;
            frf_p96_render_buffer_size = 0;
        }

        frf_p96_render_buffer = (UBYTE *)AllocVec(
            needed + 64UL, MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR);
        if (frf_p96_render_buffer == NULL) {
            return 0;
        }
        frf_p96_render_buffer_size = needed;
    }

    ri->Memory = (APTR)frf_p96_render_buffer;
    ri->BytesPerRow = (WORD)bytes_per_row;
    ri->RGBFormat = RGBFB_R5G6B5;
    return 1;
}
#endif

void video_canvas_refresh(struct video_canvas_s *canvas, unsigned int xs, unsigned int ys, unsigned int xi, unsigned int yi, unsigned int w, unsigned int h)
{

    /* FRF V37: MAME-style bulk C2P owns native refresh before all stock guards. */
    if (frf_pal_v37_should_own_refresh(canvas)) {
        frf_pal_v37_refresh(
            canvas,
            xs,
            ys,
            xi,
            yi,
            w,
            h);
        return;
    }

#ifndef HAVE_PROTO_CYBERGRAPHICS_H
    /*
     * FRF_MAY17_NATIVE_DIRTY_FASTPATH_V2:
     * Render only the requested dirty rectangle into a CPU buffer which uses
     * the destination P96 bitmap's native pixel format. p96WritePixelArray()
     * therefore copies without the later forced-RGB565 conversion path.
     */
    {
        struct Window *frf_window;
        struct RenderInfo frf_ri;
        ULONG frf_bpp;
        ULONG frf_bpr;
        ULONG frf_depth;
        ULONG frf_needed;
        unsigned int frf_phys_w;
        unsigned int frf_phys_h;
        unsigned int frf_src_x;
        unsigned int frf_src_y;
        int frf_dx;
        int frf_dy;

        if (canvas == NULL ||
            canvas->os == NULL ||
            canvas->os->window == NULL ||
            canvas->os->window->RPort == NULL ||
            canvas->draw_buffer == NULL ||
            canvas->videoconfig == NULL) {
            return;
        }

        frf_window = canvas->os->window;
        frf_phys_w = canvas->draw_buffer->canvas_physical_width;
        frf_phys_h = canvas->draw_buffer->canvas_physical_height;

        if (frf_phys_w == 0 || frf_phys_h == 0) {
            return;
        }

        frf_bpp = (ULONG)canvas->os->bpp;
        if (frf_bpp == 0 || frf_bpp > 4) {
            frf_bpp = (ULONG)(canvas->depth / 8);
        }
        if (frf_bpp == 0 || frf_bpp > 4) {
            return;
        }

        frf_bpr = (ULONG)canvas->bytes_per_line;
        if (frf_bpr < ((ULONG)frf_phys_w * frf_bpp)) {
            frf_bpr = (ULONG)frf_phys_w * frf_bpp;
        }
        if (frf_bpr == 0 || frf_bpr > 32767UL) {
            return;
        }

        frf_depth = (ULONG)canvas->depth;
        if (frf_depth == 0) {
            frf_depth = frf_bpp * 8UL;
        }

        if (canvas->os->pixfmt == 0) {
            return;
        }

        frf_needed = frf_bpr * (ULONG)frf_phys_h;
        if (frf_needed == 0) {
            return;
        }

        if (frf_may17_p96_buffer == NULL ||
            frf_may17_p96_buffer_size < frf_needed) {
            if (frf_may17_p96_buffer != NULL) {
                lib_free(frf_may17_p96_buffer);
                frf_may17_p96_buffer = NULL;
                frf_may17_p96_buffer_size = 0;
            }

            frf_may17_p96_buffer = (UBYTE *)lib_malloc(frf_needed);
            if (frf_may17_p96_buffer == NULL) {
                return;
            }
            frf_may17_p96_buffer_size = frf_needed;
        }

        xi *= canvas->videoconfig->scalex;
        w *= canvas->videoconfig->scalex;
        yi *= canvas->videoconfig->scaley;
        h *= canvas->videoconfig->scaley;

        if (w == 0 || h == 0) {
            return;
        }
        if (xi >= frf_phys_w || yi >= frf_phys_h) {
            return;
        }
        if (w > frf_phys_w - xi) {
            w = frf_phys_w - xi;
        }
        if (h > frf_phys_h - yi) {
            h = frf_phys_h - yi;
        }

        video_canvas_render(canvas,
                            frf_may17_p96_buffer,
                            w,
                            h,
                            xs,
                            ys,
                            xi,
                            yi,
                            frf_bpr,
                            frf_depth);

        frf_dx = (int)xi +
            ((canvas->os->visible_width - (int)frf_phys_w) / 2);
        frf_dy = (int)yi +
            ((canvas->os->visible_height - (int)frf_phys_h) / 2);
        frf_src_x = xi;
        frf_src_y = yi;

        if (frf_dx < 0) {
            unsigned int cut = (unsigned int)(-frf_dx);
            if (cut >= w) {
                return;
            }
            frf_src_x += cut;
            w -= cut;
            frf_dx = 0;
        }
        if (frf_dy < 0) {
            unsigned int cut = (unsigned int)(-frf_dy);
            if (cut >= h) {
                return;
            }
            frf_src_y += cut;
            h -= cut;
            frf_dy = 0;
        }

        if (frf_dx >= canvas->os->visible_width ||
            frf_dy >= canvas->os->visible_height) {
            return;
        }
        if (w > (unsigned int)(canvas->os->visible_width - frf_dx)) {
            w = (unsigned int)(canvas->os->visible_width - frf_dx);
        }
        if (h > (unsigned int)(canvas->os->visible_height - frf_dy)) {
            h = (unsigned int)(canvas->os->visible_height - frf_dy);
        }

        if (w == 0 || h == 0 || canvas->waiting_for_resize != 0) {
            return;
        }

        frf_ri.Memory = (APTR)frf_may17_p96_buffer;
        frf_ri.BytesPerRow = (WORD)frf_bpr;
        frf_ri.RGBFormat = canvas->os->pixfmt;

        p96WritePixelArray(&frf_ri,
                           (UWORD)frf_src_x,
                           (UWORD)frf_src_y,
                           frf_window->RPort,
                           (UWORD)(frf_window->BorderLeft + frf_dx),
                           (UWORD)(frf_window->BorderTop + frf_dy),
                           (UWORD)w,
                           (UWORD)h);
        return;
    }
#endif

#ifndef HAVE_PROTO_CYBERGRAPHICS_H
    /* FRF_MAY17_P96_NATIVE_DIRTY_BEGIN */
    {
        struct Window *window;
        struct RenderInfo ri;
        ULONG bpp, bpr, depth, need_size;
        unsigned int phys_w, phys_h, src_x, src_y;
        int dx, dy;

        if (canvas == NULL || canvas->os == NULL ||
            canvas->os->window == NULL ||
            canvas->os->window->RPort == NULL ||
            canvas->draw_buffer == NULL ||
            canvas->videoconfig == NULL) {
            return;
        }

        window = canvas->os->window;
        phys_w = canvas->draw_buffer->canvas_physical_width;
        phys_h = canvas->draw_buffer->canvas_physical_height;
        if (phys_w == 0 || phys_h == 0) return;

        bpp = canvas->os->bpp;
        if (bpp == 0 || bpp > 4) bpp = 2;

        bpr = canvas->bytes_per_line;
        if (bpr < (phys_w * bpp)) bpr = phys_w * bpp;

        depth = canvas->depth;
        if (depth == 0) depth = bpp * 8;

        need_size = bpr * phys_h;
        if (need_size == 0 || bpr > 32767UL) return;

        if (frf_p96_cpu_buffer == NULL ||
            frf_p96_cpu_buffer_size < need_size) {
            if (frf_p96_cpu_buffer != NULL) {
                lib_free(frf_p96_cpu_buffer);
                frf_p96_cpu_buffer = NULL;
                frf_p96_cpu_buffer_size = 0;
            }
            frf_p96_cpu_buffer = (UBYTE *)lib_malloc(need_size);
            if (frf_p96_cpu_buffer == NULL) return;
            frf_p96_cpu_buffer_size = need_size;
        }

        xi *= canvas->videoconfig->scalex;
        w  *= canvas->videoconfig->scalex;
        yi *= canvas->videoconfig->scaley;
        h  *= canvas->videoconfig->scaley;

        if (w == 0 || h == 0) return;
        if (w > phys_w) w = phys_w;
        if (h > phys_h) h = phys_h;

        video_canvas_render(canvas, frf_p96_cpu_buffer,
                            w, h, xs, ys, xi, yi, bpr, depth);

        dx = xi + ((canvas->os->visible_width - (int)phys_w) / 2);
        dy = yi + ((canvas->os->visible_height - (int)phys_h) / 2);
        src_x = xi;
        src_y = yi;

        if (dx < 0) {
            src_x += (unsigned int)(-dx);
            if (w > (unsigned int)(-dx)) w -= (unsigned int)(-dx);
            else return;
            dx = 0;
        }
        if (dy < 0) {
            src_y += (unsigned int)(-dy);
            if (h > (unsigned int)(-dy)) h -= (unsigned int)(-dy);
            else return;
            dy = 0;
        }

        if (w > (unsigned int)canvas->os->visible_width)
            w = canvas->os->visible_width;
        if (h > (unsigned int)canvas->os->visible_height)
            h = canvas->os->visible_height;

        if (w == 0 || h == 0 || canvas->waiting_for_resize != 0) return;

        ri.Memory = (APTR)frf_p96_cpu_buffer;
        ri.BytesPerRow = (WORD)bpr;
        ri.RGBFormat = canvas->os->pixfmt;
        if (ri.RGBFormat == 0) return;

        p96WritePixelArray(&ri,
                           (UWORD)src_x, (UWORD)src_y,
                           window->RPort,
                           (UWORD)(window->BorderLeft + dx),
                           (UWORD)(window->BorderTop + dy),
                           (UWORD)w, (UWORD)h);
        return;
    }
    /* FRF_MAY17_P96_NATIVE_DIRTY_END */
#endif

    int dx, dy, sx, sy;
    ULONG lock;
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
    ULONG cgx_base_addy;
#else
    struct RenderInfo ri;
#endif
#ifdef AMIGA_AROS
    int fullscreen;

    fullscreen = IsFullscreenEnabled();
#endif

#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)

    if (canvas->os->vlayer_handle) {
        if (LockVLayer(canvas->os->vlayer_handle)) {

            cgx_base_addy = GetVLayerAttr(canvas->os->vlayer_handle, VOA_BaseAddress);

            if (canvas->vlayer_yuvfmt.id == 0) {
                /* it's RGB overlay, render to it directly */
                video_canvas_render(canvas, (UBYTE *)cgx_base_addy,
                                    w, h,
                                    xs, ys,
                                    xi, yi,
                                    canvas->bytes_per_line, canvas->depth);
            } else {
                /* everything else is preset at creation time */
                canvas->vlayer_image.data = (APTR)cgx_base_addy;

                if (!canvas->videoconfig->color_tables.updated) { /* update colors as necessary */
                    video_render_update_palette(canvas);
                }
                render_yuv_image(canvas->videoconfig->doublescan, (canvas->videoconfig->filter == VIDEO_FILTER_CRT),
                                 canvas->videoconfig->video_resources.pal_blur * 64 / 1000, canvas->videoconfig->video_resources.pal_scanlineshade * 1024 / 1000,
                                 canvas->vlayer_yuvfmt, &canvas->vlayer_image, canvas->draw_buffer->draw_buffer,
                                 canvas->draw_buffer->draw_buffer_width, canvas->videoconfig,
                                 xs, ys,
                                 w, h,
                                 xi, yi);
            }

            UnlockVLayer(canvas->os->vlayer_handle);
        }
        return;
    }

#endif

    xi *= canvas->videoconfig->scalex;
    w *= canvas->videoconfig->scalex;

    yi *= canvas->videoconfig->scaley;
    h *= canvas->videoconfig->scaley;

#ifdef AMIGA_OS4
    /* this has to be done before locking the bitmap to avoid disk accesses when the lock is held */
    if (!canvas->videoconfig->color_tables.updated) {
        video_color_update_palette(canvas);
    }
#endif

                        #ifdef HAVE_PROTO_CYBERGRAPHICS_H
    if ((lock = (ULONG)LockBitMapTags(canvas->os->window_bitmap, LBMI_BASEADDRESS, (ULONG)&cgx_base_addy, TAG_DONE))) {
#else
    if (!frf_p96_prepare_cpu_render(canvas, &ri)) {
        return;
    }
    { /* FRF_WORKING_P96_CPU_BUFFER_LOCK_REPLACED */
#endif
        video_canvas_render(canvas,
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
                            (UBYTE *)cgx_base_addy,
#else
                            (UBYTE *)ri.Memory,
#endif
                            w, h,
                            xs, ys,
                            xi, yi,
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
                            ri.BytesPerRow, 16);
#else
                            (unsigned int)ri.BytesPerRow, 16);
#endif
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
        UnLockBitMap((APTR)lock);
#else
        /* FRF_WORKING_P96_CPU_BUFFER: ordinary CPU memory needs no unlock */
#endif
    }
#ifdef AMIGA_AROS
    else {
        /* We failed to lock the bitmap - so render into our flatbuffer then WPA that to the backbuffer .. */
        struct Window *window = canvas->os->window;

        video_canvas_render(canvas,
                            (UBYTE *)unlockable_buffer,
                            w, h,
                            xs, ys,
                            xi, yi,
                            canvas->bytes_per_line, canvas->depth);

        WritePixelArray((UBYTE *)unlockable_buffer,
                        0, 0,
                        canvas->bytes_per_line, backRPort,
                        0, 0,
                        window->Width - window->BorderLeft - window->BorderRight,
                        window->Height - window->BorderTop - window->BorderBottom - statusbar_get_status_height(),
                        RECTFMT_RAW);
    }
#endif

    sx = xi;
    sy = yi;
    dx = xi + ((canvas->os->visible_width - (int)canvas->draw_buffer->canvas_physical_width) / 2);
    dy = yi + ((canvas->os->visible_height - (int)canvas->draw_buffer->canvas_physical_height) / 2);
    if (dx < 0) {
        sx += -dx;
        w += dx;
        dx = 0;
    }
    if (dy < 0) {
        sy += -dy;
        h += dy;
        dy = 0;
    }
    if (w > (unsigned int)canvas->os->visible_width) {
        w = canvas->os->visible_width;
    }
    if (h > (unsigned int)canvas->os->visible_height) {
        h = canvas->os->visible_height;
    }

    if (canvas->waiting_for_resize == 0 && w > 0 && h > 0) {
        struct Window *window = canvas->os->window;
#ifdef AMIGA_AROS
        if (fullscreen == 0) {
            int blit_width = canvas->draw_buffer->canvas_physical_width;
            int blit_height = canvas->draw_buffer->canvas_physical_height;
   
            if (blit_width > (window->RPort->Layer->bounds.MaxX - window->RPort->Layer->bounds.MinX)) {
                blit_width = (window->RPort->Layer->bounds.MaxX - window->RPort->Layer->bounds.MinX);
            }
            if (blit_height > (window->RPort->Layer->bounds.MaxY - window->RPort->Layer->bounds.MinY)) {
                blit_height = (window->RPort->Layer->bounds.MaxY - window->RPort->Layer->bounds.MinY);
            }
            ClipBlit(backRPort, 0, 0, renderRPort,
                     window->BorderLeft, window->BorderTop,
                     blit_width, blit_height,
                     0xc0);
        } else
#endif
#ifdef HAVE_PROTO_CYBERGRAPHICS_H
            BltBitMapRastPort(canvas->os->window_bitmap,
                              sx, sy,
                              window->RPort,
                              window->BorderLeft + dx, window->BorderTop + dy,
                              w, h,
                              0xc0);
#else
            /* FRF_WORKING_P96_WRITEPIXELARRAY */
            p96WritePixelArray(&ri,
                               (UWORD)sx,
                               (UWORD)sy,
                               window->RPort,
                               (UWORD)(window->BorderLeft + dx),
                               (UWORD)(window->BorderTop + dy),
                               (UWORD)w,
                               (UWORD)h);
#endif
    }
}

/* dummy */

static int makecol_dummy(int r, int g, int b)
{
    return 0;
}

/* 16bit - BE */

static int makecol_RGB565BE(int r, int g, int b)
{
    int c = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3);
    return c;
}

#ifdef HAVE_PROTO_CYBERGRAPHICS_H
static int makecol_BGR565BE(int r, int g, int b)
{
    int c = ((b & 0xf8) << 8) | ((g & 0xfc) << 3) | ((r & 0xf8) >> 3);
    return c;
}
#endif

static int makecol_RGB555BE(int r, int g, int b)
{
    int c = ((r & 0xf8) << 7) | ((g & 0xf8) << 2) | ((b & 0xf8) >> 3);
    return c;
}

#ifdef HAVE_PROTO_CYBERGRAPHICS_H
static int makecol_BGR555BE(int r, int g, int b)
{
    int c = ((b & 0xf8) << 7) | ((g & 0xf8) << 2) | ((r & 0xf8) >> 3);
    return c;
}
#endif

/* 16bit - LE */

static int makecol_RGB565LE(int r, int g, int b)
{
    int c = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3);
    c = ((c << 8) & 0xff00) | ((c >> 8) & 0x00ff);
    return c;
}

static int makecol_BGR565LE(int r, int g, int b)
{
    int c = ((b & 0xf8) << 8) | ((g & 0xfc) << 3) | ((r & 0xf8) >> 3);
    c = ((c << 8) & 0xff00) | ((c >> 8) & 0x00ff);
    return c;
}

static int makecol_RGB555LE(int r, int g, int b)
{
    int c = ((r & 0xf8) << 7) | ((g & 0xf8) << 2) | ((b & 0xf8) >> 3);
    c = ((c << 8) & 0xff00) | ((c >> 8) & 0x00ff);
    return c;
}

static int makecol_BGR555LE(int r, int g, int b)
{
    int c = ((b & 0xf8) << 7) | ((g & 0xf8) << 2) | ((r & 0xf8) >> 3);
    c = ((c << 8) & 0xff00) | ((c >> 8) & 0x00ff);
    return c;
}

/* 24bit (swapped these as VICE read from LSB, *NOT* MSB when rendering) */

static int makecol_RGB24(int r, int g, int b)
{
    int c = (b << 16) | (g << 8) | r;
    return c;
}

static int makecol_BGR24(int r, int g, int b)
{
    int c = (r << 16) | (g << 8) | b;
    return c;
}

/* 32bit */

static int makecol_ARGB32(int r, int g, int b)
{
    int c = (r << 16) | (g << 8) | b;
    return c;
}

static int makecol_ABGR32(int r, int g, int b)
{
    int c = (b << 16) | (g << 8) | r;
    return c;
}

static int makecol_RGBA32(int r, int g, int b)
{
    int c = (r << 24) | (g << 16) | (b << 8);
    return c;
}

static int makecol_BGRA32(int r, int g, int b)
{
    int c = (b << 24) | (g << 16) | (r << 8);
    return c;
}

#ifdef HAVE_PROTO_CYBERGRAPHICS_H
#if defined(AMIGA_AROS) && !defined(WORDS_BIGENDIAN)
static const struct {
    unsigned long color_format;
    int (*makecol)(int r, int g, int b);
} color_formats[] = {
    { PIXFMT_RGB15, makecol_RGB555LE },
    { PIXFMT_BGR15, makecol_BGR555LE },
    { PIXFMT_RGB15PC, makecol_RGB555BE },
    { PIXFMT_BGR15PC, makecol_BGR555BE },
    { PIXFMT_RGB16, makecol_RGB565LE },
    { PIXFMT_BGR16, makecol_BGR565LE },
    { PIXFMT_RGB16PC, makecol_RGB565BE },
    { PIXFMT_BGR16PC, makecol_BGR565BE },
    { PIXFMT_RGB24, makecol_BGR24 },
    { PIXFMT_BGR24, makecol_RGB24 },
    { PIXFMT_ARGB32, makecol_BGRA32 },
    { PIXFMT_BGRA32, makecol_ARGB32 },
    { PIXFMT_RGBA32, makecol_ABGR32 },
    { PIXFMT_ABGR32, makecol_RGBA32 },
    { 0, NULL }
};
#else
static const struct {
    unsigned long color_format;
    int (*makecol)(int r, int g, int b);
} color_formats[] = {
    { PIXFMT_RGB15, makecol_RGB555BE },
    { PIXFMT_BGR15, makecol_BGR555BE },
    { PIXFMT_RGB15PC, makecol_RGB555LE },
    { PIXFMT_BGR15PC, makecol_BGR555LE },
    { PIXFMT_RGB16, makecol_RGB565BE },
    { PIXFMT_BGR16, makecol_BGR565BE },
    { PIXFMT_RGB16PC, makecol_RGB565LE },
    { PIXFMT_BGR16PC, makecol_BGR565LE },
    { PIXFMT_RGB24, makecol_RGB24 },
    { PIXFMT_BGR24, makecol_BGR24 },
    { PIXFMT_ARGB32, makecol_ARGB32 },
    { PIXFMT_BGRA32, makecol_BGRA32 },
    { PIXFMT_RGBA32, makecol_RGBA32 },
    { 0, NULL }
};
#endif
#else
static const struct {
    unsigned long color_format;
    int (*makecol)(int r, int g, int b);
} color_formats[] = {
    { RGBFB_R8G8B8, makecol_RGB24 }, /* TrueColor RGB (8 bit each) */
    { RGBFB_B8G8R8, makecol_BGR24 }, /* TrueColor BGR (8 bit each) */
    { RGBFB_R5G6B5PC, makecol_RGB565LE }, /* HiColor16 (5 bit R, 6 bit G, 5 bit B), format: gggbbbbbrrrrrggg */
    { RGBFB_R5G5B5PC, makecol_RGB555LE }, /* HiColor15 (5 bit each), format: gggbbbbb0rrrrrgg */
    { RGBFB_A8R8G8B8, makecol_ARGB32 }, /* 4 Byte TrueColor ARGB (A unused alpha channel) */
    { RGBFB_A8B8G8R8, makecol_ABGR32 }, /* 4 Byte TrueColor ABGR (A unused alpha channel) */
    { RGBFB_R8G8B8A8, makecol_RGBA32 }, /* 4 Byte TrueColor RGBA (A unused alpha channel) */
    { RGBFB_B8G8R8A8, makecol_BGRA32 }, /* 4 Byte TrueColor BGRA (A unused alpha channel) */
    { RGBFB_R5G6B5, makecol_RGB565BE }, /* HiColor16 (5 bit R, 6 bit G, 5 bit B), format: rrrrrggggggbbbbb */
    { RGBFB_R5G5B5, makecol_RGB555BE }, /* HiColor15 (5 bit each), format: 0rrrrrgggggbbbbb */
    { RGBFB_B5G6R5PC, makecol_BGR565LE }, /* HiColor16 (5 bit R, 6 bit G, 5 bit B), format: gggrrrrrbbbbbggg */
    { RGBFB_B5G5R5PC, makecol_BGR555LE }, /* HiColor15 (5 bit each), format: gggrrrrr0bbbbbbgg */
    /* END */
    { 0, NULL },
};
#endif

int video_canvas_set_palette(struct video_canvas_s *canvas, struct palette_s *palette)
{
    int (*makecol)(int r, int g, int b);
    unsigned int i;
    int col;

    if (palette == NULL) {
        return 0; /* no palette, nothing to do */
    }

    canvas->palette = palette;

    i = 0;
    makecol = makecol_dummy;

    while (color_formats[i].makecol != NULL) {
        if (color_formats[i].color_format == canvas->os->pixfmt) {
            makecol = color_formats[i].makecol;
           break;
        }
        i++;
    }

    for (i = 0; i < canvas->palette->num_entries; i++) {
        if (canvas->os != NULL
            && canvas->os->frf_pal_native_mode) {
            col = (int)i;
        } else if (canvas->depth == 8) {
            col = 0;
        } else {
            col = makecol(canvas->palette->entries[i].red,
                          canvas->palette->entries[i].green,
                          canvas->palette->entries[i].blue);
        }

        video_render_setphysicalcolor(canvas->videoconfig, i, col, canvas->depth);
    }

    if (canvas->depth > 8) {
        for (i = 0; i < 256; i++) {
            video_render_setrawrgb(i, makecol(i, 0, 0), makecol(0, i, 0), makecol(0, 0, i));
        }
        video_render_initraw(canvas->videoconfig);
    }

    if (canvas->os != NULL
        && canvas->os->frf_pal_native_mode
        && canvas->os->screen != NULL) {
        ULONG rgbtab[1 + (256 * 3) + 1];
        unsigned int count = canvas->palette->num_entries;
        unsigned int available = 256;

        if (frf_pal_depth > 0 && frf_pal_depth < 8) {
            available = 1U << frf_pal_depth;
        }
        if (count > available) {
            count = available;
        }
        if (count > 256) {
            count = 256;
        }

        rgbtab[0] = ((ULONG)count << 16) | 0;
        for (i = 0; i < count; i++) {
            rgbtab[1 + i * 3 + 0] =
                ((ULONG)canvas->palette->entries[i].red) * 0x01010101UL;
            rgbtab[1 + i * 3 + 1] =
                ((ULONG)canvas->palette->entries[i].green) * 0x01010101UL;
            rgbtab[1 + i * 3 + 2] =
                ((ULONG)canvas->palette->entries[i].blue) * 0x01010101UL;
        }
        rgbtab[1 + count * 3] = 0;
        LoadRGB32(&canvas->os->screen->ViewPort, rgbtab);
    }

    return 0;
}

void video_canvas_destroy(struct video_canvas_s *canvas)
{
    if ((canvas != NULL) && (canvas->os != NULL)) {
        ui_menu_destroy(canvas);
        statusbar_destroy(canvas);
        lib_free(canvas->os->window_name);

#if defined(HAVE_PROTO_CYBERGRAPHICS_H) && defined(HAVE_XVIDEO)
        if (canvas->os->vlayer_handle) {
            DetachVLayer(canvas->os->vlayer_handle);
            DeleteVLayerHandle(canvas->os->vlayer_handle);
            canvas->os->vlayer_handle = NULL;
        }
#endif

        closecanvaswindow(canvas);
        if (canvas->os->screen != NULL) {
            CloseScreen(canvas->os->screen);
            canvas->os->screen = NULL;
        }
        if (canvas->os->window_bitmap != NULL) {
            FreeBitMap(canvas->os->window_bitmap);
            canvas->os->window_bitmap = NULL;
        }
        frf_pal_free_native_buffer(canvas);

#ifdef AMIGA_AROS
        if (unlockable_buffer != NULL) {
            lib_free(unlockable_buffer);
            unlockable_buffer = NULL;
        }

        if (renderRPort != NULL) {
            FreeRastPort(renderRPort);
            renderRPort=NULL;
        }

        if (backRPort != NULL) {
            FreeRastPort(backRPort);
            backRPort = NULL;
        }
#endif

        if (canvaslist == canvas) {
            canvaslist = canvas->next;
        } else {
            video_canvas_t *node = canvaslist;
            while (node->next != canvas) {
                node = node->next;
            }
            node->next = canvas->next;
        }

        lib_free(canvas->os);
        canvas->os = NULL;
    }
}

void video_canvas_resize(struct video_canvas_s *canvas, char resize_canvas)
{
    if (reopen(canvas, canvas->draw_buffer->canvas_physical_width, canvas->draw_buffer->canvas_physical_height) == NULL) {
        exit(20);
    }
}

int video_arch_resources_init(void)
{
    return 0;
}

void video_arch_resources_shutdown(void)
{
}

static int fullscreen_update_needed = 0;

void video_arch_fullscreen_toggle(void)
{
    frf_pal_asl_request = 1;
    fullscreen_update_needed = 1; /* just remember the toggle */
}

void video_arch_fullscreen_native_pal(void)
{
    /* Legacy menu entry: use the same unified ASL selector. */
    video_arch_fullscreen_select_screenmode();
}

void video_arch_fullscreen_select_asl_mode(void)
{
    video_arch_fullscreen_select_screenmode();
}

void video_arch_fullscreen_select_screenmode(void)
{
    frf_pal_asl_request = 1;
    resources_set_value("FullscreenEnabled", (resource_value_t)1);
    fullscreen_update_needed = 1;
}

void video_arch_fullscreen_update(void)
{
    if (fullscreen_update_needed == 1) {
        if (canvaslist != NULL) {
            if (reopen(canvaslist,
                       canvaslist->draw_buffer->canvas_physical_width,
                       canvaslist->draw_buffer->canvas_physical_height)
                == NULL) {
                /* A bad native/ASL selection must not terminate VICE. */
                frf_pal_asl_request = 0;
                frf_pal_asl_valid = 0;
                frf_pal_native = 0;
                resources_set_value("FullscreenEnabled", (resource_value_t)0);

                (void)reopen(
                    canvaslist,
                    canvaslist->draw_buffer->canvas_physical_width,
                    canvaslist->draw_buffer->canvas_physical_height);
            }
        }
        fullscreen_update_needed = 0;
    }
}

char video_canvas_can_resize(struct video_canvas_s *canvas)
{
    return 1;
}
