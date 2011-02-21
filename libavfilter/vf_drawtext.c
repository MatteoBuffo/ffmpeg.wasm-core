/*
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2010 S.N. Hemanth Meenakshisundaram
 * Copyright (c) 2003 Gustavo Sverzut Barbieri <gsbarbieri@yahoo.com.br>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * drawtext filter, based on the original FFmpeg vhook/drawtext.c
 * filter by Gustavo Sverzut Barbieri
 */

#include <sys/time.h>
#include <time.h>

#include "libavutil/colorspace.h"
#include "libavutil/file.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/tree.h"
#include "avfilter.h"
#include "drawutils.h"

#undef time

#include <ft2build.h>
#include <freetype/config/ftheader.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#define MAX_EXPANDED_TEXT_SIZE 2048

typedef struct {
    const AVClass *class;
    char *fontfile;                 ///< font to be used
    char *text;                     ///< text to be drawn
    int ft_load_flags;              ///< flags used for loading fonts, see FT_LOAD_*
    /** buffer containing the text expanded by strftime */
    char expanded_text[MAX_EXPANDED_TEXT_SIZE];
    /** positions for each element in the text */
    FT_Vector positions[MAX_EXPANDED_TEXT_SIZE];
    char *textfile;                 ///< file with text to be drawn
    unsigned int x;                 ///< x position to start drawing text
    unsigned int y;                 ///< y position to start drawing text
    unsigned int fontsize;          ///< font size to use
    char *fontcolor_string;         ///< font color as string
    char *boxcolor_string;          ///< box color as string
    uint8_t fontcolor[4];           ///< foreground color
    uint8_t boxcolor[4];            ///< background color
    uint8_t fontcolor_rgba[4];      ///< foreground color in RGBA
    uint8_t boxcolor_rgba[4];       ///< background color in RGBA

    short int draw_box;             ///< draw box around text - true or false
    int use_kerning;                ///< font kerning is used - true/false
    int tabsize;                    ///< tab size

    FT_Library library;             ///< freetype font library handle
    FT_Face face;                   ///< freetype font face handle
    struct AVTreeNode *glyphs;      ///< rendered glyphs, stored using the UTF-32 char code
    int hsub, vsub;                 ///< chroma subsampling values
    int is_packed_rgb;
    int pixel_step[4];              ///< distance in bytes between the component of each pixel
    uint8_t rgba_map[4];            ///< map RGBA offsets to the positions in the packed RGBA format
    uint8_t *box_line[4];           ///< line used for filling the box background
} DrawTextContext;

#define OFFSET(x) offsetof(DrawTextContext, x)

