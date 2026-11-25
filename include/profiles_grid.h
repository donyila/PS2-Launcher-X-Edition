#ifndef __PROFILES_GRID_H
#define __PROFILES_GRID_H

/*
 Tile layout and focus navigation for the profile picture chooser.

 The chooser shows the ten selectable pictures (profile_1.png .. profile_10.png)
 in a 5 x 2 grid plus one extra tile that selects the default user.png picture.
 Only tiles whose picture was found and decoded are selectable, so the focus has
 to be able to skip missing files without ever getting stuck.

 This module is pure C with no PS2SDK dependency, so the navigation rules can be
 unit tested on a normal host (tests/profiles_path_test.c).
*/

#include "include/profiles_path.h"

#define PROFILE_GRID_COLUMNS      5
#define PROFILE_GRID_ROWS         2

/* Slot index of the "default picture" tile (the embedded user.png). */
#define PROFILE_GRID_DEFAULT_SLOT PROFILE_IMAGE_COUNT

/* Number of tiles drawn by the chooser (ten pictures + the default tile). */
#define PROFILE_GRID_TILE_COUNT   (PROFILE_IMAGE_COUNT + 1)

/* Grid position of a tile. The default tile sits in the middle of a third row. */
void profilesGridSlotPos(int slot, int *row, int *col);

/* Tile at a grid position, or -1 when that position is empty. */
int profilesGridSlotAt(int row, int col);

/*
 Moves the focus one step from slot in the given direction (dRow/dCol are -1, 0
 or 1) and returns the first selectable tile found along that line. When there
 is no selectable tile in that direction the current slot is returned, so the
 focus never lands on an unavailable picture and never disappears.
 available must have PROFILE_GRID_TILE_COUNT entries (non-zero = selectable).
*/
int profilesGridMove(int slot, int dRow, int dCol, const unsigned char *available);

/* First selectable tile, or -1 when there is none. */
int profilesGridFirstAvailable(const unsigned char *available);

/*
 Stored picture name for a tile: "profile_<n>.png", or "" for the default tile
 (which means "use user.png"). Returns 0 when slot or the buffer is unusable.
*/
int profilesGridSlotImage(int slot, char *out, int outLen);

/* Slot for a stored picture name, or -1. "" maps to the default tile. */
int profilesGridSlotFromImage(const char *imageName);

#endif
