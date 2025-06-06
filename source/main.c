/**
 * CleanRip - main.c
 * Copyright (C) 2010 emu_kidid
 *
 * Main driving code behind the disc ripper
 *
 * CleanRip homepage: http://code.google.com/p/cleanrip/
 * email address: emukidid@gmail.com
 *
 *
 * This program is free software; you can redistribute it and/
 * or modify it under the terms of the GNU General Public Li-
 * cence as published by the Free Software Foundation; either
 * version 2 of the Licence, or any later version.
 *
 * This program is distributed in the hope that it will be use-
 * ful, but WITHOUT ANY WARRANTY; without even the implied war-
 * ranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public Licence for more details.
 *
 **/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gccore.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <malloc.h>
#include <stdarg.h>
#include <ogc/timesupp.h>
#include <ogc/machine/processor.h>
#include "FrameBufferMagic.h"
#include "IPLFontWrite.h"
#include "ios.h"
#include "gc_dvd.h"
#include "verify.h"
#include "datel.h"
#include "main.h"
#include "crc32.h"
#include "sha1.h"
#include "md5.h"
#include <fat.h>
#include "m2loader/m2loader.h"

#define DEFAULT_FIFO_SIZE    (256*1024)//(64*1024) minimum

#ifdef HW_RVL
#include <ogc/usbstorage.h>
#include <sdcard/wiisd_io.h>
#include <wiiuse/wpad.h>
#endif

#include <ntfs.h>
static ntfs_md *mounts = NULL;

#ifdef HW_RVL
static DISC_INTERFACE* sdcard = &__io_wiisd;
static DISC_INTERFACE* usb = &__io_usbstorage;
#endif
#ifdef HW_DOL
#include <sdcard/gcsd.h>
#include <sdcard/card_cmn.h>
#include <sdcard/card_io.h>
static int sdcard_slot = 0;
static DISC_INTERFACE* sdcard = &__io_gcsda;
static DISC_INTERFACE* m2loader = &__io_m2ldr;
static DISC_INTERFACE* usb = NULL;
#endif

