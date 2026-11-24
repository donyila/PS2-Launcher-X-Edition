#ifndef __PROFILES_H
#define __PROFILES_H

#include "include/mcemu.h"
#include "include/profiles_path.h"

// Maximum number of profiles kept in the index.
#define PROFILE_MAX_COUNT 16

// Default VMC size, in megabytes. This matches the launcher's default slot size.
#define PROFILE_DEFAULT_VMC_SIZE 8

// Locate the storage device that holds the games, create PS2L_PROFILES and load
// the profile index. Returns 1 when profiles can be used, 0 otherwise.
int profilesInit(void);

int profilesAvailable(void);
const char *profilesGetRoot(void);

int profilesGetCount(void);
int profilesGetId(int index);
const char *profilesGetName(int index);

// Stored profile picture name ("profile_3.png") or "" when the default
// user.png picture is used.
const char *profilesGetImage(int index);

int profilesFindIndex(int id);

int profilesGetActiveId(void);
const char *profilesGetActiveName(void);
int profilesSetActive(int id);
int profilesClearActive(void);

// Display the profile selection screen. Returns 1 when a profile is active,
// 0 when profiles are unavailable.
int profilesShowSelection(void);

int profilesIsValidName(const char *name);

// True when another profile already uses this display name (case insensitive).
// ignoreId lets a rename keep its own current name. Pass 0 to check all.
int profilesNameInUse(const char *name, int ignoreId);

// Create, rename or delete a profile. These are the only entry points the UI
// uses, so the numeric ID and the profile directory of an existing profile can
// never change. imageName is "" for the default picture or a validated
// profile_N.png name.
int profilesCreateProfile(const char *name, const char *imageName, int *outId);
int profilesRenameProfile(int id, const char *name);
int profilesDeleteProfile(int id);
int profilesSetProfileImage(int id, const char *imageName);

// Absolute device path of a profile picture: <root>/profile_img/<name>.
int profilesImagePath(const char *imageName, char *path, int pathLen);

// Select the physical PlayStation 2 Memory Card in Slot 1 as the save
// destination. Clears the active profile and disables the profile VMC
// redirection so the original launcher memory card slot behaviour is used.
// Returns 0 when no usable Slot 1 card is detected; a card in Slot 2 alone is
// not accepted and there is no fallback to a USB VMC.
int profilesSelectMemoryCard(void);
int profilesIsPhysicalMC(void);

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