static const AVOption drawtext_options[]= {
{"fontfile", "set font file",        OFFSET(fontfile),         FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"text",     "set text",             OFFSET(text),             FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"textfile", "set text file",        OFFSET(textfile),         FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"fontcolor","set foreground color", OFFSET(fontcolor_string), FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"boxcolor", "set box color",        OFFSET(boxcolor_string),  FF_OPT_TYPE_STRING, 0,  CHAR_MIN, CHAR_MAX },
{"box",      "set box",              OFFSET(draw_box),         FF_OPT_TYPE_INT,    0,         0,        1 },
{"fontsize", "set font size",        OFFSET(fontsize),         FF_OPT_TYPE_INT,   16,         1,       72 },
{"x",        "set x",                OFFSET(x),                FF_OPT_TYPE_INT,    0,         0,  INT_MAX },
{"y",        "set y",                OFFSET(y),                FF_OPT_TYPE_INT,    0,         0,  INT_MAX },
{"tabsize",  "set tab size",         OFFSET(tabsize),          FF_OPT_TYPE_INT,    4,         0,  INT_MAX },

/* FT_LOAD_* flags */
{"ft_load_flags", "set font loading flags for libfreetype",   OFFSET(ft_load_flags),  FF_OPT_TYPE_FLAGS,  FT_LOAD_DEFAULT|FT_LOAD_RENDER, 0, INT_MAX, 0, "ft_load_flags" },
{"default",                     "set default",                     0, FF_OPT_TYPE_CONST, FT_LOAD_DEFAULT,                     INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"no_scale",                    "set no_scale",                    0, FF_OPT_TYPE_CONST, FT_LOAD_NO_SCALE,                    INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"no_hinting",                  "set no_hinting",                  0, FF_OPT_TYPE_CONST, FT_LOAD_NO_HINTING,                  INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"render",                      "set render",                      0, FF_OPT_TYPE_CONST, FT_LOAD_RENDER,                      INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"no_bitmap",                   "set no_bitmap",                   0, FF_OPT_TYPE_CONST, FT_LOAD_NO_BITMAP,                   INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"vertical_layout",             "set vertical_layout",             0, FF_OPT_TYPE_CONST, FT_LOAD_VERTICAL_LAYOUT,             INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"force_autohint",              "set force_autohint",              0, FF_OPT_TYPE_CONST, FT_LOAD_FORCE_AUTOHINT,              INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"crop_bitmap",                 "set crop_bitmap",                 0, FF_OPT_TYPE_CONST, FT_LOAD_CROP_BITMAP,                 INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"pedantic",                    "set pedantic",                    0, FF_OPT_TYPE_CONST, FT_LOAD_PEDANTIC,                    INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"ignore_global_advance_width", "set ignore_global_advance_width", 0, FF_OPT_TYPE_CONST, FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH, INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"no_recurse",                  "set no_recurse",                  0, FF_OPT_TYPE_CONST, FT_LOAD_NO_RECURSE,                  INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"ignore_transform",            "set ignore_transform",            0, FF_OPT_TYPE_CONST, FT_LOAD_IGNORE_TRANSFORM,            INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"monochrome",                  "set monochrome",                  0, FF_OPT_TYPE_CONST, FT_LOAD_MONOCHROME,                  INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"linear_design",               "set linear_design",               0, FF_OPT_TYPE_CONST, FT_LOAD_LINEAR_DESIGN,               INT_MIN, INT_MAX, 0, "ft_load_flags" },
{"no_autohint",                 "set no_autohint",                 0, FF_OPT_TYPE_CONST, FT_LOAD_NO_AUTOHINT,                 INT_MIN, INT_MAX, 0, "ft_load_flags" },
{NULL},
};

static const char *drawtext_get_name(void *ctx)
{
    return "drawtext";
}

static const AVClass drawtext_class = {
    "DrawTextContext",
    drawtext_get_name,
    drawtext_options
};

#undef __FTERRORS_H__
#define FT_ERROR_START_LIST {
#define FT_ERRORDEF(e, v, s) { (e), (s) },
#define FT_ERROR_END_LIST { 0, NULL } };

struct ft_error
{
    int err;
    const char *err_msg;
} static ft_errors[] =
#include FT_ERRORS_H

#define FT_ERRMSG(e) ft_errors[e].err_msg

typedef struct {
    FT_Glyph *glyph;
    uint32_t code;
    FT_Bitmap bitmap; ///< array holding bitmaps of font
    FT_BBox bbox;
    int advance;
    int bitmap_left;
    int bitmap_top;
} Glyph;

static int glyph_cmp(const Glyph *a, const Glyph *b)
{
    int64_t diff = (int64_t)a->code - (int64_t)b->code;
    return diff > 0 ? 1 : diff < 0 ? -1 : 0;
}

/**
 * Load glyphs corresponding to the UTF-32 codepoint code.
 */
