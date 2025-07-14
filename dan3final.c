/* DAN3 Encoder - Decoder (Emscripten Friendly with Debug Prints)
 * ------------
 * HISTORY
 * 20170220 - BETA
 * 20171123 - BETA (clean up)
 * 20171218 - CHANGE RLE ENCODING AND OPTION
 * 20180103 - ADD MAXIMUM BITS ALLOWED TO ENCODE OFFSET
 * 20180113 - ADD DECOMPRESSION ROUTINE
 * 20180118 - SPEED OPTIMIZATION IN COMPRESSION ROUTINE
 * 20180123 - FIX SPEED OPTIMIZATION
 * 20180126 - FIX RLE COMPRESSION
 * 20180126 - FIX READ GOLOMB VALUES
 *
 * Emscripten-specific modifications by Google Gemini (2025-07-10)
 * - Added emscripten.h and EMSCRIPTEN_KEEPALIVE.
 * - Removed file I/O (main, fopen, fclose, printf, scanf, etc.).
 * - Introduced wrapper functions (dan3_encode, dan3_decode) for JS interaction.
 * - Added extensive debug printf statements for WASM execution analysis.
 */
#include <stdio.h>    /* For printf (debugging) */
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* memcpy, memset */
#include <ctype.h>    /* tolower */
#include <emscripten/emscripten.h> /* For EMSCRIPTEN_KEEPALIVE */
#include <emscripten/em_asm.h> // For EM_ASM macros
#include <emscripten/console.h> // For emscripten_console_log
#include <stdint.h>   /* For uint8_t */

/*
 * - AUTHOR'S NAME -
 */
#define AUTHOR "Daniel Bienvenu aka NewColeco"
/*
 * - PROGRAM TITLE -
 */
#define PRGTITLE "DAN3 Compression Tool"
/*
 * - VERSION NUMBER -
 */
#define VERSION "BETA-20180126"
/*
 * - YEAR -
 */
#define YEAR "2018"
/*
 * - FILE EXTENSION -
 */
#define EXTENSION ".dan3"
#define EXTENSIONBIN ".bin"
/*
 * - BOOLEAN VALUES -
 */
#define TRUE -1
#define FALSE 0
/*
 * - MAX INPUT FILE SIZE -
 */
#define MAX 1024*1024 // 256KB
/*
 * - COMPRESSION CONSTANTS -
 */
#define BIT_GOLOMG_MAX	7
#define MAX_GAMMA		(1<<(BIT_GOLOMG_MAX+1)) - 2
#define BIT_OFFSET00	0
#define BIT_OFFSET0		1
#define BIT_OFFSET1		5
#define BIT_OFFSET2		8
#define BIT_OFFSET_MIN	9
#define BIT_OFFSET_MAX	16
#define BIT_OFFSET_NBR	BIT_OFFSET_MAX - BIT_OFFSET_MIN + 1

// Mark constants with EMSCRIPTEN_KEEPALIVE if JS needs their *exact* values
EMSCRIPTEN_KEEPALIVE const int C_BIT_OFFSET_MIN = BIT_OFFSET_MIN;
EMSCRIPTEN_KEEPALIVE const int C_BIT_OFFSET_MAX = BIT_OFFSET_MAX;
EMSCRIPTEN_KEEPALIVE const int C_BIT_OFFSET_NBR = BIT_OFFSET_NBR;
EMSCRIPTEN_KEEPALIVE const int C_MAX = MAX;

#define MAX_OFFSET00	(1<<BIT_OFFSET00)
#define MAX_OFFSET0		(1<<BIT_OFFSET0) + MAX_OFFSET00
#define MAX_OFFSET1		(1<<BIT_OFFSET1)
#define MAX_OFFSET2		(1<<BIT_OFFSET2) + MAX_OFFSET1
#define MAX_OFFSET		(1<<BIT_OFFSET_MAX) + MAX_OFFSET2
#define RAW_MIN	1
#define RAW_RANGE (1<<8)
#define RAW_MAX RAW_MIN + RAW_RANGE - 1

EMSCRIPTEN_KEEPALIVE int BIT_OFFSET3;
EMSCRIPTEN_KEEPALIVE int MAX_OFFSET3;
EMSCRIPTEN_KEEPALIVE int BIT_OFFSET_MAX_ALLOWED;
EMSCRIPTEN_KEEPALIVE int BIT_OFFSET_NBR_ALLOWED;

/*
 * - OPTIONS FLAGS -
 */
// These are now set by JS wrapper functions
EMSCRIPTEN_KEEPALIVE int bVerbose = FALSE;
EMSCRIPTEN_KEEPALIVE int bYes = FALSE;     // Not used in WASM context
EMSCRIPTEN_KEEPALIVE int bFAST = FALSE;
EMSCRIPTEN_KEEPALIVE int bRLE = TRUE;

/*
 * - IN-MEMORY BUFFERS -
 * These will be treated as global memory by WASM and accessed via HEAPU8/HEAP32.
 * Their addresses are accessible from JS if exported with EMSCRIPTEN_KEEPALIVE.
 */
EMSCRIPTEN_KEEPALIVE unsigned char data_src[MAX];
EMSCRIPTEN_KEEPALIVE int index_src;
EMSCRIPTEN_KEEPALIVE unsigned char data_dest[MAX];
EMSCRIPTEN_KEEPALIVE int index_dest;
EMSCRIPTEN_KEEPALIVE unsigned char bit_mask;
EMSCRIPTEN_KEEPALIVE int bit_index;

/*
 * - MATCHES -
 */
struct t_match
{
	int index;
	struct t_match *next;
};
EMSCRIPTEN_KEEPALIVE struct t_match matches[65536]; // Keepalive for its internal state

struct t_optimal
{
	int bits[BIT_OFFSET_NBR]; /* COST */
	int offset[BIT_OFFSET_NBR];
	int len[BIT_OFFSET_NBR];
};
// Make the optimals table keepalive. Its address will be _optimals in JS.
EMSCRIPTEN_KEEPALIVE struct t_optimal optimals[MAX];

/*
 * - INSERT A MATCH IN TABLE -
 */
void insert_match(struct t_match *match, int index)
{
    if (bVerbose) printf("C: insert_match: index=%d\n", index);
	struct t_match *new_match = (struct t_match *) malloc( sizeof(struct t_match) );
    if (new_match == NULL) {
        if (bVerbose) printf("C: CRITICAL ERROR: malloc failed in insert_match for index %d! Aborting.\n", index);
        emscripten_console_log("C-CRITICAL: Malloc failed in insert_match!");
        EM_ASM({ debugger; }); // Trigger JS debugger at this point
        abort(); // Force an Emscripten-catchable abort
    }
	new_match->index = match->index;
	new_match->next = match->next;
	match->index = index;
	match->next = new_match;
}

/*
 * - REMOVE MATCH(ES) FROM MEMORY -
 */
