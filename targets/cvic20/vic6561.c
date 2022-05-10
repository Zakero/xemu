/* Test-case for a very simple and inaccurate Commodore VIC-20 emulator using SDL2 library.
   Copyright (C)2016,2017 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>

   This is the VIC-20 emulation. Note: the source is overcrowded with comments by intent :)
   That it can useful for other people as well, or someone wants to contribute, etc ...

   This emulation of VIC-I chip is based on my own ideas, I can be easily wrong, so please
   don't take anything here as the absolute truth :) Also, the emulation is scanline based,
   which is not correct at all, to be precise.

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

#include <stdio.h>

#include <SDL.h>

#include "xemu/emutools.h"
#include "vic6561.h"


#define SCREEN_COLOUR	vic_cpal[0]
#define BORDER_COLOUR	vic_cpal[1]
#define SRAM_COLOUR	vic_cpal[2]
#define AUX_COLOUR	vic_cpal[3]


/* Note about VIC-I memory accessing: this part of my emulator tries to emulate VIC-I in a non-VIC20-specific way.
   That is: VIC-I sees 16K of address space with 12 bit (!) width data bus. Linearly.
   The high 4 bits of data bus (D8-D11) is connected to a 4 bit wide SRAM in VIC-20 with 1K capacity.
   However, in theory, you can have 16Kx4bit for colour SRAM ...
   Also 6502 sees quite different situation for VIC-I addresses as VIC-I itself on the VIC-20.
   To compensate the differences with keeping "some hacked VIC-20" emulations in the future simple, I've decided
   to use a map to configure the exact mapping. vic_address_space_hi4 and vic_address_space_lo8 are the pointers
   of the VIC-I memory space, at every Kbytes. Note: the init function expects 16 pointers, but in we use more
   elements. The trick here: to avoid the need of checking "overflow" of the addressing space ;-)
*/


Uint32 vic_palette[16];				// VIC palette with native SCREEN_FORMAT aware way. Must be initialized by the emulator
int scanline;					// scanline counter, must be maintained by the emulator, as increment, checking for all scanlines done, etc, BUT it is zeroed here in vic_vsync()!

static Uint8 *vic_address_space_hi4[16 + 3];	// VIC 16K address space pointers, one pointer per Kbytes [for VIC-I D8-D11, 4 higher bits], [+elements for "overflow"]
static Uint8 *vic_address_space_lo8[16 + 3];	// VIC 16K address space pointers, one pointer per Kbytes [for VIC-I D0-D7, 8 lower bits], [+elements for "overflow"]
static Uint8 vic_registers[16];			// VIC registers
static int charline;				// current character line (0-7 for 8 pixels, 0-15 for 16 pixels height characters)
static Uint32 *pixels;				// pointer to our work-texture (ie: the visible emulated VIC-20 screen, DWORD / pixel)
static int pixels_tail;				// "tail" (in DWORDs) after each texture line (must be added to pixels pointer after rendering one scanline)
static int first_active_scanline;		// first "active" (ie not top border) scanline
static int vic_vertical_area;			// vertical "area" of VIC (1 = upper border, 0 = active display, 2 = bottom border)
static Uint32 vic_cpal[4];			// SDL pixel format related colours of multicolour colours, reverse mode swaps the colours here!! Also used to render everything on the screen!
static int char_height_minus_one;		// 7 or 15 (for 8 and 16 pixels height characters)
static int char_height_shift;			// 3 for 8 pixels height characters, 4 for 16
static int first_active_dotpos;			// first dot within a scanline which belongs to the "active" (not top and bottom border) area
static int text_columns;			// number of screen text columns
static int text_rows;				// number of screen text rows
static int sram_colour_index;			// index in the "multicolour" table for data read from colour SRAM (depends on reverse mode!)
static int vic_vid_addr;			// memory address of screen (both of screen and colour SRAM to be precise, on lo8 and hi4 bus bits)
static int vic_vid_addr_bit9;			// bit9 of screen address (not used directly but on address re-calculation only)
static int vic_chr_addr;			// memory address of characer data (lo8 bus bits only)
static int vic_vid_counter;			// video address counter (from zero) relative to the given video address (vic_vid_addr)
static int vic_row_counter;			// counts the displayed (text) rows of VIC-I, if it's equal to text_rows it means end of actual active display, and start of the bottom border


/* Check constraints of the given parameters from some header file */

#if ((SCREEN_ORIGIN_DOTPOS) & 1) != 0
#	error "SCREEN_ORIGIN_DOTPOS must be an even number!"
#endif
#if (SCREEN_FIRST_VISIBLE_DOTPOS) & 1 != 0
#	error "SCREEN_FIRST_VISIBLE_DOTPOS must be an even number!"
#endif
#if (SCREEN_LAST_VISIBLE_DOTPOS) & 1 != 1
#	error "SCREEN_LAST_VISIBLE_DOTPOS must be an odd number!"
#endif
#if SCREEN_LAST_VISIBLE_SCANLINE > LAST_SCANLINE
#	error "SCREEN_LAST_VISIBLE_SCANLINE cannot be greater than LAST_SCANLINE!"
#endif



