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
#include "xemu/emutools_files.h"
#include "sdcard.h"
#include "xemu/f011_core.h"
#include "xemu/d81access.h"
#include "mega65.h"
#include "xemu/cpu65.h"
#include "io_mapper.h"
#include "sdcontent.h"
#include "memcontent.h"
#include "hypervisor.h"

#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>

#define COMPRESSED_SD
#define USE_KEEP_BUSY

#ifdef USE_KEEP_BUSY
#	define KEEP_BUSY(n)	keep_busy=n
#else
#	define KEEP_BUSY(n)
#endif

#define SD_ST_SDHC	0x10

// FIXME: invent same sane value here (the perfect solution would be to use sdcontent/mfat subsystem to query the boundaries of the FAT partitions)
#define MIN_MOUNT_SECTOR_NO 10

#define SD_BUFFER_POS 0x0E00
#define FD_BUFFER_POS 0x0C00


static Uint8	sd_regs[0x30];
static int	sdfd;			// SD-card controller emulation, UNIX file descriptor of the open image file
Uint8		sd_status;		// SD-status byte
static int	sdhc_mode = 1;
static Uint32	sdcard_size_in_blocks;	// SD card size in term of number of 512 byte blocks
static int	sdcard_bytes_read = 0;
static int	sd_fill_mode = 0;
static Uint8	sd_fill_value = 0;
static int	default_d81_is_from_sd;
#ifdef COMPRESSED_SD
static int	sd_compressed = 0;
static off_t	sd_bdata_start;
static int	compressed_block;
#endif
static int	sd_is_read_only;
#ifdef USE_KEEP_BUSY
static int	keep_busy = 0;
#endif
// 4K buffer space: Actually the SD buffer _IS_ inside this, also the F011 buffer should be (FIXME: that is not implemented yet right now!!)
Uint8		disk_buffers[0x1000];
static Uint8	sd_fill_buffer[512];	// Only used by the sd fill mode write command
Uint8		*disk_buffer_cpu_view;
// FIXME/TODO: unfortunately it seems I/O mapping of SD-buffer is buggy even on
// real MEGA65 and the new code to have mapping allowing FD _and_ SD buffer mount
// causes problems. So let's revert back to a fixed "SD-only" solution for now.
Uint8		*disk_buffer_io_mapped = disk_buffers + SD_BUFFER_POS;

static const char	*default_d81_basename[2]	= { "mega65.d81",	"mega65_9.d81"    };
static const char	*default_d81_disk_label[2]	= { "XEMU EXTERNAL",	"XEMU EXTERNAL 9" };
const char		xemu_external_d81_signature[]	= "\xFF\xFE<{[(XemuExternalDiskMagic)]}>";

Uint8 sd_reg9;

// Disk image mount information for the two units
static struct {
	char	*current_name;
	int	internal;		// is internal mount? FIXME: in this source it's a hard to understand 3-stated logic being 0,1 or -1 ... let's fix this!
	char	*force_external_name;
	Uint32	at_sector;		// valid only if "internal", internal mount sector number
	Uint32	at_sector_initial;	// internal mount sector number during only the first (reset/poweron) trap
	int	monitoring_initial;	// if true, at_sector_initial is monitored and changed, if false, not anymore
} mount_info[2];

#ifdef VIRTUAL_DISK_IMAGE_SUPPORT

#define VIRTUAL_DISK_BLOCKS_PER_CHUNK	2048
// Simulate a 4Gbyte card for virtual disk (the number is: number of blocks). Does not matter since only the actual written blocks stored in memory from it
#define VIRTUAL_DISK_SIZE_IN_BLOCKS	8388608U

#define RANGE_MAP_SIZE 256

struct virtdisk_chunk_st {
	struct	virtdisk_chunk_st *next;	// pointer to the next chunk, or NULL, if it was the last (in that case vdisk.tail should point to this chunk)
	int	used_blocks;			// number of used blocks in this chunk (up to vidsk.blocks_per_chunk)
	Uint32	block_no_min;			// optimization: lowest numbered block inside _this_ chunk of blocks
	Uint32	block_no_max;			// optimization: highest numbered block inside _this_ chunk of blocks
	Uint8	*base;				// base pointer of the data area on this chunk, 512 bytes per block then
	Uint32	list[];				// block number list for this chunk (see: used_blocks) [C99/C11 flexible array members syntax]
};
struct virtdisk_st {
	struct	virtdisk_chunk_st *head, *tail;	// pointers to the head and tail of the linked list of chunks, or NULL for both, if there is nothing yet
	int	blocks_per_chunk;		// number of blocks handled by a chunk
	int	allocation_size;		// pre-calculated to have number of bytes allocated for a chunk, including the data area + struct itself
	int	base_offset;			// pre-calculated to have the offset to the data area inside a chunk
	int	all_chunks;			// number of all currently allocated chunks
	int	all_blocks;			// number of all currently USED blocks within all chunks
	Uint8	range_map[RANGE_MAP_SIZE];	// optimization: store here if a given range is stored in memory, eliminating the need to search all chunks over, if not
	Uint32	range_map_divisor;		// optimization: for the above, to form range number from block number
	int	mode;				// what mode is used, 0=no virtual disk
};

static struct virtdisk_st vdisk = { .head = NULL };

static void virtdisk_destroy ( void )
{
	if (vdisk.head) {
		struct virtdisk_chunk_st *v = vdisk.head;
		int ranges = 0;
		for (unsigned int a = 0; a < RANGE_MAP_SIZE; a++)
			for (Uint8 m = 0x80; m; m >>= 1)
				if ((vdisk.range_map[a] & m))
					ranges++;
		DEBUGPRINT("SDCARD: VDISK: destroying %d chunks (active data: %d blocks, %dKbytes, %d%%, ranges: %d/%d) of storage." NL,
			vdisk.all_chunks, vdisk.all_blocks, vdisk.all_blocks >> 1,
			100 * vdisk.all_blocks / (vdisk.all_chunks * vdisk.blocks_per_chunk),
			ranges, RANGE_MAP_SIZE * 8
		);
		while (v) {
			struct virtdisk_chunk_st *next = v->next;
			free(v);
			v = next;
		}
	}
	vdisk.head = NULL;
	vdisk.tail = NULL;
	vdisk.all_chunks = 0;
	vdisk.all_blocks = 0;
	memset(vdisk.range_map, 0, RANGE_MAP_SIZE);
}