void flush_match(struct t_match *match)
{
    if (bVerbose) printf("C: flush_match: flushing match at %p\n", (void*)match);
	struct t_match *node;
	struct t_match *head = match->next;
	while ((node = head) != NULL)
	{
		head = head->next;
        if (bVerbose) printf("C: flush_match: freeing node %p\n", (void*)node);
		free(node);
	}
	match->next = NULL;
}

/*
 * - FREE MATCH(ES) FROM TABLE -
 */
// This function will be called from lzss_slow.
EMSCRIPTEN_KEEPALIVE void reset_matches(void)
{
    if (bVerbose) printf("C: reset_matches: Clearing all 65536 match lists\n");
	int i;
	for (i = 0;i < 65536;i++)
	{
		flush_match(&matches[i]);
		matches[i].index = -1; // Also reset the head node's index
	}
}

/*
 * - LOW BYTE VALUE - (Utility, might not be used directly in WASM context)
 */
int mask_byte(int value)
{
	return (value & 255);
}

/*
 * - ERROR MESSAGE (EOF) - (No longer used with direct memory access)
 */
// void error(void) { // printf("Output error\n"); exit(1); }

/*
 * - READ BYTE - (Works with data_src in memory)
 */
unsigned char read_byte()
{
    if (index_src < 0 || index_src >= MAX) { // This is an out-of-bounds read check
        if (bVerbose) printf("C: CRITICAL ERROR: read_byte out of bounds! index_src=%d, MAX=%d. Aborting.\n", index_src, MAX);
        emscripten_console_log("C-CRITICAL: Read_byte out of bounds!");
        EM_ASM({ debugger; });
        abort();
    }
	return data_src[index_src++];
}

/*
 * - READ BIT - (Works with data_src in memory)
 */
unsigned char read_bit()
{
	unsigned char bit;
    if (bit_mask == 0)
	{
		bit_mask  = (unsigned char) 128;
		bit_index = index_src;
        if (bit_index < 0 || bit_index >= MAX) { // Out of bounds for data_src access
            if (bVerbose) printf("C: CRITICAL ERROR: read_bit (new byte) out of bounds! bit_index=%d, MAX=%d. Aborting.\n", bit_index, MAX);
            emscripten_console_log("C-CRITICAL: Read_bit (new byte) out of bounds!");
            EM_ASM({ debugger; });
            abort();
        }
		index_src++;
	}
    // Check bit_index again before actual access if bit_mask was not 0
    if (bit_index < 0 || bit_index >= MAX) { // Defensive check in case bit_index was somehow bad
        if (bVerbose) printf("C: CRITICAL ERROR: read_bit (existing byte) out of bounds! bit_index=%d, MAX=%d. Aborting.\n", bit_index, MAX);
        emscripten_console_log("C-CRITICAL: Read_bit (existing byte) out of bounds!");
        EM_ASM({ debugger; });
        abort();
    }
	bit = (data_src[bit_index] & bit_mask);
	bit_mask >>= 1 ;
	return (bit != 0 ? 1 : 0 );
}

/*
 * - READ GOLOMB GAMMA -
 */
int read_golomb_gamma()
{
    if (bVerbose) printf("C: read_golomb_gamma START (index_src: %d, bit_index: %d)\n", index_src, bit_index);
	int value = 0;
	int i, j = 0;
	while (j < BIT_GOLOMG_MAX && read_bit() == 0) j++;
	if (j < BIT_GOLOMG_MAX)
	{
		value = 1;
		for (i = 0; i <= j; i++)
		{
			value <<= 1;
			value |= read_bit();
		}
	}
	value--;
    if (bVerbose) printf("C: read_golomb_gamma END, value: %d\n", value);
	return value;
}

/*
 * - WRITE DATA - (Works with data_dest in memory)
 */
void write_byte(unsigned char value)
{
    if (index_dest < 0 || index_dest >= MAX) { // Out of bounds write check
        if (bVerbose) printf("C: CRITICAL ERROR: write_byte out of bounds! index_dest=%d, MAX=%d. Aborting.\n", index_dest, MAX);
        emscripten_console_log("C-CRITICAL: Write_byte out of bounds!");
        EM_ASM({ debugger; });
        abort();
    }
	data_dest[index_dest++] = value;
}

void write_bit(int value)
{
	if (bit_mask == 0)
	{
		bit_mask  = (unsigned char) 128;
		bit_index = index_dest;
        if (bit_index < 0 || bit_index >= MAX) { // Out of bounds for data_dest access
            if (bVerbose) printf("C: CRITICAL ERROR: write_bit (new byte) out of bounds! bit_index=%d, MAX=%d. Aborting.\n", bit_index, MAX);
            emscripten_console_log("C-CRITICAL: Write_bit (new byte) out of bounds!");
            EM_ASM({ debugger; });
            abort();
        }
		write_byte((unsigned char) 0); // This internal call to write_byte will check bounds too
	}
    // Check bit_index again before actual access if bit_mask was not 0
    if (bit_index < 0 || bit_index >= MAX) { // Defensive check
        if (bVerbose) printf("C: CRITICAL ERROR: write_bit (existing byte) out of bounds! bit_index=%d, MAX=%d. Aborting.\n", bit_index, MAX);
        emscripten_console_log("C-CRITICAL: Write_bit (existing byte) out of bounds!");
        EM_ASM({ debugger; });
        abort();
    }
	if (value) data_dest[bit_index] |= bit_mask;
	bit_mask >>= 1 ;
}

void write_bits(int value, int size)
{
    if (bVerbose) printf("C: write_bits: value=0x%X, size=%d\n", value, size);
	int i;
	int mask = 1;
	for (i = 0 ; i < size ; i++)
	{
		mask <<= 1;
	}
	while (mask > 1)
	{
		mask >>= 1;
		write_bit (value & mask);
	}
}

void write_golomb_gamma(int value)
{
    if (bVerbose) printf("C: write_golomb_gamma: value=%d\n", value);
	int i;
	value++;
	for (i = 4; i <= value; i <<= 1)
	{
		write_bit(0);
	}
	while ((i >>= 1) > 0)
	{
		write_bit(value & i);
	}
}

void write_offset(int value, int option)
{
    if (bVerbose) printf("C: write_offset: value=%d, option=%d (BIT_OFFSET3=%d)\n", value, option, BIT_OFFSET3);
	value--;
	if (option == 1) // For len=1 (short matches)
	{
		if (value >= MAX_OFFSET00)
		{
			write_bit(1);
			value -= MAX_OFFSET00;
			write_bits(value, BIT_OFFSET0);
		}
		else
		{
			write_bit(0);
			write_bits(value, BIT_OFFSET00);
		}
	}
	else // For len > 1 (longer matches)
	{
		if (value >= MAX_OFFSET2)
		{
			write_bit(1);
			write_bit(1);
			value -= MAX_OFFSET2;
			write_bits(value >> BIT_OFFSET2, BIT_OFFSET3 - BIT_OFFSET2); // This subtraction needs BIT_OFFSET3 >= BIT_OFFSET2
			write_byte((unsigned char) (value & 255)); /* BIT_OFFSET2 = 8 */
		}
		else
		{
			if (value >= MAX_OFFSET1)
			{
				write_bit(0);
				value -= MAX_OFFSET1;
				write_byte((unsigned char) (value & 255)); /* BIT_OFFSET2 = 8 */
			}
			else
			{
				write_bit(1);
				write_bit(0);
				write_bits(value, BIT_OFFSET1);
			}
		}
	}
}