static int load_glyph(AVFilterContext *ctx, Glyph **glyph_ptr, uint32_t code)
{
    DrawTextContext *dtext = ctx->priv;
    Glyph *glyph = av_mallocz(sizeof(Glyph));
    struct AVTreeNode *node = NULL;
    int ret;

    /* load glyph into dtext->face->glyph */
    ret = FT_Load_Char(dtext->face, code, dtext->ft_load_flags);
    if (ret)
        return AVERROR(EINVAL);

    /* save glyph */
    glyph->code  = code;
    glyph->glyph = av_mallocz(sizeof(FT_Glyph));
    ret = FT_Get_Glyph(dtext->face->glyph, glyph->glyph);
    if (ret)
        return AVERROR(EINVAL);

    glyph->bitmap      = dtext->face->glyph->bitmap;
    glyph->bitmap_left = dtext->face->glyph->bitmap_left;
    glyph->bitmap_top  = dtext->face->glyph->bitmap_top;
    glyph->advance     = dtext->face->glyph->advance.x >> 6;

    /* measure text height to calculate text_height (or the maximum text height) */
    FT_Glyph_Get_CBox(*glyph->glyph, ft_glyph_bbox_pixels, &glyph->bbox);

    /* cache the newly created glyph */
    if (!node)
        node = av_mallocz(av_tree_node_size);
    av_tree_insert(&dtext->glyphs, glyph, (void *)glyph_cmp, &node);

    if (glyph_ptr)
        *glyph_ptr = glyph;
    return 0;
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    int err;
    DrawTextContext *dtext = ctx->priv;

    dtext->class = &drawtext_class;
    av_opt_set_defaults2(dtext, 0, 0);
    dtext->fontcolor_string = av_strdup("black");
    dtext->boxcolor_string = av_strdup("white");

    if ((err = (av_set_options_string(dtext, args, "=", ":"))) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error parsing options string: '%s'\n", args);
        return err;
    }

    if (!dtext->fontfile) {
        av_log(ctx, AV_LOG_ERROR, "No font filename provided\n");
        return AVERROR(EINVAL);
    }

    if (dtext->textfile) {
        uint8_t *textbuf;
        size_t textbuf_size;

        if (dtext->text) {
            av_log(ctx, AV_LOG_ERROR,
                   "Both text and text file provided. Please provide only one\n");
            return AVERROR(EINVAL);
        }
        if ((err = av_file_map(dtext->textfile, &textbuf, &textbuf_size, 0, ctx)) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "The text file '%s' could not be read or is empty\n",
                   dtext->textfile);
            return err;
        }

        if (!(dtext->text = av_malloc(textbuf_size+1)))
            return AVERROR(ENOMEM);
        memcpy(dtext->text, textbuf, textbuf_size);
        dtext->text[textbuf_size] = 0;
        av_file_unmap(textbuf, textbuf_size);
    }

    if (!dtext->text) {
        av_log(ctx, AV_LOG_ERROR,
               "Either text or a valid file must be provided\n");
        return AVERROR(EINVAL);
    }

    if ((err = av_parse_color(dtext->fontcolor_rgba, dtext->fontcolor_string, -1, ctx))) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid font color '%s'\n", dtext->fontcolor_string);
        return err;
    }

    if ((err = av_parse_color(dtext->boxcolor_rgba, dtext->boxcolor_string, -1, ctx))) {
        av_log(ctx, AV_LOG_ERROR,
               "Invalid box color '%s'\n", dtext->boxcolor_string);
        return err;
    }

    if ((err = FT_Init_FreeType(&(dtext->library)))) {
        av_log(ctx, AV_LOG_ERROR,
               "Could not load FreeType: %s\n", FT_ERRMSG(err));
        return AVERROR(EINVAL);
    }

    /* load the face, and set up the encoding, which is by default UTF-8 */
    if ((err = FT_New_Face(dtext->library, dtext->fontfile, 0, &dtext->face))) {
        av_log(ctx, AV_LOG_ERROR, "Could not load fontface from file '%s': %s\n",
               dtext->fontfile, FT_ERRMSG(err));
        return AVERROR(EINVAL);
    }
    if ((err = FT_Set_Pixel_Sizes(dtext->face, 0, dtext->fontsize))) {
        av_log(ctx, AV_LOG_ERROR, "Could not set font size to %d pixels: %s\n",
               dtext->fontsize, FT_ERRMSG(err));
        return AVERROR(EINVAL);
    }

    dtext->use_kerning = FT_HAS_KERNING(dtext->face);

    /* load the fallback glyph with code 0 */
    load_glyph(ctx, NULL, 0);

