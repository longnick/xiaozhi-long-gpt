/* Minimal 6x8 ASCII font (32..126). For brevity, only a subset shown. Replace with a full table if needed. */
#include <stdint.h>
#include "font6x8.h"

// 6x8 per char, LSB first horizontally. Here we include digits, letters, some symbols.
// For a complete set you can paste a standard 6x8 bitmap font.
const uint8_t g_font6x8[96][6] = {
  // 0x20 ' '
  {0,0,0,0,0,0},
  // 0x21 '!' (etc... minimal set)
  {0x00,0x00,0x5F,0x00,0x00,0x00},
  // 0x22 '"'
  {0x00,0x07,0x00,0x07,0x00,0x00},
  // 0x23 '#'
  {0x14,0x7F,0x14,0x7F,0x14,0x00},
  // 0x24 '$'
  {0x24,0x2A,0x7F,0x2A,0x12,0x00},
  // ... (please replace with a full 6x8 font table for production)
};
*/