// Read VIC-I register by the CPU. "addr" must be 0 ... 15!
Uint8 cpu_vic_reg_read ( int addr )
{
	if (addr == 4)
		return scanline >> 1;	// scanline counter is read (bits 8-1)
	else if (addr == 3)
		return (vic_registers[3] & 0x7F) | ((scanline & 1) ? 0x80 : 0);	// high byte of reg 3 is the bit0 of scanline counter!
	else
		return vic_registers[addr];	// otherwise, just read the "backend" (see cpu_vic_reg_write() for more information)
}



// Write VIC-I register by the CPU. "addr" must be 0 ... 15!
void cpu_vic_reg_write ( int addr, Uint8 data )
{
	vic_registers[addr] = data;
	// Also update our variables, so access is faster/easier this way in our renderer.
	switch (addr) {
		case 0: // screen X origin (in 4 pixel units) on lower 7 bits, bit 7: interlace (only for NTSC, so it does not apply here, we emulate PAL)
			first_active_dotpos = (data & 0x7F) * 4 + SCREEN_ORIGIN_DOTPOS;
			break;
		case 1:	// screen Y orgin (in 2 lines units) for all the 8 bits
			first_active_scanline = (data << 1) + SCREEN_ORIGIN_SCANLINE;
			break;
		case 2:	// number of video columns (lower 7 bits), bit 7: bit 9 of screen memory
			text_columns = data & 0x7F;
			if (text_columns > 32)	// FIXME: really 32? Not 31??
				text_columns = 32;
			vic_vid_addr_bit9 = ((data & 128) ? 0x200 : 0);
			vic_vid_addr = (vic_vid_addr & 0xFDFF) | vic_vid_addr_bit9; // re-calculate the address, only bit9 may changed of the video address
			break;
		case 3: // Bits6-1: number of rows, bit 7: bit 0 of current scanline, bit 0: 8/16 height char
			char_height_minus_one = (data & 1) ? 15 : 7;
			char_height_shift = (data & 1) ? 4 : 3;
			text_rows = (data >> 1) & 0x3F;
			if (text_rows > 32)	// FIXME: really 32? Not 31??
				text_rows = 32;
			break;
		case  5:
			vic_chr_addr = (data & 15) << 10;
			vic_vid_addr = ((data & 0xF0) << 6) | vic_vid_addr_bit9;
			break;
		case 14:
			AUX_COLOUR = vic_palette[data >> 4];
			break;
		case 15:
			BORDER_COLOUR = vic_palette[data & 7];
			if (data & 8) {	// normal mode
				SCREEN_COLOUR = vic_palette[data >> 4];
				sram_colour_index = 2;
			} else {	// reverse mode
				SRAM_COLOUR = vic_palette[data >> 4];
				sram_colour_index = 0;
			}
			break;
	}
	//DEBUG("VIC-I: %02X -> [%01X]" NL, data, addr);
}


// Should be called before each new (half) frame
void vic_vsync ( int relock_texture )
{
	if (relock_texture)
		pixels = xemu_start_pixel_buffer_access(&pixels_tail);	// get texture access stuffs for the new frame
	scanline = 0;		// current scanline (other than this, incrementation, calling vsync on last scanline etc, the emulator should manage ...)
	charline = 0;
	vic_vertical_area = 1;
	vic_vid_counter = 0;
	vic_row_counter = 0;
}



void vic_init ( Uint8 **lo8_pointers, Uint8 **hi4_pointers )
{
	int a;
	vic_vsync(1);	// we need once, since this is the first time, this will be called in update_emulator() later ....
	// Some words about "overflow" (so the fact that loop is not from 0 to 15 only):
	// overflow area (to avoid the checks of wrapping around 16K of VIC-I address space all the time)
	// for screen/colour RAM: 32*32 rows/cols resolution in theory (1K), but start address can be at 0.5K steps, so 1.5K (->2) overflow can occur
	// for chargen: for 16 pixel height chars, 4K, start can be given in 1K steps, so 3K overflow can occur
	for (a = 0; a < 16 + 3; a++) {
		cpu_vic_reg_write(a & 15, 0);	// initialize VIC-I registers for _some_ value (just to avoid the danger of uninitialized emulator variables at startup)
		// configure address space pointers, in a tricky way: we don't want to use 'and' on accesses, so offsets must be compensated!
		vic_address_space_lo8[a] = lo8_pointers[a & 15] - (a << 10);
		vic_address_space_hi4[a] = hi4_pointers[a & 15] - (a << 10);
	}
}


