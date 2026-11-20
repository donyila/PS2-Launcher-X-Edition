/*
 Host-side tests for the profile path construction.

 Build and run with tests/run_tests.sh (a normal host C compiler is enough; the
 PS2SDK is not required).
*/

#include "include/profiles_path.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (!(condition)) {                                                     \
            failures++;                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);         \
        }                                                                       \
    } while (0)

// Every test function that uses this macro declares "char path[PROFILE_PATH_MAX];".
#define CHECK_PATH(expected, call)                                              \
    do {                                                                        \
        memset(path, 0x7f, sizeof(path));                                       \
        CHECK(call);                                                            \
        CHECK(strcmp(path, expected) == 0);                                     \
        if (strcmp(path, expected) != 0)                                        \
            printf("     expected \"%s\"\n     actual   \"%s\"\n", expected, path); \
    } while (0)

static int countOccurrences(const char *haystack, const char *needle)
{
    int count = 0;
    const char *p = haystack;

    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += strlen(needle);
    }

    return count;
}

static void testKnownPaths(void)
{
    char path[PROFILE_PATH_MAX];

    CHECK_PATH("mass0:/PS2L_PROFILES", profilesPathBase(path, sizeof(path), "mass0:/"));
    CHECK_PATH("mass0:/PS2L_PROFILES/USERS", profilesPathUsers(path, sizeof(path), "mass0:/"));
    CHECK_PATH("mass0:/PS2L_PROFILES/USERS/000001", profilesPathUser(path, sizeof(path), "mass0:/", 1));
    CHECK_PATH("mass0:/PS2L_PROFILES/USERS/000001/VMC", profilesPathVmcDir(path, sizeof(path), "mass0:/", 1));
    CHECK_PATH("mass0:/PS2L_PROFILES/USERS/000001/profile.ini", profilesPathIni(path, sizeof(path), "mass0:/", 1));
    CHECK_PATH("mass0:/PS2L_PROFILES/profiles.dat", profilesPathIndex(path, sizeof(path), "mass0:/"));

    // This is the exact path that was reported on hardware, with a duplicated
    // "USERS" component. It must now contain exactly one "USERS".
    CHECK_PATH("mass0:/PS2L_PROFILES/USERS/000001/VMC/SLUS_206.58_0.bin",
        profilesPathVmcFile(path, sizeof(path), "mass0:/", 1, "SLUS_206.58_0"));
    CHECK(countOccurrences(path, "USERS") == 1);
    CHECK(strstr(path, "/USERS/USERS/") == NULL);
}

static void testRootNormalization(void)
{
    char path[PROFILE_PATH_MAX];

    // A root without a trailing slash must produce identical paths.
    CHECK_PATH("mass1:/PS2L_PROFILES", profilesPathBase(path, sizeof(path), "mass1:"));
    CHECK_PATH("mass1:/PS2L_PROFILES/USERS/000042/VMC/GAME_0.bin",
        profilesPathVmcFile(path, sizeof(path), "mass1:", 42, "GAME_0"));

    CHECK(profilesPathBase(path, sizeof(path), "") == 0);
    CHECK(profilesPathBase(path, sizeof(path), NULL) == 0);
    CHECK(profilesPathVmcFile(path, sizeof(path), "mass0:/", 1, NULL) == 0);
}

static void testIds(void)
{
    char path[PROFILE_PATH_MAX];

    CHECK(profilesPathUser(path, sizeof(path), "mass0:/", 0) == 0);
    CHECK(profilesPathUser(path, sizeof(path), "mass0:/", -1) == 0);
    CHECK(profilesPathUser(path, sizeof(path), "mass0:/", PROFILE_ID_MAX + 1) == 0);
    CHECK_PATH("mass0:/PS2L_PROFILES/USERS/999999", profilesPathUser(path, sizeof(path), "mass0:/", PROFILE_ID_MAX));
}

static void testShortBuffers(void)
{
    char small[16];

    // The full path does not fit: this must be reported as a failure instead of
    // handing a truncated (and therefore wrong) path to the VMC code.
    CHECK(profilesPathBase(small, sizeof(small), "mass0:/") == 0);
    CHECK(profilesPathVmcFile(small, sizeof(small), "mass0:/", 1, "GAME_0") == 0);
}

static void testVmcNames(void)
{
    char name[PROFILE_VMC_NAME_MAX];
    char path[PROFILE_PATH_MAX];

    CHECK(profilesNameSanitizeVmc("SLUS_206.58_0", name, sizeof(name)) == 1);
    CHECK(strcmp(name, "SLUS_206.58_0") == 0);

    CHECK(profilesNameSanitizeVmc("generic 0", name, sizeof(name)) == 1);
    CHECK(strcmp(name, "generic_0") == 0);

    // Path separators can never survive sanitizing, so no VMC name can escape
    // its profile folder.
    CHECK(profilesNameSanitizeVmc("../../etc/passwd", name, sizeof(name)) == 1);
    CHECK(strchr(name, '/') == NULL);
    CHECK(strchr(name, '\\') == NULL);
    CHECK(name[0] != '.');

    CHECK(profilesNameSanitizeVmc("", name, sizeof(name)) == 0);
    CHECK(profilesNameSanitizeVmc("...", name, sizeof(name)) == 0);
    CHECK(profilesNameSanitizeVmc(NULL, name, sizeof(name)) == 0);

    // Over-long names are truncated, still usable, and never escape the folder.
    CHECK(profilesNameSanitizeVmc("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", name, sizeof(name)) == 1);
    CHECK(strlen(name) < PROFILE_VMC_NAME_MAX);
    CHECK_PATH("mass0:/PS2L_PROFILES/USERS/000007/VMC/ABCDEFGHIJKLMNOPQRSTUVWXYZ01234.bin",
        profilesPathVmcFile(path, sizeof(path), "mass0:/", 7, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"));
}

static void testProfileNames(void)
{
    CHECK(profilesNameIsValid("Alice") == 1);
    CHECK(profilesNameIsValid("Player 1 (main)") == 1);
    CHECK(profilesNameIsValid("12345678901234567890123") == 1);   // 23 chars
    CHECK(profilesNameIsValid("123456789012345678901234") == 0);  // 24 chars
    CHECK(profilesNameIsValid("") == 0);
    CHECK(profilesNameIsValid(NULL) == 0);
    CHECK(profilesNameIsValid(" leading") == 0);
    CHECK(profilesNameIsValid("trailing ") == 0);
    CHECK(profilesNameIsValid("a/b") == 0);
    CHECK(profilesNameIsValid("a\\b") == 0);
    CHECK(profilesNameIsValid("a:b") == 0);
    CHECK(profilesNameIsValid("a|b") == 0);
    CHECK(profilesNameIsValid("tab\there") == 0);
}

int main(void)
{
    testKnownPaths();
    testRootNormalization();
    testIds();
    testShortBuffers();
    testVmcNames();
    testProfileNames();

    if (failures != 0) {
        printf("%d of %d profile path checks failed\n", failures, checks);
        return 1;
    }

    printf("All %d profile path checks passed\n", checks);
    return 0;
}
