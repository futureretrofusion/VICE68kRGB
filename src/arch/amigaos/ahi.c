/*
 * ahi.c
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
static const char frf_ahi_chain_22050_441_8_final_marker[] = "FRF_AHI_CHAIN_22050_441_8_FINAL";

#ifdef HAVE_DEVICES_AHI_H

#define __USE_INLINE__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos/dostags.h>
#include <exec/exec.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/ahi.h>
#include <devices/audio.h>
#ifdef AMIGA_OS4_ALT
#define __USE_OLD_TIMEVAL__
#include <devices/timer.h>
#endif
#include <proto/exec.h>
#include <proto/dos.h>

#include "ahi.h"
#include "timer.h"
#include "lib.h"

typedef struct audio_buffer_s {
    void *buffer;
    s32 size;
    s64 time;
    s32 used;
} audio_buffer_t;

typedef struct audio_s {
    s32 frequency;
    u32 mode;
    s32 fragsize;
    s32 frags;
    audio_buffer_t *audio_buffers;
    s64 current_time;
    s32 read_buffer;
    s32 write_buffer;
    void (*audio_sync)(s64 time);
    s32 play;
    s32 shutting_down; /* FRF_AHI_RING_STABILITY */
    s32 samples_to_bytes;
    s32 read_position;
    struct timeval tv;
    s32 write_position;

  /* tasks 'n' stuff */
    struct Task *main_task;
    struct Task *audio_task;
    struct SignalSemaphore *semaphore;
    struct Process *audio_process;
#ifdef AMIGA_OS4
    timer_t *timer;
#else
    void *timer;
#endif
} audio_t;

static audio_t audio;

/* to task:
 * SIGBREAKF_CTRL_C -
 * SIGBREAKF_CTRL_D wake up
 * SIGBREAKF_CTRL_E exit
 * SIGBREAKF_CTRL_F -
 *
 * from task:
 * SIGBREAKF_CTRL_C -
 * SIGBREAKF_CTRL_D wake up
 * SIGBREAKF_CTRL_E error
 * SIGBREAKF_CTRL_F -
 */

static char ahi_task_name[] = "AudioServerTask0000000000";

static void make_task_name(void)
{
    int task_number = 0;

    for (;;) {
        sprintf(ahi_task_name, "AudioServerTask%04d", task_number);

        if (FindTask(ahi_task_name) == NULL) {
            break;
        } else {
            task_number++;
        }
    }
}

#define FLAG_EXIT (1 << 0)

