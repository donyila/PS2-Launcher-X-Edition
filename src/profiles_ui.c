/*
 Native multi-profile interface for the PS2 Launcher.

 Everything here is drawn with the launcher's own renderer (renderman + gsKit)
 and the embedded Roboto fonts, so it runs on real PS2 hardware. The layout is
 authored in a 1280x720 reference space (the resolution of the supplied
 artwork) and scaled to the launcher's 640x480 virtual screen.

 The focus animation follows the technique used by the PS5 game carousel in
 themes.c: one continuous animation position that is eased towards the selected
 item, and a smoothstep focus value per item derived from its distance to that
 position. Sizes and outline colours are interpolated from that value, so
 nothing jumps and the layout never shifts.
*/

#include "include/opl.h"
#include "include/gui.h"
#include "include/ioman.h"
#include "include/profiles.h"
#include "include/profiles_ui.h"
#include "include/themes.h"
#include "include/fntsys.h"
#include "include/renderman.h"
#include "include/pad.h"
#include "include/sound.h"
#include "include/dia.h"

#include <math.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>

extern void *user_png;
extern void *memory_card_png;
extern void *ps2_logo_png;

// ----------------------------------------------------------
// ------------------ Reference geometry --------------------
// ----------------------------------------------------------

#define UI_DW 1280
#define UI_DH 720

#define UI_FOCUS_DIAMETER   330
#define UI_NORMAL_DIAMETER  220
#define UI_TILE_PITCH       322
#define UI_TILE_Y           385
#define UI_TITLE_Y          68
#define UI_SUBTITLE_Y       120
#define UI_LABEL_GAP        10
#define UI_LOGO_Y           680
#define UI_LOGO_W           176
#define UI_LOGO_H           43

#define UI_AVATAR_SIZE      192
#define UI_THUMB_SIZE       128
#define UI_AVATAR_SLOTS     6

#define UI_WHITE  0xFF
#define UI_BLUE_R 0x00
#define UI_BLUE_G 0xB0
#define UI_BLUE_B 0xF0
#define UI_GREY   0x7F

typedef struct
{
    int w;
    int h;
    unsigned char *rgba;
} ui_image_t;

typedef struct
{
    char key[PROFILE_IMAGE_NAME_MAX + 1];
    GSTEXTURE tex;
    int lastUse;
} ui_avatar_t;

static int uiScreenW;
static int uiScreenH;
static float uiScale;

static int uiFontTitle;
static int uiFontSub;
static int uiFontLabel;
static int uiFontSmall;
static int uiFontTiny;
static int uiFontButton;

static GSTEXTURE uiDiscTex;
static GSTEXTURE uiRingTex;
static GSTEXTURE uiPlusTex;
static GSTEXTURE uiUserTex;
static GSTEXTURE uiMemoryCardTex;
static GSTEXTURE uiPs2LogoTex;
static int uiAssetsReady;

static ui_avatar_t uiAvatars[UI_AVATAR_SLOTS];
static int uiAvatarTick;

static GSTEXTURE uiThumbs[PROFILE_IMAGE_COUNT];
static int uiThumbState[PROFILE_IMAGE_COUNT];

// ----------------------------------------------------------
// ------------------------ Helpers -------------------------
// ----------------------------------------------------------

static int uiRound(float value)
{
    return (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static int uiX(int designX)
{
    return uiScreenW / 2 + uiRound(((float)designX - (UI_DW / 2)) * uiScale);
}

static int uiY(int designY)
{
    return uiRound((float)designY * uiScale);
}

static int uiD(int designSize)
{
    return uiRound((float)designSize * uiScale);
}

static u64 uiColor(int r, int g, int b, int a)
{
    return GS_SETREG_RGBA(r, g, b, a);
}

static u64 uiFocusColor(float focus, int alpha)
{
    int r, g, b;

    if (focus < 0.0f)
        focus = 0.0f;
    if (focus > 1.0f)
        focus = 1.0f;

    r = UI_WHITE + uiRound((UI_BLUE_R - UI_WHITE) * focus);
    g = UI_WHITE + uiRound((UI_BLUE_G - UI_WHITE) * focus);
    b = UI_WHITE + uiRound((UI_BLUE_B - UI_WHITE) * focus);

    return uiColor(r, g, b, alpha);
}

static float uiSmoothStep(float t)
{
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Frame based easing, matching the game carousel's interpolation. The pad loop
// runs at the launcher's frame rate (vsync locked), which keeps this stable.
static float uiEaseTowards(float current, float target)
{
    current += (target - current) * 0.30f;
    if (fabsf(target - current) < 0.0005f)
        current = target;
    return current;
}

static void uiFreeTexture(GSTEXTURE *tex)
{
    if (tex == NULL)
        return;

    if (tex->Mem) {
        if (tex->Vram)
            rmUnloadTexture(tex);
        texFree(tex);
    }

    memset(tex, 0, sizeof(GSTEXTURE));
}

static int uiAllocTexture(GSTEXTURE *tex, int width, int height)
{
    memset(tex, 0, sizeof(GSTEXTURE));
    tex->Width = width;
    tex->Height = height;
    tex->PSM = GS_PSM_CT32;
    tex->Filter = GS_FILTER_LINEAR;
    tex->Delayed = 1;
    tex->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
    tex->Mem = memalign(128, gsKit_texture_size_ee(width, height, GS_PSM_CT32));

    if (tex->Mem == NULL) {
        LOG("PROFILES_UI Could not allocate a %dx%d texture\n", width, height);
        return 0;
    }

    memset(tex->Mem, 0, gsKit_texture_size_ee(width, height, GS_PSM_CT32));
    return 1;
}

// ----------------------------------------------------------
// -------------------- Texture builders --------------------
// ----------------------------------------------------------

// Grey disc used behind every circular item. The alpha edge is antialiased and
// premultiplied the same way the launcher's PNG loader premultiplies colours.
static void uiBuildDiscTexture(GSTEXTURE *tex, int size)
{
    struct pixel
    {
        unsigned char r, g, b, a;
    } *pixels;
    float center = (size - 1) * 0.5f;
    float radius = (size * 0.5f) - 1.0f;
    int x, y;

    if (!uiAllocTexture(tex, size, size))
        return;

    pixels = (struct pixel *)tex->Mem;

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float dx = (float)x - center;
            float dy = (float)y - center;
            float distance = sqrtf(dx * dx + dy * dy);
            float coverage = radius - distance;
            int index = y * size + x;

            if (coverage < 0.0f)
                coverage = 0.0f;
            else if (coverage > 1.0f)
                coverage = 1.0f;

            pixels[index].r = (unsigned char)(UI_GREY * coverage);
            pixels[index].g = (unsigned char)(UI_GREY * coverage);
            pixels[index].b = (unsigned char)(UI_GREY * coverage);
            pixels[index].a = (unsigned char)(coverage * 0x80);
        }
    }
}

// White ring, tinted at draw time with the white -> #00B0F0 focus colour.
static void uiBuildRingTexture(GSTEXTURE *tex, int size)
{
    struct pixel
    {
        unsigned char r, g, b, a;
    } *pixels;
    float center = (size - 1) * 0.5f;
    float radius = (size * 0.5f) - 2.0f;
    float halfThickness = 1.8f;
    int x, y;

    if (!uiAllocTexture(tex, size, size))
        return;

    pixels = (struct pixel *)tex->Mem;

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float dx = (float)x - center;
            float dy = (float)y - center;
            float distance = sqrtf(dx * dx + dy * dy);
            float coverage = (halfThickness + 0.5f) - fabsf(distance - radius);
            int index = y * size + x;

            if (coverage < 0.0f)
                coverage = 0.0f;
            else if (coverage > 1.0f)
                coverage = 1.0f;

            pixels[index].r = (unsigned char)(255.0f * coverage);
            pixels[index].g = (unsigned char)(255.0f * coverage);
            pixels[index].b = (unsigned char)(255.0f * coverage);
            pixels[index].a = (unsigned char)(coverage * 0x80);
        }
    }
}

