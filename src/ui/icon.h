// Procedurally rendered app icon (glass tile with the AirPlay glyph).
#pragma once

#include "common.h"

// 32-bit premultiplied BGRA pixels, size x size.
std::vector<uint32_t> RenderIconPixels(int size);
HICON CreateAppIcon(int size);
// Writes a multi-resolution .ico file (16..256 px).
bool WriteIconFile(const std::wstring& path);
