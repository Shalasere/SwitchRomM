// stb_image wrapper for switch build. We keep this thin so the full upstream header lives in stb_image_full.h.
#ifndef ROMM_STB_IMAGE_H
#define ROMM_STB_IMAGE_H

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS

extern "C" {
#include "stb_image_full.h"
}

#endif // ROMM_STB_IMAGE_H
