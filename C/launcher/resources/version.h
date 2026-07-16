// SAO Auto — launcher/resources/version.h
//
// VERSIONINFO fields consumed by app.rc.  Numeric fields must be a
// plain comma-separated tuple (major, minor, patch, build) — no expressions.
// The build tools/pack_cli overwrites this file at packaging time to stamp
// a build number derived from git commit count.

#pragma once

#define SAO_VER_MAJOR 0
#define SAO_VER_MINOR 2
#define SAO_VER_PATCH 0
#define SAO_VER_BUILD 0

#define SAO_VER_STRING     "0.2.0.0"
#define SAO_VER_FILE       0, 2, 0, 0
#define SAO_VER_PRODUCT    0, 2, 0, 0

#define SAO_VER_COMPANY    "SAO Auto Project"
#define SAO_VER_PRODUCT_NM "SAO Auto"
#define SAO_VER_DESC       "SAO Auto — native C++ launcher"
#define SAO_VER_COPYRIGHT  "(c) SAO Auto Contributors. All rights reserved."
#define SAO_VER_INTERNAL   "SaoAuto"
#define SAO_VER_ORIGINAL   "SaoAuto.exe"