static int calcChecksums = 0;
static int dumpCounter = 0;
static char gameName[32];
static char internalName[512];
static char mountPath[512];
static char wpadNeedScan = 0;
static char padNeedScan = 0;
int print_usb = 0;
int shutdown = 0;
int whichfb = 0;
u32 iosversion = -1;
int verify_in_use = 0;
int verify_disc_type = 0;
GXRModeObj *vmode = NULL;
u32 *xfb[2] = { NULL, NULL };
int options_map[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

enum {
	MSG_SETFILE,
	MSG_WRITE,
	MSG_FLUSH,
};

typedef union _writer_msg {
	struct {
		int command;
		void* data;
		u32 length;
		mqbox_t ret_box;
	};
	uint8_t pad[32]; // pad to 32 bytes for alignment
} writer_msg;

static void* writer_thread(void* _msgq) {
	FILE* fp = NULL;
	mqbox_t msgq = (mqbox_t)_msgq;
	writer_msg* msg;

	// stupid libogc returns TRUE even if the message queue gets destroyed while waiting
	while (MQ_Receive(msgq, (mqmsg_t*)&msg, MQ_MSG_BLOCK)==TRUE && msg) {
		switch (msg->command) {
			case MSG_SETFILE:
				fp = (FILE*)msg->data;
				break;
			case MSG_WRITE:
				if (fp && fwrite(msg->data, msg->length, 1, fp)!=1) {
					// write error, signal it by pushing a NULL message to the front
					MQ_Jam(msg->ret_box, (mqmsg_t)NULL, MQ_MSG_BLOCK);
					return NULL;
				}

				// release the block so it can be reused
				MQ_Send(msg->ret_box, (mqmsg_t)msg, MQ_MSG_BLOCK);
				break;
			case MSG_FLUSH:
				*(vu32*)msg->data = 1;
				break;
		}
	}

	return msg;
}


void print_gecko(const char* fmt, ...)
{
	if(print_usb) {
		char tempstr[2048];
		va_list arglist;
		va_start(arglist, fmt);
		vsprintf(tempstr, fmt, arglist);
		va_end(arglist);
		usb_sendbuffer_safe(1,tempstr,strlen(tempstr));
	}
}


void check_exit_status() {
#ifdef HW_DOL
	if(shutdown == 1 || shutdown == 2)
		exit(0);
#endif
#ifdef HW_RVL
	if (shutdown == 1) {//Power off System
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	}
	if (shutdown == 2) { //Return to HBC/whatever
		void (*rld)() = (void(*)()) 0x80001800;
		rld();
	}
#endif
}

#ifdef HW_RVL
u32 get_wii_buttons_pressed(u32 buttons) {
	WPADData *wiiPad;
	if (wpadNeedScan) {
		WPAD_ScanPads();
		wpadNeedScan = 0;
	}
	wiiPad = WPAD_Data(0);

	if (wiiPad->btns_h & WPAD_BUTTON_B) {
		buttons |= PAD_BUTTON_B;
	}

	if (wiiPad->btns_h & WPAD_BUTTON_A) {
		buttons |= PAD_BUTTON_A;
	}

	if (wiiPad->btns_h & WPAD_BUTTON_LEFT) {
		buttons |= PAD_BUTTON_LEFT;
	}

	if (wiiPad->btns_h & WPAD_BUTTON_RIGHT) {
		buttons |= PAD_BUTTON_RIGHT;
	}

	if (wiiPad->btns_h & WPAD_BUTTON_UP) {
		buttons |= PAD_BUTTON_UP;
	}

	if (wiiPad->btns_h & WPAD_BUTTON_DOWN) {
		buttons |= PAD_BUTTON_DOWN;
	}

	if (wiiPad->btns_h & WPAD_BUTTON_HOME) {
		shutdown = 2;
	}
	return buttons;
}
#endif

u32 get_buttons_pressed() {
	u32 buttons = 0;

	if (padNeedScan) {
		PAD_ScanPads();
		padNeedScan = 0;
	}

#ifdef HW_RVL
	buttons = get_wii_buttons_pressed(buttons);
#endif

	u16 gcPad = PAD_ButtonsDown(0);

	if (gcPad & PAD_BUTTON_B) {
		buttons |= PAD_BUTTON_B;
	}

	if (gcPad & PAD_BUTTON_A) {
		buttons |= PAD_BUTTON_A;
	}

	if (gcPad & PAD_BUTTON_LEFT) {
		buttons |= PAD_BUTTON_LEFT;
	}

	if (gcPad & PAD_BUTTON_RIGHT) {
		buttons |= PAD_BUTTON_RIGHT;
	}

	if (gcPad & PAD_BUTTON_UP) {
		buttons |= PAD_BUTTON_UP;
	}

	if (gcPad & PAD_BUTTON_DOWN) {
		buttons |= PAD_BUTTON_DOWN;
	}

	if (gcPad & PAD_TRIGGER_Z) {
		shutdown = 2;
	}
	check_exit_status();
	return buttons;
}

void wait_press_A() {
	// Draw the A button
	DrawAButton(265, 310);
	DrawFrameFinish();
	while ((get_buttons_pressed() & PAD_BUTTON_A));
	while (!(get_buttons_pressed() & PAD_BUTTON_A));
}

void wait_press_A_exit_B() {
	// Draw the A and B buttons
	DrawAButton(195, 310);
	DrawBButton(390, 310);
	DrawFrameFinish();
	while ((get_buttons_pressed() & (PAD_BUTTON_A | PAD_BUTTON_B)));
	while (1) {
		while (!(get_buttons_pressed() & (PAD_BUTTON_A | PAD_BUTTON_B)));
		if (get_buttons_pressed() & PAD_BUTTON_A) {
			break;
		}
		else if (get_buttons_pressed() & PAD_BUTTON_B) {
			print_gecko("Exit\r\n");
			exit(0);
		}
	}
}

static void InvalidatePADS(u32 retrace) {
	padNeedScan = wpadNeedScan = 1;
}

/* check for ahbprot */
int have_hw_access() {
	if (read32(HW_ARMIRQMASK) && read32(HW_ARMIRQFLAG)) {
		// disable DVD irq for starlet
		mask32(HW_ARMIRQMASK, 1<<18, 0);
		print_gecko("AHBPROT access OK\r\n");
		return 1;
	}
	return 0;
}

void ShutdownWii() {
	shutdown = 1;
}

/* start up the GameCube/Wii */
static void Initialise() {
#ifdef HW_RVL
	disable_ahbprot();
#endif
	// Initialise the video system
	VIDEO_Init();

	// This function initialises the attached controllers
	PAD_Init();
#ifdef HW_RVL
	CONF_Init();
	WPAD_Init();
	WPAD_SetIdleTimeout(120);
	WPAD_SetPowerButtonCallback((WPADShutdownCallback) ShutdownWii);
	SYS_SetPowerCallback(ShutdownWii);
#endif

	vmode = VIDEO_GetPreferredMode(NULL);
	VIDEO_Configure(vmode);
	xfb[0] = (u32 *) MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
	xfb[1] = (u32 *) MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
	VIDEO_ClearFrameBuffer(vmode, xfb[0], COLOR_BLACK);
	VIDEO_ClearFrameBuffer(vmode, xfb[1], COLOR_BLACK);
	VIDEO_SetNextFramebuffer(xfb[0]);
	VIDEO_SetPostRetraceCallback(InvalidatePADS);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (vmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	// setup the fifo and then init GX
	void *gp_fifo = NULL;
	gp_fifo = MEM_K0_TO_K1 (memalign (32, DEFAULT_FIFO_SIZE));
	memset (gp_fifo, 0, DEFAULT_FIFO_SIZE);
	GX_Init (gp_fifo, DEFAULT_FIFO_SIZE);
	// clears the bg to color and clears the z buffer
	GX_SetCopyClear ((GXColor){0,0,0,255}, 0x00000000);
	// init viewport
	GX_SetViewport (0, 0, vmode->fbWidth, vmode->efbHeight, 0, 1);
	// Set the correct y scaling for efb->xfb copy operation
	GX_SetDispCopyYScale ((f32) vmode->xfbHeight / (f32) vmode->efbHeight);
	GX_SetDispCopyDst (vmode->fbWidth, vmode->xfbHeight);
	GX_SetCullMode (GX_CULL_NONE); // default in rsp init
	GX_CopyDisp (xfb[0], GX_TRUE); // This clears the efb
	GX_CopyDisp (xfb[0], GX_TRUE); // This clears the xfb

	init_font();
	init_textures();
	whichfb = 0;
}

#ifdef HW_RVL
/* FindIOS - borrwed from Tantric */
static int FindIOS(u32 ios) {
	s32 ret;
	u32 n;

	u64 *titles = NULL;
	u32 num_titles = 0;

	ret = ES_GetNumTitles(&num_titles);
	if (ret < 0)
		return 0;

	if (num_titles < 1)
		return 0;

	titles = (u64 *) memalign(32, num_titles * sizeof(u64) + 32);
	if (!titles)
		return 0;

	ret = ES_GetTitles(titles, num_titles);
	if (ret < 0) {
		free(titles);
		return 0;
	}

	for (n = 0; n < num_titles; n++) {
		if ((titles[n] & 0xFFFFFFFF) == ios) {
			free(titles);
			return 1;
		}
	}
	free(titles);
	return 0;
}

/* check for AHBPROT & IOS58 */
static void hardware_checks() {
	if (!have_hw_access()) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(190, "AHBPROT check failed");
		WriteCentre(255, "Please install the latest HBC");
		WriteCentre(280, "Check the FAQ for more info");
		WriteCentre(315, "Press A to exit");
		wait_press_A();
		exit(0);
	}

	int ios58exists = FindIOS(58);
	print_gecko("IOS 58 Exists: %s\r\n", ios58exists ? "YES":"NO");
	if (ios58exists && iosversion != 58) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(190, "IOS Version check failed");
		WriteCentre(255, "IOS 58 exists but is not in use");
		WriteCentre(280, "Dumping to USB will be SLOW!");
		WriteCentre(315, "Press  A to continue  B to exit");
		wait_press_A_exit_B();
	}
	if (!ios58exists) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(190, "IOS Version check failed");
		WriteCentre(255, "Please install IOS58");
		WriteCentre(280, "Dumping to USB will be SLOW!");
		WriteCentre(315, "Press  A to continue  B to exit");
		wait_press_A_exit_B();
	}
}
#endif

/* show the disclaimer */
static void show_disclaimer() {
	DrawFrameStart();
	DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
	WriteCentre(190, "Disclaimer");
	WriteCentre(230, "The author is not responsible for any");
	WriteCentre(255, "damages that could occur to any");
	WriteCentre(280, "removable device used within this program");
	DrawFrameFinish();

	DrawFrameStart();
	DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
	WriteCentre(190, "Disclaimer");
	WriteCentre(230, "The author is not responsible for any");
	WriteCentre(255, "damages that could occur to any");
	WriteCentre(280, "removable device used within this program");
	WriteCentre(315, "Press  A to continue  B to exit");
	sleep(5);
	wait_press_A_exit_B();
}

/* Initialise the dvd drive + disc */
static int initialise_dvd() {
	DrawFrameStart();
	DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
#ifdef HW_DOL
	WriteCentre(255, "Insert a GameCube DVD Disc");
#else
	WriteCentre(255, "Insert a GC/Wii DVD Disc");
#endif
	WriteCentre(315, "Press  A to continue  B to exit");
	wait_press_A_exit_B();

	DrawFrameStart();
	DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
	WriteCentre(255, "Initialising Disc ...");
	DrawFrameFinish();
	int ret = init_dvd();

	if (ret == NO_DISC) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(255, "No disc detected");
		print_gecko("No disc detected\r\n");
		DrawFrameFinish();
		sleep(3);
	}
	return ret;
}

