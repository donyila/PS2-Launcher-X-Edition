/*
 Host-side tests for the profile path construction.

 Build and run with tests/run_tests.sh (a normal host C compiler is enough; the
 PS2SDK is not required).
*/

#include "include/profiles_grid.h"
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

static void testProfileImages(void)
{
    char path[PROFILE_PATH_MAX];
    char name[PROFILE_IMAGE_NAME_MAX];
    char expected[PROFILE_PATH_MAX];
    int i;

    // The final format is .png. The old .img names must not be accepted.
    CHECK(profilesNameImageIsValid("profile_1.img") == 0);
    CHECK(profilesNameImageIsValid("profile_10.img") == 0);

    CHECK(profilesNameImageIsValid("profile_1.png") == 1);
    CHECK(profilesNameImageIsValid("profile_9.png") == 1);
    CHECK(profilesNameImageIsValid("profile_10.png") == 1);
    CHECK(profilesNameImageIsValid("profile_11.png") == 0);
    CHECK(profilesNameImageIsValid("profile_0.png") == 0);
    CHECK(profilesNameImageIsValid("profile_01.png") == 0);
    CHECK(profilesNameImageIsValid("profile_001.png") == 0);
    CHECK(profilesNameImageIsValid("profile_100.png") == 0);
    CHECK(profilesNameImageIsValid("profile_1.PNG") == 0);
    CHECK(profilesNameImageIsValid("profile_.png") == 0);
    CHECK(profilesNameImageIsValid("profile_1.png.bak") == 0);
    CHECK(profilesNameImageIsValid("sub/profile_1.png") == 0);
    CHECK(profilesNameImageIsValid("../profile_1.png") == 0);
    CHECK(profilesNameImageIsValid("profile_1.png/../../x") == 0);
    CHECK(profilesNameImageIsValid("") == 0);
    CHECK(profilesNameImageIsValid(NULL) == 0);

    // Every picture name the UI offers must be accepted, resolve to
    // <root>/profile_img/profile_N.png and stay inside profile_img/.
    for (i = 1; i <= PROFILE_IMAGE_COUNT; i++) {
        snprintf(name, sizeof(name), "profile_%d.png", i);
        CHECK(profilesNameImageIsValid(name) == 1);

        snprintf(expected, sizeof(expected), "mass0:/profile_img/profile_%d.png", i);
        CHECK_PATH(expected, profilesPathProfileImage(path, sizeof(path), "mass0:/", name));
        CHECK(strstr(path, "profile_png") == NULL);
    }

    CHECK_PATH("mass0:/profile_img/profile_1.png",
        profilesPathProfileImage(path, sizeof(path), "mass0:/", "profile_1.png"));
    CHECK_PATH("mass1:/profile_img/profile_10.png",
        profilesPathProfileImage(path, sizeof(path), "mass1:", "profile_10.png"));

    CHECK(profilesPathProfileImage(path, sizeof(path), "mass0:/", "../../etc/passwd") == 0);
    CHECK(profilesPathProfileImage(path, sizeof(path), "mass0:/", NULL) == 0);
}