void write_doublet(int length, int offset)
{
    if (bVerbose) printf("C: write_doublet: len=%d, offset=%d\n", length, offset);
	write_bit(0);
	write_golomb_gamma(length);
	write_offset(offset, length);
}

void write_end()
{
    if (bVerbose) printf("C: write_end marker\n");
	write_bit(0);
	write_bits(0, BIT_GOLOMG_MAX);
	write_bit(0);
}

void write_literals_length(int length)
{
    if (bVerbose) printf("C: write_literals_length: len=%d\n", length);
	write_bit(0);
	write_bits(0, BIT_GOLOMG_MAX);
	write_bit(1);
	length -= RAW_MIN;
	write_byte((unsigned char) length);
}

void write_literal(unsigned char c)
{
    if (bVerbose) printf("C: write_literal: char=0x%02X\n", c);
	write_bit(1);
	write_byte(c);
}

// write_destination is removed as file I/O is handled in JS
// void write_destination() { // ... }

// write_lz now returns the final index_dest (compressed size)
int write_lz(int subset)
{
    if (bVerbose) printf("C: write_lz START for subset %d (BIT_OFFSET_MIN+%d)\n", subset, BIT_OFFSET_MIN);
	int i, j;
	int index;
	index_dest = 0;
	bit_mask = 0; // Reset bit_mask and bit_index for a new write operation
	bit_index = 0;

    if (bVerbose) printf("C: write_lz: Writing header (0xFE, subset+1)\n");
	write_bits(0xFE, subset + 1);
    if (bVerbose) printf("C: write_lz: Writing first raw byte 0x%02X\n", data_src[0]);
	write_byte(data_src[0]); // First byte is always written raw

	for (i = 1;i < index_src;i++)
	{
        // Debug check for optimistic access
        if (i < 0 || i >= MAX) {
            if (bVerbose) printf("C: ERROR: write_lz loop index i (%d) out of bounds (0-%d)\n", i, MAX-1);
            // Consider returning an error or breaking.
            return -1; // Indicate failure
        }
        if (optimals[i].len[subset] > 0)
		{
			index = i -  optimals[i].len[subset] + 1;
            if (bVerbose) printf("C: write_lz: pos %d (src: 0x%02X), len=%d, offset=%d, type=%s\n",
                                   i, data_src[i], optimals[i].len[subset], optimals[i].offset[subset],
                                   optimals[i].offset[subset] == 0 ? (optimals[i].len[subset] == 1 ? "Literal" : "RLE") : "Match");

            if (index < 0 || index >= MAX) {
                if (bVerbose) printf("C: ERROR: write_lz calculated source index (%d) out of bounds!\n", index);
                // Critical error.
                return -1; // Indicate failure
            }

			if (optimals[i].offset[subset] == 0)
			{
				if (optimals[i].len[subset] == 1)
				{
					write_literal(data_src[index]);
				}
				else
				{
					write_literals_length(optimals[i].len[subset]);
					for (j = 0;j < optimals[i].len[subset];j++)
					{
                        if (index + j >= MAX) {
                            if (bVerbose) printf("C: ERROR: RLE loop reading data_src[%d] out of bounds!\n", index + j);
                            return -1;
                        }
						write_byte(data_src[index + j]);
					}
				}
			}
			else
			{
				write_doublet(optimals[i].len[subset], optimals[i].offset[subset]);
			}
		} else {
            // This means the current position was "skipped" or "cleaned up" as part of a previous optimal match/RLE.
            // If bVerbose is very high, this might indicate an issue with cleanup_optimals,
            // but generally expected if len[subset] is 0.
            // if (bVerbose) printf("C: write_lz: Pos %d has len[subset] == 0 (skipped)\n", i);
        }
	}
	write_end();
    if (bVerbose) printf("C: write_lz END. Final index_dest: %d\n", index_dest);
	return index_dest; // Return the compressed size
}

/*
 * - READ SOURCE - (Removed as data comes from JS memory)
 * IMPORTANT: Corrected comment to avoid nested block comments.
 */
// unsigned char read_source() { /* ... */ } // Line 402, was problematic. Changed to //

// Function Prototype: Declare golomb_gamma_bits before it's used in count_bits
int golomb_gamma_bits(int value);

/*
 * - GOLOMB GAMMA LIMIT TO 254 -
 */
int golomb_gamma_bits(int value)
{
	int bits = 0;
	value++;
	while (value > 1)
	{
		bits += 2;
		value >>= 1;
	}
	return bits;
}

int count_bits(int offset, int len)
{
    if (bVerbose) printf("C: count_bits: offset=%d, len=%d (BIT_OFFSET3=%d)\n", offset, len, BIT_OFFSET3);
	int bits = 1 + golomb_gamma_bits(len);
	if (len == 1)
	{
		if (BIT_OFFSET00 == -1) // This condition might be specific to some variant; if BIT_OFFSET00 is always 0, this branch might not be hit.
		{
            if (bVerbose) printf("C: count_bits (len=1, BIT_OFFSET00=-1): %d + %d = %d\n", bits, BIT_OFFSET0, bits + BIT_OFFSET0);
			return bits + BIT_OFFSET0;
		}
		else
		{
            int offset_bits = (offset > MAX_OFFSET00 ? BIT_OFFSET0 : BIT_OFFSET00);
            if (bVerbose) printf("C: count_bits (len=1, BIT_OFFSET00=0): %d + 1 + %d = %d (offset_bits=%d)\n", bits, offset_bits, bits + 1 + offset_bits, offset_bits);
			return bits + 1 + offset_bits;
		}
	}
    // For len > 1
    int offset_cost;
    if (offset > MAX_OFFSET2) {
        offset_cost = 1 + BIT_OFFSET3;
    } else if (offset > MAX_OFFSET1) {
        offset_cost = BIT_OFFSET2;
    } else {
        offset_cost = 1 + BIT_OFFSET1;
    }
    if (bVerbose) printf("C: count_bits (len>1): %d + 1 + %d = %d (offset_cost=%d)\n", bits, offset_cost, bits + 1 + offset_cost, offset_cost);
	return bits + 1 + offset_cost;
}

