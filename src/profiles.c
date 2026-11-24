/*
 Multi-Profile System for PS2-Launcher V5

 Profile data and per-profile VMCs live on the same USB (BDM mass) device as
 the games, in:
   <device>:/PS2L_PROFILES/profiles.dat
   <device>:/PS2L_PROFILES/USERS/<ID>/profile.ini
   <device>:/PS2L_PROFILES/USERS/<ID>/VMC/<VMC NAME>.bin

 Game settings are intentionally left untouched: only the VMC location is
 redirected, into the active profile's VMC directory.
*/

#include "include/opl.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/dia.h"
#include "include/system.h"
#include "include/util.h"
#include "include/profiles.h"
#include "include/profiles_ui.h"
#include <delaythread.h>

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioDevctl("genvmc:")

#define PROFILE_INDEX_MAGIC "PS2LPROF1"
#define PROFILE_INDEX_MAX   4096
#define PROFILE_INI_MAX     2048
#define PROFILE_VMC_MAPS    16
#define PROFILE_VMC_WAIT_MS 60000
#define PROFILE_VMC_POLL_US 100000

typedef struct
{
    int id;
    char name[PROFILE_NAME_MAX + 1];
    char image[PROFILE_IMAGE_NAME_MAX + 1];
    int vmcMapCount;
    char vmcMapKey[PROFILE_VMC_MAPS][PROFILE_VMC_NAME_MAX];
    char vmcMapValue[PROFILE_VMC_MAPS][PROFILE_VMC_NAME_MAX];
} profile_entry_t;

typedef struct
{
    int VMC_status;
    int VMC_error;
    int VMC_progress;
    char VMC_msg[64];
} statusVMCparam_t;

static profile_entry_t profiles[PROFILE_MAX_COUNT];
static int profileCount;
static int nextProfileId;
static int activeProfileId;
static int physicalMCMode;
static int profilesReady;
static int profilesInitialized;

static char profilesRoot[16];                        // "mass0:/"
static char profilesBase[PROFILE_PATH_MAX];          // "mass0:/PS2L_PROFILES"
static char profilesUsers[PROFILE_PATH_MAX];         // ".../PS2L_PROFILES/USERS"

// ----------------------------------------------------------
// ---------------------- Small helpers ---------------------
// ----------------------------------------------------------

static char *profilesTrim(char *str)
{
    char *end;

    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n')
        str++;

    if (*str == '\0')
        return str;

    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }

    return str;
}

static int profilesReadFile(const char *path, char *buffer, int size)
{
    int fd, total = 0, result;

    if (size <= 0)
        return -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    while (total < size - 1) {
        result = read(fd, buffer + total, size - 1 - total);
        if (result <= 0)
            break;
        total += result;
    }

    close(fd);
    buffer[total] = '\0';
    return total;
}

// Writes through a temporary file so an interrupted write cannot destroy the
// previous copy of the index or of a profile.ini.
static int profilesWriteFile(const char *path, const char *data, int length)
{
    char tmpPath[PROFILE_PATH_MAX + 8];
    int fd, written;

    if ((int)strlen(path) + 5 >= (int)sizeof(tmpPath))
        return 0;

    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    unlink(tmpPath);

    fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        LOG("PROFILES Could not create %s\n", tmpPath);
        return 0;
    }

    written = write(fd, data, length);
    close(fd);

    if (written != length) {
        LOG("PROFILES Short write to %s (%d/%d)\n", tmpPath, written, length);
        unlink(tmpPath);
        return 0;
    }

    if (rename(tmpPath, path) != 0) {
        // Some FAT implementations refuse to rename over an existing file. The
        // complete new copy is already in tmpPath, so removing the old file and
        // retrying is safe (the profile directories remain the source of truth
        // for recovery if power is lost during this window).
        unlink(path);
        if (rename(tmpPath, path) != 0) {
            LOG("PROFILES Could not replace %s\n", path);
            unlink(tmpPath);
            return 0;
        }
    }

    return 1;
}

static int profilesDirExists(const char *path)
{
    DIR *dir = opendir(path);
    if (dir != NULL) {
        closedir(dir);
        return 1;
    }
    return 0;
}

static int profilesEnsureDir(const char *path)
{
    if (profilesDirExists(path))
        return 1;

    if (mkdir(path, 0777) == 0)
        return 1;

    // Another component may have created it in the meantime.
    return profilesDirExists(path);
}

// All profile paths are built by include/profiles_path.h. Keeping a single
// implementation is what prevents duplicated directory components (the old
// code appended "/USERS/" to a base that already ended in "/USERS").
static int profilesBuildProfileDir(int id, char *path, int length)
{
    return profilesPathUser(path, length, profilesRoot, id);
}

static int profilesBuildVMCDir(int id, char *path, int length)
{
    return profilesPathVmcDir(path, length, profilesRoot, id);
}

