/*
	Adrenaline
	Copyright (C) 2016-2018, TheFloW

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/rtc.h>
#include <psp2/system_param.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "utils.h"

Pad old_pad, current_pad, pressed_pad, released_pad, hold_pad, hold2_pad;
Pad hold_count, hold2_count;

int SCE_CTRL_ENTER = SCE_CTRL_CROSS, SCE_CTRL_CANCEL = SCE_CTRL_CIRCLE;

/* v40: the shared log fd. -1 = not attempted yet; -2 = open FAILED (degrade
 * silently, never retry -- a per-call retry would reintroduce the open cost
 * this change removes, and logging must never break the app); >= 0 = open. */
static SceUID ra_log_fd = -1;

static SceUID ra_log_open(void)
{
	/* v40: open once, lazily; the fd is kept for the process lifetime.
	 * Concurrent first users race benignly: each opens its own fd; whichever
	 * arrives first claims the slot (a SUCCESS may claim the slot from an
	 * untried -1 OR from a failed -2 -- the failed latch must not beat a real
	 * fd), any loser's fd is closed immediately (no leak, no torn fd --
	 * SceUID is an int, reads are not torn). A failed open parks the sentinel
	 * at -2 so no later call pays the open again; only a success can reclaim
	 * it. */
	if (ra_log_fd == -1) {
		SceUID fd = sceIoOpen("ux0:data/adrenaline_user_log.txt",
			SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
		if (fd >= 0) {
			if (!__sync_bool_compare_and_swap(&ra_log_fd, -1, fd) &&
				!__sync_bool_compare_and_swap(&ra_log_fd, -2, fd))
				sceIoClose(fd);   /* a real fd already won: keep it */
		} else {
			__sync_bool_compare_and_swap(&ra_log_fd, -1, -2);
		}
	}
	return (ra_log_fd >= 0) ? ra_log_fd : -1;
}

/* v41: bounded re-open after a failed write (V41-PLAN.md §4). Only CAS
 * winners increment ra_log_reopens, atomically; two winners on successive fds
 * can both pass the bound check, so the bound is RA_LOG_REOPEN_MAX, +1 under
 * that race (v41-audit.md I6). */
#define RA_LOG_REOPEN_MAX 3
static volatile int ra_log_reopens = 0;
static volatile unsigned int ra_log_write_errs = 0;

unsigned int ra_log_write_errors(void)
{
	return ra_log_write_errs;
}

/* v41: the failed-write path, out of line so its note[] and temporaries are
 * not in debugPrintf's frame -- debugPrintf runs on foreign pspemu/POPS
 * threads (main.c, pops.c) and every call would otherwise pay them
 * (v41-audit.md S2). The failed line is rewritten BEFORE the marker so the
 * timestamps stay monotonic for parse.py's session split (audit I1). */
static __attribute__((noinline)) void ra_log_write_failed(SceUID fd, int w, const char *string, int len)
{
	__sync_fetch_and_add(&ra_log_write_errs, 1);
	if (ra_log_reopens < RA_LOG_REOPEN_MAX &&
		__sync_bool_compare_and_swap(&ra_log_fd, fd, -1)) {
		SceUID nfd;
		int reopen = __sync_add_and_fetch(&ra_log_reopens, 1);
		nfd = ra_log_open();   /* -1 -> real fd, or -> -2 latch on failure */
		if (nfd >= 0) {
			char note[128];
			uint32_t ms;
			int n;
			sceIoWrite(nfd, string, len);
			ms = (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
			n = snprintf(note, sizeof(note),
				"[%u.%03u] [RA] v41 log fd re-opened after write error 0x%08X (reopen %d/%d)\n",
				ms / 1000u, ms % 1000u, (unsigned)w, reopen, RA_LOG_REOPEN_MAX);
			if (n > 0)
				sceIoWrite(nfd, note, n);
		}
	}
}

int debugPrintf(char *text, ...) {
	va_list list;
	char string[512];

	va_start(list, text);
	vsnprintf(string, sizeof(string), text, list);   /* v34 F0: vsprintf was unbounded on a 512-byte stack buffer */
	va_end(list);

	/* v40: one kept fd, write per line. The log's consumers read it after a
	 * crash, so there is deliberately NO buffering layer: sceIoWrite is
	 * unbuffered (no stdio layer above it), so the per-line write IS the
	 * flush -- the RetroArch/PPSSPP/DuckStation fputs/fwrite+fflush pattern
	 * with stdio's userspace buffer removed. Durability trade, stated: a
	 * per-line close committed every line to the device, so a hard-freeze
	 * tail loss is now POSSIBLE where the old pattern could not lose a
	 * written line; this is falsifiable by construction -- if a future crash
	 * log ends early, this change is the first suspect. Per-line atomicity:
	 * a single sceIoWrite request is serialized by the IO manager, so
	 * concurrent writers (render/worker/boot/chime) never tear a line; only
	 * the inter-line ORDER is arbitrary, same as before. No close call site
	 * exists: the .suprx is torn down by the kernel on module unload and the
	 * fd dies with the process (no orderly shutdown path on this side).
	 * v41: a failed write is counted (PERF werr=) and triggers at most 3
	 * re-opens; a re-open rewrites the failed line and then a `v41 log fd
	 * re-opened` marker, so a mid-session gap is visible instead of looking
	 * like a quit. */
	SceUID fd = ra_log_open();
	if (fd >= 0) {
		int len = (int)strlen(string);
		int w = sceIoWrite(fd, string, len);
		if (w < 0)
			ra_log_write_failed(fd, w, string, len);
	}

	return 0;
}

int ReadFile(char *file, void *buf, int size) {
	SceUID fd = sceIoOpen(file, SCE_O_RDONLY, 0);
	if (fd < 0) {
		return fd;
	}

	int read = sceIoRead(fd, buf, size);

	sceIoClose(fd);
	return read;
}

int WriteFile(char *file, void *buf, int size) {
	SceUID fd = sceIoOpen(file, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0) {
		return fd;
	}

	int written = sceIoWrite(fd, buf, size);

	sceIoClose(fd);
	return written;
}

extern int enter_button;
void readPad() {
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);

	SceCtrlData home;
	kuCtrlPeekBufferPositive(0, &home, 1);

	pad.buttons &= ~SCE_CTRL_PSBUTTON;
	pad.buttons |= (home.buttons & SCE_CTRL_PSBUTTON);

	memcpy(&old_pad, current_pad, sizeof(Pad));
	memset(&current_pad, 0, sizeof(Pad));

	if (pad.buttons & SCE_CTRL_UP)
		current_pad[PAD_UP] = 1;
	if (pad.buttons & SCE_CTRL_DOWN)
		current_pad[PAD_DOWN] = 1;
	if (pad.buttons & SCE_CTRL_LEFT)
		current_pad[PAD_LEFT] = 1;
	if (pad.buttons & SCE_CTRL_RIGHT)
		current_pad[PAD_RIGHT] = 1;
	if (pad.buttons & SCE_CTRL_LTRIGGER)
		current_pad[PAD_LTRIGGER] = 1;
	if (pad.buttons & SCE_CTRL_RTRIGGER)
		current_pad[PAD_RTRIGGER] = 1;
	if (pad.buttons & SCE_CTRL_TRIANGLE)
		current_pad[PAD_TRIANGLE] = 1;
	if (pad.buttons & SCE_CTRL_CIRCLE)
		current_pad[PAD_CIRCLE] = 1;
	if (pad.buttons & SCE_CTRL_CROSS)
		current_pad[PAD_CROSS] = 1;
	if (pad.buttons & SCE_CTRL_SQUARE)
		current_pad[PAD_SQUARE] = 1;
	if (pad.buttons & SCE_CTRL_START)
		current_pad[PAD_START] = 1;
	if (pad.buttons & SCE_CTRL_SELECT)
		current_pad[PAD_SELECT] = 1;
	if (pad.buttons & SCE_CTRL_PSBUTTON)
		current_pad[PAD_PSBUTTON] = 1;

	if (pad.ly < ANALOG_CENTER - ANALOG_THRESHOLD) {
		current_pad[PAD_LEFT_ANALOG_UP] = 1;
	} else if (pad.ly > ANALOG_CENTER + ANALOG_THRESHOLD) {
		current_pad[PAD_LEFT_ANALOG_DOWN] = 1;
	}

	if (pad.lx < ANALOG_CENTER - ANALOG_THRESHOLD) {
		current_pad[PAD_LEFT_ANALOG_LEFT] = 1;
	} else if (pad.lx > ANALOG_CENTER + ANALOG_THRESHOLD) {
		current_pad[PAD_LEFT_ANALOG_RIGHT] = 1;
	}

	if (pad.ry < ANALOG_CENTER - ANALOG_THRESHOLD) {
		current_pad[PAD_RIGHT_ANALOG_UP] = 1;
	} else if (pad.ry > ANALOG_CENTER + ANALOG_THRESHOLD) {
		current_pad[PAD_RIGHT_ANALOG_DOWN] = 1;
	}

	if (pad.rx < ANALOG_CENTER - ANALOG_THRESHOLD) {
		current_pad[PAD_RIGHT_ANALOG_LEFT] = 1;
	} else if (pad.rx > ANALOG_CENTER + ANALOG_THRESHOLD) {
		current_pad[PAD_RIGHT_ANALOG_RIGHT] = 1;
	}

	for (int i = 0; i < PAD_N_BUTTONS; i++) {
		pressed_pad[i] = current_pad[i] & ~old_pad[i];
		released_pad[i] = ~current_pad[i] & old_pad[i];

		hold_pad[i] = pressed_pad[i];
		hold2_pad[i] = pressed_pad[i];

		if (current_pad[i]) {
			if (hold_count[i] >= 10) {
				hold_pad[i] = 1;
				hold_count[i] = 6;
			}

			if (hold2_count[i] >= 10) {
				hold2_pad[i] = 1;
				hold2_count[i] = 10;
			}

			hold_count[i]++;
			hold2_count[i]++;
		} else {
			hold_count[i] = 0;
			hold2_count[i] = 0;
		}
	}

	if (enter_button == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) {
		old_pad[PAD_ENTER] = old_pad[PAD_CIRCLE];
		current_pad[PAD_ENTER] = current_pad[PAD_CIRCLE];
		pressed_pad[PAD_ENTER] = pressed_pad[PAD_CIRCLE];
		released_pad[PAD_ENTER] = released_pad[PAD_CIRCLE];
		hold_pad[PAD_ENTER] = hold_pad[PAD_CIRCLE];
		hold2_pad[PAD_ENTER] = hold2_pad[PAD_CIRCLE];

		old_pad[PAD_CANCEL] = old_pad[PAD_CROSS];
		current_pad[PAD_CANCEL] = current_pad[PAD_CROSS];
		pressed_pad[PAD_CANCEL] = pressed_pad[PAD_CROSS];
		released_pad[PAD_CANCEL] = released_pad[PAD_CROSS];
		hold_pad[PAD_CANCEL] = hold_pad[PAD_CROSS];
		hold2_pad[PAD_CANCEL] = hold2_pad[PAD_CROSS];
	} else {
		old_pad[PAD_ENTER] = old_pad[PAD_CROSS];
		current_pad[PAD_ENTER] = current_pad[PAD_CROSS];
		pressed_pad[PAD_ENTER] = pressed_pad[PAD_CROSS];
		released_pad[PAD_ENTER] = released_pad[PAD_CROSS];
		hold_pad[PAD_ENTER] = hold_pad[PAD_CROSS];
		hold2_pad[PAD_ENTER] = hold2_pad[PAD_CROSS];

		old_pad[PAD_CANCEL] = old_pad[PAD_CIRCLE];
		current_pad[PAD_CANCEL] = current_pad[PAD_CIRCLE];
		pressed_pad[PAD_CANCEL] = pressed_pad[PAD_CIRCLE];
		released_pad[PAD_CANCEL] = released_pad[PAD_CIRCLE];
		hold_pad[PAD_CANCEL] = hold_pad[PAD_CIRCLE];
		hold2_pad[PAD_CANCEL] = hold2_pad[PAD_CIRCLE];
	}
}

int doubleClick(uint32_t buttons, uint64_t max_time) {
	static uint32_t old_buttons, current_buttons, released_buttons;
	static uint64_t last_time = 0;
	static int clicked = 0;
	int double_clicked = 0;

	SceCtrlData pad;
	kuCtrlPeekBufferPositive(0, &pad, 1);

	old_buttons = current_buttons;
	current_buttons = pad.buttons;
	released_buttons = ~current_buttons & old_buttons;

	if (released_buttons & buttons) {
		if (clicked) {
			if ((sceKernelGetProcessTimeWide() - last_time) < max_time) {
				double_clicked = 1;
				clicked = 0;
				last_time = 0;
			} else {
				clicked = 1;
				last_time = sceKernelGetProcessTimeWide();
			}
		} else {
			clicked = 1;
			last_time = sceKernelGetProcessTimeWide();
		}
	}

	return double_clicked;
}

void getSizeString(char string[16], uint64_t size) {
	int i = 0;
	static char *units[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB" };
	while (size > 1024) {
		size >>= 10;
		i++;
	}

	snprintf(string, 16, "%.*lld %s", (i == 0) ? 0 : 2, size, units[i]);
}

void convertUtcToLocalTime(SceDateTime *time_local, SceDateTime *time_utc) {
	SceRtcTick tick;
	sceRtcGetTick(time_utc, &tick);
	sceRtcConvertUtcToLocalTime(&tick, &tick);
	sceRtcSetTick(time_local, &tick);
}

void getTimeString(char string[16], int time_format, SceDateTime *time) {
	SceDateTime time_local;
	convertUtcToLocalTime(&time_local, time);

	switch(time_format) {
		case SCE_SYSTEM_PARAM_TIME_FORMAT_12HR:
			snprintf(string, 16, "%02d:%02d %s", (time_local.hour > 12) ? (time_local.hour-12) : ((time_local.hour == 0) ? 12 : time_local.hour),
				time_local.minute, time_local.hour >= 12 ? "PM" : "AM");
			break;

		case SCE_SYSTEM_PARAM_TIME_FORMAT_24HR:
			snprintf(string, 16, "%02d:%02d", time_local.hour, time_local.minute);
			break;
	}
}

void getDateString(char string[24], int date_format, SceDateTime *time) {
	SceDateTime time_local;
	convertUtcToLocalTime(&time_local, time);

	switch (date_format) {
		case SCE_SYSTEM_PARAM_DATE_FORMAT_YYYYMMDD:
			snprintf(string, 24, "%04d/%02d/%02d", time_local.year, time_local.month, time_local.day);
			break;

		case SCE_SYSTEM_PARAM_DATE_FORMAT_DDMMYYYY:
			snprintf(string, 24, "%02d/%02d/%04d", time_local.day, time_local.month, time_local.year);
			break;

		case SCE_SYSTEM_PARAM_DATE_FORMAT_MMDDYYYY:
			snprintf(string, 24, "%02d/%02d/%04d", time_local.month, time_local.day, time_local.year);
			break;
	}
}

void SetPspemuFrameBuffer(void *base) {
	SceDisplayFrameBuf framebuf;
	memset(&framebuf, 0, sizeof(SceDisplayFrameBuf));
	framebuf.size        = sizeof(SceDisplayFrameBuf);
	framebuf.base        = base;
	framebuf.pitch       = PSP_SCREEN_LINE;
	framebuf.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	framebuf.width       = PSP_SCREEN_WIDTH;
	framebuf.height      = PSP_SCREEN_HEIGHT;
	sceDisplaySetFrameBuf(&framebuf, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

char *getPspemuMemoryStickLocation() {
	switch (config.ms_location) {
		case MEMORY_STICK_LOCATION_UR0:
			return "ur0:pspemu";
		case MEMORY_STICK_LOCATION_IMC0:
			return "imc0:pspemu";
		case MEMORY_STICK_LOCATION_XMC0:
			return "xmc0:pspemu";
		case MEMORY_STICK_LOCATION_UMA0:
			return "uma0:pspemu";
		case MEMORY_STICK_LOCATION_UMA0_PSP:
			return "uma0:";
		default:
			return "ux0:pspemu";
	}
}

char *getPspemuEfLocation() {
	switch (config.ef_location) {
		case EF_LOCATION_UMA0:
			return "uma0:pspemu";
		case EF_LOCATION_XMC0:
			return "xmc0:pspemu";
		case EF_LOCATION_UX0:
			return "ux0:pspemu";
		case EF_LOCATION_UMA0_PSP:
			return "uma0:";
		default:
			return NULL;
	}
}

char *getPspemuMemoryStickDevice() {
	switch (config.ms_location) {
		case MEMORY_STICK_LOCATION_UR0:
			return "ur0:";
		case MEMORY_STICK_LOCATION_IMC0:
			return "imc0:";
		case MEMORY_STICK_LOCATION_XMC0:
			return "xmc0:";
		case MEMORY_STICK_LOCATION_UMA0:
			return "uma0:";
		case MEMORY_STICK_LOCATION_UMA0_PSP:
			return "uma0:";
		default:
			return "ux0:";
	}
}

char *getPspemuEfDevice() {
	switch (config.ef_location) {
		case EF_LOCATION_UX0:
			return "ux0:";
		case EF_LOCATION_XMC0:
			return "xmc0:";
		case EF_LOCATION_UMA0:
		case EF_LOCATION_UMA0_PSP:
			return "uma0:";
		default:
			return NULL;
	}
}

#define THUMB_SHUFFLE(x) ((((x) & 0xFFFF0000) >> 16) | (((x) & 0xFFFF) << 16))

uint32_t encode_movw(uint8_t rd, uint16_t imm16) {
	uint32_t imm4 = (imm16 >> 12) & 0xF;
	uint32_t i = (imm16 >> 11) & 0x1;
	uint32_t imm3 = (imm16 >> 8) & 0x7;
	uint32_t imm8 = imm16 & 0xFF;
	return THUMB_SHUFFLE(0xF2400000 | (rd << 8) | (i << 26) | (imm4 << 16) | (imm3 << 12) | imm8);
}

uint32_t encode_movt(uint8_t rd, uint16_t imm16) {
	uint32_t imm4 = (imm16 >> 12) & 0xF;
	uint32_t i = (imm16 >> 11) & 0x1;
	uint32_t imm3 = (imm16 >> 8) & 0x7;
	uint32_t imm8 = imm16 & 0xFF;
	return THUMB_SHUFFLE(0xF2C00000 | (rd << 8) | (i << 26) | (imm4 << 16) | (imm3 << 12) | imm8);
}

uint32_t encode_bl(uint32_t patch_offset, uint32_t target_offset) {
	uint32_t displacement = target_offset - (patch_offset & ~0x1) - 4;
	uint32_t signbit = (displacement >> 31) & 0x1;
	uint32_t i1 = (displacement >> 23) & 0x1;
	uint32_t i2 = (displacement >> 22) & 0x1;
	uint32_t imm10 = (displacement >> 12) & 0x03FF;
	uint32_t imm11 = (displacement >> 1) & 0x07FF;
	uint32_t j1 = i1 ^ (signbit ^ 1);
	uint32_t j2 = i2 ^ (signbit ^ 1);
	uint32_t value = (signbit << 26) | (j1 << 13) | (j2 << 11) | (imm10 << 16) | imm11;
	value |= 0xF000D000;  // BL
	return THUMB_SHUFFLE(value);
}

static void* mspace;
void* mspace_create(void* base, size_t size);
void* mspace_malloc(void* mspace, size_t size);
void* mspace_realloc(void* mspace, void* ptr, size_t size);
void mspace_free(void* mspace, void* ptr);

void mspace_init() {
	SceUID mblock = sceKernelAllocMemBlock("AdrHeap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0x800000, NULL);
	void* base;
	sceKernelGetMemBlockBase(mblock, &base);
	mspace = mspace_create(base, 0x800000);
}

void* adr_malloc(size_t size) {
	return mspace_malloc(mspace, size);
}

void* adr_realloc(void* ptr, size_t size) {
	return mspace_realloc(mspace, ptr, size);
}

void adr_free(void* ptr) {
	mspace_free(mspace, ptr);
}