static void testPhysicalMemoryCard(void)
{
    // sysCheckMC(): 0 = mc0:/ (Slot 1), 1 = mc1:/ (Slot 2), negative = none.
    CHECK(profilesPhysicalMCSlotIsUsable(0) == 1);
    CHECK(profilesPhysicalMCSlotIsUsable(1) == 0); // Slot 2 alone is not enough
    CHECK(profilesPhysicalMCSlotIsUsable(-11) == 0);
    CHECK(profilesPhysicalMCSlotIsUsable(-1) == 0);

    // Physical-MC mode must leave both USB VMC descriptors inactive, even if a
    // profile or a legacy configured VMC would otherwise apply.
    CHECK(profilesVMCDescriptorAllowed(1, 0, 0, "SLUS_206.58") == 0);
    CHECK(profilesVMCDescriptorAllowed(1, 0, 1, "SLUS_206.58") == 0);
    CHECK(profilesVMCDescriptorAllowed(1, 1, 0, "SLUS_206.58") == 0);
    CHECK(profilesVMCDescriptorAllowed(1, 1, 1, "SLUS_206.58") == 0);
    CHECK(profilesVMCDescriptorAllowed(1, 0, 0, NULL) == 0);

    // A profile uses Slot 1 only; Slot 2 stays reserved and inactive.
    CHECK(profilesVMCDescriptorAllowed(0, 1, 0, "SLUS_206.58") == 1);
    CHECK(profilesVMCDescriptorAllowed(0, 1, 1, "SLUS_206.58") == 0);

    // The legacy configured VMC still works without a profile or physical card.
    CHECK(profilesVMCDescriptorAllowed(0, 0, 0, "SLUS_206.58") == 1);
    CHECK(profilesVMCDescriptorAllowed(0, 0, 1, "SLUS_206.58") == 1);
    CHECK(profilesVMCDescriptorAllowed(0, 0, 0, "") == 0);
    CHECK(profilesVMCDescriptorAllowed(0, 0, 0, NULL) == 0);
    CHECK(profilesVMCDescriptorAllowed(0, 0, 2, "SLUS_206.58") == 0);
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

// The profile directory and every VMC path are built from the numeric ID only.
// Renaming a profile therefore cannot move its folder or invalidate an existing
// VMC; this is the invariant Edit User relies on.
static void testProfileIdentityIsStable(void)
{
    char dirA[PROFILE_PATH_MAX];
    char dirB[PROFILE_PATH_MAX];
    char vmcA[PROFILE_PATH_MAX];
    char vmcB[PROFILE_PATH_MAX];

    CHECK(profilesPathUser(dirA, sizeof(dirA), "mass0:/", 7) == 1);
    CHECK(profilesPathUser(dirB, sizeof(dirB), "mass0:/", 7) == 1);
    CHECK(strcmp(dirA, dirB) == 0);
    CHECK(strstr(dirA, "000007") != NULL);

    CHECK(profilesPathVmcFile(vmcA, sizeof(vmcA), "mass0:/", 7, "SLUS_200.62_0") == 1);
    CHECK(profilesPathVmcFile(vmcB, sizeof(vmcB), "mass0:/", 7, "SLUS_200.62_0") == 1);
    CHECK(strcmp(vmcA, vmcB) == 0);
    CHECK(strstr(vmcA, "000007/VMC/") != NULL);

    // Different IDs can never share a directory or a VMC file.
    CHECK(profilesPathUser(dirB, sizeof(dirB), "mass0:/", 8) == 1);
    CHECK(strcmp(dirA, dirB) != 0);
}

static void testPictureGridLayout(void)
{
    int row, col, i;
    char name[PROFILE_IMAGE_NAME_MAX + 1];

    // Ten pictures in two rows of five, the default tile centered below them.
    profilesGridSlotPos(0, &row, &col);
    CHECK(row == 0 && col == 0);
    profilesGridSlotPos(4, &row, &col);
    CHECK(row == 0 && col == 4);
    profilesGridSlotPos(5, &row, &col);
    CHECK(row == 1 && col == 0);
    profilesGridSlotPos(9, &row, &col);
    CHECK(row == 1 && col == 4);
    profilesGridSlotPos(PROFILE_GRID_DEFAULT_SLOT, &row, &col);
    CHECK(row == 2 && col == 2);

    CHECK(profilesGridSlotAt(0, 0) == 0);
    CHECK(profilesGridSlotAt(1, 3) == 8);
    CHECK(profilesGridSlotAt(2, 2) == PROFILE_GRID_DEFAULT_SLOT);
    CHECK(profilesGridSlotAt(2, 0) == -1);
    CHECK(profilesGridSlotAt(1, 4) == 9);

    for (i = 0; i < PROFILE_IMAGE_COUNT; i++) {
        CHECK(profilesGridSlotImage(i, name, sizeof(name)) == 1);
        CHECK(strcmp(name, "") != 0);
    }

    CHECK(profilesGridSlotImage(0, name, sizeof(name)) == 1 && strcmp(name, "profile_1.png") == 0);
    CHECK(profilesGridSlotImage(9, name, sizeof(name)) == 1 && strcmp(name, "profile_10.png") == 0);
    CHECK(profilesGridSlotImage(PROFILE_GRID_DEFAULT_SLOT, name, sizeof(name)) == 1 && name[0] == '\0');
    CHECK(profilesGridSlotImage(PROFILE_GRID_TILE_COUNT, name, sizeof(name)) == 0);

    CHECK(profilesGridSlotFromImage("") == PROFILE_GRID_DEFAULT_SLOT);
    CHECK(profilesGridSlotFromImage(NULL) == PROFILE_GRID_DEFAULT_SLOT);
    CHECK(profilesGridSlotFromImage("profile_3.png") == 2);
    CHECK(profilesGridSlotFromImage("PROFILE_3.PNG") == 2);
    CHECK(profilesGridSlotFromImage("profile_11.png") == -1);
    CHECK(profilesGridSlotFromImage("user.png") == -1);
}

// The chooser must never move the focus onto an unavailable picture and must
// never paint itself into a corner: the default tile is always selectable.
static void testPictureGridNavigation(void)
{
    unsigned char available[PROFILE_GRID_TILE_COUNT];
    int slot, direction;

    // Only profile_2.png exists, but the default tile always exists too.
    memset(available, 0, sizeof(available));
    available[1] = 1;
    available[PROFILE_GRID_DEFAULT_SLOT] = 1;

    CHECK(profilesGridFirstAvailable(available) == 1);
    CHECK(profilesGridMove(0, 0, 1, available) == 1);
    CHECK(profilesGridMove(2, 0, -1, available) == 1);
    // Nothing to the right/above from a missing tile: fall back to a selectable
    // tile instead of parking the cursor on an unavailable picture.
    CHECK(profilesGridMove(4, 0, 1, available) == 1);
    CHECK(profilesGridMove(0, -1, 0, available) == 1);
    CHECK(profilesGridMove(3, 0, -1, available) == 1);
    CHECK(profilesGridMove(7, 1, 0, available) == PROFILE_GRID_DEFAULT_SLOT);
    CHECK(profilesGridMove(PROFILE_GRID_DEFAULT_SLOT, -1, 0, available) == PROFILE_GRID_DEFAULT_SLOT);

    // Only the default tile: every direction from every slot stays selectable.
    memset(available, 0, sizeof(available));
    available[PROFILE_GRID_DEFAULT_SLOT] = 1;
    CHECK(profilesGridFirstAvailable(available) == PROFILE_GRID_DEFAULT_SLOT);

    for (slot = 0; slot < PROFILE_GRID_TILE_COUNT; slot++) {
        static const int steps[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

        for (direction = 0; direction < 4; direction++) {
            int next = profilesGridMove(slot, steps[direction][0], steps[direction][1], available);
            CHECK(available[next] == 1);
        }
    }

    // A full stick: navigation visits every picture and never an empty tile.
    memset(available, 0xFF, sizeof(available));
    CHECK(profilesGridMove(0, 0, 1, available) == 1);
    CHECK(profilesGridMove(4, 0, 1, available) == 4);
    CHECK(profilesGridMove(4, 1, 0, available) == 9);
    CHECK(profilesGridMove(9, -1, 0, available) == 4);
    CHECK(profilesGridMove(7, 1, 0, available) == PROFILE_GRID_DEFAULT_SLOT);
    CHECK(profilesGridMove(PROFILE_GRID_DEFAULT_SLOT, 1, 0, available) == PROFILE_GRID_DEFAULT_SLOT);

    CHECK(profilesGridFirstAvailable(NULL) == -1);
    CHECK(profilesGridMove(3, 0, 1, NULL) == 3);
}

int main(void)
{
    testKnownPaths();
    testRootNormalization();
    testIds();
    testShortBuffers();
    testVmcNames();
    testProfileImages();
    testPhysicalMemoryCard();
    testProfileNames();
    testProfileIdentityIsStable();
    testPictureGridLayout();
    testPictureGridNavigation();

    if (failures != 0) {
        printf("%d of %d profile path checks failed\n", failures, checks);
        return 1;
    }

    printf("All %d profile path checks passed\n", checks);
    return 0;
}