static void virtdisk_init ( int blocks_per_chunk, Uint32 total_number_of_blocks )
{
	virtdisk_destroy();
	vdisk.blocks_per_chunk = blocks_per_chunk;
	vdisk.base_offset = sizeof(struct virtdisk_chunk_st) + (sizeof(Uint32) * blocks_per_chunk);
	vdisk.allocation_size = (blocks_per_chunk << 9) + vdisk.base_offset;
	vdisk.range_map_divisor = (total_number_of_blocks / (RANGE_MAP_SIZE * 8)) + 1;
	DEBUGPRINT("SDCARD: VDISK: %d blocks (%dKbytes) per chunk, range-divisor is %u" NL, blocks_per_chunk, blocks_per_chunk >> 1, vdisk.range_map_divisor);
}


// Note: you must take care to call this function with "block" not outside of the desired capacity or some logic (range_map) will cause crash.
// That's not a problem, as this function is intended to use as storage backend, thus boundary checking should be done BEFORE you call this!
static Uint8 *virtdisk_search_block ( Uint32 block, int do_allocate )
{
	struct virtdisk_chunk_st *v;
	Uint32 range_index = block / vdisk.range_map_divisor;
	Uint8  range_mask  = 1U << (range_index & 7U);
	range_index >>= 3U;
	if (XEMU_LIKELY(vdisk.head && (vdisk.range_map[range_index] & range_mask))) {
		v = vdisk.head;
		do {
			if (block >= v->block_no_min && block <= v->block_no_max)
				for (unsigned int a = 0; a < v->used_blocks; a++)
					if (v->list[a] == block)
						return v->base + (a << 9);
			v = v->next;
		} while (v);
	}
	// if we can't found the block, and do_allocate is false, we return with zero
	// otherwise we continue to allocate a block
	if (!do_allocate)
		return NULL;
	//DEBUGPRINT("RANGE-INDEX=%d RANGE-MASK=%d" NL, range_index, range_mask);
	// We're instructed to allocate block if cannot be found already elsewhere
	// This condition checks if we have room in "tail" or there is any chunk already at all (tail is not NULL)
	if (vdisk.tail && (vdisk.tail->used_blocks < vdisk.blocks_per_chunk)) {
		v = vdisk.tail;
		// OK, we had room in the tail, so put the block there
		vdisk.all_blocks++;
		if (block < v->block_no_min)
			v->block_no_min = block;
		if (block > v->block_no_max)
			v->block_no_max = block;
		vdisk.range_map[range_index] |= range_mask;
		v->list[v->used_blocks] = block;
		return v->base + ((v->used_blocks++) << 9);
	}
	// we don't have room in the tail, or not any chunk yet AT ALL!
	// Let's allocate a new chunk, and use the first block from it!
	v = xemu_malloc(vdisk.allocation_size);	// xemu_malloc() is safe, it malloc()s space or abort the whole program if it cannot ...
	v->next = NULL;
	v->base = (Uint8*)v + vdisk.base_offset;
	vdisk.all_chunks++;
	if (vdisk.tail)
		vdisk.tail->next = v;
	vdisk.tail = v;
	if (!vdisk.head)
		vdisk.head = v;
	v->list[0] = block;
	v->used_blocks = 1;
	v->block_no_min = block;
	v->block_no_max = block;
	vdisk.range_map[range_index] |= range_mask;
	vdisk.all_blocks++;
	return v->base;	// no need to add block offset in storage, always the first block is served in a new chunk!
}


static inline void virtdisk_write_block ( Uint32 block, Uint8 *buffer )
{
	// Check if the block is all zero. If yes, we can omit write if the block is not cached
	Uint8 *vbuf = virtdisk_search_block(block, has_block_nonzero_byte(buffer));
	if (vbuf)
		memcpy(vbuf, buffer, 512);
	// TODO: Like with the next function, this whole strategy needs to be changed when mixed operation is used!!!!!
}


static inline void virtdisk_read_block ( Uint32 block, Uint8 *buffer )
{
	Uint8 *vbuf = virtdisk_search_block(block, 0);
	if (vbuf)
		memcpy(buffer, vbuf, 512);
	else
		memset(buffer, 0, 512);	// if not found, we fake an "all zero" answer (TODO: later in mixed operation, image+vdisk, this must be modified!)
}
#endif


// define the callback, d81access call this, we can dispatch the change in FDC config to the F011 core emulation this way, automatically.
// So basically these stuff binds F011 emulation and d81access so the F011 emulation used the d81access framework.
void d81access_cb_chgmode ( const int which, const int mode ) {
	const int have_disk = ((mode & 0xFF) != D81ACCESS_EMPTY);
	const int can_write = (!(mode & D81ACCESS_RO));
	if (which < 2)
		DEBUGPRINT("SDCARD: configuring F011 FDC (#%d) with have_disk=%d, can_write=%d" NL, which, have_disk, can_write);
	fdc_set_disk(which, have_disk, can_write);
}
// Here we implement F011 core's callbacks using d81access (and yes, F011 uses 512 bytes long sectors for real)
int fdc_cb_rd_sec ( const int which, Uint8 *buffer, const Uint8 side, const Uint8 track, const Uint8 sector )
{
	const int ret = d81access_read_sect(which, buffer, side, track, sector, 512);
	DEBUG("SDCARD: D81: reading sector at d81_pos=(%d,%d,%d), return value=%d" NL, side, track, sector, ret);
	return ret;
}
int fdc_cb_wr_sec ( const int which, Uint8 *buffer, const Uint8 side, const Uint8 track, const Uint8 sector )
{
	const int ret = d81access_write_sect(which, buffer, side, track, sector , 512);
	DEBUG("SDCARD: D81: writing sector at d81_pos=(%d,%d,%d), return value=%d" NL, side, track, sector, ret);
	return ret;
}


static void sdcard_shutdown ( void )
{
	d81access_close_all();
	if (sdfd >= 0) {
		close(sdfd);
		sdfd = -1;
	}
#ifdef VIRTUAL_DISK_IMAGE_SUPPORT
	virtdisk_destroy();
#endif
}