static void ahi_task(void)
{
    struct AHIRequest *AHIIO = NULL, *AHIIO1 = NULL, *AHIIO2 = NULL;
    struct MsgPort *AHIMP1 = NULL, *AHIMP2 = NULL;
    u32 signals, flags;
    s32 device, used, play;
    struct AHIRequest *link = NULL;
#ifdef AMIGA_OS4
    timer_t *timer = NULL;
#else
    void *timer = NULL;
#endif
    s32 previous_read_buffer = -1;

    flags = 0;
    signals = 0;
    device = 1;

    if ((timer = timer_init())) {
        if ((AHIMP1 = CreateMsgPort())) {
            if ((AHIMP2 = CreateMsgPort())) {
                if ((AHIIO1 = (struct AHIRequest *)CreateIORequest(AHIMP1, sizeof(struct AHIRequest)))) {
                    if ((AHIIO2 = (struct AHIRequest *)CreateIORequest(AHIMP2, sizeof(struct AHIRequest)))) {
                        device = OpenDevice(AHINAME, 0, (struct IORequest *)AHIIO1, 0);
                    }
                }
            }
        }
    }

    if (device == 0) {
        *AHIIO2 = *AHIIO1;
        AHIIO1->ahir_Std.io_Message.mn_ReplyPort = AHIMP1;
        AHIIO2->ahir_Std.io_Message.mn_ReplyPort = AHIMP2;
        AHIIO1->ahir_Std.io_Message.mn_Node.ln_Pri = 0; /* FRF_AHI_CHAIN_22050_441_8_FINAL */
        AHIIO2->ahir_Std.io_Message.mn_Node.ln_Pri = 0; /* FRF_AHI_CHAIN_22050_441_8_FINAL */
        AHIIO = AHIIO1;

        Signal(audio.main_task, SIGBREAKF_CTRL_D);

        for (;;) {
            if (flags & FLAG_EXIT) {
                break;
            }

            ObtainSemaphore(audio.semaphore);
            used = audio.audio_buffers[audio.read_buffer].used;
            play = audio.play && !audio.shutting_down;
            ReleaseSemaphore(audio.semaphore);

            if (play && used) {
                AHIIO->ahir_Std.io_Command = CMD_WRITE;
                AHIIO->ahir_Std.io_Data = audio.audio_buffers[audio.read_buffer].buffer;
                AHIIO->ahir_Std.io_Length = audio.audio_buffers[audio.read_buffer].size;
                AHIIO->ahir_Std.io_Offset = 0;
                AHIIO->ahir_Frequency = audio.frequency;
                AHIIO->ahir_Type = audio.mode;
                AHIIO->ahir_Volume = 0x10000; // Volume:  0dB
                AHIIO->ahir_Position = 0x08000; // Panning: center
                AHIIO->ahir_Link = link;

                SendIO((struct IORequest *)AHIIO);

                if (link != NULL) {
                    u32 sigbit = 1L << link->ahir_Std.io_Message.mn_ReplyPort->mp_SigBit;

                    for (;;) {
                            signals = Wait(SIGBREAKF_CTRL_E
                                           | SIGBREAKF_CTRL_D
                                           | sigbit);

                            if (signals & SIGBREAKF_CTRL_E) {
                                flags |= FLAG_EXIT;

                                if (!(signals & sigbit)
                                    && !CheckIO((struct IORequest *)link)) {
                                    AbortIO((struct IORequest *)link);
                                }
                            }

                            if ((signals & sigbit) || (flags & FLAG_EXIT)) {
                                break;
                            }
                        }

#ifndef AMIGA_OS4
                    WaitIO((struct IORequest *)link);

                    if (CheckIO((struct IORequest *)AHIIO)) {
                        /* The first iorequest has finished aswell. This means we're
                         * out of sync, so force restart (make link NULL).
                         */
                        AHIIO = NULL;
                    }
#else
                    WaitIO((struct IORequest *)link);
#endif
                }

                ObtainSemaphore(audio.semaphore);

                if (previous_read_buffer >= 0) {
                    audio.audio_buffers[previous_read_buffer].used = 0;
                }

                audio.read_position = (audio.read_buffer * audio.fragsize);
                timer_gettime(timer, &audio.tv);

                link = AHIIO;

                if (AHIIO == AHIIO1) {
                    AHIIO = AHIIO2;
                } else {
                    AHIIO = AHIIO1;
                }

                previous_read_buffer = audio.read_buffer;
                audio.read_buffer++;
                if (audio.read_buffer >= audio.frags) {
                    audio.read_buffer = 0;
                }

                ReleaseSemaphore(audio.semaphore);

                if (audio.audio_sync != NULL) {
                    audio.audio_sync(audio.audio_buffers[previous_read_buffer].time);
                }

                Signal(audio.main_task, SIGBREAKF_CTRL_D);
            } else {
                signals = Wait(SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_D);
                if (signals & SIGBREAKF_CTRL_E) {
                    flags |= FLAG_EXIT;
                }
            }
        }
    }

    if (link != NULL) {
        if (!CheckIO((struct IORequest *)link)) {
            AbortIO((struct IORequest *)link);
        }
        WaitIO((struct IORequest *)link);
        link = NULL;
    }

    if (device == 0) {
        CloseDevice((struct IORequest *)AHIIO1);
    }
    if (AHIIO2 != NULL) {
        DeleteIORequest((struct IORequest *)&AHIIO2->ahir_Std);
    }
    if (AHIIO1 != NULL) {
        DeleteIORequest((struct IORequest *)&AHIIO1->ahir_Std);
    }
    if (AHIMP2 != NULL) {
        DeleteMsgPort(AHIMP2);
    }
    if (AHIMP1 != NULL) {
        DeleteMsgPort(AHIMP1);
    }
    if (timer != NULL) {
        timer_exit(timer);
    }

    /* make sure we don't race */
    Forbid();

    /* send error signal in case we failed */
    Signal(audio.main_task, SIGBREAKF_CTRL_E);
}


/*
 * FRF_HARD_PAUSE_AHI_RELEASE:
 * Preserve active backend parameters outside audio_t because ahi_close()
 * clears audio_t. Pause can release ahi.device completely and resume
 * without closing or recreating the VICE SID engine.
 */