#if !HAVE_LOCALTIME_R
    av_log(ctx, AV_LOG_WARNING, "strftime() expansion unavailable!\n");
#else
    if (strlen(dtext->text) >= MAX_EXPANDED_TEXT_SIZE) {
        av_log(ctx, AV_LOG_ERROR,
               "Impossible to print text, string is too big\n");
        return AVERROR(EINVAL);
    }
#endif

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum PixelFormat pix_fmts[] = {
        PIX_FMT_ARGB,    PIX_FMT_RGBA,
        PIX_FMT_ABGR,    PIX_FMT_BGRA,
        PIX_FMT_RGB24,   PIX_FMT_BGR24,
        PIX_FMT_YUV420P, PIX_FMT_YUV444P,
        PIX_FMT_YUV422P, PIX_FMT_YUV411P,
        PIX_FMT_YUV410P, PIX_FMT_YUV440P,
        PIX_FMT_NONE
    };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(pix_fmts));
    return 0;
}

static int glyph_enu_free(void *opaque, void *elem)
{
    av_free(elem);
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DrawTextContext *dtext = ctx->priv;
    int i;

    av_freep(&dtext->fontfile);
    av_freep(&dtext->text);
    av_freep(&dtext->fontcolor_string);
    av_freep(&dtext->boxcolor_string);
    av_tree_enumerate(dtext->glyphs, NULL, NULL, glyph_enu_free);
    av_tree_destroy(dtext->glyphs);
    dtext->glyphs = 0;
    FT_Done_Face(dtext->face);
    FT_Done_FreeType(dtext->library);

    for (i = 0; i < 4; i++) {
        av_freep(&dtext->box_line[i]);
        dtext->pixel_step[i] = 0;
    }

}

static int config_input(AVFilterLink *inlink)
{
    DrawTextContext *dtext = inlink->dst->priv;
    const AVPixFmtDescriptor *pix_desc = &av_pix_fmt_descriptors[inlink->format];
    int ret;

    dtext->hsub = pix_desc->log2_chroma_w;
    dtext->vsub = pix_desc->log2_chroma_h;

    if ((ret =
         ff_fill_line_with_color(dtext->box_line, dtext->pixel_step,
                                 inlink->w, dtext->boxcolor,
                                 inlink->format, dtext->boxcolor_rgba,
                                 &dtext->is_packed_rgb, dtext->rgba_map)) < 0)
        return ret;

    if (!dtext->is_packed_rgb) {
        uint8_t *rgba = dtext->fontcolor_rgba;
        dtext->fontcolor[0] = RGB_TO_Y_CCIR(rgba[0], rgba[1], rgba[2]);
        dtext->fontcolor[1] = RGB_TO_U_CCIR(rgba[0], rgba[1], rgba[2], 0);
        dtext->fontcolor[2] = RGB_TO_V_CCIR(rgba[0], rgba[1], rgba[2], 0);
        dtext->fontcolor[3] = rgba[3];
    }

    return 0;
}

#define GET_BITMAP_VAL(r, c)                                            \
    bitmap->pixel_mode == FT_PIXEL_MODE_MONO ?                          \
        (bitmap->buffer[(r) * bitmap->pitch + ((c)>>3)] & (0x80 >> ((c)&7))) * 255 : \
         bitmap->buffer[(r) * bitmap->pitch +  (c)]