#ifdef COMPRESSED_SD
static int detect_compressed_image ( int fd )
{
	static const char compressed_marker[] = "XemuBlockCompressedImage000";
	Uint8 buf[512];
	if (lseek(sdfd, 0, SEEK_SET) == OFF_T_ERROR || xemu_safe_read(fd, buf, 512) != 512)
		return -1;
	if (memcmp(buf, compressed_marker, sizeof compressed_marker)) {
		DEBUGPRINT("SDCARD: image is not compressed" NL);
		return 0;
	}
	if (((buf[0x1C] << 16) | (buf[0x1D] << 8) | buf[0x1E]) != 3) {
		ERROR_WINDOW("Invalid/unknown compressed image format");
		return -1;
	}
	sdcard_size_in_blocks = (buf[0x1F] << 16) | (buf[0x20] << 8) | buf[0x21];
	DEBUGPRINT("SDCARD: compressed image with %u blocks" NL, sdcard_size_in_blocks);
	sd_bdata_start = 3 * sdcard_size_in_blocks + 0x22;
	sd_is_read_only = O_RDONLY;
	return 1;
}
#endif


Uint32 sdcard_get_size ( void )
{
	return sdcard_size_in_blocks;
}


static XEMU_INLINE void set_disk_buffer_cpu_view ( void )
{
	disk_buffer_cpu_view =  disk_buffers + ((sd_regs[9] & 0x80) ? SD_BUFFER_POS : FD_BUFFER_POS);
}


static void show_card_init_done ( void )
{
	DEBUGPRINT("SDCARD: card init done, size=%u Mbytes (%s), virtsd_mode=%s, default_D81_from_sd=%d" NL,
		sdcard_size_in_blocks >> 11,
		sd_is_read_only ? "R/O" : "R/W",
		vdisk.mode ? "IN-MEMORY-VIRTUAL" : "image-file",
		default_d81_is_from_sd
	);
}


int sdcard_init ( const char *fn, const int virtsd_flag, const int default_d81_is_from_sd_in )
{
	default_d81_is_from_sd = default_d81_is_from_sd_in;
	memset(sd_regs, 0, sizeof sd_regs);			// reset all registers
	memcpy(D6XX_registers + 0x80, sd_regs, sizeof sd_regs);	// be sure, this is in sync with the D6XX register backend (used by io_mapper which also calls us ...)
	set_disk_buffer_cpu_view();	// make sure to initialize disk_buffer_cpu_view based on current sd_regs[9] otherwise disk_buffer_cpu_view may points to NULL when referenced!
	for (int a = 0; a < 2; a++) {
		mount_info[a].current_name = xemu_strdup("<INIT>");
		mount_info[a].internal = -1;
		mount_info[a].force_external_name = NULL;
		mount_info[a].at_sector = 0;
		mount_info[a].at_sector_initial = 0;
		mount_info[a].monitoring_initial = 0;
	}
	int just_created_image_file =  0;	// will signal to format image automatically for the user (if set, by default it's clear, here)
	char fnbuf[PATH_MAX + 1];
#ifdef VIRTUAL_DISK_IMAGE_SUPPORT
	if (virtsd_flag) {
		virtdisk_init(VIRTUAL_DISK_BLOCKS_PER_CHUNK, VIRTUAL_DISK_SIZE_IN_BLOCKS);
		vdisk.mode = 1;
	} else
#else
		vdisk.mode = 0;
#endif
	d81access_init();
	atexit(sdcard_shutdown);
	fdc_init(disk_buffers + FD_BUFFER_POS);	// initialize F011 emulation
	KEEP_BUSY(0);
	sd_status = 0;
	memset(sd_fill_buffer, sd_fill_value, 512);
#ifdef VIRTUAL_DISK_IMAGE_SUPPORT
	if (vdisk.mode) {
		sdfd = -1;
		sd_is_read_only = 0;
		sdcard_size_in_blocks = VIRTUAL_DISK_SIZE_IN_BLOCKS;
#ifdef COMPRESSED_SD
		sd_compressed = 0;
#endif
		show_card_init_done();
#ifdef SD_CONTENT_SUPPORT
		sdcontent_handle(sdcard_size_in_blocks, fn, SDCONTENT_FORCE_FDISK);
#endif
		return 0;
	}
#endif
retry:
	sd_is_read_only = O_RDONLY;
	sdfd = xemu_open_file(fn, O_RDWR, &sd_is_read_only, fnbuf);
	sd_is_read_only = (sd_is_read_only != XEMU_OPEN_FILE_FIRST_MODE_USED);
	if (sdfd < 0) {
		int r = errno;
		ERROR_WINDOW("Cannot open SD-card image %s, SD-card access won't work! ERROR: %s", fnbuf, strerror(r));
		DEBUG("SDCARD: cannot open image %s" NL, fn);
		if (r == ENOENT && !strcmp(fn, SDCARD_NAME)) {
			r = QUESTION_WINDOW(
				"No|Yes"
				,
				"Default SDCARD image does not exist. Would you like me to create one for you?\n"
				"Note: it will be a 4Gbytes long file, since this is the minimal size for an SDHC card,\n"
				"what MEGA65 needs. Do not worry, it's a 'sparse' file on most modern OSes which does\n"
				"not takes as much disk space as its displayed size suggests.\n"
				"This is unavoidable to emulate something uses an SDHC-card."
			);
			if (r) {
				r = xemu_create_large_empty_file(fnbuf, 4294967296UL, 1);
				if (r) {
					ERROR_WINDOW("Couldn't create SD-card image file (hint: do you have enough space?)\nError message was: %s", strerror(r));
				} else {
					just_created_image_file = 1;	// signal the rest of the code, that we have a brand new image file, which can be safely auto-formatted even w/o asking the user
					goto retry;
				}
			}
		}
	} else {
		if (sd_is_read_only)
			INFO_WINDOW("SDCARD: image file %s could be open only in R/O mode!", fnbuf);
		else
			DEBUG("SDCARD: image file re-opened in RD/WR mode, good" NL);
		// Check size!
		DEBUG("SDCARD: cool, SD-card image %s (as %s) is open" NL, fn, fnbuf);
		off_t size_in_bytes = xemu_safe_file_size_by_fd(sdfd);
		if (size_in_bytes == OFF_T_ERROR) {
			ERROR_WINDOW("Cannot query the size of the SD-card image %s, SD-card access won't work! ERROR: %s", fn, strerror(errno));
			close(sdfd);
			sdfd = -1;
			return sdfd;
		}
#ifdef COMPRESSED_SD
		sd_compressed = detect_compressed_image(sdfd);
		if (sd_compressed < 0) {
			ERROR_WINDOW("Error while trying to detect compressed SD-image");
			sdcard_size_in_blocks = 0; // just cheating to trigger error handling later
		}
#endif
		sdcard_size_in_blocks = size_in_bytes >> 9;
		DEBUG("SDCARD: detected size in Mbytes: %d" NL, (int)(size_in_bytes >> 20));
		if (size_in_bytes < 67108864UL) {
			ERROR_WINDOW("SD-card image is too small! Min required size is 64Mbytes!");
			close(sdfd);
			sdfd = -1;
			return sdfd;
		}
		if (size_in_bytes & (off_t)511) {
			ERROR_WINDOW("SD-card image size is not multiple of 512 bytes!!");
			close(sdfd);
			sdfd = -1;
			return sdfd;
		}
		if (size_in_bytes > 34359738368UL) {
			ERROR_WINDOW("SD-card image is too large! Max allowed size is 32Gbytes!");
			close(sdfd);
			sdfd = -1;
			return sdfd;
		}
	}
	if (sdfd >= 0) {
		show_card_init_done();
		//sdcontent_handle(sdcard_size_in_blocks, NULL, SDCONTENT_ASK_FDISK | SDCONTENT_ASK_FILES);
		if (just_created_image_file) {
			just_created_image_file = 0;
			// Just created SD-card image file by Xemu itself! So it's nice if we format it for the user at this point!
			if (!sdcontent_handle(sdcard_size_in_blocks, NULL, SDCONTENT_FORCE_FDISK)) {
				INFO_WINDOW("Your just created SD-card image file has\nbeen auto-fdisk/format'ed by Xemu. Great :).");
				sdcontent_write_rom_stub();
			}
		}
	}
	if (!virtsd_flag && sdfd >= 0) {
		static const char msg[] = " on the SD-card image.\nPlease use UI menu: Disks -> SD-card -> Update files ...\nUI can be accessed with right mouse click into the emulator window.";
		int r = sdcontent_check_xemu_signature();
		if (r < 0) {
			ERROR_WINDOW("Warning! Cannot read SD-card to get Xemu signature!");
		} else if (r == 0) {
			INFO_WINDOW("Cannot find Xemu's signature%s", msg);
		} else if (r < MEMCONTENT_VERSION_ID) {
			INFO_WINDOW("Xemu's singature is too old%s to upgrade", msg);
		} else if (r > MEMCONTENT_VERSION_ID) {
			INFO_WINDOW("Xemu's signature is too new%s to DOWNgrade", msg);
		}
		// TODO: check MEGA65.D81 on the disk, and get its starting sector (!) number. So we can know if mount of MEGA65.D81 is
		// requested later by sector number given in D81 "mount" registers. We can use this information to
		// give a D81 as an external mount instead then, to please users not to have the default D81 "inside"
		// the SD-card image rather than in their native file system.
	}
	return sdfd;
}


