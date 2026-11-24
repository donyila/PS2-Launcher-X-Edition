#ifndef __PROFILES_PATH_H
#define __PROFILES_PATH_H

/*
 Path and name helpers for the multi-profile system.

 Everything the profile system stores lives below a single device root, e.g.
 "mass0:/":

   <root>/PS2L_PROFILES/profiles.dat
   <root>/PS2L_PROFILES/USERS/<ID>/profile.ini
   <root>/PS2L_PROFILES/USERS/<ID>/VMC/<VMC NAME>.bin

 This module owns the one and only implementation of those layouts so the
 directory names can never be duplicated or drift apart again. It has no
 dependency on the PS2SDK so the path construction can be unit tested on a
 normal host.
*/

// Maximum length of a profile display name (not including the terminator).
#define PROFILE_NAME_MAX 23

// Highest allowed numeric profile ID (directory name is zero padded to 6 digits).
#define PROFILE_ID_MAX 999999

// Maximum length of a resolved profile path (including the terminator).
#define PROFILE_PATH_MAX 256

// Maximum length of a VMC name (without the ".bin" suffix).
#define PROFILE_VMC_NAME_MAX 32

// Maximum length of a stored profile picture file name ("profile_10.png").
#define PROFILE_IMAGE_NAME_MAX 20

// Number of selectable profile pictures, profile_1.png .. profile_10.png.
#define PROFILE_IMAGE_COUNT 10

// Directory (below the device root) that holds the selectable pictures. The
// files are ordinary PNG data even though the directory is named profile_img.
#define PROFILE_IMAGE_DIR "profile_img"

// VMC slots used by the launcher: Slot 1 is used by the profile system, Slot 2
// is reserved and must never be activated by it.
#define PROFILE_VMC_SLOT_COUNT 2

// Directory that holds every profile asset, directly on the game device root.
#define PROFILE_BASE_DIR "PS2L_PROFILES"

// <root>/PS2L_PROFILES
int profilesPathBase(char *out, int length, const char *deviceRoot);

// <root>/PS2L_PROFILES/USERS
int profilesPathUsers(char *out, int length, const char *deviceRoot);

// <root>/PS2L_PROFILES/USERS/<ID>
int profilesPathUser(char *out, int length, const char *deviceRoot, int id);

// <root>/PS2L_PROFILES/USERS/<ID>/VMC
int profilesPathVmcDir(char *out, int length, const char *deviceRoot, int id);

// <root>/PS2L_PROFILES/USERS/<ID>/VMC/<sanitized name>.bin
// The name is sanitized here, so callers may pass a raw game ID or VMC name.
int profilesPathVmcFile(char *out, int length, const char *deviceRoot, int id, const char *vmcName);

// <root>/PS2L_PROFILES/USERS/<ID>/profile.ini
int profilesPathIni(char *out, int length, const char *deviceRoot, int id);

// <root>/PS2L_PROFILES/profiles.dat
int profilesPathIndex(char *out, int length, const char *deviceRoot);

// <root>/profile_img/<name>
// The name must be a valid profile picture name (profile_1.png .. profile_10.png).
int profilesPathProfileImage(char *out, int length, const char *deviceRoot, const char *imageName);

// Produces a FAT-safe, path-traversal-safe VMC name. Invalid characters are
// replaced, never used to build a directory. Returns 0 for an unusable name.
int profilesNameSanitizeVmc(const char *input, char *output, int outputLength);

// Validates a user supplied profile display name.
int profilesNameIsValid(const char *name);

// Validates a profile picture file name: exactly "profile_<n>.png" for
// 1 <= n <= PROFILE_IMAGE_COUNT. Used before any file system access so a
// stored or hand edited value can never escape profile_img/.
int profilesNameImageIsValid(const char *name);

// Interprets the launcher's sysCheckMC() result (0 = mc0:/, 1 = mc1:/,
// negative = no card). Physical memory card mode requires Slot 1, so a card
// in Slot 2 alone is rejected.
int profilesPhysicalMCSlotIsUsable(int sysCheckResult);

// Decides whether the launcher may activate a USB VMC descriptor for a slot.
// physicalMC is the "Save to Memory Card" mode, profileActive selects the
// per-profile VMC (Slot 1 only) and configuredName is the game's legacy VMC
// setting. Physical-MC mode always returns 0 for both slots so no USB VMC can
// replace the real card.
int profilesVMCDescriptorAllowed(int physicalMC, int profileActive, int slot, const char *configuredName);

#endif
