/*
 * nanosvg_impl.c — single translation unit that expands the nanosvg
 * parser and rasterizer implementations. Include this from the build;
 * do NOT define NANOSVG_IMPLEMENTATION / NANOSVGRAST_IMPLEMENTATION
 * anywhere else in the project.
 *
 * nanosvg is licensed under the zlib license (see LICENSE.txt).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"