static inline Uint8 vic_read_mem_lo8 ( Uint16 addr )
{
	return vic_address_space_lo8[addr >> 10][addr];
}



static inline Uint8 vic_read_mem_hi4 ( Uint16 addr )
{
	return vic_address_space_hi4[addr >> 10][addr];
}


// Render a single scanline of VIC-I screen.
// It's not a correct solution to render a line in once, however it's only a sily emulator try from me, not an accurate one :-D
void vic_render_line ( void )
{
	int v_columns, v_vid, dotpos, visible_scanline, bitp;
	// Check for start the active display (end of top border) and end of active display (start of bottom border)
	if (vic_row_counter >= text_rows && vic_vertical_area == 0)	// FIXME: the exact condition! Maybe not ">" like relation but equality is checked by VIC-I only?
		vic_vertical_area = 2;	// this will be the first scanline of bottom border
	else if (scanline == first_active_scanline && vic_vertical_area == 1)
		vic_vertical_area = 0;	// this scanline will be the first non-border scanline
	// Check if we're inside the top or bottom area, so full border colour lines should be rendered
	visible_scanline = (scanline >= SCREEN_FIRST_VISIBLE_SCANLINE && scanline <= SCREEN_LAST_VISIBLE_SCANLINE);
	if (vic_vertical_area) {
		if (visible_scanline) {
			v_columns = SCREEN_LAST_VISIBLE_DOTPOS - SCREEN_FIRST_VISIBLE_DOTPOS + 1;
			while (v_columns--)
				*(pixels++) = BORDER_COLOUR;
			pixels += pixels_tail;		// add texture "tail" (that is, pitch - text_width, in 4 bytes uints, ie Uint32 pointer ...)
		}
		return;
	}
	// So, we are at the "active" display area. But still, there are left and right borders ...
	bitp = 128;
	v_columns = text_columns;
	v_vid = vic_vid_counter;
	for (dotpos = 0; dotpos < CYCLES_PER_SCANLINE * 4; dotpos++) {
		int visible_dotpos = (dotpos >= SCREEN_FIRST_VISIBLE_DOTPOS && dotpos <= SCREEN_LAST_VISIBLE_DOTPOS && visible_scanline);
		if (dotpos < first_active_dotpos) {
			if (visible_dotpos)
				*(pixels++) = BORDER_COLOUR;
		} else {
			if (v_columns) {
				int chr = vic_read_mem_lo8((vic_read_mem_lo8(vic_vid_addr + v_vid) << char_height_shift) + vic_chr_addr + charline);
				int mcm = vic_read_mem_hi4(vic_vid_addr + v_vid);	// mcm here is simply the fetched colour SRAM byte (only lower 4 bits will be used)
				if (bitp == 128) {
					// NOTE! *AFAIK* VIC-I fetches colour info from the *VERY SAME* address as the video data! It's just matter of usage in VIC-20
					// that only 1K of SRAM is connected for the upper 4 bits of the 12 bit wide data bus of VIC-I, but it can be otherwise too!
					v_vid++;	// increment video address
					vic_cpal[sram_colour_index] = vic_palette[mcm & 7];	// set text colour with the lower 3 bits fetched on the right place (depends on reverse mode: sram_colour_index)
					mcm &= 8;	// mcm: multi colour mode flag, bit 3 - so the true meaning of "mcm" ;-)
					if (mcm)
						bitp = 6;	// in case of mcm, bitp is a shift counter needed to move bits to bitpos 1,0, otherwise bitp is a bit mask in hires mode
				}
				if (visible_dotpos) {
					if (mcm) {
						// Ugly enough! I have the assumption that visible dotpos condition only changes on even number in dotpos only ... :-/
						// That is, you MUST define texture dimensions for width of even number!!
						*pixels = *(pixels + 1) = vic_cpal[(chr >> bitp) & 3]; // "double width" pixel ...
						pixels += 2;
						dotpos++;	// trick our "for" loop a bit here ...
					} else
						*(pixels++) = (chr & bitp) ? SRAM_COLOUR : SCREEN_COLOUR;
				}
				if (bitp <= 1) {	// last pixel? handle both situation with "dual" meaning of bitp for multicolour and hires mode
					v_columns--;
					bitp = 128;	// prepare for next byte fetch (video + colour)
				} else {
					if (mcm)
						bitp -= 2;
					else
						bitp >>= 1;
				}
			} else if (visible_dotpos)
				*(pixels++) = BORDER_COLOUR;
		}
	}
	if (charline >= char_height_minus_one) {
		charline = 0;
		vic_vid_counter += text_columns;	// FIXME: does VIC-I always use the text columns setting, even if picture wouldn't fit into the TV screen at all?!
		vic_row_counter++;
	} else {
		charline++;
	}
	pixels += pixels_tail;		// add texture "tail" (that is, pitch - text_width, in 4 bytes uints, ie Uint32 pointer ...)
}
