/*
 * Build wrapper for the bundled llhttp dependency.
 *
 * The STM32CubeIDE-generated include path contains coreHTTP/source/include
 * but not the bundled llhttp/include directory. Re-export the upstream
 * header from here so both coreHTTP and llhttp translation units can
 * resolve #include "llhttp.h" without per-file include-path edits.
 */

#ifndef KVS_LLHTTP_WRAPPER_H
#define KVS_LLHTTP_WRAPPER_H

#include "../dependency/3rdparty/llhttp/include/llhttp.h"

#endif /* KVS_LLHTTP_WRAPPER_H */