#define SET_PIXEL_YUV(picref, yuva_color, val, x, y, hsub, vsub) {           \
    luma_pos    = ((x)          ) + ((y)          ) * picref->linesize[0]; \
    chroma_pos1 = ((x) >> (hsub)) + ((y) >> (vsub)) * picref->linesize[1]; \
    chroma_pos2 = ((x) >> (hsub)) + ((y) >> (vsub)) * picref->linesize[2]; \
    alpha = (yuva_color[3] * (val)) / 255;                               \
    picref->data[0][luma_pos] = (alpha * yuva_color[0] + (255 - alpha) * picref->data[0][luma_pos]) >> 8; \
    alpha = (yuva_color[3] * (val)) / 224;                               \
    picref->data[1][chroma_pos1] = 16 + (alpha * (yuva_color[1]-16) + (224 - alpha) * (picref->data[1][chroma_pos1]-16)) / 224; \
    picref->data[2][chroma_pos2] = 16 + (alpha * (yuva_color[2]-16) + (224 - alpha) * (picref->data[2][chroma_pos2]-16)) / 224; \
}

static inline int draw_glyph_yuv(AVFilterBufferRef *picref, FT_Bitmap *bitmap, unsigned int x,
                                 unsigned int y, unsigned int width, unsigned int height,
                                 unsigned char yuva_color[4], int hsub, int vsub)
{
    int r, c, alpha;
    unsigned int luma_pos, chroma_pos1, chroma_pos2;
    uint8_t src_val, dst_pixel[4];

    for (r = 0; r < bitmap->rows && r+y < height; r++) {
        for (c = 0; c < bitmap->width && c+x < width; c++) {
            /* get pixel in the picref (destination) */
            dst_pixel[0] = picref->data[0][  c+x           +  (y+r)          * picref->linesize[0]];
            dst_pixel[1] = picref->data[1][((c+x) >> hsub) + ((y+r) >> vsub) * picref->linesize[1]];
            dst_pixel[2] = picref->data[2][((c+x) >> hsub) + ((y+r) >> vsub) * picref->linesize[2]];

            /* get intensity value in the glyph bitmap (source) */
            src_val = GET_BITMAP_VAL(r, c);
            if (!src_val)
                continue;

            SET_PIXEL_YUV(picref, yuva_color, src_val, c+x, y+r, hsub, vsub);
        }
    }

    return 0;
}

#define SET_PIXEL_RGB(picref, rgba_color, val, x, y, pixel_step, r_off, g_off, b_off, a_off) { \
    p   = picref->data[0] + (x) * pixel_step + ((y) * picref->linesize[0]); \
    alpha = (rgba_color[3] * (val)) / 255;                              \
    *(p+r_off) = (alpha * rgba_color[0] + (255 - alpha) * *(p+r_off)) >> 8; \
    *(p+g_off) = (alpha * rgba_color[1] + (255 - alpha) * *(p+g_off)) >> 8; \
    *(p+b_off) = (alpha * rgba_color[2] + (255 - alpha) * *(p+b_off)) >> 8; \
}

static inline int draw_glyph_rgb(AVFilterBufferRef *picref, FT_Bitmap *bitmap,
                                 unsigned int x, unsigned int y,
                                 unsigned int width, unsigned int height, int pixel_step,
                                 unsigned char rgba_color[4], uint8_t rgba_map[4])
{
    int r, c, alpha;
    uint8_t *p;
    uint8_t src_val, dst_pixel[4];

    for (r = 0; r < bitmap->rows && r+y < height; r++) {
        for (c = 0; c < bitmap->width && c+x < width; c++) {
            /* get pixel in the picref (destination) */
            dst_pixel[0] = picref->data[0][(c+x + rgba_map[0]) * pixel_step +
                                           (y+r) * picref->linesize[0]];
            dst_pixel[1] = picref->data[0][(c+x + rgba_map[1]) * pixel_step +
                                           (y+r) * picref->linesize[0]];
            dst_pixel[2] = picref->data[0][(c+x + rgba_map[2]) * pixel_step +
                                           (y+r) * picref->linesize[0]];

            /* get intensity value in the glyph bitmap (source) */
            src_val = GET_BITMAP_VAL(r, c);
            if (!src_val)
                continue;

            SET_PIXEL_RGB(picref, rgba_color, src_val, c+x, y+r, pixel_step,
                          rgba_map[0], rgba_map[1], rgba_map[2], rgba_map[3]);
        }
    }

    return 0;
}

