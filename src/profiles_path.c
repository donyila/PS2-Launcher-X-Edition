/*
 Path construction for the PS2-Launcher multi-profile system.

 Keep this file free of PS2SDK dependencies: it is compiled for the launcher and
 also built for the host by tests/profiles_path_test.c.
*/

#include "include/profiles_path.h"

#include <stdio.h>
#include <string.h>

// Copy the device root, guaranteeing exactly one trailing '/'.
static int profilesPathRoot(const char *deviceRoot, char *out, int length)
{
    int rootLength;

    if (deviceRoot == NULL || out == NULL || length < 4)
        return 0;

    rootLength = (int)strlen(deviceRoot);
    if (rootLength < 3) // shortest sane root is "x:/"
        return 0;

    if (rootLength + 2 > length)
        return 0;

    memcpy(out, deviceRoot, rootLength);
    if (out[rootLength - 1] != '/') {
        out[rootLength] = '/';
        rootLength++;
    }
    out[rootLength] = '\0';

    return 1;
}

// Join a suffix (without a leading '/') onto the normalized device root.
static int profilesPathJoin(char *out, int length, const char *deviceRoot, const char *suffix)
{
    char root[PROFILE_PATH_MAX];
    int written;

    if (out == NULL || length < 2 || suffix == NULL)
        return 0;
    if (!profilesPathRoot(deviceRoot, root, sizeof(root)))
        return 0;

    written = snprintf(out, length, "%s%s", root, suffix);
    return (written > 0 && written < length);
}

static int profilesPathUserId(char *suffix, int suffixLength, int id)
{
    int written;

    if (id < 1 || id > PROFILE_ID_MAX)
        return 0;

    written = snprintf(suffix, suffixLength, PROFILE_BASE_DIR "/USERS/%06d", id);
    return (written > 0 && written < suffixLength);
}

int profilesPathBase(char *out, int length, const char *deviceRoot)
{
    return profilesPathJoin(out, length, deviceRoot, PROFILE_BASE_DIR);
}

int profilesPathUsers(char *out, int length, const char *deviceRoot)
{
    return profilesPathJoin(out, length, deviceRoot, PROFILE_BASE_DIR "/USERS");
}

int profilesPathUser(char *out, int length, const char *deviceRoot, int id)
{
    char suffix[PROFILE_PATH_MAX];

    if (!profilesPathUserId(suffix, sizeof(suffix), id))
        return 0;

    return profilesPathJoin(out, length, deviceRoot, suffix);
}

int profilesPathVmcDir(char *out, int length, const char *deviceRoot, int id)
{
    char suffix[PROFILE_PATH_MAX];
    int written;

    if (id < 1 || id > PROFILE_ID_MAX)
        return 0;

    written = snprintf(suffix, sizeof(suffix), PROFILE_BASE_DIR "/USERS/%06d/VMC", id);
    if (written <= 0 || written >= (int)sizeof(suffix))
        return 0;

    return profilesPathJoin(out, length, deviceRoot, suffix);
}

int profilesPathVmcFile(char *out, int length, const char *deviceRoot, int id, const char *vmcName)
{
    char dir[PROFILE_PATH_MAX];
    char safeName[PROFILE_VMC_NAME_MAX];
    int written;

    if (out == NULL || length < 2 || vmcName == NULL)
        return 0;
    if (!profilesNameSanitizeVmc(vmcName, safeName, sizeof(safeName)))
        return 0;
    if (!profilesPathVmcDir(dir, sizeof(dir), deviceRoot, id))
        return 0;

    written = snprintf(out, length, "%s/%s.bin", dir, safeName);
    return (written > 0 && written < length);
}

int profilesPathIni(char *out, int length, const char *deviceRoot, int id)
{
    char suffix[PROFILE_PATH_MAX];
    int written;

    if (id < 1 || id > PROFILE_ID_MAX)
        return 0;

    written = snprintf(suffix, sizeof(suffix), PROFILE_BASE_DIR "/USERS/%06d/profile.ini", id);
    if (written <= 0 || written >= (int)sizeof(suffix))
        return 0;

    return profilesPathJoin(out, length, deviceRoot, suffix);
}

