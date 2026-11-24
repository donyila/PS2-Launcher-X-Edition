#ifndef __PROFILES_UI_H
#define __PROFILES_UI_H

/*
 Native profile interface for the PS2 Launcher.

 The screens are drawn with the launcher's own renderer (renderman, gsKit and
 the theme fonts) and are driven by the normal PS2 pad handling. They are not
 themed launcher elements: they replace the whole screen with a black backdrop,
 exactly like the reference artwork in img/.

 This module only draws and returns what the user picked. All profile storage
 work stays in profiles.c.
*/

enum {
    PROFILES_UI_ACTION_NONE = 0,
    PROFILES_UI_ACTION_PROFILE,     // start with profileIndex
    PROFILES_UI_ACTION_ADD,         // open the Add User screen
    PROFILES_UI_ACTION_MEMORY_CARD, // use the physical memory card
    PROFILES_UI_ACTION_OPTIONS      // open rename/delete for profileIndex
};

typedef struct
{
    int action;
    int profileIndex;
} profiles_ui_result_t;

// Loads the shared fonts/textures needed by the profile screens. Safe to call
// more than once.
void profilesUiBegin(void);

// Releases every texture the profile screens allocated.
void profilesUiEnd(void);

// Runs the profile selection carousel. *selectedIndex is the highlighted item
// (0 = Add User, 1..count = profile, count+1 = Save to Memory Card) and is
// updated before returning, so the focus stays on the same card when the
// screen is re-entered after Add User / Options.
profiles_ui_result_t profilesUiSelect(int *selectedIndex);

// Add User screen. name/image are in/out: the caller can pass a remembered
// name and picture, and receives the confirmed values. Returns 1 on OK and 0
// when cancelled.
int profilesUiAddUser(char *name, int nameLen, char *image, int imageLen);

// The ten picture chooser. currentImage may be "". Returns 1 when a picture
// was chosen and writes its file name to out, 0 when cancelled.
int profilesUiPictureChooser(const char *currentImage, char *out, int outLen);

// Small modal lists. choice receives the selected entry (0 based) for
// profilesUiOptions and profilesUiConfirm. Both return 1 when confirmed.
int profilesUiOptions(const char *profileName, int *choice);
int profilesUiConfirm(const char *title, const char *message, const char *okLabel, const char *cancelLabel);

// Informational modal with a single OK button.
void profilesUiMessage(const char *title, const char *message);

#endif