#ifdef HW_DOL
int select_sd_gecko_slot() {
	int slot = 0;
	while ((get_buttons_pressed() & PAD_BUTTON_A));
	while (1) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(255, "Please select SDGecko Slot");
		DrawSelectableButton(100, 310, -1, 340, "Slot A", slot == 0 ? B_SELECTED : B_NOSELECT, -1);
		DrawSelectableButton(240, 310, -1, 340, "Slot B", slot == 1 ? B_SELECTED : B_NOSELECT, -1);
		DrawSelectableButton(380, 310, -1, 340, "SD2SP2", slot == 2 ? B_SELECTED : B_NOSELECT, -1);
		DrawFrameFinish();
		while (!(get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT
				| PAD_BUTTON_B | PAD_BUTTON_A)));
		u32 btns = get_buttons_pressed();
		if (btns & PAD_BUTTON_RIGHT) {
			slot++;
			if (slot > 2) slot = 0;
		}
		if (btns & PAD_BUTTON_LEFT) {
			slot--;
			if (slot < 0) slot = 2;
		}
		if (btns & PAD_BUTTON_A)
			break;
		while ((get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT
				| PAD_BUTTON_B | PAD_BUTTON_A)));
	}
	while ((get_buttons_pressed() & PAD_BUTTON_A));

	return slot;
}

DISC_INTERFACE* get_sd_card_handler(int slot) {
	switch (slot) {
		case 1:
			return &__io_gcsdb;
		case 2:
			return &__io_gcsd2;
		default: /* Also handles case 0 */
			return &__io_gcsda;
	}
}
#endif

/* Initialise the device */
static int initialise_device(int type, int fs) {
	int ret = 0;

	DrawFrameStart();
	DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
	if (type == TYPE_SD) {
#ifdef HW_DOL
		sdcard_slot = select_sd_gecko_slot();
		sdcard = get_sd_card_handler(sdcard_slot);

		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
#endif
		WriteCentre(255, "Insert a SD FAT/NTFS formatted device");
	}
#ifdef HW_DOL
	else if (type == TYPE_M2LOADER) {
		WriteCentre(255, "Insert a M.2 FAT/NTFS formatted device");
	}
#else
	else if (type == TYPE_USB) {
		WriteCentre(255, "Insert a USB FAT/NTFS formatted device");
	}
#endif
	WriteCentre(315, "Press  A to continue  B to exit");
	wait_press_A_exit_B();

	if (fs == TYPE_FAT) {
		switch (type) {
			case TYPE_SD:
				ret = fatMountSimple("fat", sdcard);
				break;
#ifdef HW_DOL
			case TYPE_M2LOADER:
				ret = fatMountSimple("fat", m2loader);
				break;
#else
			case TYPE_USB:
				ret = fatMountSimple("fat", usb);
				break;
#endif
		}
		if (ret != 1) {
			DrawFrameStart();
			DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
			sprintf(txtbuffer, "Error Mounting Device [%08X]", ret);
			WriteCentre(255, txtbuffer);
			WriteCentre(315, "Press A to try again  B to exit");
			wait_press_A_exit_B();
		}
		sprintf(&mountPath[0], "fat:/");
	}
	else if (fs == TYPE_NTFS) {
		int mountCount = 0;
		switch (type) {
			case TYPE_SD:
				mountCount = ntfsMountDevice(sdcard, &mounts, NTFS_DEFAULT | NTFS_RECOVER);
				break;
#ifdef HW_DOL
			case TYPE_M2LOADER:
				mountCount = ntfsMountDevice(m2loader, &mounts, NTFS_DEFAULT | NTFS_RECOVER);
				break;
#else
			case TYPE_USB:
				mountCount = ntfsMountDevice(usb, &mounts, NTFS_DEFAULT | NTFS_RECOVER);
				break;
#endif
		}

		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		if (!mountCount || mountCount == -1) {
			if (mountCount == -1) {
				sprintf(txtbuffer, "Error whilst mounting devices (%i)", errno);
			} else {
				sprintf(txtbuffer, "No NTFS volume(s) were found and/or mounted");
			}
			WriteCentre(255, txtbuffer);
			WriteCentre(315, "Press A to try again  B to exit");
			wait_press_A_exit_B();
		} else {
			sprintf(txtbuffer, "%s Mounted", ntfsGetVolumeName(mounts[0].name));
			WriteCentre(230, txtbuffer);
			sprintf(txtbuffer, "%i NTFS volume(s) mounted!", mountCount);
			WriteCentre(255, txtbuffer);
			WriteCentre(315, "Press  A  to continue");
			wait_press_A();
			sprintf(&mountPath[0], "%s:/", mounts[0].name);
			ret = 1;
		}
	}
	return ret;
}

/* identify whether this disc is a Gamecube or Wii disc */
static int identify_disc() {
	char readbuf[2048] __attribute__((aligned(32)));

	memset(&internalName[0],0,512);
	// Read the header
	DVD_LowRead64(readbuf, 2048, 0ULL);
	if (readbuf[0]) {
		strncpy(&gameName[0], readbuf, 6);
		gameName[6] = 0;
		// Multi Disc identifier support
		if (readbuf[6]) {
			size_t lastPos = strlen(gameName);
			sprintf(&gameName[lastPos], "-disc%i", (readbuf[6]) + 1);
		}
		strncpy(&internalName[0],&readbuf[32],512);
		internalName[511] = '\0';
	} else {
		sprintf(&gameName[0], "disc%i", dumpCounter);
	}
	if ((*(volatile u32*)(readbuf + 0x1C)) == NGC_MAGIC) {
		print_gecko("NGC disc\r\n");
		return IS_NGC_DISC;
	}
	if ((*(volatile u32*)(readbuf + 0x18)) == WII_MAGIC) {
		print_gecko("Wii disc\r\n");
		return IS_WII_DISC;
	}
	else {
		print_gecko("Unkown disc\r\n");
		return IS_UNK_DISC;
	}
}

const char* const get_game_name() {
	return gameName;
}

/* the user must specify the disc type */
static int force_disc() {
	int type = IS_NGC_DISC;
	while ((get_buttons_pressed() & PAD_BUTTON_A));
	while (1) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(190, "Failed to detect the disc type");
		WriteCentre(255, "Please select the correct type");
		DrawSelectableButton(100, 310, -1, 340, "Gamecube", (type
				== IS_NGC_DISC) ? B_SELECTED : B_NOSELECT, -1);
		DrawSelectableButton(380, 310, -1, 340, "Wii",
				(type == IS_WII_DISC) ? B_SELECTED : B_NOSELECT, -1);
		DrawFrameFinish();
		while (!(get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT
				| PAD_BUTTON_B | PAD_BUTTON_A)))
			;
		u32 btns = get_buttons_pressed();
		if (btns & PAD_BUTTON_RIGHT)
			type ^= 1;
		if (btns & PAD_BUTTON_LEFT)
			type ^= 1;
		if (btns & PAD_BUTTON_A)
			break;
		while ((get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT
				| PAD_BUTTON_B | PAD_BUTTON_A)))
			;
	}
	while ((get_buttons_pressed() & PAD_BUTTON_A))
		;
	return type;
}