static int profilesEnsureProfileDirs(int id)
{
    char profileDir[PROFILE_PATH_MAX];
    char vmcDir[PROFILE_PATH_MAX];

    if (!profilesReady || id < 1 || id > PROFILE_ID_MAX)
        return 0;

    if (!profilesEnsureDir(profilesBase))
        return 0;
    if (!profilesEnsureDir(profilesUsers))
        return 0;
    if (!profilesBuildProfileDir(id, profileDir, sizeof(profileDir)))
        return 0;
    if (!profilesEnsureDir(profileDir))
        return 0;
    if (!profilesBuildVMCDir(id, vmcDir, sizeof(vmcDir)))
        return 0;
    if (!profilesEnsureDir(vmcDir))
        return 0;

    return 1;
}

int profilesFindIndex(int id)
{
    int i;
    for (i = 0; i < profileCount; i++) {
        if (profiles[i].id == id)
            return i;
    }
    return -1;
}

// ----------------------------------------------------------
// ------------------- Name and path safety -----------------
// ----------------------------------------------------------

int profilesIsValidName(const char *name)
{
    return profilesNameIsValid(name);
}

// VMC names are sanitized by the shared path module so the name used for the
// file is always the exact same one used while constructing its path.
static int profilesSanitizeVMCName(const char *input, char *output, int outputLen)
{
    return profilesNameSanitizeVmc(input, output, outputLen);
}

static const char *profilesFindVMCMap(profile_entry_t *profile, const char *gameId)
{
    int i;

    if (profile == NULL || gameId == NULL || gameId[0] == '\0')
        return NULL;

    for (i = 0; i < profile->vmcMapCount; i++) {
        if (strcasecmp(profile->vmcMapKey[i], gameId) == 0)
            return profile->vmcMapValue[i];
    }

    return NULL;
}

// ----------------------------------------------------------
// --------------------- profile.ini I/O --------------------
// ----------------------------------------------------------

static int profilesReadIni(profile_entry_t *profile, int useNameFallback)
{
    char path[PROFILE_PATH_MAX];
    char data[PROFILE_INI_MAX];
    char *line, *next;
    int section = 0;

    profile->vmcMapCount = 0;

    if (!profilesPathIni(path, sizeof(path), profilesRoot, profile->id))
        return 0;

    if (profilesReadFile(path, data, sizeof(data)) < 0)
        return 0;

    line = data;
    while (line != NULL && *line != '\0') {
        char *value, *trimmed;

        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';

        trimmed = profilesTrim(line);
        if (*trimmed == '[') {
            if (strncasecmp(trimmed, "[VMC]", 5) == 0)
                section = 2;
            else if (strncasecmp(trimmed, "[Profile]", 9) == 0)
                section = 1;
            else
                section = 0;
        } else if ((value = strchr(trimmed, '=')) != NULL) {
            *value++ = '\0';
            trimmed = profilesTrim(trimmed);
            value = profilesTrim(value);

            if (section == 2) {
                char key[PROFILE_VMC_NAME_MAX], val[PROFILE_VMC_NAME_MAX];
                if (profile->vmcMapCount < PROFILE_VMC_MAPS && profilesSanitizeVMCName(trimmed, key, sizeof(key)) && profilesSanitizeVMCName(value, val, sizeof(val))) {
                    strncpy(profile->vmcMapKey[profile->vmcMapCount], key, PROFILE_VMC_NAME_MAX - 1);
                    profile->vmcMapKey[profile->vmcMapCount][PROFILE_VMC_NAME_MAX - 1] = '\0';
                    strncpy(profile->vmcMapValue[profile->vmcMapCount], val, PROFILE_VMC_NAME_MAX - 1);
                    profile->vmcMapValue[profile->vmcMapCount][PROFILE_VMC_NAME_MAX - 1] = '\0';
                    profile->vmcMapCount++;
                }
            } else if (section == 1) {
                if (strcasecmp(trimmed, "name") == 0 && useNameFallback && profilesIsValidName(value)) {
                    strncpy(profile->name, value, PROFILE_NAME_MAX);
                    profile->name[PROFILE_NAME_MAX] = '\0';
                } else if (strcasecmp(trimmed, "image") == 0) {
                    // A stored picture that no longer matches the expected
                    // naming falls back to the default user.png instead of
                    // failing the whole profile.
                    if (profilesNameImageIsValid(value)) {
                        strncpy(profile->image, value, PROFILE_IMAGE_NAME_MAX);
                        profile->image[PROFILE_IMAGE_NAME_MAX] = '\0';
                    } else
                        profile->image[0] = '\0';
                }
            }
        }

        line = next;
    }

    return 1;
}

static int profilesWriteIni(profile_entry_t *profile)
{
    char path[PROFILE_PATH_MAX];
    char data[PROFILE_INI_MAX];
    int length = 0, i, result;

    if (!profilesPathIni(path, sizeof(path), profilesRoot, profile->id))
        return 0;

    result = snprintf(data + length, sizeof(data) - length,
        "[Profile]\r\nid=%d\r\nname=%s\r\nimage=%s\r\n\r\n[VMC]\r\n",
        profile->id, profile->name, profile->image);
    if (result <= 0 || result >= (int)sizeof(data) - length)
        return 0;
    length += result;

    for (i = 0; i < profile->vmcMapCount; i++) {
        result = snprintf(data + length, sizeof(data) - length, "%s=%s\r\n",
            profile->vmcMapKey[i], profile->vmcMapValue[i]);
        if (result <= 0 || result >= (int)sizeof(data) - length)
            break;
        length += result;
    }

    return profilesWriteFile(path, data, length);
}