EMSCRIPTEN_KEEPALIVE void set_BIT_OFFSET3(int i)
{
    // This function can be called very frequently; verbose print might be too much.
    // if (bVerbose) printf("C: set_BIT_OFFSET3(%d): BIT_OFFSET3=%d, MAX_OFFSET3=%d\n", i, BIT_OFFSET_MIN + i, (1 << (BIT_OFFSET_MIN + i)) + MAX_OFFSET2);
	BIT_OFFSET3 = BIT_OFFSET_MIN + i;
	MAX_OFFSET3 = (1 << BIT_OFFSET3) + MAX_OFFSET2;
}

void update_optimal(int index, int len, int offset)
{
    // This function is called for every (index, len, offset) combination.
    // Full verbose output here will be overwhelming for large files.
    // Only enable if debugging a very specific index.
    // if (bVerbose) printf("C: update_optimal START: index=%d, len=%d, offset=%d\n", index, len, offset);

	int i;
	int cost;
	i = BIT_OFFSET_NBR_ALLOWED - 1;
	while (i >= 0)
	{
        // if (bVerbose) printf("C:   update_optimal: checking subset %d\n", i);
        if (index < 0 || index >= MAX) {
            if (bVerbose) printf("C: CRITICAL ERROR: update_optimal index (%d) out of bounds for optimals array! Aborting.\n", index);
            emscripten_console_log("C-CRITICAL: update_optimal index OOB!");
            EM_ASM({ debugger; });
            abort();
        }

		if (offset == 0) // Literal or RLE
		{
			if (index > 0)
			{
                int prev_bits_idx = (index - 1);
                // Defensive check before accessing optimals[index-1]
                if (prev_bits_idx < 0 || prev_bits_idx >= MAX) {
                    if (bVerbose) printf("C: CRITICAL ERROR: update_optimal prev_bits_idx (%d) out of bounds for optimals array! Aborting.\n", prev_bits_idx);
                    emscripten_console_log("C-CRITICAL: update_optimal prev_bits_idx OOB!");
                    EM_ASM({ debugger; });
                    abort();
                }
                if (optimals[index-1].bits[i] == 0x7FFFFFFF) { // If previous state is unreachable
                    // if (bVerbose) printf("C:     update_optimal: prev state (index-1) unreachable for subset %d\n", i);
                    i--;
                    continue;
                }

				if (len == 1)
				{
					// Literal: cost = previous_cost + 1_bit_flag + 8_bits_data
					cost = optimals[prev_bits_idx].bits[i] + 1 + 8;
					if (optimals[index].bits[i] > cost)
					{
                        // if (bVerbose) printf("C:       update_optimal: Literal improved for subset %d, cost %d -> %d\n", i, optimals[index].bits[i], cost);
					    optimals[index].bits[i] = cost;
					    optimals[index].offset[i] = 0;
					    optimals[index].len[i] = 1;
                    }
				}
				else // RLE
				{
                    int prev_len_bits_idx = (index - len);
                    // Defensive check before accessing optimals[index-len]
                    if (prev_len_bits_idx < 0 || prev_len_bits_idx >= MAX) {
                        if (bVerbose) printf("C: CRITICAL ERROR: update_optimal prev_len_bits_idx (%d) out of bounds for optimals array! Aborting.\n", prev_len_bits_idx);
                        emscripten_console_log("C-CRITICAL: update_optimal prev_len_bits_idx OOB!");
                        EM_ASM({ debugger; });
                        abort();
                    }
                    if (optimals[prev_len_bits_idx].bits[i] == 0x7FFFFFFF) { // If previous RLE base state unreachable
                        // if (bVerbose) printf("C:     update_optimal: prev RLE state (index-len=%d) unreachable for subset %d\n", prev_len_bits_idx, i);
                        i--;
                        continue;
                    }
					cost = optimals[prev_len_bits_idx].bits[i] + 1 + BIT_GOLOMG_MAX + 1 + 8 + len * 8;
					if (optimals[index].bits[i] > cost)
					{
                        // if (bVerbose) printf("C:       update_optimal: RLE len=%d improved for subset %d, cost %d -> %d\n", len, i, optimals[index].bits[i], cost);
						optimals[index].bits[i] = cost;
						optimals[index].offset[i] = 0;
						optimals[index].len[i] = len;
					}
				}
			}
			else // index == 0 (first byte)
			{
				optimals[index].bits[i] = 8;
				optimals[index].offset[i] = 0;
				optimals[index].len[i] = 1;
                // if (bVerbose) printf("C:       update_optimal: First byte, cost = 8 for subset %d\n", i);
			}
		}
		else // Match
		{
			if (offset > index) { // Invalid offset (match goes before start of data_src)
                if (bVerbose) printf("C:     update_optimal: Match offset %d > index %d, invalid for subset %d\n", offset, index, i);
				i--;
				continue;
			}

            int prev_match_bits_idx = (index - len);
            // Defensive check before accessing optimals[index-len]
            if (prev_match_bits_idx < 0 || prev_match_bits_idx >= MAX) {
                if (bVerbose) printf("C: CRITICAL ERROR: update_optimal prev_match_bits_idx (%d) out of bounds for optimals array! Aborting.\n", prev_match_bits_idx);
                emscripten_console_log("C-CRITICAL: update_optimal prev_match_bits_idx OOB!");
                EM_ASM({ debugger; });
                abort();
            }
            if (optimals[prev_match_bits_idx].bits[i] == 0x7FFFFFFF) { // If previous match base state unreachable
                // if (bVerbose) printf("C:     update_optimal: prev match state (index-len=%d) unreachable for subset %d\n", prev_match_bits_idx, i);
                i--;
                continue;
            }

			if (offset > MAX_OFFSET1)
			{
				set_BIT_OFFSET3(i);
				if (offset > MAX_OFFSET3) {
                    if (bVerbose) printf("C:     update_optimal: Match offset %d > MAX_OFFSET3 (%d) for subset %d. Skipping.\n", offset, MAX_OFFSET3, i);
                    i--; // Decrement i before continuing the loop
                    continue; // Offset too large for this subset, try next subset
                }
			}
			cost = optimals[prev_match_bits_idx].bits[i] + count_bits(offset, len);
			if (optimals[index].bits[i] > cost)
			{
                // if (bVerbose) printf("C:       update_optimal: Match len=%d offset=%d improved for subset %d, cost %d -> %d\n", len, offset, i, optimals[index].bits[i], cost);
				optimals[index].bits[i] = cost;
				optimals[index].offset[i] = offset;
				optimals[index].len[i] = len;
			}
		}
		i--;
	}
    // if (bVerbose) printf("C: update_optimal END.\n");
}

/*
 * - REMOVE USELESS FOUND OPTIMALS -
 */