static s32 frf_ahi_pause_frequency = 0;
static u32 frf_ahi_pause_mode = 0;
static s32 frf_ahi_pause_fragsize = 0;
static s32 frf_ahi_pause_frags = 0;
static void (*frf_ahi_pause_callback)(s64 time) = NULL;
static int frf_ahi_pause_reopen = 0;

s32 ahi_open(s32 frequency, u32 mode, s32 fragsize, s32 frags, void (*callback)(s64 time))
{

    /*
     * FRF_AHI_CHAIN_22050_441_8_FINAL_APPLY:
     * 22050 Hz / 50 PAL frames = 441 samples per frame.
     */
    frequency = 22050;
    fragsize = 441;
    frags = 8;

#ifdef AMIGA_MORPHOS
    struct TagItem ti[] = {
        { NP_CodeType, CODETYPE_PPC },
        { NP_Entry, (ULONG)ahi_task },
        { NP_Name, (ULONG)ahi_task_name },
        { NP_PPCStackSize, 32768 },
        { NP_Priority, 10 },
        { NP_Input, 0 },
        { NP_Output, 0 },
        { NP_CurrentDir, 0 },
        { NP_CopyVars, FALSE },
        { NP_WindowPtr, -1 },
        { NP_HomeDir, 0 },
        { TAG_DONE, 0 }
};
#else
    struct TagItem ti[] = {
        { NP_Entry, (ULONG)ahi_task },
        { NP_Name, (ULONG)ahi_task_name },
        { NP_StackSize, 4 * 65536 }, /* 64KB should be enough */
        { NP_Priority, 10 },
        { TAG_DONE, 0 }
};
#endif
    s32 i;
    u32 signals;

    /* reset structure */
    memset(&audio, 0, sizeof(audio_t));

    audio.timer = timer_init();
    if (audio.timer == NULL) {
        goto fail;
    }

    /* samples to bytes */
    audio.samples_to_bytes = 1;
    if (mode & AUDIO_MODE_16BIT) {
        audio.samples_to_bytes *= 2;
    }
    if (mode & AUDIO_MODE_STEREO) {
        audio.samples_to_bytes *= 2;
    }
    audio.samples_to_bytes >>= 1; /* we use shifts */

    fragsize <<= audio.samples_to_bytes;

    /* store settings */
    audio.frequency = frequency;
    audio.mode = mode;
    audio.fragsize = fragsize;
    audio.frags = frags;
    audio.audio_sync = callback;
    audio.read_position = -1;

    /* allocate buffers */
    audio.audio_buffers = lib_AllocVec(audio.frags * sizeof(audio_buffer_t), MEMF_PUBLIC | MEMF_CLEAR);
    if (audio.audio_buffers == NULL) {
        goto fail;
    }
    for (i = 0; i < audio.frags; i++) {
        audio.audio_buffers[i].buffer = lib_AllocVec(fragsize, MEMF_PUBLIC | MEMF_CLEAR);
        if (audio.audio_buffers[i].buffer == NULL) {
            goto fail;
        }
        audio.audio_buffers[i].size = 0;
    }

    /* get task pointer */
    audio.main_task = FindTask(NULL);

    /* remove signals */
    SetSignal(0, SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E);

    /* allocate semaphore */
    audio.semaphore = lib_AllocVec(sizeof(struct SignalSemaphore), MEMF_PUBLIC | MEMF_CLEAR);
    if (audio.semaphore == NULL) {
        goto fail;
    }
    memset(audio.semaphore, 0, sizeof(struct SignalSemaphore));
    InitSemaphore(audio.semaphore);

    /* make sure we don't race */
    Forbid();

    make_task_name();
    audio.audio_process = CreateNewProc(ti);

    Permit();

    audio.audio_task = (struct Task *)audio.audio_process;
    if (audio.audio_task == NULL) {
        goto fail;
    }

    signals = Wait(SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E);
    if (signals & SIGBREAKF_CTRL_E) {
        audio.audio_task = NULL;
        goto fail;
    }

    return 0;

fail:

    ahi_close(); /* free any allocated resources */

    return -1;
}