static XEMU_INLINE Uint32 U8A_TO_U32 ( Uint8 *a )
{
	return ((Uint32)a[0]) | ((Uint32)a[1] << 8) | ((Uint32)a[2] << 16) | ((Uint32)a[3] << 24);
}


static int host_seek ( Uint32 block )
{
	if (sdfd < 0)
		FATAL("host_seek is called with invalid sdfd!");	// FIXME: this check can go away, once we're sure it does not apply!
	off_t offset;
#ifdef COMPRESSED_SD
	if (sd_compressed) {
		offset = block * 3 + 0x22;
		if (lseek(sdfd, offset, SEEK_SET) != offset)
			FATAL("SDCARD: SEEK: compressed image host-OS seek failure: %s", strerror(errno));
		Uint8 buf[3];
		if (xemu_safe_read(sdfd, buf, 3) != 3)
			FATAL("SDCARD: SEEK: compressed image host-OK pre-read failure: %s", strerror(errno));
		compressed_block = (buf[0] & 0x80);
		buf[0] &= 0x7F;
		offset = ((off_t)((buf[0] << 16) | (buf[1] << 8) | buf[2]) << 9) + sd_bdata_start;
		//DEBUGPRINT("SD-COMP: got address: %d" NL, (int)offset);
	} else {
		offset = (off_t)block << 9;
	}
#else
	offset = (off_t)block << 9;
#endif
	if (lseek(sdfd, offset, SEEK_SET) != offset)
		FATAL("SDCARD: SEEK: image seek host-OS failure: %s", strerror(errno));
	return 0;
}


// static int status_read_counter = 0;

// This tries to emulate the behaviour, that at least another one status query
// is needed to BUSY flag to go away instead of with no time. DUNNO if it is needed at all.
static Uint8 sdcard_read_status ( void )
{
	Uint8 ret = sd_status;
	DEBUG("SDCARD: reading SD status $D680 result is $%02X PC=$%04X" NL, ret, cpu65.pc);
//	if (status_read_counter > 20) {
//		sd_status &= ~(SD_ST_BUSY1 | SD_ST_BUSY0);
//		status_read_counter = 0;
//		DEBUGPRINT(">>> SDCARD resetting status read counter <<<" NL);
//	}
//	status_read_counter++;
	// Suggested by @Jimbo on MEGA65/Xemu Discord: a workaround to report busy status
	// if external SD bus is used, always when reading status. It seems to be needed now
	// with newer hyppo, otherwise it misinterprets the SDHC detection method on the external bus!
	if (ret & SD_ST_EXT_BUS)
		ret |= SD_ST_BUSY1 | SD_ST_BUSY0;
#ifdef USE_KEEP_BUSY
	if (!keep_busy)
		sd_status &= ~(SD_ST_BUSY1 | SD_ST_BUSY0);
#endif
	//ret |= 0x10;	// FIXME? according to Paul, the old "SDHC" flag stuck to one always from now
	return ret;
}


// TODO: later we need to deal with buffer selection, whatever
static XEMU_INLINE Uint8 *get_buffer_memory ( int is_write )
{
	// Currently the only buffer available in Xemu is the SD buffer, UNLESS it's a write operation and "fill mode" is used
	// (sd_fill_buffer is just filled with a single byte value)
	return (is_write && sd_fill_mode) ? sd_fill_buffer : (disk_buffers + SD_BUFFER_POS);
}