EMSCRIPTEN_KEEPALIVE void cleanup_optimals(int subset)
{
    if (bVerbose) printf("C: cleanup_optimals START for subset %d (index_src=%d)\n", subset, index_src);
	int j;
	int i = index_src - 1;
	int len;
	while (i > 1) // Loop from end backwards
	{
        if (i < 0 || i >= MAX) {
            if (bVerbose) printf("C: ERROR: cleanup_optimals loop index i (%d) out of bounds (0-%d)\n", i, MAX-1);
            break; // Stop processing this optimal
        }
        if (subset < 0 || subset >= BIT_OFFSET_NBR) {
            if (bVerbose) printf("C: ERROR: cleanup_optimals subset (%d) out of bounds (0-%d)\n", subset, BIT_OFFSET_NBR-1);
            break; // Stop processing
        }

		len = optimals[i].len[subset];
        // if (bVerbose) printf("C:   cleanup_optimals: at index %d, len = %d\n", i, len);

        if (len <= 0) { // If it's a literal or already cleaned up
             i--; // Move to previous position
             continue; // Nothing to clean before this point related to this segment
        }

		for (j = i - 1; j > i - len;j--) // Clean up positions covered by this optimal token
		{
            if (j < 0 || j >= MAX) {
                if (bVerbose) printf("C: ERROR: cleanup_optimals inner loop index j (%d) out of bounds!\n", j);
                break; // Prevent crash
            }
            if (optimals[j].offset[subset] != 0 || optimals[j].len[subset] != 0) {
                 if (bVerbose) printf("C:     cleanup_optimals: Clearing index %d (was offset=%d, len=%d)\n", j, optimals[j].offset[subset], optimals[j].len[subset]);
            }
			optimals[j].offset[subset] = 0;
			optimals[j].len[subset] = 0;
		}
		i = i - len; // Jump back to the start of the current optimal token
	}
    if (bVerbose) printf("C: cleanup_optimals END.\n");
}

/* DAN3 Encoder - Decoder (Emscripten Friendly with Debug Prints)
 * Fixed bounds checking issue in LZ MATCH OF 2+ section
 * The key fix: Move bounds checking BEFORE calling update_optimal()
 */

// ... [Keep all the existing includes and defines as they are] ...

// In the lzss_slow() function, replace the problematic section with this fixed version:

int lzss_slow()
{
    if (bVerbose) printf("C: lzss_slow START. index_src: %d, bRLE: %d, bFAST: %d\n", index_src, bRLE, bFAST);
	int best_len;
	int len;
	int i, j, k;
	int offset;
	int match_index, prev_match_index = -1;
	int bits_minimum_temp, bits_minimum;
	struct t_match *match;

    // Reset internal state for a fresh compression run
    reset_matches();
    // Initialize optimals table with a very large value (effectively Infinity)
    if (bVerbose) printf("C: lzss_slow: Initializing optimals table...\n");
    for(int x = 0; x < MAX; x++) { // Loop up to MAX, as index_src might be smaller
        for(int y = 0; y < BIT_OFFSET_NBR; y++) {
            optimals[x].bits[y] = 0x7FFFFFFF; // Max signed 32-bit int, acts as Infinity
            optimals[x].offset[y] = 0;
            optimals[x].len[y] = 0;
        }
    }
    // Initialize the first byte
    if (index_src > 0) {
        update_optimal(0, 1, 0);
    } else {
        if (bVerbose) printf("C: lzss_slow: index_src is 0, nothing to compress.\n");
        return 0; // Return 0 length if input is empty
    }

	i = 1;
	while (i < index_src)
	{
		if (bVerbose && (i % 1000 == 0 || i == index_src - 1)) {
            printf("C: lzss_slow: Scan progress %d/%d bytes\n", i + 1, index_src);
        }
		/* TRY LITERALS */
		update_optimal(i, 1, 0);

		/* STRING OF LITERALS (RLE) */
		if (bRLE)
		{
			if (i >= RAW_MIN)
			{
				j = RAW_MAX;
				if (j > i) j = i;
				if (RAW_MIN == 1)
				{
					for (k = j ; k > RAW_MIN; k--)
					{
						update_optimal(i, k, 0);
					}
				}
				else
				{
					/* RAW MINIMUM > 1 */
					for (k = j ; k >= RAW_MIN; k--)
					{
						update_optimal(i, k, 0);
					}
				}
			}
		}

		/* LZ MATCH OF 1 */
		j = (BIT_OFFSET00 == -1 ? (1 << BIT_OFFSET0) : MAX_OFFSET0);
		if (j > i) j = i;
		for (k = 1; k <= j; k++)
		{
            // Check data_src bounds before access in LZ MATCH OF 1
            if (i < 0 || i >= MAX || (i - k) < 0 || (i - k) >= MAX) {
                if (bVerbose) printf("C: CRITICAL ERROR: LZ MATCH OF 1 data_src[%d] or data_src[%d] out of bounds (i=%d, k=%d)! Aborting.\n", i, i-k, i, k);
                emscripten_console_log("C-CRITICAL: LZ MATCH OF 1 OOB!");
                EM_ASM({ debugger; });
                abort();
            }
			if (data_src[i] == data_src[i-k])
			{
				update_optimal(i, 1, k);
			}
		}

		/* LZ MATCH OF 2+ - FIXED VERSION */
        if (i -1 < 0 || i >= MAX) { // Defensive check for data_src[i-1]
            if (bVerbose) printf("C: ERROR: LZ MATCH OF 2+ (i=%d) out of bounds for data_src[%d]\n", i, i-1);
            // Potentially return error.
            prev_match_index = -1; // Cannot process, reset
        } else {
		    match_index = ((int) data_src[i-1]) << 8 | ((int) data_src[i] & 255);
		    match = &matches[match_index];

		    if (prev_match_index == match_index && bFAST == TRUE && optimals[i-1].offset[0] == 1 && optimals[i-1].len[0] > 2)
		    {
			    len = optimals[i-1].len[0];
			    if (len < MAX_GAMMA)
                {
                    // BOUNDS CHECK BEFORE update_optimal call
                    if (i >= len && i - len >= 0 && i - len < MAX) {
				        update_optimal(i, len + 1, 1);
                    }
                }
		    }
		    else
		    {
			    best_len = 1;
			    for (;match->next != NULL; match = match->next)
			    {
				    offset = i - match->index;
				    if (offset > MAX_OFFSET)
				    {
					    flush_match(match); // Remove old matches
					    break;
				    }
                    if (offset <= 0 || i - offset < 0) { // Defensive check for offset validity
                        if (bVerbose) printf("C: ERROR: LZ MATCH OF 2+ (i=%d, offset=%d) invalid for match. Skipping.\n", i, offset);
                        continue;
                    }
				    
                    // FIXED: Check bounds BEFORE trying different lengths
				    for (len = 2; len <= MAX_GAMMA; len++)
				    {
                        // CRITICAL FIX: Move bounds check BEFORE update_optimal call
                        if (i - len < 0 || (i - len - offset) < 0 || (i - len) >= MAX || (i - len - offset) >= MAX) {
                            if (bVerbose) printf("C: BOUNDS: LZ MATCH OF 2+ would access out of bounds for len=%d, offset=%d at i=%d. Breaking.\n", len, offset, i);
                            break; // Stop trying longer lengths for this offset
                        }
                        
                        // Now it's safe to call update_optimal
					    update_optimal(i, len, offset);
					    best_len = len;
                        
                        // Check if the match continues (this is the original match verification logic)
					    if (i < offset + len || data_src[i-len] != data_src[i-len-offset])
					    {
						    break;
					    }
				    }
				    if (bFAST && best_len > 255) break;
			    }
		    }
		    prev_match_index = match_index;
		    insert_match(&matches[match_index], i);
        }
		i++;
	}
    if (bVerbose) printf("C: lzss_slow: Scan done.\n");

    // Select the best subset
    if (index_src <= 0) { // Handle empty input gracefully after scan
        if (bVerbose) printf("C: lzss_slow: Empty input after scan, returning 0.\n");
        return 0; // Return 0 length if input is empty
    }

	bits_minimum = optimals[index_src-1].bits[0];
    if (optimals[index_src-1].bits[0] == 0x7FFFFFFF) {
        if (bVerbose) printf("C: lzss_slow: Subset 0 is unreachable at end. Trying others.\n");
    }

	j = 0; // j will hold the index of the best subset based on bits_minimum
	for (i = 0;i < BIT_OFFSET_NBR_ALLOWED;i++)
	{
		bits_minimum_temp = optimals[index_src-1].bits[i];
        if (bits_minimum_temp == 0x7FFFFFFF) { // If this subset is unreachable
            if (bVerbose) printf("C: lzss_slow: Subset %d is unreachable.\n", i);
            continue;
        }
        if (bVerbose) printf("C: lzss_slow: Subset %d (%d bits) cost: %d\n", i, BIT_OFFSET_MIN + i, bits_minimum_temp);

		if (bits_minimum_temp < bits_minimum)
		{
			bits_minimum = bits_minimum_temp;
			j = i;
		}
	}
    if (bVerbose) printf("C: lzss_slow: Best subset chosen: %d (offset_bits: %d) with %d bits.\n", j, BIT_OFFSET_MIN + j, bits_minimum);

    if (bits_minimum == 0x7FFFFFFF) { // If even the "best" subset is unreachable
        if (bVerbose) printf("C: ERROR: lzss_slow: All subsets unreachable. Cannot compress.\n");
        return -1; // Indicate failure
    }

	set_BIT_OFFSET3(j); // Set globals based on the chosen optimal subset
	cleanup_optimals(j); // Clean up based on the chosen optimal subset
	return write_lz(j); // Write the compressed data and return its size
}

