/* A work-in-progess MEGA65 (Commodore 65 clone origins) emulator
   Part of the Xemu project, please visit: https://github.com/lgblgblgb/xemu
   Copyright (C)2016-2022 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


#include "xemu/emutools.h"
#include "ui.h"
#include "xemu/emutools_gui.h"
#include "mega65.h"
#include "xemu/emutools_files.h"
#include "sdcard.h"
#include "sdcontent.h"
#include "xemu/emutools_hid.h"
#include "xemu/c64_kbd_mapping.h"
#include "inject.h"
#include "input_devices.h"
#include "matrix_mode.h"
#include "uart_monitor.h"
#include "xemu/f011_core.h"
#include "dma65.h"
#include "memory_mapper.h"
#include "xemu/basic_text.h"
#include "audio65.h"
#include "vic4.h"
#include "configdb.h"
#include "rom.h"
#include "hypervisor.h"
#include "xemu/cpu65.h"


#ifdef CONFIG_DROPFILE_CALLBACK
void emu_dropfile_callback ( const char *fn )
{
	DEBUGGUI("UI: file drop event, file: %s" NL, fn);
	switch (QUESTION_WINDOW("Cancel|Mount as D81|Run/inject as PRG", "What to do with the dropped file?")) {
		case 1:
			sdcard_force_external_mount(0, fn, "D81 mount failure");
			break;
		case 2:
			reset_mega65();
			inject_register_prg(fn, 0);
			break;
	}
}
#endif


static void ui_cb_attach_d81 ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, 0);
	const int drive = VOIDPTR_TO_INT(m->user_data);
	char fnbuf[PATH_MAX + 1];
	static char dir[PATH_MAX + 1] = "";
	if (!xemugui_file_selector(
		XEMUGUI_FSEL_OPEN | XEMUGUI_FSEL_FLAG_STORE_DIR,
		"Select D81 to attach",
		dir,
		fnbuf,
		sizeof fnbuf
	)) {
		sdcard_force_external_mount(drive, fnbuf, "D81 mount failure");
	} else {
		DEBUGPRINT("UI: file selection for D81 mount was cancelled." NL);
	}
}


static void ui_cb_detach_d81 ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, 0);
	const int drive = VOIDPTR_TO_INT(m->user_data);
	sdcard_force_external_mount(drive, NULL, NULL);
}


static void ui_run_prg_by_browsing ( void )
{
	char fnbuf[PATH_MAX + 1];
	static char dir[PATH_MAX + 1] = "";
	if (!xemugui_file_selector(
		XEMUGUI_FSEL_OPEN | XEMUGUI_FSEL_FLAG_STORE_DIR,
		"Select PRG to directly load and run",
		dir,
		fnbuf,
		sizeof fnbuf
	)) {
		reset_mega65();
		inject_register_prg(fnbuf, 0);
	} else
		DEBUGPRINT("UI: file selection for PRG injection was cancelled." NL);
}

#ifdef CBM_BASIC_TEXT_SUPPORT
static void ui_save_basic_as_text ( void )
{
	Uint8 *start = main_ram + 0x2001;
	Uint8 *end = main_ram + 0x4000;
	Uint8 *buffer;
	int size = xemu_basic_to_text_malloc(&buffer, 1000000, start, 0x2001, end, 0, 0);
	if (size < 0)
		return;
	if (size == 0) {
		INFO_WINDOW("BASIC memory is empty.");
		free(buffer);
		return;
	}
	printf("%s", buffer);
	FILE *f = fopen("/tmp/prgout.txt", "wb");
	if (f) {
		fwrite(buffer, size, 1, f);
		fclose(f);
	}
	size = SDL_SetClipboardText((const char *)buffer);
	free(buffer);
	if (size)
		ERROR_WINDOW("Cannot set clipboard: %s", SDL_GetError());
}
#endif


static void ui_format_sdcard ( void )
{
	if (ARE_YOU_SURE(
		"Formatting your SD-card image file will cause ALL your data,\n"
		"system files (etc!) to be lost, forever!\n"
		"Are you sure to continue this self-destruction sequence? :)"
		,
		0
	)) {
		if (!sdcontent_handle(sdcard_get_size(), NULL, SDCONTENT_FORCE_FDISK))
			INFO_WINDOW("Your SD-card file has been partitioned/formatted\nMEGA65 emulation is about to RESET now!");
	}
	reset_mega65();
}


static char dir_rom[PATH_MAX + 1] = "";


static void ui_update_sdcard ( void )
{
	char fnbuf[PATH_MAX + 1];
	xemu_load_buffer_p = NULL;
	if (!*dir_rom)
		strcpy(dir_rom, sdl_pref_dir);
	// Select ROM image
	if (xemugui_file_selector(
		XEMUGUI_FSEL_OPEN | XEMUGUI_FSEL_FLAG_STORE_DIR,
		"Select your ROM image",
		dir_rom,
		fnbuf,
		sizeof fnbuf
	)) {
		WARNING_WINDOW("Cannot update: you haven't selected a ROM image");
		goto ret;
	}
	// Load selected ROM image into memory, also checks the size!
	if (xemu_load_file(fnbuf, NULL, 0x20000, 0x20000, "Cannot start updating, bad C65/M65 ROM image has been selected!") != 0x20000)
		goto ret;
	// Check the loaded ROM: let's warn the user if it's open-ROMs, since it seems users are often confused to think,
	// that's the right choice for every-day usage.
	rom_detect_date(xemu_load_buffer_p);
	if (rom_date < 0) {
		if (!ARE_YOU_SURE("Selected ROM cannot be identified as a valid C65/MEGA65 ROM. Are you sure to continue?", ARE_YOU_SURE_DEFAULT_NO)) {
			INFO_WINDOW("SD-card system files update was aborted by the user.");
			goto ret;
		}
	} else {
		if (rom_is_openroms) {
			if (!ARE_YOU_SURE(
				"Are you sure you want to use Open-ROMs on your SD-card?\n\n"
				"You've selected a ROM for update which belongs to the\n"
				"Open-ROMs projects. Please note, that Open-ROMs are not\n"
				"yet ready for usage by an average user! For general usage\n"
				"currently, closed-ROMs are recommended! Open-ROMs\n"
				"currently can be interesting for mostly developers and\n"
				"for curious minds.",
				ARE_YOU_SURE_DEFAULT_NO
			))
				goto ret;
		}
		if (rom_is_stub) {
			ERROR_WINDOW(
				"The selected ROM image is an Xemu-internal ROM image.\n"
				"This cannot be used to update your emulated SD-card."
			);
			goto ret;
		}
	}
	DEBUGPRINT("UI: upgrading SD-card system files, ROM %d (%s)" NL, rom_date, rom_name);
	// Copy file to the pref'dir (if not the same as the selected file)
	char fnbuf_target[PATH_MAX];
	strcpy(fnbuf_target, sdl_pref_dir);
	strcpy(fnbuf_target + strlen(sdl_pref_dir), MEGA65_ROM_NAME);
	if (strcmp(fnbuf_target, MEGA65_ROM_NAME)) {
		DEBUGPRINT("Backing up ROM image %s to %s" NL, fnbuf, fnbuf_target);
		if (xemu_save_file(
			fnbuf_target,
			xemu_load_buffer_p,
			0x20000,
			"Cannot save the selected ROM file for the updater"
		))
			goto ret;
	}
	// store our character ROM
	strcpy(fnbuf_target + strlen(sdl_pref_dir), CHAR_ROM_NAME);
	if (xemu_save_file(
		fnbuf_target,
		xemu_load_buffer_p + 0xD000,
		CHAR_ROM_SIZE,
		"Cannot save the extracted CHAR ROM file for the updater"
	))
		goto ret;
	// Call the updater :)
	if (!sdcontent_handle(sdcard_get_size(), NULL, SDCONTENT_DO_FILES | SDCONTENT_OVERWRITE_FILES)) {
		INFO_WINDOW(
			"System files on your SD-card image seems to be updated successfully.\n"
			"Next time you may need this function, you can use MEGA65.ROM which is a backup copy of your selected ROM.\n\n"
			"ROM: %d (%s)\n\n"
			"Your emulated MEGA65 is about to RESET now!", rom_date, rom_name
		);
	}
	reset_mega65();
	rom_unset_requests();
ret:
	if (xemu_load_buffer_p) {
		free(xemu_load_buffer_p);
		xemu_load_buffer_p = NULL;
	}
	// make sure we have the correct detected results again based on the actual memory content,
	// since we've used the detect function on the to-be-loaded ROM to check
	rom_detect_date(main_ram + 0x20000);
}

static void reset_via_hyppo ( void )
{
	if (ARE_YOU_SURE("Are you sure to HYPPO-RESET your emulated machine?", i_am_sure_override | ARE_YOU_SURE_DEFAULT_YES)) {
		if (hypervisor_level_reset())
			ERROR_WINDOW("Currently in hypervisor mode.\nNot possible to trigger a trap now");
	}
}

static void reset_into_custom_rom ( void )
{
	char fnbuf[PATH_MAX + 1];
	if (!*dir_rom)
		strcpy(dir_rom, sdl_pref_dir);
	// Select ROM image
	if (xemugui_file_selector(
		XEMUGUI_FSEL_OPEN | XEMUGUI_FSEL_FLAG_STORE_DIR,
		"Select ROM image",
		dir_rom,
		fnbuf,
		sizeof fnbuf
	))
		return;
	if (rom_load_custom(fnbuf)) {
		if (!reset_mega65_asked())
			WARNING_WINDOW("You refused reset, loaded ROM can be only activated at the next reset.");
	}
}

static void reset_into_utility_menu ( void )
{
	if (reset_mega65_asked()) {
		rom_stubrom_requested = 0;
		rom_initrom_requested = 0;
		hwa_kbd_fake_key(0x20);
		KBD_RELEASE_KEY(0x75);
	}
}

static void reset_into_c64_mode ( void )
{
	if (reset_mega65_asked()) {
		rom_stubrom_requested = 0;
		rom_initrom_requested = 0;
		// we need this, because autoboot disk image would bypass the "go to C64 mode" on 'Commodore key' feature
		// this call will deny disk access, and re-enable on the READY. state.
		inject_register_allow_disk_access();
		hid_set_autoreleased_key(0x75);
		KBD_PRESS_KEY(0x75);	// "MEGA" key is pressed for C64 mode
	}

}

static void reset_generic ( void )
{
	if (reset_mega65_asked()) {
		KBD_RELEASE_KEY(0x75);
		hwa_kbd_fake_key(0);
	}
}

static void reset_into_xemu_stubrom ( void )
{
	if (reset_mega65_asked()) {
		rom_initrom_requested = 0;
		rom_stubrom_requested = 1;
	}
}

static void reset_into_xemu_initrom ( void )
{
	if (reset_mega65_asked()) {
		rom_stubrom_requested = 0;
		rom_initrom_requested = 1;
	}
}

static void reset_into_c65_mode_noboot ( void )
{
	if (reset_mega65_asked()) {
		rom_stubrom_requested = 0;
		rom_initrom_requested = 0;
		inject_register_allow_disk_access();
		KBD_RELEASE_KEY(0x75);
		hwa_kbd_fake_key(0);
	}
}

static void ui_cb_use_default_rom ( const struct menu_st *m, int *query )
{
	if (query) {
		if (!rom_is_overriden)
			*query |= XEMUGUI_MENUFLAG_HIDDEN | XEMUGUI_MENUFLAG_SEPARATOR;
		return;
	}
	if (rom_is_overriden) {
		if (reset_mega65_asked()) {
			rom_unset_requests();
		}
	}
}

#ifdef HAS_UARTMON_SUPPORT
static void ui_cb_start_umon ( const struct menu_st *m, int *query )
{
	int is_active = uartmon_is_active();
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, is_active);
	if (is_active) {
		INFO_WINDOW("UART monitor is already active.\nCurrently stopping it is not supported.");
		return;
	}
	if (!uartmon_init(UMON_DEFAULT_PORT))
		INFO_WINDOW("UART monitor has been starton on " UMON_DEFAULT_PORT);
}
#endif

static void ui_cb_matrix_mode ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, in_the_matrix);
	matrix_mode_toggle(!in_the_matrix);
}

static void ui_cb_hdos_virt ( const struct menu_st *m, int *query )
{
	int status = hypervisor_hdos_virtualization_status(-1, NULL);
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, status);
	(void)hypervisor_hdos_virtualization_status(!status, NULL);
}

static char last_used_dump_directory[PATH_MAX + 1] = "";

static void ui_dump_memory ( void )
{
	char fnbuf[PATH_MAX + 1];
	if (!xemugui_file_selector(
		XEMUGUI_FSEL_SAVE | XEMUGUI_FSEL_FLAG_STORE_DIR,
		"Dump main memory content into file",
		last_used_dump_directory,
		fnbuf,
		sizeof fnbuf
	)) {
		dump_memory(fnbuf);
	}
}

static void ui_dump_colram ( void )
{
	char fnbuf[PATH_MAX + 1];
	if (!xemugui_file_selector(
		XEMUGUI_FSEL_SAVE | XEMUGUI_FSEL_FLAG_STORE_DIR,
		"Dump colour memory content into file",
		last_used_dump_directory,
		fnbuf,
		sizeof fnbuf
	)) {
		xemu_save_file(fnbuf, colour_ram, sizeof colour_ram, "Cannot dump colour RAM content into file");
	}
}

static void ui_dump_hyperram ( void )
{
	char fnbuf[PATH_MAX + 1];
	if (!xemugui_file_selector(
		XEMUGUI_FSEL_SAVE | XEMUGUI_FSEL_FLAG_STORE_DIR,
		"Dump hyperRAM content into file",
		last_used_dump_directory,
		fnbuf,
		sizeof fnbuf
	)) {
		xemu_save_file(fnbuf, slow_ram, SLOW_RAM_SIZE, "Cannot dump hyperRAM content into file");
	}
}

static void ui_emu_info ( void )
{
	char td_stat_str[XEMU_CPU_STAT_INFO_BUFFER_SIZE];
	xemu_get_timing_stat_string(td_stat_str, sizeof td_stat_str);
	char uname_str[100];
	xemu_get_uname_string(uname_str, sizeof uname_str);
	sha1_hash_str rom_now_hash_str;
	sha1_checksum_as_string(rom_now_hash_str, main_ram + 0x20000, 0x20000);
	const char *hdos_root;
	int hdos_virt = hypervisor_hdos_virtualization_status(-1, &hdos_root);
	INFO_WINDOW(
		"DMA chip current revision: %d (F018 rev-%s)\n"
		"ROM version detected: %d %s (%s,%s)\n"
		"ROM SHA1: %s (%s)\n"
		"Last RESET type: %s\n"
		"Hyppo version: %s (%s)\n"
		"HDOS virtualization: %s, root = %s\n"
		"Disk8 = %s\nDisk9 = %s\n"
		"C64 'CPU' I/O port (low 3 bits): DDR=%d OUT=%d\n"
		"Current PC: $%04X (linear: $%07X)\n"
		"Current VIC and I/O mode: %s %s, hot registers are %s\n"
		"\n"
		"Xemu host CPU usage so far: %s\n"
		"Xemu's host OS: %s"
		,
		dma_chip_revision, dma_chip_revision ? "B, new" : "A, old",
		rom_date, rom_name, rom_is_overriden ? "OVERRIDEN" : "installed", rom_is_external ? "external" : "internal",
		rom_now_hash_str, strcmp(rom_hash_str, rom_now_hash_str) ? "MANGLED" : "intact",
		last_reset_type,
		hyppo_version_string, hickup_is_overriden ?  "OVERRIDEN" : "built-in",
		hdos_virt ? "ON" : "OFF", hdos_root,
		sdcard_get_mount_info(0, NULL), sdcard_get_mount_info(1, NULL),
		memory_get_cpu_io_port(0) & 7, memory_get_cpu_io_port(1) & 7,
		cpu65.pc, memory_cpurd2linear_xlat(cpu65.pc),
		vic_iomode < 4 ? iomode_names[vic_iomode] : "?INVALID?", videostd_name, (vic_registers[0x5D] & 0x80) ? "enabled" : "disabled",
		td_stat_str,
		uname_str
	);
}


static void ui_put_screen_text_into_paste_buffer ( void )
{
	char text[8192];
	char *result = xemu_cbm_screen_to_text(
		text,
		sizeof text,
		main_ram + ((vic_registers[0x31] & 0x80) ? (vic_registers[0x18] & 0xE0) << 6 : (vic_registers[0x18] & 0xF0) << 6),	// pointer to screen RAM, try to audo-tected: FIXME: works only in bank0!
		(vic_registers[0x31] & 0x80) ? 80 : 40,		// number of columns, try to auto-detect it
		25,						// number of rows
		(vic_registers[0x18] & 2)			// lowercase font? try to auto-detect by checking selected address chargen addr, LSB
	);
	if (result == NULL)
		return;
	if (*result) {
		if (SDL_SetClipboardText(result))
			ERROR_WINDOW("Cannot insert text into the OS paste buffer: %s", SDL_GetError());
		else
			OSD(-1, -1, "Copied to OS paste buffer.");
	} else
		INFO_WINDOW("Screen is empty, nothing to capture.");
}


static void ui_put_paste_buffer_into_screen_text ( void )
{
	char *t = SDL_GetClipboardText();
	if (t == NULL)
		goto no_clipboard;
	char *t2 = t;
	while (*t2 && (*t2 == '\t' || *t2 == '\r' || *t2 == '\n' || *t2 == ' '))
		t2++;
	if (!*t2)
		goto no_clipboard;
	xemu_cbm_text_to_screen(
		main_ram + ((vic_registers[0x31] & 0x80) ? (vic_registers[0x18] & 0xE0) << 6 : (vic_registers[0x18] & 0xF0) << 6),	// pointer to screen RAM, try to audo-tected: FIXME: works only in bank0!
		(vic_registers[0x31] & 0x80) ? 80 : 40,		// number of columns, try to auto-detect it
		25,						// number of rows
		t2,						// text buffer as input
		(vic_registers[0x18] & 2)			// lowercase font? try to auto-detect by checking selected address chargen addr, LSB
	);
	SDL_free(t);
	return;
no_clipboard:
	if (t)
		SDL_free(t);
	ERROR_WINDOW("Clipboard query error, or clipboard was empty");
}


static void ui_cb_mono_downmix ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, VOIDPTR_TO_INT(m->user_data) == stereo_separation);
	audio_set_stereo_parameters(AUDIO_UNCHANGED_VOLUME, VOIDPTR_TO_INT(m->user_data));
}


static void ui_cb_audio_volume ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, VOIDPTR_TO_INT(m->user_data) == audio_volume);
	audio_set_stereo_parameters(VOIDPTR_TO_INT(m->user_data), AUDIO_UNCHANGED_VOLUME);
}


static void ui_cb_video_standard ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, VOIDPTR_TO_INT(m->user_data) == videostd_id);
	if (VOIDPTR_TO_INT(m->user_data))
		vic_registers[0x6F] |= 0x80;
	else
		vic_registers[0x6F] &= 0x7F;
	configdb.force_videostd = -1;	// turn off possible CLI/config dictated force video mode, otherwise it won't work to change video standard ...
}


static void ui_cb_video_standard_disallow_change ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, vic4_disallow_video_std_change);
	vic4_disallow_video_std_change = vic4_disallow_video_std_change ? 0 : 2;
}


static void ui_cb_fullborders ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, configdb.fullborders);
	configdb.fullborders = !configdb.fullborders;
	vic_readjust_sdl_viewport = 1;		// To force readjust viewport on the next frame open.
}


// FIXME: should be renamed with better name ;)
// FIXME: should be moved into the core
static void ui_cb_toggle_int_inverted ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, !*(int*)m->user_data);
	*(int*)m->user_data = !*(int*)m->user_data;
}


// FIXME: should be renamed with better name ;)
// FIXME: should be moved into the core
static void ui_cb_toggle_int ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, *(int*)m->user_data);
	*(int*)m->user_data = !*(int*)m->user_data;
}


static void ui_cb_sids_enabled ( const struct menu_st *m, int *query )
{
	const int mask = VOIDPTR_TO_INT(m->user_data);
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, (configdb.sidmask & mask));
	configdb.sidmask ^= mask;
}

static void ui_cb_render_scale_quality ( const struct menu_st *m, int *query )
{
	XEMUGUI_RETURN_CHECKED_ON_QUERY(query, VOIDPTR_TO_INT(m->user_data) == configdb.sdlrenderquality);
	char req_str[] = { VOIDPTR_TO_INT(m->user_data) + '0', 0 };
	SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, req_str, SDL_HINT_OVERRIDE);
	configdb.sdlrenderquality = VOIDPTR_TO_INT(m->user_data);
	register_new_texture_creation = 1;
}


/**** MENU SYSTEM ****/