int sdcard_read_block ( Uint32 block, Uint8 *buffer )
{
	if (block >= sdcard_size_in_blocks) {
		DEBUGPRINT("SDCARD: SEEK: invalid block was requested to READ: block=%u (max_block=%u) @ PC=$%04X" NL, block, sdcard_size_in_blocks, cpu65.pc);
		return -1;
	}
#ifdef VIRTUAL_DISK_IMAGE_SUPPORT
	if (vdisk.mode) {
		virtdisk_read_block(block, buffer);
		return 0;
	}
#endif
	if (host_seek(block))
		return -1;
	if (xemu_safe_read(sdfd, buffer, 512) == 512)
		return 0;
	else
		return -1;
}


int sdcard_write_block ( Uint32 block, Uint8 *buffer )
{
	if (block >= sdcard_size_in_blocks) {
		DEBUGPRINT("SDCARD: SEEK: invalid block was requested to WRITE: block=%u (max_block=%u) @ PC=$%04X" NL, block, sdcard_size_in_blocks, cpu65.pc);
		return -1;
	}
	if (sd_is_read_only)	// on compressed SD image, it's also set btw
		return -1;	// read-only SD-card
#ifdef VIRTUAL_DISK_IMAGE_SUPPORT
	if (vdisk.mode) {
		virtdisk_write_block(block, buffer);
		return 0;
	}
#endif
	if (host_seek(block))
		return -1;
	if (xemu_safe_write(sdfd, buffer, 512) == 512)
		return 0;
	else
		return -1;
}


/* Lots of TODO's here:
 * + study M65's quite complex error handling behaviour to really match ...
 * + with external D81 mounting: have a "fake D81" on the card, and redirect accesses to that, if someone if insane enough to try to access D81 at the SD-level too ...
 * + In general: SD emulation is "too fast" done in zero emulated CPU time, which can affect the emulation badly if an I/O-rich task is running on Xemu/M65
 * */
static void sdcard_block_io ( Uint32 block, int is_write )
{
	static int protect_important_blocks = 1;
	DEBUG("SDCARD: %s block #%u @ PC=$%04X" NL,
		is_write ? "writing" : "reading",
		block, cpu65.pc
	);
	if (XEMU_UNLIKELY(is_write && (block == 0 || block == XEMU_INFO_SDCARD_BLOCK_NO) && sdfd >= 0 && protect_important_blocks)) {
		if (protect_important_blocks == 2) {
			goto error;
		} else {
			char msg[128];
			sprintf(msg, "Program tries to overwrite SD sector #%d!\nUnless you fdisk/format your card, it's not something you want.", block);
			switch (QUESTION_WINDOW("Reject this|Reject all|Allow this|Allow all", msg)) {
				case 0:
					goto error;
				case 1:
					protect_important_blocks = 2;
					goto error;
				case 2:
					break;
				case 3:
					protect_important_blocks = 0;
					break;
			}
		}
	}
	if (XEMU_UNLIKELY(sd_status & SD_ST_EXT_BUS)) {
		DEBUGPRINT("SDCARD: bus #1 is empty" NL);
		// FIXME: what kind of error we should create here?????
		sd_status |= SD_ST_ERROR | SD_ST_FSM_ERROR | SD_ST_BUSY1 | SD_ST_BUSY0;
		KEEP_BUSY(1);
		return;
	}
	Uint8 *buffer = get_buffer_memory(is_write);
	int ret = is_write ? sdcard_write_block(block, buffer) : sdcard_read_block(block, buffer);
	if (ret || !sdhc_mode) {
	error:
		sd_status |= SD_ST_ERROR | SD_ST_FSM_ERROR; // | SD_ST_BUSY1 | SD_ST_BUSY0;
			sd_status |= SD_ST_BUSY1 | SD_ST_BUSY0;
			//KEEP_BUSY(1);
		sdcard_bytes_read = 0;
		return;
	}
	sd_status &= ~(SD_ST_ERROR | SD_ST_FSM_ERROR);
	sdcard_bytes_read = 512;
#if 0
	off_t offset = sd_sector;
	offset <<= 9;	// make byte offset from sector (always SDHC card!)
	int ret = host__seek(offset);
	if (XEMU_UNLIKELY(!ret && is_write && sd_is_read_only)) {
		ret = 1;	// write protected SD image?
	}
	if (XEMU_LIKELY(!ret)) {
		Uint8 *wp = get_buffer_memory(is_write);
		if (
#ifdef COMPRESSED_SD
			(is_write && compressed_block) ||
#endif
			(is_write ? xemu_safe_write(sdfd, wp, 512) : xemu_safe_read(sdfd, wp, 512)) != 512
		)
			ret = -1;
	}
	if (XEMU_UNLIKELY(ret < 0)) {
		sd_status |= SD_ST_ERROR | SD_ST_FSM_ERROR; // | SD_ST_BUSY1 | SD_ST_BUSY0;
			sd_status |= SD_ST_BUSY1 | SD_ST_BUSY0;
			//KEEP_BUSY(1);
		sdcard_bytes_read = 0;
		return;
	}
	sd_status &= ~(SD_ST_ERROR | SD_ST_FSM_ERROR);
	sdcard_bytes_read = 512;
#endif
}