// ----------------------------------------------------------
// -------------------- Index management --------------------
// ----------------------------------------------------------

static int profilesSaveIndex(void)
{
    char path[PROFILE_PATH_MAX];
    char data[PROFILE_INDEX_MAX];
    int length = 0, i, result;

    if (!profilesReady)
        return 0;
    if (!profilesEnsureDir(profilesBase))
        return 0;

    if (!profilesPathIndex(path, sizeof(path), profilesRoot))
        return 0;

    result = snprintf(data + length, sizeof(data) - length, "%s\r\nversion=1\r\nnext_id=%d\r\n",
        PROFILE_INDEX_MAGIC, nextProfileId);
    if (result <= 0 || result >= (int)sizeof(data) - length)
        return 0;
    length += result;

    for (i = 0; i < profileCount; i++) {
        result = snprintf(data + length, sizeof(data) - length, "profile=%d|%s\r\n",
            profiles[i].id, profiles[i].name);
        if (result <= 0 || result >= (int)sizeof(data) - length)
            return 0;
        length += result;
    }

    return profilesWriteFile(path, data, length);
}

static void profilesParseIndex(const char *data)
{
    char *line, *next;

    line = (char *)data;
    while (line != NULL && *line != '\0') {
        char *trimmed;

        next = strchr(line, '\n');
        if (next != NULL)
            *next++ = '\0';

        trimmed = profilesTrim(line);

        if (strncmp(trimmed, "next_id=", 8) == 0) {
            int id = atoi(trimmed + 8);
            if (id >= 1 && id <= PROFILE_ID_MAX)
                nextProfileId = id;
        } else if (strncmp(trimmed, "profile=", 8) == 0) {
            char *separator = strchr(trimmed + 8, '|');
            if (separator != NULL) {
                int id;
                *separator = '\0';
                id = atoi(trimmed + 8);

                if (id >= 1 && id <= PROFILE_ID_MAX && profilesIsValidName(separator + 1) && profileCount < PROFILE_MAX_COUNT && profilesFindIndex(id) < 0) {
                    profile_entry_t *profile = &profiles[profileCount];
                    memset(profile, 0, sizeof(profile_entry_t));
                    profile->id = id;
                    strncpy(profile->name, separator + 1, PROFILE_NAME_MAX);
                    profile->name[PROFILE_NAME_MAX] = '\0';
                    profilesReadIni(profile, 0);
                    profileCount++;
                }
            }
        }

        line = next;
    }
}

// Rebuild the index by scanning USERS/<ID>/profile.ini. Used when profiles.dat
// is missing or unusable, so an interrupted write cannot lose every profile.
static int profilesRecoverFromDisk(void)
{
    DIR *dir;
    struct dirent *entry;
    int i;

    profileCount = 0;
    nextProfileId = 1;

    if (!profilesEnsureDir(profilesUsers))
        return 0;

    dir = opendir(profilesUsers);
    if (dir == NULL)
        return 0;

    while ((entry = readdir(dir)) != NULL) {
        int id = 0, valid = 1;

        if (strlen(entry->d_name) != 6)
            continue;

        for (int j = 0; j < 6; j++) {
            if (entry->d_name[j] < '0' || entry->d_name[j] > '9')
                valid = 0;
        }
        if (!valid)
            continue;

        id = atoi(entry->d_name);
        if (id < 1 || id > PROFILE_ID_MAX || profilesFindIndex(id) >= 0 || profileCount >= PROFILE_MAX_COUNT)
            continue;

        {
            profile_entry_t *profile = &profiles[profileCount];
            memset(profile, 0, sizeof(profile_entry_t));
            profile->id = id;
            snprintf(profile->name, sizeof(profile->name), "Profile %d", id);
            profilesReadIni(profile, 1);
            if (!profilesIsValidName(profile->name))
                snprintf(profile->name, sizeof(profile->name), "Profile %d", id);
            profileCount++;
        }
    }

    closedir(dir);

    for (i = 0; i < profileCount; i++) {
        if (profiles[i].id >= nextProfileId)
            nextProfileId = profiles[i].id + 1;
    }

    if (profileCount > 0) {
        LOG("PROFILES Recovered %d profile(s) from USERS\n", profileCount);
        profilesSaveIndex();
    }

    return profileCount > 0;
}

// ----------------------------------------------------------
// -------------------- Device selection --------------------
// ----------------------------------------------------------

static int profilesProbeDevice(const char *root)
{
    DIR *dir = opendir(root);
    if (dir != NULL) {
        closedir(dir);
        return 1;
    }
    return 0;
}