/* 
 * KEY CHANGES MADE:
 * 
 * 1. CRITICAL FIX: Moved the bounds checking BEFORE the update_optimal() call in the LZ MATCH OF 2+ section
 * 2. Added proper bounds checking for the fast path optimization case
 * 3. Changed the bounds check behavior from abort() to break, allowing the algorithm to continue with shorter lengths
 * 4. Added more defensive programming for edge cases
 * 
 * The main issue was that update_optimal() was being called with potentially invalid parameters,
 * and it would try to access the optimals array with out-of-bounds indices before the bounds check
 * could catch the problem. Now the bounds are verified first, making the code much safer.
 */

/*
 * - DECOMPRESSION LOGIC - (Core decompression logic)
 */
int delzss()
{
    if (bVerbose) printf("C: delzss START. index_src (compressed_len): %d\n", index_src);
	int	subset = 0;
	int old_index_src = index_src; // Total length of compressed input
	int len, offset;
	int i;

	// Reset bit counters for reading
	index_src = 0; // Reset index_src to start of compressed data
	bit_mask = 0;
	bit_index = 0;

	// Read subset header
    if (old_index_src <= 0) {
        if (bVerbose) printf("C: delzss: Empty compressed input.\n");
        return 0;
    }
    if (index_src >= old_index_src) { // Check if we ran out of input after header
        if (bVerbose) printf("C: delzss: Compressed input too short to read header.\n");
        return -1; // Error
    }
    if (bVerbose) printf("C: delzss: Reading subset header (index_src: %d, old_index_src: %d)...\n", index_src, old_index_src);
	while (read_bit() != 0)
	{
		subset++;
        if (subset > BIT_OFFSET_NBR || index_src >= old_index_src) { // Prevent infinite loop or OOB read
            if (bVerbose) printf("C: ERROR: delzss: Subset header read too long or OOB!\n");
            return -1;
        }
	}
    if (bVerbose) printf("C: delzss: Selected subset %d (offset_bits %d).\n", subset, subset + BIT_OFFSET_MIN);

	// First byte raw
	index_dest = 0; // Reset index_dest for writing decompressed data
    if (index_src >= old_index_src) { // Check if we ran out of input after header
        if (bVerbose) printf("C: ERROR: delzss: Compressed input too short after subset header to read first byte.\n");
        return -1;
    }
    unsigned char first_byte = read_byte();
	write_byte(first_byte);
    if (bVerbose) printf("C: delzss: Wrote first byte: 0x%02X at index_dest %d\n", first_byte, index_dest - 1);


	while (index_src < old_index_src) // Loop until end of compressed input
	{
        if (bVerbose && index_dest % 1000 == 0) {
            printf("C: delzss: Decompression progress: %d bytes decompressed\n", index_dest);
        }
        if (index_src >= old_index_src) { // Check for read_bit, read_byte from OOB
            if (bVerbose) printf("C: delzss: End of compressed data reached unexpectedly.\n");
            break;
        }
		if (read_bit()) // Is next byte literal or match (1=literal, 0=match/RLE/End)
		{
			/* LITERAL */
            if (index_src >= old_index_src) {
                if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for literal byte.\n");
                return -1;
            }
            unsigned char lit_byte = read_byte();
			write_byte(lit_byte);
            if (bVerbose) printf("C: delzss: Decompressed literal byte 0x%02X at index_dest %d\n", lit_byte, index_dest - 1);
		}
		else // Match, RLE, or End marker
		{
			len = read_golomb_gamma();
            if (bVerbose) printf("C: delzss: Read golomb gamma len: %d\n", len);
			if (len == -1) // Special code / End marker
			{
                if (index_src >= old_index_src) {
                    if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for end/RLE flag.\n");
                    return -1;
                }
				if (read_bit() == 0) // End marker (0 0s, then 0)
				{
                    if (bVerbose) printf("C: delzss: End marker reached.\n");
					break; // EOF
				}
				else // RLE (0 0s, then 1)
				{
                    if (index_src >= old_index_src) {
                        if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for RLE length byte.\n");
                        return -1;
                    }
					len = read_byte() + 1; // Actual RLE length
                    if (bVerbose) printf("C: delzss: Decompressing RLE of length %d\n", len);
					for (i = 0; i < len; i++)
					{
                        if (index_src >= old_index_src) {
                            if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for RLE data byte %d/%d.\n", i, len);
                            return -1;
                        }
						write_byte(read_byte());
					}
				}
			}
			else // Match
			{
				offset = 0;
                if (bVerbose) printf("C: delzss: Decoding match (len=%d)...\n", len);

				if (len == 1) // Match length 1
				{
                    if (index_src >= old_index_src) {
                        if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for match offset bit (len=1).\n");
                        return -1;
                    }
					if (read_bit()) // Read 1 bit for short offset
					{
						offset = read_bit() + 1;
					} else {
                        offset = 0; // If len is 1 and first offset bit is 0, offset is 0. This seems unusual for LZSS matches.
                    }
                    if (bVerbose) printf("C: delzss: Match (len=1) offset: %d\n", offset);
				}
				else // Match length > 1
				{
                    if (index_src >= old_index_src) {
                        if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for match offset type bit (len>1).\n");
                        return -1;
                    }
					if (!read_bit()) // Read 1 bit for offset type (0 = 8-bit offset, 1 = longer offset)
					{
						/* 8bit offset */
                        if (index_src >= old_index_src) {
                            if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for 8-bit offset byte.\n");
                            return -1;
                        }
						offset = read_byte() + 32; // This '32' constant implies a specific offset base
                        if (bVerbose) printf("C: delzss: Match (len=%d) 8-bit offset: %d\n", len, offset);
					}
					else // Longer offset encoding (first bit was 1)
					{
                        if (index_src >= old_index_src) {
                            if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for long offset type bit.\n");
                            return -1;
                        }
						if (read_bit()) // If second bit is 1 (1 1 = very long offset)
						{
                            if (bVerbose) printf("C: delzss: Match (len=%d) very long offset (subset=%d, BIT_OFFSET_MIN=%d)\n", len, subset, BIT_OFFSET_MIN);
							for (i = 0;i < subset + BIT_OFFSET_MIN - 8;i++) // Read remaining bits for the full offset value
							{
                                if (index_src >= old_index_src) {
                                    if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for long offset bit %d/%d.\n", i, subset + BIT_OFFSET_MIN - 8);
                                    return -1;
                                }
								offset <<= 1;
								offset |= read_bit();
							}
                            if (index_src >= old_index_src) {
                                if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for long offset byte.\n");
                                return -1;
                            }
							offset <<= 8; // Shift to make room for the byte
							offset |= read_byte(); // Read the byte part
							offset += 256 + 32; // Add base offset
                            if (bVerbose) printf("C: delzss: Match (len=%d) very long offset calculated: %d\n", len, offset);
						}
						else // If second bit is 0 (1 0 = 5-bit offset)
						{
							/* 5 bits offset */
                            if (bVerbose) printf("C: delzss: Match (len=%d) 5-bit offset...\n", len);
							for (i = 0;i < 5;i++)
							{
                                if (index_src >= old_index_src) {
                                    if (bVerbose) printf("C: ERROR: delzss: Compressed input too short for 5-bit offset bit %d/5.\n", i);
                                    return -1;
                                }
								offset <<= 1;
								offset |= read_bit();
							}
                            if (bVerbose) printf("C: delzss: Match (len=%d) 5-bit offset calculated: %d\n", len, offset);
						}
					}
				}
				// Perform the match copy
                if (bVerbose) printf("C: delzss: Copying match: src_start_dest_index=%d, len=%d, offset=%d\n", index_dest - offset - 1, len, offset);

                int source_start_index = index_dest - offset - 1;
                if (source_start_index < 0 || source_start_index + len > index_dest) { // Basic bounds check for source
                    if (bVerbose) printf("C: CRITICAL ERROR: delzss: Match copy source bounds invalid! src_idx=%d, len=%d, current_dest=%d. Aborting.\n", source_start_index, len, index_dest);
                    emscripten_console_log("C-CRITICAL: Decomp match source OOB!");
                    EM_ASM({ debugger; });
                    abort();
                }
                if (index_dest + len > MAX) { // Basic bounds check for destination
                    if (bVerbose) printf("C: CRITICAL ERROR: delzss: Match copy dest bounds invalid! dest_idx=%d, len=%d, MAX=%d. Aborting.\n", index_dest, len, MAX);
                    emscripten_console_log("C-CRITICAL: Decomp match dest OOB!");
                    EM_ASM({ debugger; });
                    abort();
                }

				for (i = 0; i < len; i++)
				{
					// Ensure no read out of bounds from data_dest for source
					// This is `data_dest[current_write_pos] = data_dest[current_write_pos - offset - 1]`
					data_dest[index_dest + i] = data_dest[source_start_index + i];
				}
				index_dest += len;
			}
		}
	}
    if (bVerbose) printf("C: delzss END. Final index_dest: %d\n", index_dest);
	return index_dest; // Return decompressed size
}

