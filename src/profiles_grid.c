/*
 Tile layout and focus navigation for the profile picture chooser.
 Pure C, no PS2SDK dependency: also built by the host-side tests.
*/

#include "include/profiles_grid.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

void profilesGridSlotPos(int slot, int *row, int *col)
{
    int r = -1, c = -1;

    if (slot >= 0 && slot < PROFILE_IMAGE_COUNT) {
        r = slot / PROFILE_GRID_COLUMNS;
        c = slot % PROFILE_GRID_COLUMNS;
    } else if (slot == PROFILE_GRID_DEFAULT_SLOT) {
        // Centered under the second row.
        r = PROFILE_GRID_ROWS;
        c = PROFILE_GRID_COLUMNS / 2;
    }

    if (row != NULL)
        *row = r;
    if (col != NULL)
        *col = c;
}

int profilesGridSlotAt(int row, int col)
{
    int slot;

    for (slot = 0; slot < PROFILE_GRID_TILE_COUNT; slot++) {
        int slotRow, slotCol;

        profilesGridSlotPos(slot, &slotRow, &slotCol);
        if (slotRow == row && slotCol == col)
            return slot;
    }

    return -1;
}

int profilesGridMove(int slot, int dRow, int dCol, const unsigned char *available)
{
    int row, col;

    if (available == NULL)
        return slot;

    profilesGridSlotPos(slot, &row, &col);
    if (row < 0 || col < 0)
        return slot;

    row += dRow;
    col += dCol;

    while (row >= 0 && row <= PROFILE_GRID_ROWS && col >= 0 && col < PROFILE_GRID_COLUMNS) {
        int candidate = profilesGridSlotAt(row, col);

        if (candidate >= 0 && available[candidate])
            return candidate;

        row += dRow;
        col += dCol;
    }

    // Nothing selectable in that direction. Staying on the current tile is only
    // safe when it is selectable itself; otherwise fall back to the first
    // selectable tile so the cursor can never rest on a missing picture.
    if (available[slot])
        return slot;

    {
        int first;

        for (first = 0; first < PROFILE_GRID_TILE_COUNT; first++) {
            if (available[first])
                return first;
        }
    }

    return slot;
}

int profilesGridFirstAvailable(const unsigned char *available)
{
    int slot;

    if (available == NULL)
        return -1;

    for (slot = 0; slot < PROFILE_GRID_TILE_COUNT; slot++) {
        if (available[slot])
            return slot;
    }

    return -1;
}

int profilesGridSlotImage(int slot, char *out, int outLen)
{
    int written;

    if (out == NULL || outLen < 1)
        return 0;

    if (slot == PROFILE_GRID_DEFAULT_SLOT) {
        out[0] = '\0';
        return 1;
    }

    if (slot < 0 || slot >= PROFILE_IMAGE_COUNT)
        return 0;

    written = snprintf(out, outLen, "profile_%d.png", slot + 1);
    return (written > 0 && written < outLen);
}

int profilesGridSlotFromImage(const char *imageName)
{
    int i;
    char name[PROFILE_IMAGE_NAME_MAX + 1];

    if (imageName == NULL || imageName[0] == '\0')
        return PROFILE_GRID_DEFAULT_SLOT;

    for (i = 0; i < PROFILE_IMAGE_COUNT; i++) {
        if (!profilesGridSlotImage(i, name, sizeof(name)))
            continue;
        if (strcasecmp(name, imageName) == 0)
            return i;
    }

    return -1;
}