static int profilesSelectDevice(void)
{
    char root[16];
    int attempt, i;

    profilesRoot[0] = '\0';

    // USB mass devices are mounted asynchronously by bdm; give them a short,
    // bounded window instead of failing the first time they are not ready.
    for (attempt = 0; attempt < 20; attempt++) {
        // Prefer the device that the launcher actually uses for games.
        if (oplGetGameDeviceRoot(profilesRoot, sizeof(profilesRoot))) {
            if (profilesProbeDevice(profilesRoot))
                return 1;
            LOG("PROFILES Game device %s is not readable\n", profilesRoot);
            profilesRoot[0] = '\0';
        }

        for (i = 0; i < MAX_BDM_DEVICES; i++) {
            snprintf(root, sizeof(root), "mass%d:/", i);
            if (profilesProbeDevice(root)) {
                snprintf(profilesRoot, sizeof(profilesRoot), "%s", root);
                return 1;
            }
        }

        profilesRoot[0] = '\0';
        DelayThread(100000);
    }

    return 0;
}

int profilesInit(void)
{
    char path[PROFILE_PATH_MAX];

    if (profilesInitialized)
        return profilesReady;

    profilesInitialized = 1;
    profilesReady = 0;
    profileCount = 0;
    nextProfileId = 1;
    activeProfileId = 0;

    if (!profilesSelectDevice()) {
        LOG("PROFILES No writable BDM mass device found; profiles disabled\n");
        return 0;
    }

    if (!profilesPathBase(profilesBase, sizeof(profilesBase), profilesRoot) ||
        !profilesPathUsers(profilesUsers, sizeof(profilesUsers), profilesRoot)) {
        LOG("PROFILES Could not build profile paths on %s\n", profilesRoot);
        profilesRoot[0] = '\0';
        return 0;
    }

    if (!profilesEnsureDir(profilesBase) || !profilesEnsureDir(profilesUsers)) {
        LOG("PROFILES Could not create %s\n", profilesBase);
        profilesRoot[0] = '\0';
        return 0;
    }

    profilesReady = 1;
    if (!profilesPathIndex(path, sizeof(path), profilesRoot)) {
        LOG("PROFILES Could not build the profile index path\n");
        profilesReady = 0;
        profilesRoot[0] = '\0';
        return 0;
    }

    {
        char data[PROFILE_INDEX_MAX];
        if (profilesReadFile(path, data, sizeof(data)) >= 0)
            profilesParseIndex(data);
    }

    if (profileCount == 0) {
        // Either a first run, or a missing/corrupt index. Recover from
        // profile.ini files when any exist, otherwise start empty.
        profilesRecoverFromDisk();
        if (profileCount == 0)
            profilesSaveIndex();
    } else {
        int i, maxId = 0;
        for (i = 0; i < profileCount; i++) {
            if (profiles[i].id > maxId)
                maxId = profiles[i].id;
        }
        if (nextProfileId <= maxId)
            nextProfileId = maxId + 1;
    }

    LOG("PROFILES Initialized on %s (%d profile(s))\n", profilesRoot, profileCount);
    return 1;
}

int profilesAvailable(void)
{
    return profilesReady;
}

const char *profilesGetRoot(void)
{
    return profilesRoot;
}

int profilesGetCount(void)
{
    return profileCount;
}

int profilesGetId(int index)
{
    if (index < 0 || index >= profileCount)
        return 0;
    return profiles[index].id;
}

const char *profilesGetName(int index)
{
    if (index < 0 || index >= profileCount)
        return "";
    return profiles[index].name;
}

const char *profilesGetImage(int index)
{
    if (index < 0 || index >= profileCount)
        return "";
    return profiles[index].image;
}

int profilesImagePath(const char *imageName, char *path, int pathLen)
{
    if (!profilesReady || imageName == NULL || path == NULL || pathLen < 2)
        return 0;

    return profilesPathProfileImage(path, pathLen, profilesRoot, imageName);
}

int profilesNameInUse(const char *name, int ignoreId)
{
    int i;

    if (name == NULL)
        return 0;

    for (i = 0; i < profileCount; i++) {
        if (ignoreId > 0 && profiles[i].id == ignoreId)
            continue;
        if (strcasecmp(profiles[i].name, name) == 0)
            return 1;
    }

    return 0;
}

int profilesGetActiveId(void)
{
    return activeProfileId;
}

const char *profilesGetActiveName(void)
{
    int index = profilesFindIndex(activeProfileId);
    if (index < 0)
        return "";
    return profiles[index].name;
}

int profilesDeviceMatches(const char *path)
{
    int prefixLength;

    if (!profilesReady || path == NULL)
        return 0;

    prefixLength = strlen(profilesRoot);
    if (prefixLength > 1 && profilesRoot[prefixLength - 1] == '/')
        prefixLength--; // compare "mass0:" only

    return strncmp(path, profilesRoot, prefixLength) == 0;
}

// ----------------------------------------------------------
// -------------------- Profile operations ------------------
// ----------------------------------------------------------