static void sdcard_command ( Uint8 cmd )
{
	static Uint32 multi_io_block;
	static Uint8 sd_last_ok_cmd;
	DEBUG("SDCARD: writing command register $D680 with $%02X PC=$%04X" NL, cmd, cpu65.pc);
	sd_status &= ~(SD_ST_BUSY1 | SD_ST_BUSY0);	// ugly hack :-@
	KEEP_BUSY(0);
//	status_read_counter = 0;
	switch (cmd) {
		case 0x00:	// RESET SD-card
		case 0x10:	// RESET SD-card with flags specified [FIXME: I don't know what the difference is ...]
			sd_status = SD_ST_RESET | (sd_status & SD_ST_EXT_BUS);	// clear all other flags, but not the bus selection, FIXME: bus selection should not be touched?
			memset(sd_regs + 1, 0, 4);	// clear SD-sector 4 byte register. FIXME: what should we zero/reset other than this, still?
			sdhc_mode = 1;
			break;
		case 0x01:	// END RESET
		case 0x11:	// ... [FIXME: again, I don't know what the difference is ...]
			sd_status &= ~(SD_ST_RESET | SD_ST_ERROR | SD_ST_FSM_ERROR);
			break;
		case 0x57:	// write sector gate
			break;	// FIXME: implement this!!!
		case 0x02:	// read block
			sdcard_block_io(U8A_TO_U32(sd_regs + 1), 0);
			break;
		case 0x03:	// write block
			sdcard_block_io(U8A_TO_U32(sd_regs + 1), 1);
			break;
		case 0x04:	// multi sector write - first sector
			if (sd_last_ok_cmd != 0x04) {
				multi_io_block = U8A_TO_U32(sd_regs + 1);
				sdcard_block_io(multi_io_block, 1);
			} else {
				DEBUGPRINT("SDCARD: bad multi-command sequence command $%02X after command $%02X" NL, cmd, sd_last_ok_cmd);
				sd_status |= SD_ST_ERROR | SD_ST_FSM_ERROR;
			}
			break;
		case 0x05:	// multi sector write - not the first, neither the last sector
			if (sd_last_ok_cmd == 0x04 || sd_last_ok_cmd == 0x05 || sd_last_ok_cmd == 0x57) {
				multi_io_block++;
				sdcard_block_io(multi_io_block, 1);
			} else {
				DEBUGPRINT("SDCARD: bad multi-command sequence command $%02X after command $%02X" NL, cmd, sd_last_ok_cmd);
				sd_status |= SD_ST_ERROR | SD_ST_FSM_ERROR;
			}
			break;
		case 0x06:	// multi sector write - last sector
			if (sd_last_ok_cmd == 0x04 || sd_last_ok_cmd == 0x05 || sd_last_ok_cmd == 0x57) {
				multi_io_block++;
				sdcard_block_io(multi_io_block, 1);
			} else {
				DEBUGPRINT("SDCARD: bad multi-command sequence command $%02X after command $%02X" NL, cmd, sd_last_ok_cmd);
				sd_status |= SD_ST_ERROR | SD_ST_FSM_ERROR;
			}
			break;
		case 0x0C:	// request flush of the SD-card [currently does nothing in Xemu ...]
			break;
		case 0x40:	// SDHC mode OFF - Not supported on newer M65s!
			sd_status &= ~SD_ST_SDHC;
			sdhc_mode = 0;
			break;
		case 0x41:	// SDHC mode ON - Not supported on newer M65s!
			sd_status |= SD_ST_SDHC;
			sdhc_mode = 1;
			break;
		case 0x44:	// sd_clear_error <= '0'	FIXME: what is this?
			break;
		case 0x45:	// sd_clear_error <= '1'	FIXME: what is this?
			break;
		case 0x81:	// map SD-buffer
			sd_status |= SD_ST_MAPPED;
			sd_status &= ~(SD_ST_ERROR | SD_ST_FSM_ERROR);
			break;
		case 0x82:	// unmap SD-buffer
			sd_status &= ~(SD_ST_MAPPED | SD_ST_ERROR | SD_ST_FSM_ERROR);
			break;
		case 0x83:	// SD write fill mode set
			sd_fill_mode = 1;
			break;
		case 0x84:	// SD write fill mode clear
			sd_fill_mode = 0;
			break;
		case 0xC0:	// select internal SD-card bus
			sd_status &= ~SD_ST_EXT_BUS;
			break;
		case 0xC1:	// select external SD-card bus
			sd_status |= SD_ST_EXT_BUS;
			break;
		default:
			sd_status |= SD_ST_ERROR;
			DEBUGPRINT("SDCARD: warning, unimplemented SD-card controller command $%02X" NL, cmd);
			break;
	}
	if (XEMU_UNLIKELY(sd_status & (SD_ST_ERROR | SD_ST_FSM_ERROR))) {
		sd_last_ok_cmd = 0xFF;
	} else {
		sd_last_ok_cmd = cmd;
	}
}


// These are called from hdos.c as part of the beginning/end of the machine
// initialization (ie, trap reset). The purpose here currently is activating
// and deactivating mount monitoring so we can override later the default
// on-sdcard D81 image mount with external one, knowing the mount-sector-number.


void sdcard_notify_system_start_begin ( void )
{
	for (int unit = 0; unit < 2; unit++) {
		mount_info[unit].at_sector_initial = 0;
		mount_info[unit].monitoring_initial = 1;
	}
}


void sdcard_notify_system_start_end ( void )
{
	for (int unit = 0; unit < 2; unit++) {
		mount_info[unit].monitoring_initial = 0;	// stop monitoring initial D81 mounts, end of "machine-start" state
		if (!unit && !mount_info[unit].at_sector_initial)
			DEBUGPRINT("SDCARD: D81-DEFAULT: WARNING: could not determine default on-sd D81 mount sector info during the RESET TRAP for unit #%d!" NL, unit);
	}
}


static int do_default_d81_mount_hack ( const int unit )
{
	static char *default_d81_path[2] = { NULL, NULL };
	if (!default_d81_path[unit]) {
		// Prepare to determine the full path of the default external d81 image, if we haven't got it yet
		const char *hdosroot;
		(void)hypervisor_hdos_virtualization_status(-1, &hdosroot);
		const int len = strlen(hdosroot) + strlen(default_d81_basename[unit]) + 1;
		default_d81_path[unit] = xemu_malloc(len);
		snprintf(default_d81_path[unit], len, "%s%s", hdosroot, default_d81_basename[unit]);
	}
	DEBUGPRINT("SDCARD: D81-DEFAULT: trying to mount external D81 instead of internal default one as %s on unit #%d" NL, default_d81_path[unit], unit);
	// we want to create the default image file if does not exist (note the "0" for d81access_create_image_file, ie do not overwrite exisiting image)
	// d81access_create_image_file() returns with 0 if image was created, -2 if image existed before, and -1 for other errors
	if (d81access_create_image_file(default_d81_path[unit], default_d81_disk_label[unit], 0, NULL) == -1)
		return -1;
	// ... so, the file existed before case allows to reach this point, since then retval is -2 (and not -1 as a generic error)
	return sdcard_force_external_mount(unit, default_d81_path[unit], "Cannot mount default external D81");
}


int sdcard_default_d81_mount ( const int unit )
{
	if (default_d81_is_from_sd) {
		ERROR_WINDOW("This function is not available when\n\"default D81 mount from SD\" option is inactive!");
		return -1;
	}
	return do_default_d81_mount_hack(unit);
}