static inline void drawbox(AVFilterBufferRef *picref, unsigned int x, unsigned int y,
                           unsigned int width, unsigned int height,
                           uint8_t *line[4], int pixel_step[4], uint8_t color[4],
                           int hsub, int vsub, int is_rgba_packed, uint8_t rgba_map[4])
{
    int i, j, alpha;

    if (color[3] != 0xFF) {
        if (is_rgba_packed) {
            uint8_t *p;
            for (j = 0; j < height; j++)
                for (i = 0; i < width; i++)
                    SET_PIXEL_RGB(picref, color, 255, i+x, y+j, pixel_step[0],
                                  rgba_map[0], rgba_map[1], rgba_map[2], rgba_map[3]);
        } else {
            unsigned int luma_pos, chroma_pos1, chroma_pos2;
            for (j = 0; j < height; j++)
                for (i = 0; i < width; i++)
                    SET_PIXEL_YUV(picref, color, 255, i+x, y+j, hsub, vsub);
        }
    } else {
        ff_draw_rectangle(picref->data, picref->linesize,
                          line, pixel_step, hsub, vsub,
                          x, y, width, height);
    }
}

static int draw_text(AVFilterContext *ctx, AVFilterBufferRef *picref,
                     int width, int height)
{
    DrawTextContext *dtext = ctx->priv;
    char *text = dtext->text;
    uint32_t code = 0, prev_code = 0;
    int x = 0, y = 0, i = 0;
    int text_height, baseline;
    uint8_t *p;
    int str_w, str_w_max;
    int y_min = 32000, y_max = -32000;
    FT_Vector delta;
    Glyph *glyph = NULL, *prev_glyph = NULL;
    Glyph dummy = { 0 };

#if HAVE_LOCALTIME_R
    time_t now = time(0);
    struct tm ltime;
    size_t expanded_text_len;

    dtext->expanded_text[0] = '\1';
    expanded_text_len = strftime(dtext->expanded_text, MAX_EXPANDED_TEXT_SIZE,
                                 text, localtime_r(&now, &ltime));
    text = dtext->expanded_text;
    if (expanded_text_len == 0 && dtext->expanded_text[0] != '\0') {
        av_log(ctx, AV_LOG_ERROR,
               "Impossible to print text, string is too big\n");
        return AVERROR(EINVAL);
    }
#endif

    str_w = str_w_max = 0;
    x = dtext->x;
    y = dtext->y;

    /* load and cache glyphs */
    for (i = 0, p = text; *p; i++) {
        GET_UTF8(code, *p++, continue;);

        /* get glyph */
        dummy.code = code;
        glyph = av_tree_find(dtext->glyphs, &dummy, (void *)glyph_cmp, NULL);
        if (!glyph)
            load_glyph(ctx, &glyph, code);

        y_min = FFMIN(glyph->bbox.yMin, y_min);
        y_max = FFMAX(glyph->bbox.yMax, y_max);
    }
    text_height = y_max - y_min;
    baseline    = y_max;

    /* compute and save position for each glyph */
    glyph = NULL;
    for (i = 0, p = text; *p; i++) {
        GET_UTF8(code, *p++, continue;);

        /* skip the \n in the sequence \r\n */
        if (prev_code == '\r' && code == '\n')
            continue;

        /* get glyph */
        prev_glyph = glyph;
        dummy.code = code;
        glyph = av_tree_find(dtext->glyphs, &dummy, (void *)glyph_cmp, NULL);

        /* kerning */
        if (dtext->use_kerning && prev_glyph && glyph->code) {
            FT_Get_Kerning(dtext->face, prev_glyph->code, glyph->code,
                           ft_kerning_default, &delta);
            x += delta.x >> 6;
        }

        if (x + glyph->advance >= width || code == '\r' || code == '\n') {
            if (x + glyph->advance >= width)
                str_w_max = width - dtext->x - 1;
            y += text_height;
            x = dtext->x;
        }

        /* save position */
        dtext->positions[i].x = x + glyph->bitmap_left;
        dtext->positions[i].y = y - glyph->bitmap_top + baseline;
        if (code != '\n' && code != '\r') {
            int advance = glyph->advance;
            if (code == '\t')
                advance *= dtext->tabsize;
            x     += advance;
            str_w += advance;
        }
        prev_code = code;
    }

    y += text_height;
    if (str_w_max == 0)
        str_w_max = str_w;

    /* draw box */
    if (dtext->draw_box) {
        /* check if it doesn't pass the limits */
        str_w_max = FFMIN(str_w_max, width - dtext->x - 1);
        y = FFMIN(y, height - 1);

        /* draw background */
        drawbox(picref, dtext->x, dtext->y, str_w_max, y-dtext->y,
                dtext->box_line, dtext->pixel_step, dtext->boxcolor,
                dtext->hsub, dtext->vsub, dtext->is_packed_rgb, dtext->rgba_map);
    }

    /* draw glyphs */
    for (i = 0, p = text; *p; i++) {
        Glyph dummy = { 0 };
        GET_UTF8(code, *p++, continue;);

        /* skip new line chars, just go to new line */
        if (code == '\n' || code == '\r' || code == '\t')
            continue;

        dummy.code = code;
        glyph = av_tree_find(dtext->glyphs, &dummy, (void *)glyph_cmp, NULL);

        if (glyph->bitmap.pixel_mode != FT_PIXEL_MODE_MONO &&
            glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
            return AVERROR(EINVAL);

        if (dtext->is_packed_rgb) {
            draw_glyph_rgb(picref, &glyph->bitmap,
                           dtext->positions[i].x, dtext->positions[i].y, width, height,
                           dtext->pixel_step[0], dtext->fontcolor_rgba, dtext->rgba_map);
        } else {
            draw_glyph_yuv(picref, &glyph->bitmap,
                           dtext->positions[i].x, dtext->positions[i].y, width, height,
                           dtext->fontcolor, dtext->hsub, dtext->vsub);
        }
    }

    return 0;
}

static void null_draw_slice(AVFilterLink *link, int y, int h, int slice_dir) { }

static void end_frame(AVFilterLink *inlink)
{
    AVFilterLink *outlink = inlink->dst->outputs[0];
    AVFilterBufferRef *picref = inlink->cur_buf;

    draw_text(inlink->dst, picref, picref->video->w, picref->video->h);

    avfilter_draw_slice(outlink, 0, picref->video->h, 1);
    avfilter_end_frame(outlink);
}

AVFilter avfilter_vf_drawtext = {
    .name          = "drawtext",
    .description   = NULL_IF_CONFIG_SMALL("Draw text on top of video frames using libfreetype library."),
    .priv_size     = sizeof(DrawTextContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,

    .inputs    = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_VIDEO,
                                    .get_video_buffer = avfilter_null_get_video_buffer,
                                    .start_frame      = avfilter_null_start_frame,
                                    .draw_slice       = null_draw_slice,
                                    .end_frame        = end_frame,
                                    .config_props     = config_input,
                                    .min_perms        = AV_PERM_WRITE |
                                                        AV_PERM_READ,
                                    .rej_perms        = AV_PERM_PRESERVE },
                                  { .name = NULL}},
    .outputs   = (AVFilterPad[]) {{ .name             = "default",
                                    .type             = AVMEDIA_TYPE_VIDEO, },
                                  { .name = NULL}},
};