int profilesSetActive(int id)
{
    int index = profilesFindIndex(id);

    if (index < 0)
        return 0;

    if (!profilesEnsureProfileDirs(profiles[index].id)) {
        LOG("PROFILES Could not create directories for profile %d\n", id);
        return 0;
    }

    physicalMCMode = 0;
    activeProfileId = profiles[index].id;
    LOG("PROFILES Active profile: %s (%06d)\n", profiles[index].name, activeProfileId);
    return 1;
}

int profilesClearActive(void)
{
    activeProfileId = 0;
    physicalMCMode = 0;
    return 1;
}

int profilesSelectMemoryCard(void)
{
    // sysCheckMC() probes mc0: and then mc1:. A negative result means no card
    // is mounted, so the caller must show an error instead of silently
    // falling back to a USB VMC.
    if (sysCheckMC() < 0) {
        LOG("PROFILES Physical memory card mode requested, but no card is available\n");
        return 0;
    }

    activeProfileId = 0;
    physicalMCMode = 1;
    LOG("PROFILES Physical memory card mode selected\n");
    return 1;
}

int profilesIsPhysicalMC(void)
{
    return physicalMCMode;
}

static void profilesNormalizeName(const char *source, char *dest, int destLen)
{
    char temp[PROFILE_NAME_MAX + 1];
    int length;

    strncpy(temp, source, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    strncpy(dest, temp, destLen - 1);
    dest[destLen - 1] = '\0';

    length = strlen(dest);
    while (length > 0 && (dest[length - 1] == ' ' || dest[length - 1] == '\t')) {
        dest[--length] = '\0';
    }

    {
        int start = 0;
        while (dest[start] == ' ' || dest[start] == '\t')
            start++;
        if (start > 0)
            memmove(dest, dest + start, strlen(dest + start) + 1);
    }
}

static int profilesCreate(const char *name, const char *imageName, int *outId)
{
    char cleanName[PROFILE_NAME_MAX + 1];
    char cleanImage[PROFILE_IMAGE_NAME_MAX + 1];
    char profileDir[PROFILE_PATH_MAX];
    profile_entry_t *profile;
    int id, attempts = 0;

    if (!profilesReady || profileCount >= PROFILE_MAX_COUNT)
        return 0;

    profilesNormalizeName(name, cleanName, sizeof(cleanName));
    if (!profilesIsValidName(cleanName))
        return 0;
    if (profilesNameInUse(cleanName, 0))
        return 0;

    cleanImage[0] = '\0';
    if (imageName != NULL && imageName[0] != '\0') {
        if (!profilesNameImageIsValid(imageName))
            return 0;
        strncpy(cleanImage, imageName, PROFILE_IMAGE_NAME_MAX);
        cleanImage[PROFILE_IMAGE_NAME_MAX] = '\0';
    }

    id = nextProfileId;
    if (id < 1)
        id = 1;

    // Never hand out an ID whose directory already exists.
    for (attempts = 0; attempts < 1024; attempts++) {
        if (id > PROFILE_ID_MAX)
            id = 1;
        if (profilesFindIndex(id) < 0) {
            if (profilesBuildProfileDir(id, profileDir, sizeof(profileDir)) && !profilesDirExists(profileDir))
                break;
        }
        id++;
    }

    if (attempts >= 1024)
        return 0;

    if (!profilesEnsureProfileDirs(id))
        return 0;

    profile = &profiles[profileCount];
    memset(profile, 0, sizeof(profile_entry_t));
    profile->id = id;
    strncpy(profile->name, cleanName, PROFILE_NAME_MAX);
    profile->name[PROFILE_NAME_MAX] = '\0';
    strncpy(profile->image, cleanImage, PROFILE_IMAGE_NAME_MAX);
    profile->image[PROFILE_IMAGE_NAME_MAX] = '\0';

    if (!profilesWriteIni(profile)) {
        LOG("PROFILES Could not write profile.ini for %d\n", id);
        if (profilesBuildProfileDir(id, profileDir, sizeof(profileDir)))
            sysDeleteFolder(profileDir);
        return 0;
    }

    profileCount++;
    nextProfileId = id + 1;
    if (nextProfileId > PROFILE_ID_MAX)
        nextProfileId = 1;

    if (!profilesSaveIndex()) {
        LOG("PROFILES Could not write profiles.dat\n");
        profileCount--;
        if (profilesBuildProfileDir(id, profileDir, sizeof(profileDir)))
            sysDeleteFolder(profileDir);
        nextProfileId = id;
        return 0;
    }

    if (outId != NULL)
        *outId = id;

    LOG("PROFILES Created profile %s (%06d)\n", cleanName, id);
    return 1;
}

static int profilesRename(int id, const char *name)
{
    char cleanName[PROFILE_NAME_MAX + 1];
    char oldName[PROFILE_NAME_MAX + 1];
    int index = profilesFindIndex(id);

    if (index < 0)
        return 0;

    profilesNormalizeName(name, cleanName, sizeof(cleanName));
    if (!profilesIsValidName(cleanName))
        return 0;
    if (profilesNameInUse(cleanName, id))
        return 0;

    strncpy(oldName, profiles[index].name, sizeof(oldName));
    oldName[PROFILE_NAME_MAX] = '\0';

    strncpy(profiles[index].name, cleanName, PROFILE_NAME_MAX);
    profiles[index].name[PROFILE_NAME_MAX] = '\0';

    if (!profilesWriteIni(&profiles[index]) || !profilesSaveIndex()) {
        strncpy(profiles[index].name, oldName, PROFILE_NAME_MAX);
        profiles[index].name[PROFILE_NAME_MAX] = '\0';
        LOG("PROFILES Could not rename profile %d\n", id);
        return 0;
    }

    LOG("PROFILES Renamed profile %d to %s\n", id, profiles[index].name);
    return 1;
}

static int profilesDelete(int id)
{
    char profileDir[PROFILE_PATH_MAX];
    int index = profilesFindIndex(id);

    if (index < 0)
        return 0;

    if (!profilesBuildProfileDir(id, profileDir, sizeof(profileDir)))
        return 0;

    // Remove only this profile's own directory (profile.ini + VMC saves).
    if (profilesDirExists(profileDir)) {
        if (sysDeleteFolder(profileDir) < 0) {
            LOG("PROFILES Could not delete %s\n", profileDir);
            return 0;
        }
    }

    if (activeProfileId == id)
        profilesClearActive();

    memmove(&profiles[index], &profiles[index + 1], (profileCount - index - 1) * sizeof(profile_entry_t));
    profileCount--;

    if (!profilesSaveIndex()) {
        LOG("PROFILES Could not update profiles.dat after deleting %d\n", id);
        return 0;
    }

    LOG("PROFILES Deleted profile %d\n", id);
    return 1;
}

// ----------------------------------------------------------
// ------------------- Public operations --------------------
// ----------------------------------------------------------

int profilesCreateProfile(const char *name, const char *imageName, int *outId)
{
    return profilesCreate(name, imageName, outId);
}

int profilesRenameProfile(int id, const char *name)
{
    return profilesRename(id, name);
}

int profilesDeleteProfile(int id)
{
    return profilesDelete(id);
}

int profilesSetProfileImage(int id, const char *imageName)
{
    char oldImage[PROFILE_IMAGE_NAME_MAX + 1];
    int index = profilesFindIndex(id);

    if (index < 0)
        return 0;

    if (imageName != NULL && imageName[0] != '\0' && !profilesNameImageIsValid(imageName))
        return 0;

    strncpy(oldImage, profiles[index].image, sizeof(oldImage));
    oldImage[PROFILE_IMAGE_NAME_MAX] = '\0';

    if (imageName != NULL && imageName[0] != '\0') {
        strncpy(profiles[index].image, imageName, PROFILE_IMAGE_NAME_MAX);
        profiles[index].image[PROFILE_IMAGE_NAME_MAX] = '\0';
    } else {
        profiles[index].image[0] = '\0';
    }

    if (!profilesWriteIni(&profiles[index])) {
        strncpy(profiles[index].image, oldImage, PROFILE_IMAGE_NAME_MAX);
        profiles[index].image[PROFILE_IMAGE_NAME_MAX] = '\0';
        LOG("PROFILES Could not store the picture for profile %d\n", id);
        return 0;
    }

    return 1;
}

// ----------------------------------------------------------
// ------------------- VMC path resolution ------------------
// ----------------------------------------------------------

int profilesGetVMCFile(const char *gameId, const char *configuredVMC, int slot, char *path, int pathLen)
{
    char raw[PROFILE_VMC_NAME_MAX];
    char vmcName[PROFILE_VMC_NAME_MAX];
    const char *mapped;
    int index;
    profile_entry_t *profile;

    if (!profilesReady || activeProfileId <= 0)
        return 0;
    if (slot < 0 || slot >= PROFILE_VMC_SLOT_COUNT)
        return 0;
    if (path == NULL || pathLen < 2)
        return 0;

    index = profilesFindIndex(activeProfileId);
    if (index < 0)
        return 0;
    profile = &profiles[index];

    // Multi-disc support: discs mapped to the same VMC name share saves.
    mapped = profilesFindVMCMap(profile, gameId);
    if (mapped != NULL && mapped[0] != '\0') {
        snprintf(raw, sizeof(raw), "%s", mapped);
    } else if (configuredVMC != NULL && configuredVMC[0] != '\0') {
        snprintf(raw, sizeof(raw), "%s", configuredVMC);
    } else {
        char safeGame[PROFILE_VMC_NAME_MAX];
        if (!profilesSanitizeVMCName((gameId != NULL && gameId[0] != '\0') ? gameId : "GAME", safeGame, sizeof(safeGame)))
            snprintf(safeGame, sizeof(safeGame), "GAME");
        snprintf(raw, sizeof(raw), "%s_%d", safeGame, slot);
    }

    if (!profilesSanitizeVMCName(raw, vmcName, sizeof(vmcName))) {
        snprintf(vmcName, sizeof(vmcName), "GAME_%d", slot);
    }

    if (!profilesEnsureProfileDirs(activeProfileId)) {
        LOG("PROFILES Could not prepare VMC directory for profile %d\n", activeProfileId);
        return 0;
    }

    if (!profilesPathVmcFile(path, pathLen, profilesRoot, activeProfileId, vmcName)) {
        LOG("PROFILES Could not build a VMC path for profile %d\n", activeProfileId);
        return 0;
    }

    LOG("PROFILES VMC slot %d for %s: %s\n", slot, (gameId != NULL && gameId[0] != '\0') ? gameId : "(unknown)", path);
    return 1;
}

int profilesGetVMCFilePath(const char *vmcName, char *path, int pathLen)
{
    if (!profilesReady || activeProfileId <= 0 || vmcName == NULL || pathLen < 2)
        return 0;

    if (!profilesEnsureProfileDirs(activeProfileId))
        return 0;

    if (!profilesPathVmcFile(path, pathLen, profilesRoot, activeProfileId, vmcName)) {
        LOG("PROFILES Could not build a VMC path for profile %d\n", activeProfileId);
        return 0;
    }

    return 1;
}

// ----------------------------------------------------------
// --------------------- VMC creation -----------------------
// ----------------------------------------------------------

int profilesPrepareVMC(const char *path, vmc_superblock_t *vmc_superblock)
{
    int size, createResult, failure = 0, finished = 0, sawBusy = 0;
    int waited;

    if (path == NULL || path[0] == '\0')
        return -1;

    size = sysCheckVMCPath(path, 0, vmc_superblock);
    if (size > 0)
        return size;

    LOG("PROFILES Creating VMC %s (%d MB)\n", path, PROFILE_DEFAULT_VMC_SIZE);

    // An existing but invalid file (bad size or superblock) must not be left in
    // place for genvmc to overwrite partially.
    if (size == 0)
        unlink(path);

    createResult = sysCreateVMCFile(path, PROFILE_DEFAULT_VMC_SIZE);
    if (createResult < 0) {
        LOG("PROFILES genvmc create request failed: %d\n", createResult);
        return -1;
    }

    for (waited = 0; waited <= PROFILE_VMC_WAIT_MS; waited += PROFILE_VMC_POLL_US / 1000) {
        statusVMCparam_t status;

        DelayThread(PROFILE_VMC_POLL_US);

        // Poll the file itself every 500 ms; the VMC superblock must be valid
        // before the card is handed to the game.
        if ((waited % 500) == 0) {
            size = sysCheckVMCPath(path, 0, vmc_superblock);
            if (size > 0) {
                LOG("PROFILES VMC ready: %s (%d MB)\n", path, size);
                return size;
            }
        }

        // genvmc reports 0 for "available", i.e. the job is no longer running.
        // Only trust that once the job was seen running, otherwise a status
        // query racing the create request would end the wait early.
        if (fileXioDevctl("genvmc:", 0xC0DE0003, NULL, 0, (void *)&status, sizeof(status)) == 0) {
            if (status.VMC_status != 0) {
                sawBusy = 1;
            } else if (sawBusy) {
                if (status.VMC_error != 0) {
                    failure = status.VMC_error;
                    LOG("PROFILES genvmc error %d: %s\n", status.VMC_error, status.VMC_msg);
                }
                finished = 1;
                break;
            }
        }
    }

    if (failure != 0) {
        // genvmc reported a failure: never hand a half written card to a game.
        return -1;
    }

    if (!finished) {
        LOG("PROFILES Timeout while creating VMC %s\n", path);
        fileXioDevctl("genvmc:", 0xC0DE0002, NULL, 0, NULL, 0); // abort
        return -1;
    }

    // Give the I/O side a moment to flush the finished card, then verify it.
    for (waited = 0; waited < 2000; waited += PROFILE_VMC_POLL_US / 1000) {
        size = sysCheckVMCPath(path, 0, vmc_superblock);
        if (size > 0) {
            LOG("PROFILES VMC ready: %s (%d MB)\n", path, size);
            return size;
        }
        DelayThread(PROFILE_VMC_POLL_US);
    }

    // Never report success for a partially written card.
    LOG("PROFILES VMC %s was not created correctly\n", path);
    return -1;
}


// ----------------------------------------------------------
// ------------------- Selection screen ---------------------
// ----------------------------------------------------------

// The native carousel, Add User screen and picture chooser live in
// profiles_ui.c. This file owns the state transitions so every storage rule
// stays in one place.

static int profileSelectedIndex;
static int profileSelectionEntered;

static void profilesHandleCreate(void)
{
    char name[PROFILE_NAME_MAX + 1];
    char image[PROFILE_IMAGE_NAME_MAX + 1];
    int newId = 0;

    snprintf(name, sizeof(name), "User %d", profileCount + 1);
    image[0] = '\0';

    for (;;) {
        if (!profilesUiAddUser(name, sizeof(name), image, sizeof(image)))
            return;

        profilesNormalizeName(name, name, sizeof(name));
        // profilesUiAddUser checks both conditions and shows an inline error;
        // this guard makes sure an invalid value can never reach the index.
        if (!profilesIsValidName(name) || profilesNameInUse(name, 0)) {
            profilesUiMessage("Invalid Name", "Enter 1-23 characters without : / \\ | and use a name that is not taken yet.");
            continue;
        }

        if (!profilesCreate(name, image, &newId)) {
            profilesUiMessage("Could Not Create Profile", "The profile could not be saved.\nCheck the USB device and its free space.");
            continue;
        }

        // Keep the focus on the card that was just created.
        profileSelectedIndex = profilesFindIndex(newId) + 1;
        return;
    }
}

static void profilesHandleRename(int index)
{
    char name[PROFILE_NAME_MAX + 1];
    int id;

    if (index < 0 || index >= profileCount)
        return;

    id = profiles[index].id;
    snprintf(name, sizeof(name), "%s", profiles[index].name);

    if (!diaShowKeyb(name, sizeof(name), 0, "Rename Profile"))
        return;

    profilesNormalizeName(name, name, sizeof(name));

    if (!profilesIsValidName(name)) {
        profilesUiMessage("Invalid Name", "Enter 1-23 characters without : / \\ |.");
        return;
    }

    if (profilesNameInUse(name, id)) {
        profilesUiMessage("Name In Use", "Another profile already has that name.");
        return;
    }

    // profilesRename only writes the name. The numeric ID and the
    // USERS/<id>/VMC directory are untouched.
    if (!profilesRename(id, name))
        profilesUiMessage("Could Not Rename Profile", "The new name could not be saved.\nCheck the USB device.");
}

static void profilesHandlePicture(int index)
{
    char current[PROFILE_IMAGE_NAME_MAX + 1];
    char chosen[PROFILE_IMAGE_NAME_MAX + 1];
    int id;

    if (index < 0 || index >= profileCount)
        return;

    id = profiles[index].id;
    snprintf(current, sizeof(current), "%s", profiles[index].image);
    snprintf(chosen, sizeof(chosen), "%s", current);

    if (!profilesUiPictureChooser(current, chosen, sizeof(chosen)))
        return;

    if (!profilesSetProfileImage(id, chosen))
        profilesUiMessage("Could Not Save Picture", "The picture could not be stored in profile.ini.\nCheck the USB device.");
}

static void profilesHandleDelete(int index)
{
    char message[224];
    int id;

    if (index < 0 || index >= profileCount)
        return;

    id = profiles[index].id;
    snprintf(message, sizeof(message), "Delete profile \"%s\"?\n\nIts memory card saves will be deleted too.", profiles[index].name);

    if (!profilesUiConfirm("Delete Profile", message, "Delete", "Cancel"))
        return;

    if (!profilesDelete(id)) {
        profilesUiMessage("Could Not Delete Profile", "Some files may remain on the USB device.\nCheck the device and try again.");
        return;
    }

    if (profileSelectedIndex > profileCount)
        profileSelectedIndex = profileCount + 1;
    if (profileSelectedIndex < 0)
        profileSelectedIndex = 0;
}

int profilesShowSelection(void)
{
    if (!profilesInitialized)
        profilesInit();

    if (!profilesReady)
        return 0;

    profilesUiBegin();

    if (!profileSelectionEntered) {
        // With existing profiles the first profile is focused, like the
        // reference artwork. With no profiles, Add User is focused.
        profileSelectedIndex = (profileCount > 0) ? 1 : 0;
        profileSelectionEntered = 1;
    }

    if (profileSelectedIndex < 0)
        profileSelectedIndex = 0;
    if (profileSelectedIndex > profileCount + 1)
        profileSelectedIndex = profileCount + 1;

    for (;;) {
        profiles_ui_result_t result = profilesUiSelect(&profileSelectedIndex);

        switch (result.action) {
            case PROFILES_UI_ACTION_PROFILE:
                if (result.profileIndex < 0 || result.profileIndex >= profileCount)
                    break;

                if (!profilesSetActive(profiles[result.profileIndex].id)) {
                    profilesUiMessage("Could Not Start Profile", "The profile's folders could not be prepared.\nCheck the USB device.");
                    break;
                }

                LOG("PROFILES Starting with profile %s\n", profilesGetActiveName());
                profilesUiEnd();
                return 1;

            case PROFILES_UI_ACTION_ADD:
                profilesHandleCreate();
                break;

            case PROFILES_UI_ACTION_MEMORY_CARD:
                if (!profilesSelectMemoryCard()) {
                    profilesUiMessage("No Memory Card", "No usable PlayStation 2 Memory Card was found.\nInsert a card in slot 1 or 2 and try again.\n\nNo other save destination was selected.");
                    break;
                }

                LOG("PROFILES Starting with the physical memory card\n");
                profilesUiEnd();
                return 1;

            case PROFILES_UI_ACTION_OPTIONS: {
                int choice = 3;

                if (result.profileIndex < 0 || result.profileIndex >= profileCount)
                    break;

                if (!profilesUiOptions(profiles[result.profileIndex].name, &choice))
                    break;

                if (choice == 0)
                    profilesHandleRename(result.profileIndex);
                else if (choice == 1)
                    profilesHandlePicture(result.profileIndex);
                else if (choice == 2)
                    profilesHandleDelete(result.profileIndex);
                break;
            }

            default:
                break;
        }
    }
}
