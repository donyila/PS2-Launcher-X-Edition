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

static int profilesBuildProfileDir(int id, char *path, int length)
{
    int result = snprintf(path, length, "%s/%06d", profilesUsers, id);
    return (result > 0 && result < length);
}

static int profilesBuildVMCDir(int id, char *path, int length)
{
    int result = snprintf(path, length, "%s/%06d/VMC", profilesUsers, id);
    return (result > 0 && result < length);
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
    int i, length;

    if (name == NULL)
        return 0;

    length = strlen(name);
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

// Produces a FAT-safe, path-traversal-safe VMC name. Invalid characters are
// replaced, never used to build a directory.
static int profilesSanitizeVMCName(const char *input, char *output, int outputLen)
{
    int i, out = 0;

    if (input == NULL || output == NULL || outputLen < 2)
        return 0;

    for (i = 0; input[i] != '\0' && out < outputLen - 1; i++) {
        unsigned char c = (unsigned char)input[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            output[out++] = (char)c;
        } else if (c == ' ') {
            output[out++] = '_';
        } else {
            output[out++] = '_';
        }
    }

    output[out] = '\0';

    // Trim characters that FAT does not accept at the end of a name.
    while (out > 0 && (output[out - 1] == '.' || output[out - 1] == ' ' || output[out - 1] == '_'))
        output[--out] = '\0';

    if (out == 0 || strcmp(output, ".") == 0 || strcmp(output, "..") == 0)
        return 0;

    return 1;
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

    if (!profilesBuildProfileDir(profile->id, path, sizeof(path)))
        return 0;
    if ((int)strlen(path) + 12 >= (int)sizeof(path))
        return 0;
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/profile.ini");

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

    if (!profilesBuildProfileDir(profile->id, path, sizeof(path)))
        return 0;
    if ((int)strlen(path) + 12 >= (int)sizeof(path))
        return 0;
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/profile.ini");

    result = snprintf(data + length, sizeof(data) - length,
        "[Profile]\r\nid=%d\r\nname=%s\r\n\r\n[VMC]\r\n",
        profile->id, profile->name);
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
    char path[PROFILE_PATH_MAX + 16];
    char data[PROFILE_INDEX_MAX];
    int length = 0, i, result;

    if (!profilesReady)
        return 0;
    if (!profilesEnsureDir(profilesBase))
        return 0;

    snprintf(path, sizeof(path), "%s/profiles.dat", profilesBase);

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

    snprintf(profilesBase, sizeof(profilesBase), "%s%s", profilesRoot, PROFILE_BASE_DIR);
    snprintf(profilesUsers, sizeof(profilesUsers), "%s/USERS", profilesBase);

    if (!profilesEnsureDir(profilesBase) || !profilesEnsureDir(profilesUsers)) {
        LOG("PROFILES Could not create %s\n", profilesBase);
        profilesRoot[0] = '\0';
        return 0;
    }

    profilesReady = 1;
    snprintf(path, sizeof(path), "%s/profiles.dat", profilesBase);

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

    activeProfileId = profiles[index].id;
    LOG("PROFILES Active profile: %s (%06d)\n", profiles[index].name, activeProfileId);
    return 1;
}

int profilesClearActive(void)
{
    activeProfileId = 0;
    return 1;
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

static int profilesCreate(const char *name, int *outId)
{
    char cleanName[PROFILE_NAME_MAX + 1];
    char profileDir[PROFILE_PATH_MAX];
    profile_entry_t *profile;
    int id, attempts = 0;

    if (!profilesReady || profileCount >= PROFILE_MAX_COUNT)
        return 0;

    profilesNormalizeName(name, cleanName, sizeof(cleanName));
    if (!profilesIsValidName(cleanName))
        return 0;

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

    if (snprintf(path, pathLen, "%s/USERS/%06d/VMC/%s.bin", profilesUsers, activeProfileId, vmcName) >= pathLen)
        return 0;

    return 1;
}

int profilesGetVMCFilePath(const char *vmcName, char *path, int pathLen)
{
    char safeName[PROFILE_VMC_NAME_MAX];

    if (!profilesReady || activeProfileId <= 0 || vmcName == NULL || pathLen < 2)
        return 0;

    if (!profilesSanitizeVMCName(vmcName, safeName, sizeof(safeName)))
        return 0;

    if (!profilesEnsureProfileDirs(activeProfileId))
        return 0;

    if (snprintf(path, pathLen, "%s/USERS/%06d/VMC/%s.bin", profilesUsers, activeProfileId, safeName) >= pathLen)
        return 0;

    return 1;
}

// ----------------------------------------------------------
// --------------------- VMC creation -----------------------
// ----------------------------------------------------------

int profilesPrepareVMC(const char *path, vmc_superblock_t *vmc_superblock)
{
    int size, createResult, failure = 0;
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
    if (createResult < 0)
        LOG("PROFILES genvmc create request failed: %d\n", createResult);

    for (waited = 0; waited <= PROFILE_VMC_WAIT_MS; waited += PROFILE_VMC_POLL_US / 1000) {
        statusVMCparam_t status;

        DelayThread(PROFILE_VMC_POLL_US);

        // Poll the file itself every 500 ms; the VMC superblock must be valid.
        if ((waited % 500) == 0) {
            size = sysCheckVMCPath(path, 0, vmc_superblock);
            if (size > 0) {
                LOG("PROFILES VMC ready: %s (%d MB)\n", path, size);
                return size;
            }
        }

        if (fileXioDevctl("genvmc:", 0xC0DE0003, NULL, 0, (void *)&status, sizeof(status)) == 0) {
            if (status.VMC_status == 0 && status.VMC_error < 0) {
                failure = status.VMC_error;
                LOG("PROFILES genvmc error %d: %s\n", status.VMC_error, status.VMC_msg);
                break;
            }
        }
    }

    if (failure == 0) {
        LOG("PROFILES Timeout while creating VMC %s\n", path);
        fileXioDevctl("genvmc:", 0xC0DE0002, NULL, 0, NULL, 0); // abort
    }

    // Never report success for a partially written card.
    size = sysCheckVMCPath(path, 0, vmc_superblock);
    if (size > 0)
        return size;

    return -1;
}

// ----------------------------------------------------------
// ------------------- Selection screen ---------------------
// ----------------------------------------------------------

enum {
    PSL_LBL_TITLE = 1,
    PSL_LBL_PROFILE,
    PSL_LBL_INFO,
    PSL_ENUM_PROFILE,
    PSL_BTN_USE,
    PSL_BTN_NEW,
    PSL_BTN_RENAME,
    PSL_BTN_DELETE,
    PSL_BTN_CONTINUE
};

static const char *profileEnumValues[PROFILE_MAX_COUNT + 1];
static char profileInfoText[160];
static int profileSelectedIndex;

static struct UIItem diaProfileSelect[] = {
    {UI_LABEL, PSL_LBL_TITLE, 1, 1, -1, 0, 0, {.label = {"PS2-Launcher Profiles", -1}}},
    {UI_SPLITTER, 0, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_LABEL, PSL_LBL_PROFILE, 1, 1, -1, 0, 0, {.label = {"Profile:", -1}}},
    {UI_ENUM, PSL_ENUM_PROFILE, 1, 1, -1, 0, 0, {.intvalue = {0, 0, 0, PROFILE_MAX_COUNT - 1}}},
    {UI_LABEL, PSL_LBL_INFO, 1, 1, -1, 0, 0, {.label = {"", -1}}},
    {UI_SPLITTER, 0, 1, 1, -1, 0, 0, {.label = {NULL, -1}}},
    {UI_BUTTON, PSL_BTN_USE, 1, 1, -1, 0, 0, {.label = {"Use Profile", -1}}},
    {UI_BUTTON, PSL_BTN_NEW, 1, 1, -1, 0, 0, {.label = {"New Profile", -1}}},
    {UI_BUTTON, PSL_BTN_RENAME, 1, 1, -1, 0, 0, {.label = {"Rename Profile", -1}}},
    {UI_BUTTON, PSL_BTN_DELETE, 1, 1, -1, 0, 0, {.label = {"Delete Profile", -1}}},
    {UI_BUTTON, PSL_BTN_CONTINUE, 1, 1, -1, 0, 0, {.label = {"Continue", -1}}},
    {UI_TERMINATOR, 0, 1, 1, -1, 0, 0, {.label = {NULL, -1}}}};

static void profilesRefreshScreen(void)
{
    int i;
    const char *activeName = profilesGetActiveName();

    for (i = 0; i < profileCount; i++)
        profileEnumValues[i] = profiles[i].name;

    if (profileCount == 0) {
        profileEnumValues[0] = "<no profiles yet>";
        profileEnumValues[1] = NULL;
        profileSelectedIndex = 0;
    } else {
        profileEnumValues[profileCount] = NULL;
        if (profileSelectedIndex >= profileCount)
            profileSelectedIndex = profileCount - 1;
        if (profileSelectedIndex < 0)
            profileSelectedIndex = 0;
    }

    diaSetEnum(diaProfileSelect, PSL_ENUM_PROFILE, profileEnumValues);
    diaSetInt(diaProfileSelect, PSL_ENUM_PROFILE, profileSelectedIndex);

    diaSetEnabled(diaProfileSelect, PSL_ENUM_PROFILE, profileCount > 0);
    diaSetEnabled(diaProfileSelect, PSL_BTN_USE, profileCount > 0);
    diaSetEnabled(diaProfileSelect, PSL_BTN_RENAME, profileCount > 0);
    diaSetEnabled(diaProfileSelect, PSL_BTN_DELETE, profileCount > 0);
    diaSetEnabled(diaProfileSelect, PSL_ENUM_PROFILE, profileCount > 0);

    if (profileCount == 0)
        snprintf(profileInfoText, sizeof(profileInfoText), "No profiles yet. Create one to continue.");
    else if (activeName[0] != '\0')
        snprintf(profileInfoText, sizeof(profileInfoText), "%d profile(s). Active: %s", profileCount, activeName);
    else
        snprintf(profileInfoText, sizeof(profileInfoText), "%d profile(s) on %s", profileCount, profilesRoot);

    diaSetLabel(diaProfileSelect, PSL_LBL_INFO, profileInfoText);
}

static int profilesPromptCreate(void)
{
    char name[PROFILE_NAME_MAX + 1];
    int newId = 0;

    snprintf(name, sizeof(name), "Player %d", profileCount + 1);

    if (!diaShowKeyb(name, sizeof(name), 0, "New Profile Name"))
        return 0;

    profilesNormalizeName(name, name, sizeof(name));
    if (!profilesIsValidName(name)) {
        guiMsgBox("Invalid profile name.\nUse 1-23 characters without : / \\ |", 0, NULL);
        return 0;
    }

    if (!profilesCreate(name, &newId)) {
        guiMsgBox("Could not create the profile.\nCheck the USB device and free space.", 0, NULL);
        return 0;
    }

    profileSelectedIndex = profilesFindIndex(newId);
    guiMsgBox("Profile created.", 0, NULL);
    return newId;
}

static void profilesPromptRename(void)
{
    char name[PROFILE_NAME_MAX + 1];
    int id, index;

    if (profileCount == 0)
        return;

    if (profileSelectedIndex >= profileCount)
        profileSelectedIndex = profileCount - 1;
    index = profileSelectedIndex;
    id = profiles[index].id;

    snprintf(name, sizeof(name), "%s", profiles[index].name);

    if (!diaShowKeyb(name, sizeof(name), 0, "Rename Profile"))
        return;

    profilesNormalizeName(name, name, sizeof(name));
    if (!profilesIsValidName(name)) {
        guiMsgBox("Invalid profile name.\nUse 1-23 characters without : / \\ |", 0, NULL);
        return;
    }

    if (!profilesRename(id, name))
        guiMsgBox("Could not rename the profile.", 0, NULL);
}

static void profilesPromptDelete(void)
{
    char message[160];
    int id;

    if (profileCount == 0)
        return;

    if (profileSelectedIndex >= profileCount)
        profileSelectedIndex = profileCount - 1;
    id = profiles[profileSelectedIndex].id;

    snprintf(message, sizeof(message), "Delete profile \"%s\"?\n\nAll of its VMC save data will be deleted.", profiles[profileSelectedIndex].name);
    if (!guiMsgBox(message, 1, NULL))
        return;

    if (!profilesDelete(id)) {
        guiMsgBox("Could not delete the profile.\nNo data was removed.", 0, NULL);
        return;
    }

    if (profileSelectedIndex >= profileCount && profileSelectedIndex > 0)
        profileSelectedIndex--;
}

int profilesShowSelection(void)
{
    int result = -1;

    if (!profilesInitialized)
        profilesInit();

    if (!profilesReady)
        return 0;

    for (;;) {
        profilesRefreshScreen();

        result = diaExecuteDialog(diaProfileSelect, result, 0, NULL);

        // Read the value the user left in the enum (up/down changes it while the
        // control has focus).
        {
            int selected = profileSelectedIndex;
            if (diaGetInt(diaProfileSelect, PSL_ENUM_PROFILE, &selected) && selected >= 0 && selected < profileCount)
                profileSelectedIndex = selected;
        }

        if (result == PSL_BTN_USE || result == PSL_BTN_CONTINUE) {
            if (profileCount == 0) {
                if (profilesPromptCreate() > 0) {
                    profilesSetActive(profiles[profileSelectedIndex].id);
                    return 1;
                }
                continue;
            }

            if (profileSelectedIndex < 0 || profileSelectedIndex >= profileCount)
                profileSelectedIndex = 0;

            if (!profilesSetActive(profiles[profileSelectedIndex].id)) {
                guiMsgBox("Could not activate the profile.\nCheck the USB device.", 0, NULL);
                continue;
            }
            return 1;
        } else if (result == PSL_BTN_NEW) {
            profilesPromptCreate();
        } else if (result == PSL_BTN_RENAME) {
            profilesPromptRename();
        } else if (result == PSL_BTN_DELETE) {
            profilesPromptDelete();
        } else {
            // Cancel: a profile is mandatory at startup, so keep the user here
            // until one is active (or created).
            if (activeProfileId > 0)
                return 1;
            if (profileCount == 0)
                guiMsgBox("A profile is required.\nPress New Profile to create one.", 0, NULL);
            else
                guiMsgBox("Select a profile to continue.", 0, NULL);
        }
    }
}