// Return values:
//	* -1: error
//	*  0: no image was mounted internally (not enabled etc)
//	*  1: image has been mounted
static int internal_mount ( const int unit )
{
	int ro_flag = 0;
	Uint32 at_sector;
	if (!unit) {
		// must be 'image enabled' and 'disk present' bit set for "unit 0"
		if ((sd_regs[0xB] & 0x03) != 0x03 || mount_info[0].force_external_name)
			return 0;
		if (/*(sd_regs[0xB] & 0x04) ||*/ sd_is_read_only)	// it seems checking the register as well causes RO mount for some reason, let's ignore for now!
			ro_flag = D81ACCESS_RO;
		at_sector = U8A_TO_U32(sd_regs + 0x0C);
	} else {
		// must be 'image enabled' and 'disk present' bit set for "unit 1"
		if ((sd_regs[0xB] & 0x18) != 0x18 || mount_info[1].force_external_name)
			return 0;
		if (/*(sd_regs[0xB] & 0x20) ||*/ sd_is_read_only)	// see above at the similar line for drive-0
			ro_flag = D81ACCESS_RO;
		at_sector = U8A_TO_U32(sd_regs + 0x10);
	}
	// FIXME: actual upper limit should take account the image size to mount which can be different now than D81_SIZE like in case of D65 image (not now, but in the future?)
	// FIXME: also a better algorithm to find limits, see the comment at defining MIN_MOUNT_SECTOR_NO near the beginning in this file
	if (at_sector < MIN_MOUNT_SECTOR_NO || ((at_sector + (D81_SIZE >> 9)) >= sdcard_size_in_blocks)) {
		DEBUGPRINT("SDCARD: D81: internal mount #%d **INVALID** SD sector (mount refused!), too %s $%X" NL, unit, at_sector < MIN_MOUNT_SECTOR_NO ? "low" : "high", at_sector);
		d81access_close(unit);
		mount_info[unit].current_name[0] = '\0';
		mount_info[unit].internal = -1;
		return -1;
	}
	mount_info[unit].at_sector = at_sector;
	if (XEMU_UNLIKELY(mount_info[unit].monitoring_initial && !mount_info[unit].at_sector_initial))
		mount_info[unit].at_sector_initial = at_sector;
	if (at_sector == mount_info[unit].at_sector_initial && !default_d81_is_from_sd) {
		// it seems the first ever mounted D81 wanted to be mounted now, so let's override that with external mount at this point
		if (!do_default_d81_mount_hack(unit))
			return 1;	// if succeeded, stop processing further! (note the return value needs of this functions!)
	}
	// FIXME/TODO: no support yet to take account D64,D71,D65 mount from SD-card! However it's not so common I guess from SD-card in case of emulator, but more from external FS in that case
	DEBUGPRINT("SDCARD: D81: internal mount #%d from SD sector $%X (%s)" NL, unit, at_sector, ro_flag ? "R/O" : "R/W");
	d81access_attach_fd(unit, sdfd, (off_t)at_sector << 9, D81ACCESS_IMG | ro_flag);
	char fn[32];
	sprintf(fn, "<SD@$%X:%s>", at_sector, ro_flag ? "RO" : "RW");
	xemu_restrdup(&mount_info[unit].current_name, fn);
	mount_info[unit].internal = 1;
	return 1;
}


static int some_mount ( const int unit )
{
	const char *extfn = mount_info[unit].force_external_name;
	if (extfn) {	// force external mount
		if (strcmp(mount_info[unit].current_name, extfn)) {
			DEBUGPRINT("SDCARD: D81: external mount #%d change from \"%s\" to \"%s\"" NL, unit, mount_info[unit].current_name, extfn);
			if (d81access_attach_fsobj(unit, extfn, D81ACCESS_IMG | D81ACCESS_PRG | D81ACCESS_DIR | D81ACCESS_AUTOCLOSE | D81ACCESS_D64 | D81ACCESS_D71)) {
				DEBUGPRINT("SDCARD: D81: external mount #%d failed at \"%s\", closing unit." NL, unit, extfn);
				d81access_close(unit);
				mount_info[unit].current_name[0] = '\0';
				return -1;
			} else {
				xemu_restrdup(&mount_info[unit].current_name, extfn);
			}
		} else {
			DEBUGPRINT("SDCARD: D81: external mount #%d but no change, \"%s\" = \"%s\"" NL, unit, mount_info[unit].current_name, extfn);
		}
		mount_info[unit].internal = 0;
		// FIXME: at this point, let's update sd_regs register to reflect that we use external mount ...
		return 0;
	}
	// fall back to try internal mount
	if (internal_mount(unit) == 0) {
		DEBUGPRINT("SDCARD: D81: internal mount #%d has no condition to mount anything." NL, unit);
		d81access_close(unit);	// unmount, if internal_mount() finds no internal mount situation
		mount_info[unit].current_name[0] = '\0';
		mount_info[unit].internal = -1;
	} /* else
		DEBUGPRINT("SDCARD: D81: internal mount #%d failed?" NL, unit); */	// FIXME: remove/rethink this part! internal_mount() can return with non-zero, zero or negative as answer!!
	return 0;
}


const char *sdcard_get_mount_info ( const int unit, int *is_internal )
{
	if (is_internal)
		*is_internal = mount_info[unit & 1].internal;
	return mount_info[unit & 1].current_name;
}


int sdcard_force_external_mount ( const int unit, const char *filename, const char *cry )
{
	DEBUGPRINT("SDCARD: D81: %s(%d, \"%s\", \"%s\");" NL, __func__, unit, filename, cry);
	if (filename && *filename) {
		xemu_restrdup(&mount_info[unit].force_external_name, filename);
	} else if (mount_info[unit].force_external_name) {
		free(mount_info[unit].force_external_name);
		mount_info[unit].force_external_name = NULL;
	}
	if (some_mount(unit)) {
		// error. close access - if ANY
		d81access_close(unit);
		mount_info[unit].current_name[0] = '\0';
		if (mount_info[unit].force_external_name) {
			if (cry) {
				ERROR_WINDOW("%s\nCould not mount requested file as unit #%d:\n%s", cry, unit, filename);
			}
			free(mount_info[unit].force_external_name);
			mount_info[unit].force_external_name = NULL;
		}
		return -1;
	}
	return 0;
}


int sdcard_force_external_mount_with_image_creation ( const int unit, const char *filename, const int do_overwrite, const char *cry )
{
	if (d81access_create_image_file(filename, NULL, do_overwrite, "Cannot create D81"))
		return -1;
	return sdcard_force_external_mount(unit, filename, cry);
}