/*
 * - WRAPPER FUNCTIONS FOR JAVASCRIPT -
 * These functions will be called from JavaScript via Emscripten.
 * They handle copying data between JavaScript's memory (passed pointers)
 * and C's internal global buffers (`data_src`, `data_dest`).
 */
// Function to set global compression options from JS
EMSCRIPTEN_KEEPALIVE
void set_dan3_options(int max_bits, int rle_enabled, int fast_mode) {
    if (bVerbose) printf("C: set_dan3_options called. max_bits=%d, rle=%d, fast=%d\n", max_bits, rle_enabled, fast_mode);
    if (max_bits > BIT_OFFSET_MAX) max_bits = BIT_OFFSET_MAX;
    if (max_bits < BIT_OFFSET_MIN) max_bits = BIT_OFFSET_MIN;
    BIT_OFFSET_MAX_ALLOWED = max_bits;
    BIT_OFFSET_NBR_ALLOWED = BIT_OFFSET_MAX_ALLOWED - BIT_OFFSET_MIN + 1;

    bRLE = rle_enabled; // C's TRUE/FALSE are -1/0. JS boolean is 1/0.
    bFAST = fast_mode;  // C's TRUE/FALSE are -1/0.
    if (bVerbose) printf("C: set_dan3_options: BIT_OFFSET_MAX_ALLOWED=%d, BIT_OFFSET_NBR_ALLOWED=%d, bRLE=%d, bFAST=%d\n",
                           BIT_OFFSET_MAX_ALLOWED, BIT_OFFSET_NBR_ALLOWED, bRLE, bFAST);
}