static const struct menu_st menu_video_standard[] = {
	{ "Disallow change by programs",XEMUGUI_MENUID_CALLABLE | XEMUGUI_MENUFLAG_SEPARATOR |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_video_standard_disallow_change, NULL },
	{ "PAL @ 50Hz",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_video_standard, (void*)0 },
	{ "NTSC @ 60Hz",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_video_standard, (void*)1 },
	{ NULL }
};
static const struct menu_st menu_window_size[] = {
	// TODO: unfinished work, see: https://github.com/lgblgblgb/xemu/issues/246
#if 0
	{ "Fullscreen",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	xemugui_cb_windowsize, (void*)0 },
	{ "Window - 100%",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	xemugui_cb_windowsize, (void*)1 },
	{ "Window - 200%",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	xemugui_cb_windowsize, (void*)2 },
#endif
	{ "Fullscreen",			XEMUGUI_MENUID_CALLABLE,	xemugui_cb_windowsize, (void*)0 },
	{ "Window - 100%",		XEMUGUI_MENUID_CALLABLE,	xemugui_cb_windowsize, (void*)1 },
	{ "Window - 200%",		XEMUGUI_MENUID_CALLABLE,	xemugui_cb_windowsize, (void*)2 },
	{ NULL }
};
static const struct menu_st menu_render_scale_quality[] = {
	{ "Nearest pixel sampling",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_render_scale_quality, (void*)0 },
	{ "Linear filtering",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_render_scale_quality, (void*)1 },
	{ "Anisotropic (Direct3D only)",XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_render_scale_quality, (void*)2 },
	{ NULL }
};
static const struct menu_st menu_display[] = {
	{ "Render scale quality",	XEMUGUI_MENUID_SUBMENU,		NULL, menu_render_scale_quality },
	{ "Window size / fullscreen",	XEMUGUI_MENUID_SUBMENU,		NULL, menu_window_size },
	{ "Video standard",		XEMUGUI_MENUID_SUBMENU,		NULL, menu_video_standard },
	{ "Show full borders",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_fullborders, NULL },
	{ "Show drive LED",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK |
					XEMUGUI_MENUFLAG_SEPARATOR,	ui_cb_toggle_int, (void*)&configdb.show_drive_led },
#ifdef XEMU_FILES_SCREENSHOT_SUPPORT
	{ "Screenshot",			XEMUGUI_MENUID_CALLABLE,	xemugui_cb_set_integer_to_one, &registered_screenshot_request },
#endif
	{ "Screen to OS paste buffer",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_put_screen_text_into_paste_buffer },
	{ "OS paste buffer to screen",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_put_paste_buffer_into_screen_text },
	{ NULL }
};
static const struct menu_st menu_sdcard[] = {
	{ "Re-format SD image",		XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_format_sdcard },
	{ "Update files on SD image",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_update_sdcard },
	{ NULL }
};
static const struct menu_st menu_reset[] = {
	{ "Reset back to default ROM",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_use_default_rom, NULL				},
	{ "Reset", 			XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_generic		},
	{ "Reset without autoboot",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_into_c65_mode_noboot	},
	{ "Reset into utility menu",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_into_utility_menu	},
	{ "Reset into C64 mode",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_into_c64_mode		},
	{ "Reset into Xemu stub-ROM",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_into_xemu_stubrom	},
	{ "Reset into boot init-ROM",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_into_xemu_initrom	},
	{ "Reset via HYPPO",		XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_via_hyppo		},
	{ "Reset CPU only",		XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_mega65_cpu_only	},
	{ "Reset/use custom ROM file",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, reset_into_custom_rom	},
	{ NULL }
};
static const struct menu_st menu_inputdevices[] = {
	{ "Enable mouse grab + emu",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	xemugui_cb_set_mouse_grab, NULL },
	{ "Use OSD key debugger",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	xemugui_cb_osd_key_debugger, NULL },
	{ "Swap emulated joystick port",XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, input_toggle_joy_emu },
#if 0
	{ "Devices as joy port 2 (vs 1)",	XEMUGUI_MENUID_SUBMENU,		NULL, menu_joy_devices },
#endif
	{ NULL }
};
static const struct menu_st menu_debug[] = {
#ifdef HAS_UARTMON_SUPPORT
	{ "Start umon on " UMON_DEFAULT_PORT,
					XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_start_umon, NULL },
#endif
	{ "Allow freezer trap",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_toggle_int, (void*)&configdb.allowfreezer },
	{ "Try external ROM first",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_toggle_int, (void*)&rom_from_prefdir_allowed },
	{ "HDOS virtualization",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_hdos_virt, NULL },
	{ "Matrix mode",		XEMUGUI_MENUID_CALLABLE | XEMUGUI_MENUFLAG_SEPARATOR |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_matrix_mode, NULL },
	{ "Emulation state info",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_emu_info },
	{ "Dump main RAM info file",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_dump_memory },
	{ "Dump colour RAM into file",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_dump_colram },
	{ "Dump hyperRAM into file",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_dump_hyperram },
	{ NULL }
};
#ifdef HAVE_XEMU_EXEC_API
static const struct menu_st menu_help[] = {
	{ "Xemu MEGA65 help page",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_web_help_main, "help" },
	{ "Check update / useful MEGA65 links",
					XEMUGUI_MENUID_CALLABLE,	xemugui_cb_web_help_main, "versioncheck" },
	{ "Xemu download page",		XEMUGUI_MENUID_CALLABLE,	xemugui_cb_web_help_main, "downloadpage" },
	{ "Download MEGA65 book",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_web_help_main, "downloadmega65book" },
	{ NULL }
};
#endif
static const struct menu_st menu_d81[] = {
	{ "Attach user D81 on drv-8",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_attach_d81, (void*)0 },
	{ "Use internal D81 on drv-8",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_detach_d81, (void*)0 },
	{ "Attach user D81 on drv-9",	XEMUGUI_MENUID_CALLABLE,	ui_cb_attach_d81, (void*)1 },
	{ "Detach user D81 on drv-9",	XEMUGUI_MENUID_CALLABLE,	ui_cb_detach_d81, (void*)1 },
	{ NULL }
};
static const struct menu_st menu_audio_stereo[] = {
	{ "Hard stereo separation",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*) 100 },
	{ "Stereo separation 80%",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*)  80 },
	{ "Stereo separation 60%",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*)  60 },
	{ "Stereo separation 40%",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*)  40 },
	{ "Stereo separation 20%",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*)  20 },
	{ "Full mono downmix (0%)",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*)   0 },
	{ "Stereo separation -20%",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*) -20 },
	{ "Stereo separation -40%",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*) -40 },
	{ "Stereo separation -60%",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*) -60 },
	{ "Stereo separation -80%",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*) -80 },
	{ "Hard stereo - reserved",	XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_mono_downmix, (void*)-100 },
	{ NULL }
};
static const struct menu_st menu_audio_volume[] = {
	{ "100%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*) 100 },
	{ "90%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  90 },
	{ "80%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  80 },
	{ "70%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  70 },
	{ "60%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  60 },
	{ "50%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  50 },
	{ "40%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  40 },
	{ "30%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  30 },
	{ "20%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  20 },
	{ "10%",			XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_audio_volume, (void*)  10 },
	{ NULL }
};
static const struct menu_st menu_audio_sids[] = {
	{ "SID @ $D400",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_sids_enabled, (void*)1 },
	{ "SID @ $D420",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_sids_enabled, (void*)2 },
	{ "SID @ $D440",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_sids_enabled, (void*)4 },
	{ "SID @ SD460",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_sids_enabled, (void*)8 },
	{ NULL }
};
static const struct menu_st menu_audio[] = {
	{ "Audio output",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_toggle_int_inverted, (void*)&configdb.nosound },
	{ "OPL3 emulation",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	ui_cb_toggle_int_inverted, (void*)&configdb.noopl3 },
	{ "Clear audio registers",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, audio65_clear_regs },
	{ "Emulated SIDs",		XEMUGUI_MENUID_SUBMENU,		NULL, menu_audio_sids   },
	{ "Stereo separation",		XEMUGUI_MENUID_SUBMENU,		NULL, menu_audio_stereo },
	{ "Master volume",		XEMUGUI_MENUID_SUBMENU,		NULL, menu_audio_volume },
	{ NULL }
};
static const struct menu_st menu_main[] = {
	{ "Display",			XEMUGUI_MENUID_SUBMENU,		NULL, menu_display },
	{ "Input devices",		XEMUGUI_MENUID_SUBMENU,		NULL, menu_inputdevices },
	{ "Audio",			XEMUGUI_MENUID_SUBMENU,		NULL, menu_audio   },
	{ "SD-card",			XEMUGUI_MENUID_SUBMENU,		NULL, menu_sdcard  },
	{ "FD D81",			XEMUGUI_MENUID_SUBMENU,		NULL, menu_d81     },
	{ "Reset / ROM switching",	XEMUGUI_MENUID_SUBMENU,		NULL, menu_reset   },
	{ "Debug / Advanced",		XEMUGUI_MENUID_SUBMENU,		NULL, menu_debug   },
	{ "Run PRG directly",		XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_run_prg_by_browsing },
#ifdef CBM_BASIC_TEXT_SUPPORT
	{ "Save BASIC as text",		XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_user_data, ui_save_basic_as_text },
#endif
#ifdef XEMU_ARCH_WIN
	{ "System console",		XEMUGUI_MENUID_CALLABLE |
					XEMUGUI_MENUFLAG_QUERYBACK,	xemugui_cb_sysconsole, NULL },
#endif
#ifdef HAVE_XEMU_EXEC_API
	{ "Help (online)",		XEMUGUI_MENUID_SUBMENU,		NULL, menu_help },
#endif
	{ "About",			XEMUGUI_MENUID_CALLABLE,	xemugui_cb_about_window, NULL },
#ifdef HAVE_XEMU_EXEC_API
	{ "Browse system folder",	XEMUGUI_MENUID_CALLABLE,	xemugui_cb_native_os_prefdir_browser, NULL },
#endif
	{ "Quit",			XEMUGUI_MENUID_CALLABLE,	xemugui_cb_call_quit_if_sure, NULL },
	{ NULL }
};


void ui_enter ( void )
{
	DEBUGGUI("UI: handler has been called." NL);
	if (xemugui_popup(menu_main)) {
		DEBUGPRINT("UI: oops, POPUP does not worked :(" NL);
	}
}