// White plus used by the Add User card.
static void uiBuildPlusTexture(GSTEXTURE *tex, int size)
{
    struct pixel
    {
        unsigned char r, g, b, a;
    } *pixels;
    int barW = (size * 56) / 100;
    int barH = (size * 9) / 100;
    int x0 = (size - barW) / 2;
    int y0 = (size - barH) / 2;
    int y, x;

    if (!uiAllocTexture(tex, size, size))
        return;

    pixels = (struct pixel *)tex->Mem;

    for (y = y0; y < y0 + barH; y++) {
        for (x = x0; x < x0 + barW; x++) {
            int index = y * size + x;
            pixels[index].r = 255;
            pixels[index].g = 255;
            pixels[index].b = 255;
            pixels[index].a = 0x80;
        }
    }

    x0 = (size - barH) / 2;
    y0 = (size - barW) / 2;

    for (y = y0; y < y0 + barW; y++) {
        for (x = x0; x < x0 + barH; x++) {
            int index = y * size + x;
            pixels[index].r = 255;
            pixels[index].g = 255;
            pixels[index].b = 255;
            pixels[index].a = 0x80;
        }
    }
}

// ----------------------------------------------------------
// ---------------------- PNG loading -----------------------
// ----------------------------------------------------------

typedef struct
{
    const unsigned char *data;
    int size;
    int offset;
} ui_png_src_t;

static void uiPngRead(png_structp pngPtr, png_bytep out, png_size_t length)
{
    ui_png_src_t *src = (ui_png_src_t *)png_get_io_ptr(pngPtr);

    if (src->offset + (int)length > src->size)
        png_error(pngPtr, "truncated profile image");

    memcpy(out, src->data + src->offset, length);
    src->offset += (int)length;
}

static void uiImageFree(ui_image_t *img)
{
    if (img->rgba != NULL)
        free(img->rgba);
    img->rgba = NULL;
    img->w = 0;
    img->h = 0;
}

// Decodes a PNG buffer to 8-bit RGBA. Profile pictures are ordinary PNG files
// loaded from <root>/profile_img/profile_N.png. The signature is checked
// before parsing; anything else is rejected cleanly instead of being guessed at.
static int uiDecodePng(const unsigned char *data, int size, ui_image_t *img)
{
    png_structp pngPtr = NULL;
    png_infop infoPtr = NULL;
    ui_png_src_t src;
    png_bytep *rows = NULL;
    png_uint_32 width, height;
    int bitDepth, colorType;
    int ok = 0;
    int y;

    img->rgba = NULL;
    img->w = 0;
    img->h = 0;

    if (data == NULL || size < 8)
        return 0;

    if (png_sig_cmp((png_bytep)data, 0, 8) != 0)
        return 0;

    pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (pngPtr == NULL)
        return 0;

    infoPtr = png_create_info_struct(pngPtr);
    if (infoPtr == NULL) {
        png_destroy_read_struct(&pngPtr, NULL, NULL);
        return 0;
    }

    if (setjmp(png_jmpbuf(pngPtr))) {
        png_destroy_read_struct(&pngPtr, &infoPtr, NULL);
        free(rows);
        uiImageFree(img);
        return 0;
    }

    src.data = data;
    src.size = size;
    src.offset = 0;

    png_set_read_fn(pngPtr, &src, uiPngRead);
    png_read_info(pngPtr, infoPtr);
    png_get_IHDR(pngPtr, infoPtr, &width, &height, &bitDepth, &colorType, NULL, NULL, NULL);

    // Bound the work: profile pictures are icons, not wallpapers.
    if (width < 1 || height < 1 || width > 1600 || height > 1600)
        png_error(pngPtr, "unsupported profile image size");

    if (bitDepth == 16)
        png_set_strip_16(pngPtr);
    png_set_expand(pngPtr);
    png_set_gray_to_rgb(pngPtr);
    png_set_tRNS_to_alpha(pngPtr);
    png_read_update_info(pngPtr, infoPtr);

    if (png_get_channels(pngPtr, infoPtr) == 3) {
        png_set_filler(pngPtr, 0xFF, PNG_FILLER_AFTER);
        png_read_update_info(pngPtr, infoPtr);
    }

    if (png_get_channels(pngPtr, infoPtr) != 4)
        png_error(pngPtr, "unsupported profile image format");

    img->rgba = (unsigned char *)malloc((size_t)width * height * 4);
    if (img->rgba == NULL)
        png_error(pngPtr, "out of memory");

    rows = (png_bytep *)malloc(sizeof(png_bytep) * height);
    if (rows == NULL)
        png_error(pngPtr, "out of memory");

    for (y = 0; y < (int)height; y++)
        rows[y] = img->rgba + (size_t)y * width * 4;

    png_read_image(pngPtr, rows);
    png_read_end(pngPtr, NULL);

    img->w = (int)width;
    img->h = (int)height;
    ok = 1;

    free(rows);
    png_destroy_read_struct(&pngPtr, &infoPtr, NULL);
    return ok;
}