/*
 Detect if a dual-layer disc was inserted by checking if reading from sectors
 on the second layer is succesful or not. Returns the correct disc size.
*/
int detect_duallayer_disc() {
	char *readBuf = (char*)memalign(32,64);
	int ret = WII_D1_SIZE;
	uint64_t offset = (uint64_t)WII_D1_SIZE << 11;
	if (DVD_LowRead64(readBuf, 64, offset) == 0) {
		ret = WII_D5_SIZE;
	}
	offset = (uint64_t)WII_D5_SIZE << 11;//offsetToSecondLayer
	if (DVD_LowRead64(readBuf, 64, offset) == 0) {
		ret = WII_D9_SIZE;
	}
	free(readBuf);

	print_gecko("Detect: %s\r\n", (ret == WII_D1_SIZE) ? "Wii mini DVD size"
		: (ret == WII_D5_SIZE) ? "Wii Single Layer"
		: "Wii Dual Layer");

	return ret;
}

/* the user must specify the device type */
int device_type() {
	int selected_type = 0;
	
	while ((get_buttons_pressed() & PAD_BUTTON_A));
	while (1) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(255, "Please select the device type");
#ifdef HW_DOL
		DrawSelectableButton(140, 310, -1, 340, "SD Card",
				(selected_type == 0) ? B_SELECTED : B_NOSELECT, -1);
		DrawSelectableButton(380, 310, -1, 340, "M.2 Loader",
				(selected_type == 1) ? B_SELECTED : B_NOSELECT, -1);
#endif
#ifdef HW_RVL
		DrawSelectableButton(100, 310, -1, 340, "USB",
				(selected_type == 0) ? B_SELECTED : B_NOSELECT, -1);
		DrawSelectableButton(380, 310, -1, 340, "Front SD",
				(selected_type == 1) ? B_SELECTED : B_NOSELECT, -1);
#endif
		DrawFrameFinish();
		while (!(get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT
				| PAD_BUTTON_B | PAD_BUTTON_A)));
		u32 btns = get_buttons_pressed();

		if (btns & PAD_BUTTON_RIGHT)
			selected_type ^= 1;
		if (btns & PAD_BUTTON_LEFT)
			selected_type ^= 1;

		if (btns & PAD_BUTTON_A)
			break;

		while ((get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT
				| PAD_BUTTON_B | PAD_BUTTON_A)));
	}
	while ((get_buttons_pressed() & PAD_BUTTON_A));

#ifdef HW_DOL
	return selected_type == 0 ? TYPE_SD : TYPE_M2LOADER;
#endif
#ifdef HW_RVL
	return selected_type == 0 ? TYPE_USB : TYPE_SD;
#endif
}

/* the user must specify the file system type */
int filesystem_type() {
	int type = TYPE_FAT;
	while ((get_buttons_pressed() & PAD_BUTTON_A));
	while (1) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(255, "Please select the filesystem type");
		DrawSelectableButton(100, 310, -1, 340, "FAT",
				(type == TYPE_FAT) ? B_SELECTED : B_NOSELECT, -1);
		DrawSelectableButton(380, 310, -1, 340, "NTFS",
				(type == TYPE_NTFS) ? B_SELECTED : B_NOSELECT, -1);
		DrawFrameFinish();
		while (!(get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT
				| PAD_BUTTON_B | PAD_BUTTON_A)));
		u32 btns = get_buttons_pressed();
		if (btns & PAD_BUTTON_RIGHT)
			type ^= 1;
		if (btns & PAD_BUTTON_LEFT)
			type ^= 1;
		if (btns & PAD_BUTTON_A)
			break;
		while ((get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT
				| PAD_BUTTON_B | PAD_BUTTON_A)));
	}
	while ((get_buttons_pressed() & PAD_BUTTON_A));
	return type;
}

char *getShrinkOption() {
	int opt = options_map[NGC_SHRINK_ISO];
	if (opt == SHRINK_ALL)
		return "Shrink All";
	else if (opt == SHRINK_PAD_GARBAGE)
		return "Wipe Garbage";
	else if (opt == SHRINK_NONE)
		return "No";
	return 0;
}

char *getAlignOption() {
	int opt = options_map[NGC_ALIGN_FILES];
	if (opt == ALIGN_ALL)
		return "Align All";
	else if (opt == ALIGN_AUDIO)
		return "Audio Only";
	return 0;
}

char *getAlignmentBoundaryOption() {
	int opt = options_map[NGC_ALIGN_BOUNDARY];
	if (opt == ALIGN_32)
		return "32Kb";
	else if (opt == ALIGN_2)
		return "2KB";
	else if (opt == ALIGN_512)
		return "512B";
	return 0;
}

char *getDualLayerOption() {
	int opt = options_map[WII_DUAL_LAYER];
	if (opt == AUTO_DETECT)
		return "Auto";
	else if (opt == SINGLE_MINI)
		return "1.4GB";
	else if (opt == SINGLE_LAYER)
		return "4.4GB";
	else if (opt == DUAL_LAYER)
		return "8GB";
	return 0;
}

char *getNewFileOption() {
	int opt = options_map[WII_NEWFILE];
	if (opt == ASK_USER)
		return "Yes";
	else if (opt == AUTO_CHUNK)
		return "No";
	return 0;
}

char *getChunkSizeOption() {
	int opt = options_map[WII_CHUNK_SIZE];
	if (opt == CHUNK_1GB)
		return "1GB";
	else if (opt == CHUNK_2GB)
		return "2GB";
	else if (opt == CHUNK_3GB)
		return "3GB";
	else if (opt == CHUNK_MAX)
		return "Max";
	return 0;
}

int getMaxPos(int option_pos) {
	switch (option_pos) {
	case WII_DUAL_LAYER:
		return DUAL_DELIM;
	case WII_CHUNK_SIZE:
		return CHUNK_DELIM;
	case NGC_ALIGN_BOUNDARY:
		return ALIGNB_DELIM;
	case NGC_ALIGN_FILES:
		return ALIGN_DELIM;
	case NGC_SHRINK_ISO:
		return SHRINK_DELIM;
	case WII_NEWFILE:
		return NEWFILE_DELIM;
	}
	return 0;
}