int profilesPathIndex(char *out, int length, const char *deviceRoot)
{
    return profilesPathJoin(out, length, deviceRoot, PROFILE_BASE_DIR "/profiles.dat");
}

int profilesPathProfileImage(char *out, int length, const char *deviceRoot, const char *imageName)
{
    char suffix[PROFILE_PATH_MAX];
    int written;

    if (out == NULL || length < 2 || imageName == NULL)
        return 0;
    if (!profilesNameImageIsValid(imageName))
        return 0;

    written = snprintf(suffix, sizeof(suffix), PROFILE_IMAGE_DIR "/%s", imageName);
    if (written <= 0 || written >= (int)sizeof(suffix))
        return 0;

    return profilesPathJoin(out, length, deviceRoot, suffix);
}

int profilesNameSanitizeVmc(const char *input, char *output, int outputLength)
{
    int i, out = 0;

    if (input == NULL || output == NULL || outputLength < 2)
        return 0;

    for (i = 0; input[i] != '\0' && out < outputLength - 1; i++) {
        unsigned char c = (unsigned char)input[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            output[out++] = (char)c;
        } else {
            // Spaces and every other character (including '/' and '\\') are
            // folded to '_' so a name can never escape the VMC directory.
            output[out++] = '_';
        }
    }

    output[out] = '\0';

    // Trim characters that FAT does not accept at the end of a name.
    while (out > 0 && (output[out - 1] == '.' || output[out - 1] == ' ' || output[out - 1] == '_'))
        output[--out] = '\0';

    // Never produce hidden/dotted names.
    while (out > 0 && output[0] == '.') {
        memmove(output, output + 1, out); // moves the remaining characters + terminator
        out--;
    }

    if (out == 0 || strcmp(output, ".") == 0 || strcmp(output, "..") == 0)
        return 0;

    return 1;
}

int profilesNameImageIsValid(const char *name)
{
    const char *cursor;
    int number = 0, digits = 0;

    if (name == NULL)
        return 0;

    if (strncmp(name, "profile_", 8) != 0)
        return 0;

    cursor = name + 8;
    while (*cursor >= '0' && *cursor <= '9' && digits < 3) {
        number = number * 10 + (*cursor - '0');
        digits++;
        cursor++;
    }

    // Reject "profile_01.png" and "profile_001.png" style names so there is
    // only one spelling of every picture.
    if (digits == 0 || digits > 2)
        return 0;
    if (name[8] == '0')
        return 0;
    if (strcmp(cursor, ".png") != 0)
        return 0;

    return (number >= 1 && number <= PROFILE_IMAGE_COUNT);
}

int profilesNameIsValid(const char *name)
{
    int i, length;

    if (name == NULL)
        return 0;

    length = (int)strlen(name);
    if (length < 1 || length > PROFILE_NAME_MAX)
        return 0;

    if (name[0] == ' ' || name[length - 1] == ' ')
        return 0;

    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];

        if (c < 0x20 || c > 0x7E)
            return 0;
        if (c == '|' || c == '/' || c == '\\' || c == ':')
            return 0;
    }

    return 1;
}

int profilesPhysicalMCSlotIsUsable(int sysCheckResult)
{
    // sysCheckMC() reports 0 for mc0:/ (Slot 1), 1 for mc1:/ (Slot 2) and a
    // negative value when neither card can be opened. "Save to Memory Card"
    // always writes to Slot 1, so a Slot 2 card is not a substitute.
    return sysCheckResult == 0;
}

int profilesVMCDescriptorAllowed(int physicalMC, int profileActive, int slot, const char *configuredName)
{
    if (slot < 0 || slot >= PROFILE_VMC_SLOT_COUNT)
        return 0;

    // Physical memory card mode must leave both USB VMC descriptors inactive
    // so the game talks to the real card in Slot 1.
    if (physicalMC)
        return 0;

    // The profile system uses Slot 1 only; Slot 2 is reserved and inactive.
    if (profileActive)
        return slot == 0;

    return configuredName != NULL && configuredName[0] != '\0';
}