static unsigned char *uiReadFile(const char *path, int *outSize)
{
    int fd, size;
    unsigned char *buffer;
    int total = 0;

    *outSize = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    size = lseek(fd, 0, SEEK_END);
    if (size <= 0 || size > 4 * 1024 * 1024) {
        close(fd);
        return NULL;
    }

    lseek(fd, 0, SEEK_SET);
    buffer = (unsigned char *)malloc(size);
    if (buffer == NULL) {
        close(fd);
        return NULL;
    }

    while (total < size) {
        int result = read(fd, buffer + total, size - total);
        if (result <= 0)
            break;
        total += result;
    }

    close(fd);

    if (total != size) {
        free(buffer);
        return NULL;
    }

    *outSize = size;
    return buffer;
}

// ----------------------------------------------------------
// ------------------ Avatar composition --------------------
// ----------------------------------------------------------

static void uiSampleImage(const ui_image_t *img, float fx, float fy, int *r, int *g, int *b, int *a)
{
    int x0, y0, x1, y1;
    float tx, ty;
    int c00, c10, c01, c11;

    if (img == NULL || img->rgba == NULL || img->w <= 0 || img->h <= 0) {
        *r = *g = *b = UI_GREY;
        *a = 255;
        return;
    }

    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fx > (float)(img->w - 1)) fx = (float)(img->w - 1);
    if (fy > (float)(img->h - 1)) fy = (float)(img->h - 1);

    x0 = (int)fx;
    y0 = (int)fy;
    x1 = (x0 + 1 < img->w) ? x0 + 1 : x0;
    y1 = (y0 + 1 < img->h) ? y0 + 1 : y0;
    tx = fx - x0;
    ty = fy - y0;

    c00 = (y0 * img->w + x0) * 4;
    c10 = (y0 * img->w + x1) * 4;
    c01 = (y1 * img->w + x0) * 4;
    c11 = (y1 * img->w + x1) * 4;

    *r = (int)((1.0f - tx) * (1.0f - ty) * img->rgba[c00] + tx * (1.0f - ty) * img->rgba[c10] + (1.0f - tx) * ty * img->rgba[c01] + tx * ty * img->rgba[c11]);
    *g = (int)((1.0f - tx) * (1.0f - ty) * img->rgba[c00 + 1] + tx * (1.0f - ty) * img->rgba[c10 + 1] + (1.0f - tx) * ty * img->rgba[c01 + 1] + tx * ty * img->rgba[c11 + 1]);
    *b = (int)((1.0f - tx) * (1.0f - ty) * img->rgba[c00 + 2] + tx * (1.0f - ty) * img->rgba[c10 + 2] + (1.0f - tx) * ty * img->rgba[c01 + 2] + tx * ty * img->rgba[c11 + 2]);
    *a = (int)((1.0f - tx) * (1.0f - ty) * img->rgba[c00 + 3] + tx * (1.0f - ty) * img->rgba[c10 + 3] + (1.0f - tx) * ty * img->rgba[c01 + 3] + tx * ty * img->rgba[c11 + 3]);
}

// Composites a picture into a circular, grey backed avatar. Pixels outside the
// circle stay transparent so the card's own disc remains visible.
static int uiBuildAvatarTexture(GSTEXTURE *tex, const ui_image_t *img, int size)
{
    struct pixel
    {
        unsigned char r, g, b, a;
    } *pixels;
    float center = (size - 1) * 0.5f;
    float radius = (size * 0.5f) - 1.0f;
    float side = 0.0f;
    int x, y;

    if (!uiAllocTexture(tex, size, size))
        return 0;

    pixels = (struct pixel *)tex->Mem;

    if (img != NULL && img->w > 0 && img->h > 0)
        side = (float)((img->w < img->h) ? img->w : img->h);

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            float dx = (float)x - center;
            float dy = (float)y - center;
            float distance = sqrtf(dx * dx + dy * dy);
            float coverage = radius - distance;
            int index = y * size + x;
            int r = UI_GREY, g = UI_GREY, b = UI_GREY;

            if (coverage < 0.0f)
                coverage = 0.0f;
            else if (coverage > 1.0f)
                coverage = 1.0f;

            if (img != NULL && img->rgba != NULL && side > 0.0f) {
                // Cover crop: the centre square of the source fills the whole
                // circle, so a rectangular picture is never squeezed.
                float u = ((float)x + 0.5f) / (float)size;
                float v = ((float)y + 0.5f) / (float)size;
                float fx = (float)(img->w - 1) * 0.5f + (u - 0.5f) * side;
                float fy = (float)(img->h - 1) * 0.5f + (v - 0.5f) * side;
                int sr, sg, sb, sa;

                uiSampleImage(img, fx, fy, &sr, &sg, &sb, &sa);
                r = (sr * sa + UI_GREY * (255 - sa)) / 255;
                g = (sg * sa + UI_GREY * (255 - sa)) / 255;
                b = (sb * sa + UI_GREY * (255 - sa)) / 255;
            }

            pixels[index].r = (unsigned char)((float)r * coverage);
            pixels[index].g = (unsigned char)((float)g * coverage);
            pixels[index].b = (unsigned char)((float)b * coverage);
            pixels[index].a = (unsigned char)(coverage * 0x80);
        }
    }

    return 1;
}

