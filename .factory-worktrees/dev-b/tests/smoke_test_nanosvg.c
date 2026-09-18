/*
 * smoke_test_nanosvg.c — Task 1 smoke test.
 *
 * Parses a small inline SVG via nanosvg, rasterizes it to an RGBA pixel
 * buffer with nanosvgrast, and asserts that the rasterizer produced a
 * non-empty image with the expected dimensions. This validates that the
 * vendored nanosvg headers compile and link (the implementation is
 * expanded in third_party/nanosvg/nanosvg_impl.c).
 */
/* declarations only here; implementation expanded in nanosvg_impl.c */
#include "nanosvg.h"
#include "nanosvgrast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A tiny SVG: a 64x64 canvas with one red rectangle. */
static const char *SVG =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\""
    " viewBox=\"0 0 64 64\">"
    "<rect x=\"8\" y=\"8\" width=\"48\" height=\"48\" fill=\"#ff0000\"/>"
    "</svg>";

int main(void) {
    /* nsvgParse modifies its input buffer in place, so work on a copy. */
    char *svg_buf = (char *)malloc(strlen(SVG) + 1);
    if (!svg_buf) {
        fprintf(stderr, "smoke_test_nanosvg: malloc failed\n");
        return 1;
    }
    strcpy(svg_buf, SVG);

    NSVGimage *image = nsvgParse(svg_buf, "px", 96.0f);
    free(svg_buf);
    if (!image) {
        fprintf(stderr, "smoke_test_nanosvg: nsvgParse returned NULL\n");
        return 1;
    }
    if (image->width != 64.0f || image->height != 64.0f) {
        fprintf(stderr, "smoke_test_nanosvg: unexpected dims %gx%g\n",
                image->width, image->height);
        nsvgDelete(image);
        return 1;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        fprintf(stderr, "smoke_test_nanosvg: nsvgCreateRasterizer failed\n");
        nsvgDelete(image);
        return 1;
    }

    const int W = 64, H = 64;
    unsigned char *pixels = (unsigned char *)malloc((size_t)W * H * 4);
    if (!pixels) {
        fprintf(stderr, "smoke_test_nanosvg: malloc failed\n");
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return 1;
    }

    nsvgRasterize(rast, image, 0.0f, 0.0f, 1.0f, pixels, W, H, W * 4);

    /* The red rect covers the center; confirm at least one red pixel exists. */
    int found_red = 0;
    for (int i = 0; i < W * H; i++) {
        unsigned char r = pixels[i * 4 + 0];
        unsigned char g = pixels[i * 4 + 1];
        unsigned char b = pixels[i * 4 + 2];
        if (r > 200 && g < 60 && b < 60) { found_red = 1; break; }
    }

    free(pixels);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    if (!found_red) {
        fprintf(stderr, "smoke_test_nanosvg: no red pixel rasterized\n");
        return 1;
    }

    printf("smoke_test_nanosvg: OK\n");
    return 0;
}