void toggleOption(int option_pos, int dir) {
	int max = getMaxPos(option_pos);
	if (options_map[option_pos] + dir >= max) {
		options_map[option_pos] = 0;
	} else if (options_map[option_pos] + dir < 0) {
		options_map[option_pos] = max - 1;
	} else {
		options_map[option_pos] += dir;
	}
}

static void get_settings(int disc_type) {
	int currentSettingPos = 0, maxSettingPos =
			((disc_type == IS_WII_DISC) ? MAX_WII_OPTIONS : MAX_NGC_OPTIONS) -1;

	while ((get_buttons_pressed() & PAD_BUTTON_A));
	while (1) {
		DrawFrameStart();
		DrawEmptyBox(75, 120, vmode->fbWidth - 78, 400, COLOR_BLACK);
		sprintf(txtbuffer, "%s Disc Ripper Setup:",
				disc_type == IS_WII_DISC ? "Wii" : "Gamecube");
		WriteCentre(130, txtbuffer);

		// Gamecube Settings
		if (disc_type == IS_NGC_DISC) {
		/*
			WriteFont(80, 160 + (32* 1 ), "Shrink ISO");
			DrawSelectableButton(vmode->fbWidth-220, 160+(32*1), -1, 160+(32*1)+30, getShrinkOption(), (!currentSettingPos) ? B_SELECTED:B_NOSELECT);
			WriteFont(80, 160+(32*2), "Align Files");
			DrawSelectableButton(vmode->fbWidth-220, 160+(32*2), -1, 160+(32*2)+30, getAlignOption(), (currentSettingPos==1) ? B_SELECTED:B_NOSELECT);
			WriteFont(80, 160+(32*3), "Alignment boundary");
			DrawSelectableButton(vmode->fbWidth-220, 160+(32*3), -1, 160+(32*3)+30, getAlignmentBoundaryOption(), (currentSettingPos==2) ? B_SELECTED:B_NOSELECT);
		*/
		}
		// Wii Settings
		else if (disc_type == IS_WII_DISC) {
			WriteFont(80, 160 + (32 * 1), "Dump Size");
			DrawSelectableButton(vmode->fbWidth - 220, 160 + (32 * 1), -1, 160 + (32 * 1) + 30, getDualLayerOption(), (!currentSettingPos) ? B_SELECTED : B_NOSELECT, -1);
			WriteFont(80, 160 + (32 * 2), "Chunk Size");
			DrawSelectableButton(vmode->fbWidth - 220, 160 + (32 * 2), -1, 160 + (32 * 2) + 30, getChunkSizeOption(), (currentSettingPos == 1) ? B_SELECTED : B_NOSELECT, -1);
			WriteFont(80, 160 + (32 * 3), "New device per chunk");
			DrawSelectableButton(vmode->fbWidth - 220, 160 + (32 * 3), -1, 160 + (32 * 3) + 30, getNewFileOption(), (currentSettingPos == 2) ? B_SELECTED : B_NOSELECT, -1);
		}
		WriteCentre(370,"Press  A  to continue");
		DrawAButton(265,360);
		DrawFrameFinish();

		while (!(get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT | PAD_BUTTON_A | PAD_BUTTON_UP | PAD_BUTTON_DOWN)));
		u32 btns = get_buttons_pressed();
		if(btns & PAD_BUTTON_RIGHT) {
			toggleOption(currentSettingPos+((disc_type == IS_WII_DISC)?MAX_NGC_OPTIONS:0), 1);
		}
		if(btns & PAD_BUTTON_LEFT) {
			toggleOption(currentSettingPos+((disc_type == IS_WII_DISC)?MAX_NGC_OPTIONS:0), -1);
		}
		if(btns & PAD_BUTTON_UP) {
			currentSettingPos = (currentSettingPos>0) ? (currentSettingPos-1):maxSettingPos;
		}
		if(btns & PAD_BUTTON_DOWN) {
			currentSettingPos = (currentSettingPos<maxSettingPos) ? (currentSettingPos+1):0;
		}
		if(btns & PAD_BUTTON_A) {
			break;
		}
		while (get_buttons_pressed() & (PAD_BUTTON_RIGHT | PAD_BUTTON_LEFT | PAD_BUTTON_A | PAD_BUTTON_UP | PAD_BUTTON_DOWN));
	}
	while(get_buttons_pressed() & PAD_BUTTON_B);
}

void prompt_new_file(FILE **fp, int chunk, int type, int fs, int silent) {
	// Close the file and unmount the fs
	fclose(*fp);
	if(silent == ASK_USER) {
		if (fs == TYPE_FAT) {
			fatUnmount("fat:/");
			if (type == TYPE_SD) {
				sdcard->shutdown(sdcard);
			}
#ifdef HW_DOL
			else if (type == TYPE_M2LOADER) {
				m2loader->shutdown(m2loader);
			}
#else
			else if (type == TYPE_USB) {
				usb->shutdown(usb);
			}
#endif
		}
		else if (fs == TYPE_NTFS) {
			ntfsUnmount(mounts[0].name, true);
			free(mounts);
			if (type == TYPE_SD) {
				sdcard->shutdown(sdcard);
			}
#ifdef HW_DOL
			else if (type == TYPE_M2LOADER) {
				m2loader->shutdown(m2loader);
			}
#else
			else if (type == TYPE_USB) {
				usb->shutdown(usb);
			}
#endif
		}
		// Stop the disc if we're going to wait on the user
		dvd_motor_off(0);
	}

	if(silent == ASK_USER) {
		int ret = -1;
		do {
				DrawFrameStart();
				DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
				WriteCentre(255, "Insert a device for the next chunk");
				WriteCentre(315, "Press  A to continue  B to exit");
				wait_press_A_exit_B();

			if (fs == TYPE_FAT) {
				int i = 0;
				for (i = 0; i < 10; i++) {
					ret = fatMountSimple("fat", type == TYPE_USB ? usb : sdcard);
					if (ret == 1) {
						break;
					}
				}
			}
			else if (fs == TYPE_NTFS) {
				int mountCount = ntfsMountDevice(type == TYPE_USB ? usb : sdcard,
						&mounts, NTFS_DEFAULT | NTFS_RECOVER);
				if (mountCount && mountCount != -1) {
					sprintf(&mountPath[0], "%s:/", mounts[0].name);
					ret = 1;
				} else {
					ret = -1;
				}
			}
			if (ret != 1) {
				DrawFrameStart();
				DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
				sprintf(txtbuffer, "Error Mounting Device [%08X]", ret);
				WriteCentre(255, txtbuffer);
				WriteCentre(315, "Press A to try again  B to exit");
				wait_press_A_exit_B();
			}
		} while (ret != 1);
	}

	*fp = NULL;
	sprintf(txtbuffer, "%s%s.part%i.iso", &mountPath[0], &gameName[0], chunk);
	remove(&txtbuffer[0]);
	*fp = fopen(&txtbuffer[0], "wb");
	if (*fp == NULL) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(230, "Failed to create file:");
		WriteCentre(255, txtbuffer);
		WriteCentre(315, "Exiting in 5 seconds");
		DrawFrameFinish();
		sleep(5);
		exit(0);
	}
	if(silent == ASK_USER) {
		init_dvd();
	}
}