static void uiFreeAvatars(void)
{
    int i;

    for (i = 0; i < UI_AVATAR_SLOTS; i++) {
        uiFreeTexture(&uiAvatars[i].tex);
        uiAvatars[i].key[0] = '\0';
        uiAvatars[i].lastUse = 0;
    }
}

// Returns a circular avatar for a stored picture name, or NULL when the file is
// missing/invalid (the caller then draws the default user.png). The result is
// cached, including failures, so a missing USB file is not re-read every frame.
static GSTEXTURE *uiAvatarFor(const char *imageName)
{
    char path[PROFILE_PATH_MAX];
    ui_image_t image;
    unsigned char *data;
    int size = 0;
    int i, slot = -1, oldest = 0;

    if (imageName == NULL || imageName[0] == '\0')
        return NULL;
    if (!profilesNameImageIsValid(imageName))
        return NULL;

    for (i = 0; i < UI_AVATAR_SLOTS; i++) {
        if (uiAvatars[i].key[0] != '\0' && strcmp(uiAvatars[i].key, imageName) == 0)
            return uiAvatars[i].tex.Mem ? &uiAvatars[i].tex : NULL;
    }

    for (i = 0; i < UI_AVATAR_SLOTS; i++) {
        if (uiAvatars[i].lastUse < uiAvatars[oldest].lastUse)
            oldest = i;
        if (uiAvatars[i].key[0] == '\0') {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        slot = oldest;
        uiFreeTexture(&uiAvatars[slot].tex);
    }

    memset(&uiAvatars[slot], 0, sizeof(ui_avatar_t));
    strncpy(uiAvatars[slot].key, imageName, PROFILE_IMAGE_NAME_MAX);
    uiAvatars[slot].key[PROFILE_IMAGE_NAME_MAX] = '\0';
    uiAvatars[slot].lastUse = ++uiAvatarTick;

    if (!profilesImagePath(imageName, path, sizeof(path))) {
        LOG("PROFILES_UI Could not resolve profile picture %s\n", imageName);
        return NULL;
    }

    data = uiReadFile(path, &size);
    if (data == NULL) {
        LOG("PROFILES_UI Profile picture missing: %s\n", path);
        return NULL;
    }

    memset(&image, 0, sizeof(image));
    if (!uiDecodePng(data, size, &image)) {
        LOG("PROFILES_UI Unsupported profile picture: %s\n", path);
        free(data);
        return NULL;
    }

    free(data);

    if (!uiBuildAvatarTexture(&uiAvatars[slot].tex, &image, UI_AVATAR_SIZE)) {
        uiImageFree(&image);
        return NULL;
    }

    uiImageFree(&image);
    return &uiAvatars[slot].tex;
}

static void uiEnsureAvatars(float animPos, int itemCount)
{
    int i;

    for (i = 1; i < itemCount - 1; i++) {
        float distance = (float)i - animPos;
        if (distance < 0.0f)
            distance = -distance;
        if (distance <= 2.6f)
            uiAvatarFor(profilesGetImage(i - 1));
    }
}

// ----------------------------------------------------------
// --------------------- Shared drawing ---------------------
// ----------------------------------------------------------

static void uiDrawDisc(int x, int y, int size)
{
    if (uiDiscTex.Mem)
        rmDrawPixmap(&uiDiscTex, x, y, ALIGN_CENTER, size, size, SCALING_RATIO, uiColor(255, 255, 255, 0x80));
}

static void uiDrawRing(int x, int y, int size, float focus)
{
    if (uiRingTex.Mem)
        rmDrawPixmap(&uiRingTex, x, y, ALIGN_CENTER, size, size, SCALING_RATIO, uiFocusColor(focus, 0x80));
}

static void uiDrawDefaultAvatar(int x, int y, int size)
{
    int icon = (size * 62) / 100;

    if (uiUserTex.Mem) {
        rmDrawPixmap(&uiUserTex, x, y, ALIGN_CENTER, icon, icon, SCALING_RATIO, uiColor(255, 255, 255, 0x80));
    } else {
        // Fallback if the embedded PNG could not be loaded: a white rounded
        // square, still the same silhouette as user.png.
        rmDrawRoundedRect(x - icon / 2, y - icon / 2, icon, icon, icon / 8, uiColor(255, 255, 255, 0x80));
    }
}

static void uiDrawAvatar(int x, int y, int size, const char *imageName)
{
    GSTEXTURE *avatar = uiAvatarFor(imageName);

    if (avatar != NULL) {
        rmDrawPixmap(avatar, x, y, ALIGN_CENTER, size, size, SCALING_RATIO, uiColor(255, 255, 255, 0x80));
    } else {
        uiDrawDefaultAvatar(x, y, size);
    }
}

static void uiDrawAddUserIcon(int x, int y, int size)
{
    if (uiPlusTex.Mem)
        rmDrawPixmap(&uiPlusTex, x, y, ALIGN_CENTER, size, size, SCALING_RATIO, uiColor(255, 255, 255, 0x80));
}

static void uiDrawMemoryCardIcon(int x, int y, int size)
{
    if (uiMemoryCardTex.Mem) {
        // memory_card.png is 311x375, so keep that aspect ratio instead of
        // stretching the icon into a square.
        rmDrawPixmap(&uiMemoryCardTex, x, y, ALIGN_CENTER, (size * 42) / 100, (size * 51) / 100, SCALING_RATIO, uiColor(255, 255, 255, 0x80));
    } else {
        int card = (size * 34) / 100;
        rmDrawRoundedRect(x - card / 2, y - card * 6 / 10, card, card * 12 / 10, 3, uiColor(255, 255, 255, 0x80));
    }
}

static void uiDrawLogo(void)
{
    int y = uiY(UI_LOGO_Y);

    if (uiPs2LogoTex.Mem) {
        rmDrawPixmap(&uiPs2LogoTex, uiX(UI_DW / 2), y, ALIGN_CENTER, uiD(UI_LOGO_W), uiD(UI_LOGO_H), SCALING_RATIO, uiColor(255, 255, 255, 0x80));
    } else {
        fntRenderString(uiFontLabel, uiX(UI_DW / 2), y, ALIGN_CENTER, 0, 0, "PS2", uiColor(255, 255, 255, 0x80));
    }
}

static void uiDrawTitles(const char *title, const char *subtitle, int subtitleFont)
{
    if (title != NULL)
        fntRenderString(uiFontTitle, uiX(UI_DW / 2), uiY(UI_TITLE_Y), ALIGN_CENTER, 0, 0, title, uiColor(255, 255, 255, 0x80));

    if (subtitle != NULL)
        fntRenderString(subtitleFont, uiX(UI_DW / 2), uiY(UI_SUBTITLE_Y), ALIGN_CENTER, 0, 0, subtitle, uiColor(255, 255, 255, 0x80));
}

static void uiDrawButton(int cx, int cy, int width, int height, const char *label, float focus)
{
    int growX = uiD(6);
    int growY = uiD(4);
    int w = width + uiRound(growX * focus);
    int h = height + uiRound(growY * focus);
    int x = cx - w / 2;
    int y = cy - h / 2;
    int border = uiD(3);

    if (border < 1)
        border = 1;

    rmDrawRoundedRect(x, y, w, h, uiD(7), uiFocusColor(focus, 0x80));
    rmDrawRoundedRect(x + border, y + border, w - border * 2, h - border * 2, uiD(5), uiColor(8, 8, 8, 0x80));
    fntRenderString(uiFontButton, cx, cy, ALIGN_CENTER, 0, 0, label, uiColor(255, 255, 255, 0x80));
}

static void uiFrameBegin(void)
{
    guiStartFrame();
    readPads();
    rmDrawRect(0, 0, uiScreenW, uiScreenH, uiColor(0, 0, 0, 0x80));
}

static void uiFrameEnd(void)
{
    guiExecDeferredOps();
    guiEndFrame();
}

static int uiConfirmPressed(void)
{
    if (getKeyOn(gSelectButton))
        return 1;
    if (gSelectButton != KEY_CROSS && getKeyOn(KEY_CROSS))
        return 1;
    return 0;
}

static int uiBackPressed(void)
{
    if (gSelectButton == KEY_CIRCLE)
        return getKeyOn(KEY_CROSS);
    return getKeyOn(KEY_CIRCLE);
}

static void uiPlayCursor(void)
{
    sfxPlay(SFX_CURSOR);
}

// ----------------------------------------------------------
// ------------------------ Lifecycle -----------------------
// ----------------------------------------------------------
void profilesUiBegin(void)
{
    thmEnsurePS5UI();

    rmGetScreenExtents(&uiScreenW, &uiScreenH);
    uiScale = (float)uiScreenH / (float)UI_DH;

    uiFontTitle = thmGetPS5ProfileTitleFont();
    uiFontSub = thmGetPS5ProfileSubFont();
    uiFontLabel = thmGetPS5SemiBoldFont();
    uiFontSmall = thmGetPS5SmallFont();
    uiFontTiny = thmGetPS5TinyFont();
    uiFontButton = thmGetPS5SemiBoldFont();

    if (uiAssetsReady)
        return;

    uiBuildDiscTexture(&uiDiscTex, 192);
    uiBuildRingTexture(&uiRingTex, 256);
    uiBuildPlusTexture(&uiPlusTex, 128);

    memset(&uiUserTex, 0, sizeof(uiUserTex));
    memset(&uiMemoryCardTex, 0, sizeof(uiMemoryCardTex));
    memset(&uiPs2LogoTex, 0, sizeof(uiPs2LogoTex));

    if (texLoadMem(&uiUserTex, &user_png) < 0)
        LOG("PROFILES_UI Could not load the embedded user.png\n");
    if (texLoadMem(&uiMemoryCardTex, &memory_card_png) < 0)
        LOG("PROFILES_UI Could not load the embedded memory_card.png\n");
    if (texLoadMem(&uiPs2LogoTex, &ps2_logo_png) < 0)
        LOG("PROFILES_UI Could not load the embedded ps2_logo.png\n");

    uiAssetsReady = 1;
}

void profilesUiEnd(void)
{
    int i;

    if (!uiAssetsReady)
        return;

    uiFreeTexture(&uiDiscTex);
    uiFreeTexture(&uiRingTex);
    uiFreeTexture(&uiPlusTex);
    uiFreeTexture(&uiUserTex);
    uiFreeTexture(&uiMemoryCardTex);
    uiFreeTexture(&uiPs2LogoTex);

    for (i = 0; i < PROFILE_IMAGE_COUNT; i++) {
        uiFreeTexture(&uiThumbs[i]);
        uiThumbState[i] = 0;
    }

    uiFreeAvatars();
    uiAssetsReady = 0;
}

// ----------------------------------------------------------
// ------------------- Selection screen ---------------------
// ----------------------------------------------------------
profiles_ui_result_t profilesUiSelect(int *selectedIndex)
{
    profiles_ui_result_t result;
    int count = profilesGetCount();
    int itemCount = count + 2;
    int focus = (selectedIndex != NULL) ? *selectedIndex : 0;
    static float animPos = -1.0f;
    static int lastItemCount = -1;
    int i;

    result.action = PROFILES_UI_ACTION_NONE;
    result.profileIndex = -1;

    if (focus < 0)
        focus = 0;
    if (focus >= itemCount)
        focus = itemCount - 1;

    if (animPos < 0.0f || lastItemCount != itemCount) {
        animPos = (float)focus;
        lastItemCount = itemCount;
    }

    for (;;) {
        struct
        {
            int index;
            float focus;
        } order[PROFILE_MAX_COUNT + 2];
        int orderCount = 0;
        char subtitle[160];
        int itemCountNow = count + 2;

        animPos = uiEaseTowards(animPos, (float)focus);

        // Load the pictures that are about to be visible before the frame
        // starts, so the render itself never touches the USB stick.
        uiEnsureAvatars(animPos, itemCountNow);

        if (count > 0)
            snprintf(subtitle, sizeof(subtitle), "Please Select a Profile");
        else
            snprintf(subtitle, sizeof(subtitle), "No Profile has been created yet, please create one or choose \"Save to Memory Card\"");

        uiFrameBegin();

        uiDrawTitles(count > 0 ? "Welcome Back to PlayStation" : "Welcome to PlayStation",
            subtitle, count > 0 ? uiFontSub : uiFontSmall);

        // Draw cards from the least focused to the most focused, so the large
        // card overlaps its neighbours exactly like the reference.
        for (i = 0; i < itemCountNow; i++) {
            float distance = (float)i - animPos;
            float t;

            if (distance < 0.0f)
                distance = -distance;
            t = uiSmoothStep(1.0f - distance);
            order[orderCount].index = i;
            order[orderCount].focus = t;
            orderCount++;
        }

        for (i = 1; i < orderCount; i++) {
            int j = i;
            while (j > 0 && order[j - 1].focus > order[j].focus) {
                int tmpIndex = order[j - 1].index;
                float tmpFocus = order[j - 1].focus;
                order[j - 1].index = order[j].index;
                order[j - 1].focus = order[j].focus;
                order[j].index = tmpIndex;
                order[j].focus = tmpFocus;
                j--;
            }
        }

        for (i = 0; i < orderCount; i++) {
            int index = order[i].index;
            float t = order[i].focus;
            int size = uiD(UI_NORMAL_DIAMETER) + uiRound((float)uiD(UI_FOCUS_DIAMETER - UI_NORMAL_DIAMETER) * t);
            int x = uiX(UI_DW / 2) + uiRound(((float)index - animPos) * (float)uiD(UI_TILE_PITCH));
            int y = uiY(UI_TILE_Y);
            const char *label;

            if (x < -size || x > uiScreenW + size)
                continue;

            if (index == 0) {
                uiDrawDisc(x, y, size);
                uiDrawAddUserIcon(x, y, size);
                label = "Add User";
            } else if (index == itemCountNow - 1) {
                uiDrawDisc(x, y, size);
                uiDrawMemoryCardIcon(x, y, size);
                label = "Save to Memory Card";
            } else {
                uiDrawDisc(x, y, size);
                uiDrawAvatar(x, y, size, profilesGetImage(index - 1));
                label = profilesGetName(index - 1);
            }

            uiDrawRing(x, y, size, t);

            {
                int gap = uiD(UI_LABEL_GAP) + uiRound((float)uiD(18) * t);
                int labelY = y + size / 2 + gap;
                int font = (t > 0.5f) ? uiFontLabel : uiFontSmall;

                fntRenderString(font, x, labelY, ALIGN_HCENTER | ALIGN_TOP, 0, 0, label, uiColor(255, 255, 255, 0x80));
            }
        }

        uiDrawLogo();
        uiFrameEnd();

        if (selectedIndex != NULL)
            *selectedIndex = focus;

        if (getKey(KEY_LEFT)) {
            if (focus > 0) {
                focus--;
                uiPlayCursor();
            }
        } else if (getKey(KEY_RIGHT)) {
            if (focus + 1 < itemCountNow) {
                focus++;
                uiPlayCursor();
            }
        }

        // Triangle opens the rename/delete actions for an existing profile.
        if (getKeyOn(KEY_TRIANGLE) && focus >= 1 && focus <= count) {
            sfxPlay(SFX_CONFIRM);
            result.action = PROFILES_UI_ACTION_OPTIONS;
            result.profileIndex = focus - 1;
            return result;
        }

        if (uiConfirmPressed() || (getKeyOn(KEY_START) && focus >= 1 && focus <= count)) {
            sfxPlay(SFX_CONFIRM);

            if (focus == 0) {
                result.action = PROFILES_UI_ACTION_ADD;
            } else if (focus == itemCountNow - 1) {
                result.action = PROFILES_UI_ACTION_MEMORY_CARD;
            } else {
                result.action = PROFILES_UI_ACTION_PROFILE;
                result.profileIndex = focus - 1;
            }

            return result;
        }

        // Circle is the launcher's usual back button. On the top level screen
        // there is nothing behind it, so it only moves the focus back to the
        // first profile instead of dropping the user into an undefined state.
        if (uiBackPressed() && count > 0 && focus != 1) {
            focus = 1;
            sfxPlay(SFX_CANCEL);
        }
    }
}

// ----------------------------------------------------------
// --------------------- Picture chooser --------------------
// ----------------------------------------------------------
static int uiImageIndex(const char *imageName)
{
    int i;
    char name[PROFILE_IMAGE_NAME_MAX];

    if (imageName == NULL || imageName[0] == '\0')
        return 0;

    for (i = 1; i <= PROFILE_IMAGE_COUNT; i++) {
        snprintf(name, sizeof(name), "profile_%d.png", i);
        if (strcasecmp(name, imageName) == 0)
            return i - 1;
    }

    return 0;
}

static void uiLoadThumbnails(void)
{
    int i;

    for (i = 0; i < PROFILE_IMAGE_COUNT; i++) {
        char name[PROFILE_IMAGE_NAME_MAX];
        char path[PROFILE_PATH_MAX];
        ui_image_t image;
        unsigned char *data;
        int size = 0;

        if (uiThumbState[i] != 0)
            continue;

        uiThumbState[i] = -1;
        snprintf(name, sizeof(name), "profile_%d.png", i + 1);

        if (!profilesImagePath(name, path, sizeof(path)))
            continue;

        data = uiReadFile(path, &size);
        if (data == NULL)
            continue;

        memset(&image, 0, sizeof(image));
        if (uiDecodePng(data, size, &image)) {
            if (uiBuildAvatarTexture(&uiThumbs[i], &image, UI_THUMB_SIZE))
                uiThumbState[i] = 1;
            else
                uiFreeTexture(&uiThumbs[i]);
        }

        uiImageFree(&image);
        free(data);
    }
}

static void uiFreeThumbnails(void)
{
    int i;

    for (i = 0; i < PROFILE_IMAGE_COUNT; i++) {
        uiFreeTexture(&uiThumbs[i]);
        uiThumbState[i] = 0;
    }
}

int profilesUiPictureChooser(const char *currentImage, char *out, int outLen)
{
    int focus = uiImageIndex(currentImage);
    float animPos = (float)focus;
    int result = 0;
    int done = 0;

    uiLoadThumbnails();

    while (!done) {
        int i;

        animPos = uiEaseTowards(animPos, (float)focus);

        uiFrameBegin();
        uiDrawTitles("Choose a Profile Picture", NULL, uiFontSub);

        for (i = 0; i < PROFILE_IMAGE_COUNT; i++) {
            int column = i % 5;
            int row = i / 5;
            float distance = (float)i - animPos;
            float t;
            int size, x, y;

            if (distance < 0.0f)
                distance = -distance;
            t = uiSmoothStep(1.0f - distance);

            size = uiD(140) + uiRound((float)uiD(32) * t);
            x = uiX(UI_DW / 2 + (column - 2) * 166);
            y = uiY(252 + row * 172);

            uiDrawDisc(x, y, size);

            if (uiThumbState[i] == 1 && uiThumbs[i].Mem) {
                rmDrawPixmap(&uiThumbs[i], x, y, ALIGN_CENTER, size, size, SCALING_RATIO, uiColor(255, 255, 255, 0x80));
            } else {
                char label[24];
                snprintf(label, sizeof(label), "profile_%d.png", i + 1);
                fntRenderString(uiFontTiny, x, y, ALIGN_CENTER, 0, 0, label, uiColor(0xD0, 0xD0, 0xD0, 0x80));
            }

            uiDrawRing(x, y, size, t);
        }

        uiDrawLogo();
        uiFrameEnd();

        if (getKey(KEY_LEFT)) {
            if (focus % 5 > 0) {
                focus--;
                uiPlayCursor();
            }
        } else if (getKey(KEY_RIGHT)) {
            if (focus % 5 < 4) {
                focus++;
                uiPlayCursor();
            }
        } else if (getKey(KEY_UP)) {
            if (focus >= 5) {
                focus -= 5;
                uiPlayCursor();
            }
        } else if (getKey(KEY_DOWN)) {
            if (focus < 5) {
                focus += 5;
                uiPlayCursor();
            }
        }

        if (uiConfirmPressed()) {
            if (uiThumbState[focus] == 1) {
                char name[PROFILE_IMAGE_NAME_MAX];
                snprintf(name, sizeof(name), "profile_%d.png", focus + 1);
                snprintf(out, outLen, "%s", name);
                sfxPlay(SFX_CONFIRM);
                result = 1;
                done = 1;
            } else {
                sfxPlay(SFX_CANCEL);
            }
        } else if (uiBackPressed()) {
            sfxPlay(SFX_CANCEL);
            done = 1;
        }
    }

    uiFreeThumbnails();
    return result;
}

// ----------------------------------------------------------
// ------------------------ Add User ------------------------
// ----------------------------------------------------------
static void uiTrimName(char *name)
{
    int length;
    int start = 0;

    while (name[start] == ' ' || name[start] == '\t')
        start++;

    if (start > 0)
        memmove(name, name + start, strlen(name + start) + 1);

    length = (int)strlen(name);
    while (length > 0 && (name[length - 1] == ' ' || name[length - 1] == '\t'))
        name[--length] = '\0';
}

int profilesUiAddUser(char *name, int nameLen, char *image, int imageLen)
{
    // The reference screen has OK highlighted when the screen opens.
    int focus = 2;
    int result = 0;
    int done = 0;
    float focusAnim[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    char error[96];

    error[0] = '\0';

    if (name[0] == '\0')
        snprintf(name, nameLen, "User %d", profilesGetCount() + 1);

    while (!done) {
        int i;

        for (i = 0; i < 4; i++)
            focusAnim[i] = uiEaseTowards(focusAnim[i], (i == focus) ? 1.0f : 0.0f);

        // Make sure the preview's picture is ready before the frame begins.
        uiAvatarFor(image);

        uiFrameBegin();
        uiDrawTitles("Add User", NULL, uiFontSub);

        // Profile picture preview (also the picture button).
        {
            int size = uiD(182) + uiRound((float)uiD(16) * focusAnim[0]);
            int x = uiX(UI_DW / 2);
            int y = uiY(238);

            uiDrawDisc(x, y, size);
            uiDrawAvatar(x, y, size, image);
            uiDrawRing(x, y, size, focusAnim[0]);
        }

        // Name label and input field.
        fntRenderString(uiFontSmall, uiX(455), uiY(396), ALIGN_LEFT, 0, 0, "Name:", uiColor(255, 255, 255, 0x80));
        {
            int x = uiX(455);
            int y = uiY(415);
            int w = uiD(370);
            int h = uiD(55);
            int border = uiD(3);

            if (border < 1)
                border = 1;

            rmDrawRoundedRect(x, y, w, h, uiD(7), uiFocusColor(focusAnim[1], 0x80));
            rmDrawRoundedRect(x + border, y + border, w - border * 2, h - border * 2, uiD(5), uiColor(0x3C, 0x3C, 0x3C, 0x80));
            fntRenderString(uiFontSmall, x + uiD(16), y + h / 2, ALIGN_LEFT | ALIGN_VCENTER, 0, 0, name, uiColor(255, 255, 255, 0x80));
        }

        if (error[0] != '\0')
            fntRenderString(uiFontSmall, uiX(640), uiY(486), ALIGN_HCENTER | ALIGN_TOP, uiD(700), uiD(26), error, uiColor(0xF0, 0x60, 0x60, 0x80));

        uiDrawButton(uiX(455 + 90), uiY(552), uiD(179), uiD(47), "OK", focusAnim[2]);
        uiDrawButton(uiX(643 + 90), uiY(552), uiD(181), uiD(47), "Cancel", focusAnim[3]);

        uiDrawLogo();
        uiFrameEnd();

        if (getKey(KEY_LEFT)) {
            if (focus == 3)
                focus = 2;
            else if (focus == 2)
                focus = 3;
            else if (focus > 0)
                focus--;
            else
                focus = 3;
            uiPlayCursor();
        } else if (getKey(KEY_RIGHT)) {
            if (focus == 2)
                focus = 3;
            else if (focus == 3)
                focus = 2;
            else if (focus < 3)
                focus++;
            else
                focus = 0;
            uiPlayCursor();
        } else if (getKey(KEY_UP)) {
            focus = (focus == 0) ? 3 : focus - 1;
            uiPlayCursor();
        } else if (getKey(KEY_DOWN)) {
            focus = (focus == 3) ? 0 : focus + 1;
            uiPlayCursor();
        }

        if (uiConfirmPressed()) {
            if (focus == 0) {
                char chosen[PROFILE_IMAGE_NAME_MAX];

                snprintf(chosen, sizeof(chosen), "%s", image);
                if (profilesUiPictureChooser(image, chosen, sizeof(chosen))) {
                    snprintf(image, imageLen, "%s", chosen);
                    error[0] = '\0';
                }
            } else if (focus == 1) {
                diaShowKeyb(name, nameLen, 0, "Profile Name");
                uiTrimName(name);
                error[0] = '\0';
            } else if (focus == 2) {
                uiTrimName(name);

                if (!profilesIsValidName(name)) {
                    snprintf(error, sizeof(error), "Enter 1-23 characters without : / \\ |");
                    sfxPlay(SFX_CANCEL);
                } else if (profilesNameInUse(name, 0)) {
                    snprintf(error, sizeof(error), "That name is already in use");
                    sfxPlay(SFX_CANCEL);
                } else {
                    sfxPlay(SFX_CONFIRM);
                    result = 1;
                    done = 1;
                }
            } else {
                sfxPlay(SFX_CANCEL);
                done = 1;
            }
        } else if (uiBackPressed()) {
            sfxPlay(SFX_CANCEL);
            done = 1;
        }
    }

    return result;
}

// ----------------------------------------------------------
// --------------------- Modal dialogs ----------------------
// ----------------------------------------------------------

// Returns the chosen entry, or -1 when Circle/back was pressed.
static int uiRunChoice(const char *title, const char *message, const char *const *labels, int labelCount)
{
    int focus = 0;
    int i;
    int result = -1;
    float focusAnim[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    if (labelCount > 4)
        labelCount = 4;

    for (;;) {
        int totalWidth = 0;
        int x;

        for (i = 0; i < 4; i++)
            focusAnim[i] = uiEaseTowards(focusAnim[i], (i == focus) ? 1.0f : 0.0f);

        uiFrameBegin();
        uiDrawTitles(title, message, uiFontSub);

        for (i = 0; i < labelCount; i++)
            totalWidth += uiD(180) + ((i > 0) ? uiD(18) : 0);

        x = uiX(UI_DW / 2) - totalWidth / 2;

        for (i = 0; i < labelCount; i++) {
            int width = uiD(180);
            uiDrawButton(x + width / 2, uiY(520), width, uiD(52), labels[i], focusAnim[i]);
            x += width + uiD(18);
        }

        uiDrawLogo();
        uiFrameEnd();

        if (getKey(KEY_LEFT)) {
            focus = (focus > 0) ? focus - 1 : labelCount - 1;
            uiPlayCursor();
        } else if (getKey(KEY_RIGHT)) {
            focus = (focus + 1 < labelCount) ? focus + 1 : 0;
            uiPlayCursor();
        }

        if (uiConfirmPressed()) {
            sfxPlay(SFX_CONFIRM);
            result = focus;
            break;
        }

        if (uiBackPressed()) {
            sfxPlay(SFX_CANCEL);
            result = -1;
            break;
        }
    }

    return result;
}

int profilesUiOptions(const char *profileName, int *choice)
{
    static const char *labels[4] = {"Rename", "Change Picture", "Delete", "Cancel"};
    char title[128];
    int selected;

    snprintf(title, sizeof(title), "Options: %s", profileName);
    selected = uiRunChoice(title, "Choose an action for this profile", labels, 4);

    if (selected < 0)
        return 0;

    if (choice != NULL)
        *choice = selected;

    return 1;
}

int profilesUiConfirm(const char *title, const char *message, const char *okLabel, const char *cancelLabel)
{
    const char *labels[2];
    int selected;

    labels[0] = okLabel;
    labels[1] = cancelLabel;

    selected = uiRunChoice(title, message, labels, 2);
    return (selected == 0) ? 1 : 0;
}

void profilesUiMessage(const char *title, const char *message)
{
    const char *labels[1];

    labels[0] = "OK";
    uiRunChoice(title, message, labels, 1);
}