// Wrapper for encode function
// Takes input data, its length, and an output buffer pointer.
// Returns the compressed length.
EMSCRIPTEN_KEEPALIVE
int dan3_encode(uint8_t* input_buf, int input_len, uint8_t* output_buf) {
    if (bVerbose) printf("C: dan3_encode START. input_len=%d, input_buf=%p, output_buf=%p\n", input_len, (void*)input_buf, (void*)output_buf);
    // Ensure input_len doesn't exceed MAX
    if (input_len > MAX) {
        if (bVerbose) printf("C: ERROR: dan3_encode input_len %d exceeds MAX %d\n", input_len, MAX);
        return -1; // Indicate error
    }

    // Copy input data from JS memory to C's global data_src
    memcpy(data_src, input_buf, input_len);
    index_src = input_len; // Set C's global index_src

    // Reset bit counters before compression begins
    bit_mask = 0;
    bit_index = 0;

    // Call the original compression logic
    int compressed_len = lzss_slow();

    // Copy compressed data from C's global data_dest to JS-provided output_buf
    // Only copy if compression was successful (len >= 0)
    if (compressed_len >= 0) {
        // Defensive check: Ensure compressed_len doesn't exceed data_dest's capacity
        // This generally implies index_dest should not exceed MAX within write_lz
        if (compressed_len > MAX) {
            if (bVerbose) printf("C: ERROR: dan3_encode: compressed_len (%d) exceeds MAX (%d) after lzss_slow!\n", compressed_len, MAX);
            return -1; // Indicates internal overflow
        }
        memcpy(output_buf, data_dest, compressed_len);
        if (bVerbose) printf("C: dan3_encode END. Returned compressed_len: %d\n", compressed_len);
    } else {
        if (bVerbose) printf("C: dan3_encode END. lzss_slow returned error: %d\n", compressed_len);
    }

    return compressed_len;
}

// Wrapper for decode function
// Takes compressed input data, its length, and an output buffer pointer.
// Returns the decompressed length.
EMSCRIPTEN_KEEPALIVE
int dan3_decode(uint8_t* input_buf, int input_len, uint8_t* output_buf) {
    if (bVerbose) printf("C: dan3_decode START. input_len=%d, input_buf=%p, output_buf=%p\n", input_len, (void*)input_buf, (void*)output_buf);
    // Ensure input_len doesn't exceed MAX
    if (input_len > MAX) {
        if (bVerbose) printf("C: ERROR: dan3_decode input_len %d exceeds MAX %d\n", input_len, MAX);
        return -1; // Indicate error
    }

    // Copy compressed input data from JS memory to C's global data_src
    memcpy(data_src, input_buf, input_len);
    index_src = input_len; // Set C's global index_src (for reading compressed data)

    // Reset bit counters before decompression begins
    bit_mask = 0;
    bit_index = 0;

    // Call the original decompression logic
    int decompressed_len = delzss();

    // Copy decompressed data from C's global data_dest to JS-provided output_buf
    // Only copy if decompression was successful (len >= 0)
    if (decompressed_len >= 0) {
        // Defensive check: Ensure decompressed_len doesn't exceed data_dest's capacity (or original MAX if it's assumed)
        // This implies index_dest should not exceed MAX within delzss
        if (decompressed_len > MAX) {
            if (bVerbose) printf("C: ERROR: dan3_decode: decompressed_len (%d) exceeds MAX (%d) after delzss!\n", decompressed_len, MAX);
            return -1; // Indicates internal overflow
        }
        memcpy(output_buf, data_dest, decompressed_len);
        if (bVerbose) printf("C: dan3_decode END. Returned decompressed_len: %d\n", decompressed_len);
    } else {
        if (bVerbose) printf("C: dan3_decode END. delzss returned error: %d\n", decompressed_len);
    }

    return decompressed_len;
}

// Keeping original functions keepalive for direct internal testing if needed,
// but the wrappers are preferred for JS interaction.
// Note: These now call the new wrapper functions implicitly assuming data_src/dest are populated.
EMSCRIPTEN_KEEPALIVE int encode() { return dan3_encode(data_src, index_src, data_dest); }
EMSCRIPTEN_KEEPALIVE int decode() { return dan3_decode(data_src, index_src, data_dest); }

// Keep set_max_bits_allowed keepalive if it's explicitly called from JS
// (Though set_dan3_options replaces its functionality combined with flags)
// It's fine to keep it for now.
EMSCRIPTEN_KEEPALIVE void set_max_bits_allowed(int bits)
{
    if (bVerbose) printf("C: set_max_bits_allowed called (legacy). bits=%d\n", bits);
	if (bits > BIT_OFFSET_MAX) bits = BIT_OFFSET_MAX;
	if (bits < BIT_OFFSET_MIN) bits = BIT_OFFSET_MIN;
	BIT_OFFSET_MAX_ALLOWED = bits;
	BIT_OFFSET_NBR_ALLOWED = BIT_OFFSET_MAX_ALLOWED - BIT_OFFSET_MIN + 1;
}

// --- Debugging getter functions ---
EMSCRIPTEN_KEEPALIVE
int get_optimal_bits(int index, int subset) {
    if (index >= 0 && index < MAX && subset >= 0 && subset < BIT_OFFSET_NBR) {
        return optimals[index].bits[subset];
    }
    // Return a distinguishable error value
    return 0x7FFFFFFF; // Max signed 32-bit int, matches "Infinity" representation
}

EMSCRIPTEN_KEEPALIVE
int get_optimal_offset(int index, int subset) {
    if (index >= 0 && index < MAX && subset >= 0 && subset < BIT_OFFSET_NBR) {
        return optimals[index].offset[subset];
    }
    return -1;
}

EMSCRIPTEN_KEEPALIVE
int get_optimal_len(int index, int subset) {
    if (index >= 0 && index < MAX && subset >= 0 && subset < BIT_OFFSET_NBR) {
        return optimals[index].len[subset];
    }
    return -1;
}

EMSCRIPTEN_KEEPALIVE
int get_bit_mask() {
    return (int)bit_mask;
}

EMSCRIPTEN_KEEPALIVE
int get_bit_index() {
    return bit_index;
}

EMSCRIPTEN_KEEPALIVE
int get_bFAST() {
    return bFAST;
}

EMSCRIPTEN_KEEPALIVE
int get_bRLE() {
    return bRLE;
}

EMSCRIPTEN_KEEPALIVE
int get_BIT_OFFSET3() {
    return BIT_OFFSET3;
}

EMSCRIPTEN_KEEPALIVE
int get_MAX_OFFSET3() {
    return MAX_OFFSET3;
}

EMSCRIPTEN_KEEPALIVE
int get_BIT_OFFSET_MAX_ALLOWED() {
    return BIT_OFFSET_MAX_ALLOWED;
}

EMSCRIPTEN_KEEPALIVE
int get_BIT_OFFSET_NBR_ALLOWED() {
    return BIT_OFFSET_NBR_ALLOWED;
}

/*
 * - REMOVED MAIN FUNCTION AND FILE I/O HELPERS -
 * (These are handled by the JavaScript environment)
 */
// int main(int argc, char *argv[]) { // ... }
// void help(void) { // ... }
// int file_exits(char *filename) { // ... }
// long file_size(FILE *file) { // ... }
// int yesno () { // ... }
// void ask_overwrite(char *filename) { // ... }
// char *newfilepathLZ(char* filepath) { // ... }
// char *newfilepathRAW(char* filepath) { // ... }