int sdcard_unmount ( const int unit )
{
	d81access_close(unit);
	mount_info[unit].internal = 0;	// FIXME ???
	xemu_restrdup(&mount_info[unit].current_name, "<EMPTY>");
	if (mount_info[unit].force_external_name) {
		// We must null this out, otherwise next (even internal) mount will pick it up and use it again as this external mount!
		free(mount_info[unit].force_external_name);
		mount_info[unit].force_external_name = NULL;
	}
	return 0;
}


void sdcard_write_register ( int reg, Uint8 data )
{
	const Uint8 prev_data = sd_regs[reg];
	sd_regs[reg] = data;
	// Note: don't update D6XX backend as it's already done in the I/O decoder
	switch (reg) {
		case 0x00:		// command/status register
			sdcard_command(data);
			break;
		case 0x01:		// sector address
		case 0x02:		// sector address
		case 0x03:		// sector address
		case 0x04:		// sector address
			DEBUG("SDCARD: writing sector number register $%04X with $%02X PC=$%04X" NL, reg + 0xD680, data, cpu65.pc);
			break;
		case 0x06:		// write-only register: byte value for SD-fill mode on SD write command
			sd_fill_value = data;
			if (sd_fill_value != sd_fill_buffer[0])
				memset(sd_fill_buffer, sd_fill_value, 512);
			break;
		case 0x09:
			set_disk_buffer_cpu_view();	// update disk_buffer_cpu_view pointer according to sd_regs[9] just written
			break;
		case 0x0B:
			DEBUGPRINT("SDCARD: writing FDC configuration register $%04X with $%02X (old_data=$%02X) PC=$%04X" NL, reg + 0xD680, data, prev_data, cpu65.pc);
			if ((data ^ prev_data) & 0x07)	// FIXME: ignores bit 6 "superfloppy"
				some_mount(0);
			if ((data ^ prev_data) & 0x38)	// FIXME: ignores bit 7 "superfloppy"
				some_mount(1);
			break;
		case 0x0C:	// first D81 disk image starting sector registers
		case 0x0D:
		case 0x0E:
		case 0x0F:
			if (data != prev_data) {
				DEBUGPRINT("SDCARD: writing D81 #0 sector register $%04X with $%02X PC=$%04X" NL, reg + 0xD680, data, cpu65.pc);
				internal_mount(0);
			}
			break;
		case 0x10:	// second D81 disk image starting sector registers
		case 0x11:
		case 0x12:
		case 0x13:
			if (data != prev_data) {
				DEBUGPRINT("SDCARD: writing D81 #1 sector register $%04X with $%02X PC=$%04X" NL, reg + 0xD680, data, cpu65.pc);
				internal_mount(1);
			}
			break;
		default:
			DEBUGPRINT("SDCARD: unimplemented register: $%02X tried to be written with data $%02X" NL, reg, data);
			break;
	}
}


Uint8 sdcard_read_register ( int reg )
{
	Uint8 data = sd_regs[reg];	// default answer
	switch (reg) {
		case 0:
			return sdcard_read_status();
		case 1:
		case 2:
		case 3:
		case 4:
			break;
		case 6:
			break;
		case 8:	// SDcard read bytes low byte	: THIS IS WRONG!!!! It's a very old thing does not exist anymore, FIXME!
			return sdcard_bytes_read & 0xFF;
		case 9:
			return sd_reg9;
		case 0xB:
			break;
		case 0xC:
		case 0xD:
		case 0xE:
		case 0xF:
			break;
		case 0x10:
		case 0x11:
		case 0x12:
		case 0x13:
			break;
#if 0
			// SDcard read bytes hi byte
			data = sdcard_bytes_read >> 8;
#endif
			break;
		default:
			DEBUGPRINT("SDCARD: unimplemented register: $%02X tried to be read, defaulting to the back storage with data $%02X" NL, reg, data);
			break;
	}
	return data;
}


/* --- SNAPSHOT RELATED --- */


#ifdef XEMU_SNAPSHOT_SUPPORT

#include <string.h>

#define SNAPSHOT_SDCARD_BLOCK_VERSION	0
#define SNAPSHOT_SDCARD_BLOCK_SIZE	(0x100 + sizeof(disk_buffers))

int sdcard_snapshot_load_state ( const struct xemu_snapshot_definition_st *def, struct xemu_snapshot_block_st *block )
{
	Uint8 buffer[SNAPSHOT_SDCARD_BLOCK_SIZE];
	int a;
	if (block->block_version != SNAPSHOT_SDCARD_BLOCK_VERSION || block->sub_counter || block->sub_size != sizeof buffer)
		RETURN_XSNAPERR_USER("Bad SD-Card block syntax");
	a = xemusnap_read_file(buffer, sizeof buffer);
	if (a) return a;
	/* loading state ... */
	memcpy(sd_sector_registers, buffer, 4);
	memcpy(sd_d81_img1_start, buffer + 4, 4);
	fd_mounted = (int)P_AS_BE32(buffer + 8);
	sdcard_bytes_read = (int)P_AS_BE32(buffer + 12);
	sd_is_read_only = (int)P_AS_BE32(buffer + 16);
	//d81_is_read_only = (int)P_AS_BE32(buffer + 20);
	//use_d81 = (int)P_AS_BE32(buffer + 24);
	sd_status = buffer[0xFF];
	memcpy(disk_buffers, buffer + 0x100, sizeof disk_buffers);
	return 0;
}


int sdcard_snapshot_save_state ( const struct xemu_snapshot_definition_st *def )
{
	Uint8 buffer[SNAPSHOT_SDCARD_BLOCK_SIZE];
	int a = xemusnap_write_block_header(def->idstr, SNAPSHOT_SDCARD_BLOCK_VERSION);
	if (a) return a;
	memset(buffer, 0xFF, sizeof buffer);
	/* saving state ... */
	memcpy(buffer, sd_sector_registers, 4);
	memcpy(buffer + 4,sd_d81_img1_start, 4);
	U32_AS_BE(buffer + 8, fd_mounted);
	U32_AS_BE(buffer + 12, sdcard_bytes_read);
	U32_AS_BE(buffer + 16, sd_is_read_only);
	//U32_AS_BE(buffer + 20, d81_is_read_only);
	//U32_AS_BE(buffer + 24, use_d81);
	buffer[0xFF] = sd_status;
	memcpy(buffer + 0x100, disk_buffers, sizeof disk_buffers);
	return xemusnap_write_sub_block(buffer, sizeof buffer);
}

#endif
