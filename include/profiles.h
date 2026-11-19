#ifndef __PROFILES_H
#define __PROFILES_H

#include "include/mcemu.h"

// Maximum number of profiles kept in the index.
#define PROFILE_MAX_COUNT 16

// Maximum length of a profile display name (not including the terminator).
#define PROFILE_NAME_MAX 23

// Highest allowed numeric profile ID (directory name is zero padded to 6 digits).
#define PROFILE_ID_MAX 999999

// Default VMC size, in megabytes. This matches the launcher's default slot size.
#define PROFILE_DEFAULT_VMC_SIZE 8

// Slot 1 is used by the profile system. Slot 2 is reserved for a future update.
#define PROFILE_VMC_SLOT_COUNT 2

#define PROFILE_VMC_NAME_MAX 32
#define PROFILE_PATH_MAX     256

#define PROFILE_BASE_DIR "PS2L_PROFILES"

// Locate the storage device that holds the games, create PS2L_PROFILES and load
// the profile index. Returns 1 when profiles can be used, 0 otherwise.
int profilesInit(void);

int profilesAvailable(void);
const char *profilesGetRoot(void);

int profilesGetCount(void);
int profilesGetId(int index);
const char *profilesGetName(int index);
int profilesFindIndex(int id);

int profilesGetActiveId(void);
const char *profilesGetActiveName(void);
int profilesSetActive(int id);
int profilesClearActive(void);

// Display the profile selection screen. Returns 1 when a profile is active,
// 0 when profiles are unavailable.
int profilesShowSelection(void);

int profilesIsValidName(const char *name);

// True when the given device path (e.g. "mass0:/DVD/") is on the storage device
// that owns the profiles directory.
int profilesDeviceMatches(const char *path);

// Resolve the VMC file that belongs to the active profile for a game.
// configuredVMC is the game's existing slot setting (may be empty or NULL); the
// per-profile multi-disc map takes precedence, then the configured name, then
// "<gameId>_<slot>".
int profilesGetVMCFile(const char *gameId, const char *configuredVMC, int slot, char *path, int pathLen);

// Resolve an explicit VMC name inside the active profile's VMC directory.
// Used by the per-game VMC settings dialog so it manages profile saves.
int profilesGetVMCFilePath(const char *vmcName, char *path, int pathLen);

// Verify the VMC file and create it (8 MB) through genvmc when it is missing or
// invalid. Waits for the asynchronous creation to finish. Returns the VMC size
// in megabytes, or -1 on failure.
int profilesPrepareVMC(const char *path, vmc_superblock_t *vmc_superblock);

#endif