void dump_bca() {
	sprintf(txtbuffer, "%s%s.bca", &mountPath[0], &gameName[0]);
	remove(&txtbuffer[0]);
	FILE *fp = fopen(txtbuffer, "wb");
	if (fp) {
		char bca_data[64] __attribute__((aligned(32)));
		DCZeroRange(bca_data, 64);
		DCFlushRange(bca_data, 64);
		dvd_read_bca(bca_data);
		fwrite(bca_data, 1, 0x40, fp);
		fclose(fp);
	}
}

void dump_info(char *md5, char *sha1, u32 crc32, int verified, u32 seconds, char* name) {
	char infoLine[1024];
	char timeLine[256];
	memset(infoLine, 0, 1024);
	memset(timeLine, 0, 256);
	time_t curtime;
	time(&curtime);
	strftime(timeLine, sizeof(timeLine), "%Y-%m-%d %H:%M:%S", localtime(&curtime));

	if(md5 && sha1 && crc32) {
		sprintf(infoLine, "--File Generated by CleanRip v%i.%i.%i--"
						  "\r\n\r\nFilename: %s\r\nInternal Name: %s\r\nMD5: %s\r\n"
						  "SHA-1: %s\r\nCRC32: %08X\r\nVersion: 1.0%i\r\nVerified: %s\r\nDuration: %u min. %u sec\r\nDumped at: %s.\r\n",
				V_MAJOR,V_MID,V_MINOR,&gameName[0],&internalName[0], md5, sha1, crc32, *(u8*)0x80000007,
				verified ? "Yes" : "No", seconds/60, seconds%60, timeLine);
	}
	else {
		sprintf(infoLine, "--File Generated by CleanRip v%i.%i.%i--"
						  "\r\n\r\nFilename: %s\r\nInternal Name: %s\r\n"
						  "Version: 1.0%i\r\nChecksum calculations disabled\r\nDuration: %u min. %u sec\r\nDumped at: %s.\r\n",
				V_MAJOR,V_MID,V_MINOR,&gameName[0],&internalName[0], *(u8*)0x80000007, seconds/60, seconds%60, timeLine);
	}

	if (name != NULL) {
		sprintf(txtbuffer, "%s%s-dumpinfo.txt", &mountPath[0], &name[0]);
	}
	else {
		sprintf(txtbuffer, "%s%s-dumpinfo.txt", &mountPath[0], &gameName[0]);
	}
	
	remove(&txtbuffer[0]);
	FILE *fp = fopen(txtbuffer, "wb");
	if (fp) {
		fwrite(infoLine, 1, strlen(&infoLine[0]), fp);
		fclose(fp);
	}
}

void renameFile(char* mountPath, char* befor, char* after, char* base) {
	char tempstr[2048];

	if (mountPath == NULL || befor == NULL || after == NULL || base == NULL) return;

	sprintf(txtbuffer, "%s%s%s", &mountPath[0], &befor[0], &base[0]);
	sprintf(tempstr, "%s%s%s", &mountPath[0], &after[0], &base[0]);
	remove(&tempstr[0]);
	if (rename(txtbuffer, tempstr) == 0) {
		print_gecko("Renamed: %s\r\n\t->%s\r\n", txtbuffer, tempstr);
	}
	else {
		print_gecko("Rename failed: %s\r\n", txtbuffer);
	}
}
#define MSG_COUNT 8
#define THREAD_PRIO 128