static s32 ahi_writable_bytes_locked(void)
{
    s32 i;
    s32 index;
    s32 free_bytes;
    audio_buffer_t *buffer;

    /*
     * FRF_AHI_RING_STABILITY:
     * read_position == write_position is ambiguous in a circular buffer:
     * it can mean completely empty or completely full. Determine writable
     * capacity from the fragment ownership flags instead.
     *
     * Only consecutive slots beginning at write_buffer are writable because
     * the producer cannot skip an occupied fragment.
     */
    if (audio.audio_buffers == NULL || audio.frags <= 0
        || audio.fragsize <= 0 || audio.shutting_down) {
        return 0;
    }

    free_bytes = 0;
    index = audio.write_buffer;

    for (i = 0; i < audio.frags; i++) {
        buffer = &audio.audio_buffers[index];

        if (buffer->used) {
            break;
        }

        if (i == 0 && buffer->size > 0
            && buffer->size < audio.fragsize) {
            free_bytes += audio.fragsize - buffer->size;
        } else {
            /*
             * A completed free fragment may still have size == fragsize.
             * The writer resets that stale size to zero before reusing it.
             */
            free_bytes += audio.fragsize;
        }

        index++;
        if (index >= audio.frags) {
            index = 0;
        }
    }

    return free_bytes;
}

static s32 ahi_in_buffer(void)
{
    s32 free_bytes;
    s32 total_bytes;

    if (audio.audio_task == NULL || audio.semaphore == NULL
        || audio.audio_buffers == NULL) {
        return 0;
    }

    ObtainSemaphore(audio.semaphore);
    total_bytes = audio.frags * audio.fragsize;
    free_bytes = ahi_writable_bytes_locked();
    ReleaseSemaphore(audio.semaphore);

    if (free_bytes < 0) {
        free_bytes = 0;
    } else if (free_bytes > total_bytes) {
        free_bytes = total_bytes;
    }

    return total_bytes - free_bytes;
}

s32 ahi_play_samples(void *data, s32 size, s64 time, s32 wait)
{
    s32 copy;
    s32 max;
    s32 free_bytes;
    s32 wait_loops;
    u8 *source;
    struct Task *task;
    audio_buffer_t *buffer;

    if (size <= 0) {
        return 0;
    }

    if (audio.audio_task == NULL || audio.semaphore == NULL
        || audio.audio_buffers == NULL || audio.shutting_down) {
        return -1;
    }

    size <<= audio.samples_to_bytes;
    source = (u8 *)data;

    /*
     * DOWAIT is bounded. NOWAIT never waits. A stuck AHI request must not
     * freeze VICE or the Amiga task indefinitely.
     */
    wait_loops = 0;
    for (;;) {
        ObtainSemaphore(audio.semaphore);
        free_bytes = ahi_writable_bytes_locked();
        ReleaseSemaphore(audio.semaphore);

        if (free_bytes >= size) {
            break;
        }

        if (wait == NOWAIT || ++wait_loops >= 250) {
            return -1;
        }

        timer_usleep(audio.timer, 2000);
    }

    if (time != NOTIME) {
        audio.current_time = time;
    }
    time = audio.current_time;

    while (size > 0) {
        ObtainSemaphore(audio.semaphore);

        if (audio.audio_task == NULL || audio.audio_buffers == NULL
            || audio.shutting_down) {
            ReleaseSemaphore(audio.semaphore);
            return -1;
        }

        buffer = &audio.audio_buffers[audio.write_buffer];

        if (buffer->used) {
            /*
             * This should be impossible after the capacity check. Fail
             * safely rather than entering the original unbounded Wait().
             */
            ReleaseSemaphore(audio.semaphore);
            return -1;
        }

        if (buffer->size >= audio.fragsize) {
            buffer->size = 0;
        }

        if (buffer->size == 0) {
            buffer->time = time;
        }

        max = audio.fragsize - buffer->size;
        copy = size;
        if (copy > max) {
            copy = max;
        }

        memcpy((u8 *)buffer->buffer + buffer->size, source, copy);
        buffer->size += copy;
        source += copy;
        size -= copy;

        time += ((copy >> audio.samples_to_bytes) * TIMEBASE)
                / audio.frequency;

        audio.write_position =
            (audio.write_buffer * audio.fragsize) + buffer->size;

        if (buffer->size == audio.fragsize) {
            buffer->used = 1;
            audio.write_buffer++;
            if (audio.write_buffer >= audio.frags) {
                audio.write_buffer = 0;
            }
        }

        audio.play = 1;
        task = audio.audio_task;
        ReleaseSemaphore(audio.semaphore);

        if (task != NULL) {
            Signal(task, SIGBREAKF_CTRL_D);
        }
    }

    audio.current_time = time;
    return 0;
}

