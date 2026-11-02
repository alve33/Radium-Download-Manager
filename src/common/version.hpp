// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef RADIUM_VERSION_MAJOR
#define RADIUM_VERSION_MAJOR 1
#endif

#ifndef RADIUM_VERSION_MINOR
#define RADIUM_VERSION_MINOR 0
#endif

#ifndef RADIUM_VERSION_PATCH
#define RADIUM_VERSION_PATCH 0
#endif

#ifndef RADIUM_VERSION_STRING
#define RADIUM_VERSION_STRING "1.0.0"
#endif

#ifndef RADIUM_PRODUCT_NAME
#define RADIUM_PRODUCT_NAME "Radium Download Manager"
#endif

#ifndef RADIUM_FULL_VERSION_STRING
#define RADIUM_FULL_VERSION_STRING "Radium Download Manager v1.0.0 (Radium) [Brisk Engine + Cat Catch Media Detection + N_m3u8DL-RE Pipeline]\n"
#endif

// Legacy Compatibility Aliases for external scripts and build tools
#ifndef FLUX_VERSION_MAJOR
#define FLUX_VERSION_MAJOR RADIUM_VERSION_MAJOR
#endif
#ifndef FLUX_VERSION_MINOR
#define FLUX_VERSION_MINOR RADIUM_VERSION_MINOR
#endif
#ifndef FLUX_VERSION_PATCH
#define FLUX_VERSION_PATCH RADIUM_VERSION_PATCH
#endif
#ifndef FLUX_VERSION_STRING
#define FLUX_VERSION_STRING RADIUM_VERSION_STRING
#endif
#ifndef FLUX_PRODUCT_NAME
#define FLUX_PRODUCT_NAME RADIUM_PRODUCT_NAME
#endif
#ifndef FLUX_FULL_VERSION_STRING
#define FLUX_FULL_VERSION_STRING RADIUM_FULL_VERSION_STRING
#endif