int dump_game(int disc_type, int type, int fs) {

	md5_state_t state;
	md5_byte_t digest[16];
	SHA1Context sha;
	u32 crc32 = 0;
	u32 crc100000 = 0;
	char *buffer;
	mqbox_t msgq, blockq;
	lwp_t writer;
	writer_msg *wmsg;
	writer_msg msg;
	int i;

	MQ_Init(&blockq, MSG_COUNT);
	MQ_Init(&msgq, MSG_COUNT);

	// since libogc is too shitty to be able to get the current thread priority, just force it to a known value
	LWP_SetThreadPriority(0, THREAD_PRIO);
	// writer thread should have same priority so it can be yielded to
	LWP_CreateThread(&writer, writer_thread, (void*)msgq, NULL, 0, THREAD_PRIO);

	// Check if we will ask the user to insert a new device per chunk
	int silent = options_map[WII_NEWFILE];

	// The read size
	u32 opt_read_size = READ_SIZE;

	u32 startLBA = 0;
	u32 endLBA = (disc_type == IS_NGC_DISC || disc_type == IS_DATEL_DISC) ? NGC_DISC_SIZE
		: (options_map[WII_DUAL_LAYER] == AUTO_DETECT ? detect_duallayer_disc()
			: (options_map[WII_DUAL_LAYER] == SINGLE_MINI ? WII_D1_SIZE
				: (options_map[WII_DUAL_LAYER] == DUAL_LAYER ? WII_D9_SIZE
					: WII_D5_SIZE)));

	// Work out the chunk size
	u32 chunk_size_wii = options_map[WII_CHUNK_SIZE];
	u32 opt_chunk_size;
	if (chunk_size_wii == CHUNK_MAX) {
		// use 4GB chunks max for FAT drives
		if (fs == TYPE_FAT) {
			opt_chunk_size = 4 * ONE_GIGABYTE - (opt_read_size>>11) - 1;
		} else {
			opt_chunk_size = endLBA + (opt_read_size>>11);
		}
	} else {
		opt_chunk_size = (chunk_size_wii + 1) * ONE_GIGABYTE;
	}

	if (disc_type == IS_NGC_DISC || disc_type == IS_DATEL_DISC || options_map[WII_DUAL_LAYER] == SINGLE_MINI) {
		opt_chunk_size = NGC_DISC_SIZE;
	}

	// Dump the BCA for Nintendo discs
#ifdef HW_RVL
	dump_bca();
#endif

	// Create the read buffers
	buffer = memalign(32, MSG_COUNT*(opt_read_size+sizeof(writer_msg)));
	for (i=0; i < MSG_COUNT; i++) {
		MQ_Send(blockq, (mqmsg_t)(buffer+i*(opt_read_size+sizeof(writer_msg))), MQ_MSG_BLOCK);
	}

	// Reset MD5/SHA-1/CRC
	md5_init(&state);
	SHA1Reset(&sha);
	crc32 = 0;

	// There will be chunks, name accordingly
	if (opt_chunk_size < endLBA) {
		sprintf(txtbuffer, "%s%s.part0.iso", &mountPath[0], &gameName[0]);
	} else {
		sprintf(txtbuffer, "%s%s.iso", &mountPath[0], &gameName[0]);
	}
	remove(&txtbuffer[0]);
	FILE *fp = fopen(&txtbuffer[0], "wb");
	if (fp == NULL) {
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		WriteCentre(230, "Failed to create file:");
		WriteCentre(255, txtbuffer);
		WriteCentre(315, "Exiting in 5 seconds");
		DrawFrameFinish();
		sleep(5);
		exit(0);
	}
	msg.command = MSG_SETFILE;
	msg.data = fp;
	MQ_Send(msgq, (mqmsg_t)&msg, MQ_MSG_BLOCK);

	int ret = 0;
	u32 lastLBA = 0;
	u64 lastCheckedTime = gettime();
	u64 startTime = gettime();
	int chunk = 1;
	int isKnownDatel = 0;

	while (!ret && (startLBA < endLBA)) {
		MQ_Receive(blockq, (mqmsg_t*)&wmsg, MQ_MSG_BLOCK);
		if (wmsg==NULL) { // asynchronous write error
			LWP_JoinThread(writer, NULL);
			fclose(fp);
			DrawFrameStart();
			DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
			WriteCentre(255, "Write Error!");
			WriteCentre(315, "Exiting in 10 seconds");
			DrawFrameFinish();
			sleep(10);
			exit(1);
		}

		if (startLBA > (opt_chunk_size * chunk)) {
			// wait for writing to finish
			vu32 sema = 0;
			msg.command = MSG_FLUSH;
			msg.data = (void*)&sema;
			MQ_Send(msgq, (mqmsg_t)&msg, MQ_MSG_BLOCK);
			while (!sema)
				LWP_YieldThread();

			// open new file
			u64 wait_begin = gettime();
			prompt_new_file(&fp, chunk, type, fs, silent);
			// pretend the wait didn't happen
			startTime -= (gettime() - wait_begin);

			// set writing file
			msg.command = MSG_SETFILE;
			msg.data = fp;
			MQ_Send(msgq, (mqmsg_t)&msg, MQ_MSG_BLOCK);
			chunk++;
		}

		opt_read_size = (startLBA + (opt_read_size>>11)) <= endLBA ? opt_read_size : ((u32)((endLBA-startLBA)<<11));

		wmsg->command =  MSG_WRITE;
		wmsg->data = wmsg+1;
		wmsg->length = opt_read_size;
		wmsg->ret_box = blockq;

		// Read from Disc
		if(disc_type == IS_DATEL_DISC)
			ret = DVD_LowRead64Datel(wmsg->data, (u32)opt_read_size, (u64)startLBA << 11, isKnownDatel);
		else
			ret = DVD_LowRead64(wmsg->data, (u32)opt_read_size, (u64)startLBA << 11);
		MQ_Send(msgq, (mqmsg_t)wmsg, MQ_MSG_BLOCK);
		if(calcChecksums) {
			// Calculate MD5
			md5_append(&state, (const md5_byte_t *) (wmsg+1), (u32) opt_read_size);
			// Calculate SHA-1
			SHA1Input(&sha, (const unsigned char *) (wmsg+1), (u32) opt_read_size);
			// Calculate CRC32
			crc32 = Crc32_ComputeBuf( crc32, wmsg+1, (u32) opt_read_size);
		}

		if(disc_type == IS_DATEL_DISC && (((u64)startLBA<<11) + opt_read_size == 0x100000)){
			crc100000 = crc32;
			isKnownDatel = datel_findCrcSum(crc100000);
			DrawFrameStart();
			DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
			if(!isKnownDatel) {
				WriteCentre(215, "(Warning: This disc will take a while to dump!)");
			}
			sprintf(txtbuffer, "%s CRC100000=%08X", (isKnownDatel ? "Known":"Unknown"), crc100000);
			WriteCentre(255, txtbuffer);
			WriteCentre(315, "Press  A to continue  B to exit");
			u64 waitTimeStart = gettime();
			wait_press_A_exit_B();
			startTime += (gettime() - waitTimeStart);	// Don't throw time off because we'd paused here
		}

		check_exit_status();

		if (get_buttons_pressed() & PAD_BUTTON_B) {
			ret = -61;
		}
		// Update status every second
		u64 curTime = gettime();
		s32 timePassed = diff_msec(lastCheckedTime, curTime);
		if (timePassed >= 1000) {
			u32 bytes_since_last_read = (u32)(((startLBA - lastLBA)<<11) * (1000.0f/timePassed));
			u64 remainder = (((u64)endLBA - startLBA)<<11) - opt_read_size;

			u32 etaTime = (remainder / bytes_since_last_read);
			sprintf(txtbuffer, "%dMB %4.2fKB/s - ETA %02d:%02d:%02d",
					(int) (((u64) ((u64) startLBA << 11)) / (1024*1024)),
				(float)bytes_since_last_read/1024.0f,
				(int)((etaTime/3600)%60),(int)((etaTime/60)%60),(int)(etaTime%60));
			DrawFrameStart();
			DrawProgressBar((int)((float)((float)startLBA/(float)endLBA)*100), txtbuffer);
      		DrawFrameFinish();
  			lastCheckedTime = curTime;
			lastLBA = startLBA;
		}
		startLBA+=opt_read_size>>11;
	}
	if(calcChecksums) {
		md5_finish(&state, digest);
	}

	// signal writer to finish
	MQ_Send(msgq, (mqmsg_t)NULL, MQ_MSG_BLOCK);
	LWP_JoinThread(writer, NULL);
	fclose(fp);

	free(buffer);
	MQ_Close(blockq);
	MQ_Close(msgq);

	if(ret != -61 && ret) {
		DrawFrameStart();
		DrawEmptyBox (30,180, vmode->fbWidth-38, 350, COLOR_BLACK);
		sprintf(txtbuffer, "%s",dvd_error_str());
		print_gecko("Error: %s\r\n",txtbuffer);
		WriteCentre(255,txtbuffer);
		WriteCentre(315,"Press  A  to continue");
		dvd_motor_off(1);
		wait_press_A();
		return 0;
	}
	else if (ret == -61) {
		DrawFrameStart();
		DrawEmptyBox (30,180, vmode->fbWidth-38, 350, COLOR_BLACK);
		sprintf(txtbuffer, "Copy Cancelled");
		print_gecko("%s\r\n",txtbuffer);
		WriteCentre(255,txtbuffer);
		WriteCentre(315,"Press  A  to continue");
		dvd_motor_off(0);
		wait_press_A();
		return 0;
	}
	else {
		sprintf(txtbuffer,"Copy completed in %u mins. Press A",diff_sec(startTime, gettime())/60);
		DrawFrameStart();
		DrawEmptyBox (30,180, vmode->fbWidth-38, 350, COLOR_BLACK);
		WriteCentre(190,txtbuffer);

		int verified = 0;
		char tempstr[32];

		if ((disc_type == IS_DATEL_DISC)) {
				dump_skips(&mountPath[0], crc100000);
		}
		if (calcChecksums) {
			char md5sum[64];
			char sha1sum[64];
			memset(&md5sum[0], 0, 64);
			memset(&sha1sum[0], 0, 64);
			int i; for (i=0; i<16; i++) sprintf(&md5sum[i*2],"%02x",digest[i]);
			if(SHA1Result(&sha)) {
				for (i=0; i<5; i++) sprintf(&sha1sum[i*8],"%08x",sha.Message_Digest[i]);
			}
			else {
				sprintf(sha1sum, "Error computing SHA-1");
			}

			char* name = NULL;
			verified = (verify_is_available(disc_type) && verify_findMD5Sum(&md5sum[0], disc_type));
			if (verified) {
				if (opt_chunk_size < endLBA) {
					for (int i = 0; i < chunk; i++) {
						sprintf(tempstr, ".part%i.iso", i);
						renameFile(&mountPath[0], &gameName[0], verify_get_name(0), &tempstr[0]);
					}
				}
				else {
					renameFile(&mountPath[0], &gameName[0], verify_get_name(0), ".iso");
				}
#ifdef HW_RVL
				renameFile(&mountPath[0], &gameName[0], verify_get_name(0), ".bca");
#endif

				name = verify_get_name(0);
			}
			if ((disc_type == IS_DATEL_DISC)) {
				verified = datel_findMD5Sum(&md5sum[0]);
				if (verified) {
					renameFile(&mountPath[0], &gameName[0], datel_get_name(0), ".iso");
					renameFile(&mountPath[0], &gameName[0], datel_get_name(0), ".skp");
#ifdef HW_RVL
					renameFile(&mountPath[0], &gameName[0], datel_get_name(0), ".bca");
#endif
					name = datel_get_name(0);
				}
			}

			dump_info(&md5sum[0], &sha1sum[0], crc32, verified, diff_sec(startTime, gettime()), name);

			print_gecko("MD5: %s\r\n", verified ? "Verified OK" : "Not Verified ");

			sprintf(txtbuffer, "MD5: %s", verified ? "Verified OK" : "");
			WriteCentre(230, txtbuffer);
			if ((disc_type == IS_DATEL_DISC)) {
				WriteCentre(255, verified ? datel_get_name(1) : "Not Verified with datel.dat");
			}
			else {
				WriteCentre(255, verified ? verify_get_name(1) : "Not Verified with redump.org");
			}
			WriteCentre(280, &md5sum[0]);
		}
		else {
			dump_info(NULL, NULL, 0, 0, diff_sec(startTime, gettime()), NULL);
		}
		if ((disc_type == IS_DATEL_DISC) && !(verified)) {
			dump_skips(&mountPath[0], crc100000);
			
			char tempstr[32];
			sprintf(tempstr, "datel_%08x", crc100000);
			renameFile(&mountPath[0], &gameName[0], &tempstr[0], ".iso");
			renameFile(&mountPath[0], &gameName[0], &tempstr[0], "-dumpinfo.txt");
			renameFile(&mountPath[0], &gameName[0], &tempstr[0], ".skp");
#ifdef HW_RVL
			renameFile(&mountPath[0], &gameName[0], &tempstr[0], ".bca");
#endif
		}
		WriteCentre(315,"Press  A to continue  B to exit");
		dvd_motor_off(1);
		wait_press_A_exit_B();
	}
	return 1;
}