s32 ahi_samples_to_bytes(s32 samples)
{
    return samples <<= audio.samples_to_bytes;
}

s32 ahi_bytes_to_samples(s32 bytes)
{
    return bytes >>= audio.samples_to_bytes;
}

s32 ahi_samples_buffered(void)
{
    return ahi_in_buffer() >> audio.samples_to_bytes;
}

s32 ahi_samples_free(void)
{
    s32 in_buffer, total_buffer;

    in_buffer = ahi_in_buffer();
    total_buffer = (audio.fragsize * audio.frags);
    total_buffer -= in_buffer;

    return total_buffer >> audio.samples_to_bytes;
}

void ahi_pause(void)
{
    /*
     * FRF_AHI_TRANSIENT_SUSPEND:
     * Dialogs, cartridge attachment, reset and warp transitions only stop
     * new playback temporarily. Keep ahi.device, the task and ring buffers
     * allocated so these routine operations cannot race a device teardown.
     */
    if (audio.audio_task == NULL || audio.semaphore == NULL) {
        return;
    }

    ObtainSemaphore(audio.semaphore);
    audio.play = 0;
    ReleaseSemaphore(audio.semaphore);
}

void ahi_continue(void)
{
    struct Task *task;

    /*
     * FRF_AHI_TRANSIENT_CONTINUE:
     * Wake the existing AHI task and continue any queued fragments. This is
     * the matching operation for ahi_pause(); it does not reopen ahi.device.
     */
    if (audio.audio_task == NULL || audio.semaphore == NULL) {
        return;
    }

    ObtainSemaphore(audio.semaphore);
    if (audio.shutting_down) {
        ReleaseSemaphore(audio.semaphore);
        return;
    }
    audio.play = 1;
    task = audio.audio_task;
    ReleaseSemaphore(audio.semaphore);

    if (task != NULL) {
        Signal(task, SIGBREAKF_CTRL_D);
    }
}

void ahi_close(void)
{
    s32 i;
    u32 signals;
    struct Task *task;

    task = NULL;

    if (audio.semaphore != NULL) {
        ObtainSemaphore(audio.semaphore);
        audio.shutting_down = 1;
        audio.play = 0;
        task = audio.audio_task;
        ReleaseSemaphore(audio.semaphore);
    } else {
        audio.shutting_down = 1;
        audio.play = 0;
        task = audio.audio_task;
    }

    if (task != NULL) {
        Signal(task, SIGBREAKF_CTRL_E);

        do {
            signals = Wait(SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E);
        } while (!(signals & SIGBREAKF_CTRL_E));

        audio.audio_task = NULL;
    }

    if (audio.semaphore != NULL) {
        lib_FreeVec(audio.semaphore);
    }

    if (audio.audio_buffers != NULL) {
        for (i = 0; i < audio.frags; i++) {
            if (audio.audio_buffers[i].buffer != NULL) {
                lib_FreeVec(audio.audio_buffers[i].buffer);
            }
        }
        lib_FreeVec(audio.audio_buffers);
    }

    if (audio.timer != NULL) {
        timer_exit(audio.timer);
    }

    memset(&audio, 0, sizeof(audio_t));
}

void ahi_release(void)
{
    /*
     * FRF_AHI_HARD_RELEASE:
     * Full device release is reserved for Right-Amiga+P. Save the active
     * backend settings before ahi_close() clears audio_t.
     */
    if (audio.audio_task == NULL || frf_ahi_pause_reopen) {
        return;
    }

    frf_ahi_pause_frequency = audio.frequency;
    frf_ahi_pause_mode = audio.mode;
    frf_ahi_pause_fragsize =
        audio.fragsize >> audio.samples_to_bytes;
    frf_ahi_pause_frags = audio.frags;
    frf_ahi_pause_callback = audio.audio_sync;
    frf_ahi_pause_reopen = 1;

    ahi_close();
}

s32 ahi_resume(void)
{
    s32 result;

    if (!frf_ahi_pause_reopen) {
        return 0;
    }

    result = ahi_open(frf_ahi_pause_frequency,
                      frf_ahi_pause_mode,
                      frf_ahi_pause_fragsize,
                      frf_ahi_pause_frags,
                      frf_ahi_pause_callback);

    if (result == 0) {
        frf_ahi_pause_reopen = 0;
    }

    return result;
}
#endif