int main(int argc, char **argv) {
#ifdef HW_RVL
	// disable ahbprot and reload IOS to clear up memory
	IOS_ReloadIOS(IOS_GetVersion());
	disable_ahbprot();
#endif
	Initialise();
#ifdef HW_RVL
	iosversion = IOS_GetVersion();
#endif
	if(usb_isgeckoalive(1)) {
		usb_flush(1);
		print_usb = 1;
	}
	print_gecko("CleanRip Version %i.%i.%i\r\n",V_MAJOR, V_MID, V_MINOR);
	print_gecko("Arena Size: %iKb\r\n",(SYS_GetArena1Hi()-SYS_GetArena1Lo())/1024);

#ifdef HW_RVL
	print_gecko("Running on IOS ver: %i\r\n", iosversion);
#endif
	show_disclaimer();
#ifdef HW_RVL
	hardware_checks();
#endif

	// Ask the user if they want checksum calculations enabled this time?
	calcChecksums = DrawYesNoDialog("Enable checksum calculations?",
									"(Enabling will add about 3 minutes)");

	int reuseSettings = NOT_ASKED;
	while (1) {
		int type, fs, ret;
		if(reuseSettings == NOT_ASKED || reuseSettings == ANSWER_NO) {
			type = device_type();
			fs = filesystem_type();

			ret = -1;
			do {
				ret = initialise_device(type, fs);
			} while (ret != 1);
		}

		if(calcChecksums) {
			// Try to load up redump.org dat files
			verify_init(&mountPath[0]);
#ifdef HW_RVL
			// Ask the user if they want to download new ones
			verify_download(&mountPath[0]);

			// User might've got some new files.
			verify_init(&mountPath[0]);
#endif
		}

		// Init the drive and try to detect disc type
		ret = NO_DISC;
		do {
			ret = initialise_dvd();
		} while (ret == NO_DISC);

		int disc_type = identify_disc();

		if (disc_type == IS_UNK_DISC) {
			disc_type = force_disc();
		}

		if(reuseSettings == NOT_ASKED || reuseSettings == ANSWER_NO) {
			if (disc_type == IS_WII_DISC) {
				get_settings(disc_type);
			}
		
			// Ask the user if they want to force Datel check this time?
			if(DrawYesNoDialog("Is this a unlicensed datel disc?",
								 "(Will attempt auto-detect if no)")) {
				disc_type = IS_DATEL_DISC;
				datel_init(&mountPath[0]);
#ifdef HW_RVL
				datel_download(&mountPath[0]);
				datel_init(&mountPath[0]);
#endif
				calcChecksums = 1;
			}
		}
		
		if(reuseSettings == NOT_ASKED) {
			if(DrawYesNoDialog("Remember settings?",
								 "Will only ask again next session")) {
				reuseSettings = ANSWER_YES;
			}
		}

		verify_in_use = verify_is_available(disc_type);
		verify_disc_type = disc_type;

		ret = dump_game(disc_type, type, fs);
		verify_in_use = 0;
		dumpCounter += (ret ? 1 : 0);
		
		DrawFrameStart();
		DrawEmptyBox(30, 180, vmode->fbWidth - 38, 350, COLOR_BLACK);
		sprintf(txtbuffer, "%i disc(s) dumped", dumpCounter);
		WriteCentre(190, txtbuffer);
		WriteCentre(255, "Dump another disc?");
		WriteCentre(315, "Press  A to continue  B to exit");
		wait_press_A_exit_B();
	}

	return 0;
}
