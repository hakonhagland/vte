/*
 * Copyright (C) 2003,2008 Red Hat, Inc.
 * Copyright © 2019 Christian Persch
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#define WITH_UNICODE_NEXT

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "attr.hh"
#include "bidi.hh"
#include "vtedraw.hh"
#include "vtedefines.hh"
#include "debug.h"

#include <pango/pangocairo.h>

/* Have a space between letters to make sure ligatures aren't used when caching the glyphs: bug 793391. */
#define VTE_DRAW_SINGLE_WIDE_CHARACTERS	\
					"  ! \" # $ % & ' ( ) * + , - . / " \
					"0 1 2 3 4 5 6 7 8 9 " \
					": ; < = > ? @ " \
					"A B C D E F G H I J K L M N O P Q R S T U V W X Y Z " \
					"[ \\ ] ^ _ ` " \
					"a b c d e f g h i j k l m n o p q r s t u v w x y z " \
					"{ | } ~ " \
					""

static inline bool
_vte_double_equal(double a,
                  double b)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
        return a == b;
#pragma GCC diagnostic pop
}

/* Overview:
 *
 *
 * This file implements vte rendering using pangocairo.  Note that this does
 * NOT implement any kind of complex text rendering.  That's not currently a
 * goal.
 *
 * The aim is to be super-fast and avoid unneeded work as much as possible.
 * Here is an overview of how that is accomplished:
 *
 *   - We attach a font_info to the draw.  A font_info has all the information
 *     to quickly draw text.
 *
 *   - A font_info keeps uses unistr_font_info structs that represent all
 *     information needed to quickly draw a single vteunistr.  The font_info
 *     creates those unistr_font_info structs on demand and caches them
 *     indefinitely.  It uses a direct array for the ASCII range and a hash
 *     table for the rest.
 *
 *
 * Fast rendering of unistrs:
 *
 * A unistr_font_info (uinfo) calls Pango to set text for the unistr upon
 * initialization and then caches information needed to draw the results
 * later.  It uses three different internal representations and respectively
 * three drawing paths:
 *
 *   - COVERAGE_USE_CAIRO_GLYPH:
 *     Keeping a single glyph index and a cairo scaled-font.  This is the
 *     fastest way to draw text as it bypasses Pango completely and allows
 *     for stuffing multiple glyphs into a single cairo_show_glyphs() request
 *     (if scaled-fonts match).  This method is used if the glyphs used for
 *     the vteunistr as determined by Pango consists of a single regular glyph
 *     positioned at 0,0 using a regular font.  This method is used for more
 *     than 99% of the cases.  Only exceptional cases fall through to the
 *     other two methods.
 *
 *   - COVERAGE_USE_PANGO_GLYPH_STRING:
 *     Keeping a pango glyphstring and a pango font.  This is slightly slower
 *     than the previous case as drawing each glyph goes through pango
 *     separately and causes a separate cairo_show_glyphs() call.  This method
 *     is used when the previous method cannot be used but the glyphs for the
 *     character all use a single font.  This is the method used for hexboxes
 *     and "empty" characters like U+200C ZERO WIDTH NON-JOINER for example.
 *
 *   - COVERAGE_USE_PANGO_LAYOUT_LINE:
 *     Keeping a pango layout line.  This method is used only in the very
 *     weird and exceptional case that a single vteunistr uses more than one
 *     font to be drawn.  This happens for example if some diacretics is not
 *     available in the font chosen for the base character.
 *
 *
 * Caching of font infos:
 *
 * To avoid recreating font info structs for the same font again and again we
 * do the following:
 *
 *   - Use a global cache to share font info structs across different widgets.
 *     We use pango language, cairo font options, resolution, and font description
 *     as the key for our hash table.
 *
 *   - When a font info struct is no longer used by any widget, we delay
 *     destroying it for a while (FONT_CACHE_TIMEOUT seconds).  This is
 *     supposed to serve two purposes:
 *
 *       * Destroying a terminal widget and creating it again right after will
 *         reuse the font info struct from the previous widget.
 *
 *       * Zooming in and out a terminal reuses the font info structs.
 *
 *
 * Pre-caching ASCII letters:
 *
 * When initializing a font info struct we measure a string consisting of all
 * ASCII letters and some other ASCII characters.  Since we have a shaped pango
 * layout at hand, we walk over it and cache unistr font info for the ASCII
 * letters if we can do that easily using COVERAGE_USE_CAIRO_GLYPH.  This
 * means that we precache all ASCII letters without any extra pango shaping
 * involved.
 */



#define FONT_CACHE_TIMEOUT (30) /* seconds */


/* All shared data structures are implicitly protected by GDK mutex, because
 * that's how vte.c works and we only get called from there. */


/* cairo_show_glyphs accepts runs up to 102 glyphs before it allocates a
 * temporary array.
 *
 * Setting this to a large value can cause dramatic slow-downs for some
 * xservers (notably fglrx), see bug #410534.
 */
#define MAX_RUN_LENGTH 100


enum unistr_coverage {
	/* in increasing order of speed */
	COVERAGE_UNKNOWN = 0,		/* we don't know about the character yet */
	COVERAGE_USE_PANGO_LAYOUT_LINE,	/* use a PangoLayoutLine for the character */
	COVERAGE_USE_PANGO_GLYPH_STRING,	/* use a PangoGlyphString for the character */
	COVERAGE_USE_CAIRO_GLYPH	/* use a cairo_glyph_t for the character */
};

union unistr_font_info {
	/* COVERAGE_USE_PANGO_LAYOUT_LINE */
	struct {
		PangoLayoutLine *line;
	} using_pango_layout_line;
	/* COVERAGE_USE_PANGO_GLYPH_STRING */
	struct {
		PangoFont *font;
		PangoGlyphString *glyph_string;
	} using_pango_glyph_string;
	/* COVERAGE_USE_CAIRO_GLYPH */
	struct {
		cairo_scaled_font_t *scaled_font;
		unsigned int glyph_index;
	} using_cairo_glyph;
};

struct unistr_info {
	guchar coverage;
	guchar has_unknown_chars;
	guint16 width;
	union unistr_font_info ufi;
};

static struct unistr_info *
unistr_info_create (void)
{
	return g_slice_new0 (struct unistr_info);
}

static void
unistr_info_finish (struct unistr_info *uinfo)
{
	union unistr_font_info *ufi = &uinfo->ufi;

	switch (uinfo->coverage) {
	default:
	case COVERAGE_UNKNOWN:
		break;
	case COVERAGE_USE_PANGO_LAYOUT_LINE:
		/* we hold a manual reference on layout */
		g_object_unref (ufi->using_pango_layout_line.line->layout);
		ufi->using_pango_layout_line.line->layout = NULL;
		pango_layout_line_unref (ufi->using_pango_layout_line.line);
		ufi->using_pango_layout_line.line = NULL;
		break;
	case COVERAGE_USE_PANGO_GLYPH_STRING:
		if (ufi->using_pango_glyph_string.font)
			g_object_unref (ufi->using_pango_glyph_string.font);
		ufi->using_pango_glyph_string.font = NULL;
		pango_glyph_string_free (ufi->using_pango_glyph_string.glyph_string);
		ufi->using_pango_glyph_string.glyph_string = NULL;
		break;
	case COVERAGE_USE_CAIRO_GLYPH:
		cairo_scaled_font_destroy (ufi->using_cairo_glyph.scaled_font);
		ufi->using_cairo_glyph.scaled_font = NULL;
		break;
	}
}

static void
unistr_info_destroy (struct unistr_info *uinfo)
{
	unistr_info_finish (uinfo);
	g_slice_free (struct unistr_info, uinfo);
}

struct font_info {
	/* lifecycle */
	int ref_count;
	guint destroy_timeout; /* only used when ref_count == 0 */

	/* reusable layout set with font and everything set */
	PangoLayout *layout;

	/* cache of character info */
	struct unistr_info ascii_unistr_info[128];
	GHashTable *other_unistr_info;

        /* cell metrics as taken from the font, not yet scaled by cell_{width,height}_scale */
	gint width, height, ascent;

	/* reusable string for UTF-8 conversion */
	GString *string;

#ifdef VTE_DEBUG
	/* profiling info */
	int coverage_count[4];
#endif
};


static struct unistr_info *
font_info_find_unistr_info (struct font_info    *info,
			    vteunistr            c)
{
	struct unistr_info *uinfo;

	if (G_LIKELY (c < G_N_ELEMENTS (info->ascii_unistr_info)))
		return &info->ascii_unistr_info[c];

	if (G_UNLIKELY (info->other_unistr_info == NULL))
		info->other_unistr_info = g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify) unistr_info_destroy);

	uinfo = (struct unistr_info *)g_hash_table_lookup (info->other_unistr_info, GINT_TO_POINTER (c));
	if (G_LIKELY (uinfo))
		return uinfo;

	uinfo = unistr_info_create ();
	g_hash_table_insert (info->other_unistr_info, GINT_TO_POINTER (c), uinfo);
	return uinfo;
}


static void
font_info_cache_ascii (struct font_info *info)
{
	PangoLayoutLine *line;
	PangoGlyphItemIter iter;
	PangoGlyphItem *glyph_item;
	PangoGlyphString *glyph_string;
	PangoFont *pango_font;
	cairo_scaled_font_t *scaled_font;
	const char *text;
	gboolean more;
	PangoLanguage *language;
	gboolean latin_uses_default_language;
	
	/* We have info->layout holding most ASCII characters.  We want to
	 * cache as much info as we can about the ASCII letters so we don't
	 * have to look them up again later */

	/* Don't cache if unknown glyphs found in layout */
	if (pango_layout_get_unknown_glyphs_count (info->layout) != 0)
		return;

	language = pango_context_get_language (pango_layout_get_context (info->layout));
	if (language == NULL)
		language = pango_language_get_default ();
	latin_uses_default_language = pango_language_includes_script (language, PANGO_SCRIPT_LATIN);

	text = pango_layout_get_text (info->layout);

	line = pango_layout_get_line_readonly (info->layout, 0);

	/* Don't cache if more than one font used for the line */
	if (G_UNLIKELY (!line || !line->runs || line->runs->next))
		return;

	glyph_item = (PangoGlyphItem *)line->runs->data;
	glyph_string = glyph_item->glyphs;
	pango_font = glyph_item->item->analysis.font;
	if (!pango_font)
		return;
	scaled_font = pango_cairo_font_get_scaled_font ((PangoCairoFont *) pango_font);
	if (!scaled_font)
		return;

	for (more = pango_glyph_item_iter_init_start (&iter, glyph_item, text);
	     more;
	     more = pango_glyph_item_iter_next_cluster (&iter))
	{
		struct unistr_info *uinfo;
		union unistr_font_info *ufi;
	 	PangoGlyphGeometry *geometry;
		PangoGlyph glyph;
		vteunistr c;

		/* Only cache simple clusters */
		if (iter.start_char +1 != iter.end_char  ||
		    iter.start_index+1 != iter.end_index ||
		    iter.start_glyph+1 != iter.end_glyph)
			continue;

		c = text[iter.start_index];
		glyph = glyph_string->glyphs[iter.start_glyph].glyph;
		geometry = &glyph_string->glyphs[iter.start_glyph].geometry;

		/* If not using the default locale language, only cache non-common
		 * characters as common characters get their font from their neighbors
		 * and we don't want to force Latin on them. */
		if (!latin_uses_default_language &&
                    g_unichar_get_script (c) <= G_UNICODE_SCRIPT_INHERITED)
			continue;

		/* Only cache simple glyphs */
		if (!(glyph <= 0xFFFF) || (geometry->x_offset | geometry->y_offset) != 0)
			continue;

		uinfo = font_info_find_unistr_info (info, c);
		if (G_UNLIKELY (uinfo->coverage != COVERAGE_UNKNOWN))
			continue;

		ufi = &uinfo->ufi;

		uinfo->width = PANGO_PIXELS_CEIL (geometry->width);
		uinfo->has_unknown_chars = FALSE;

		uinfo->coverage = COVERAGE_USE_CAIRO_GLYPH;

		ufi->using_cairo_glyph.scaled_font = cairo_scaled_font_reference (scaled_font);
		ufi->using_cairo_glyph.glyph_index = glyph;

#ifdef VTE_DEBUG
		info->coverage_count[0]++;
		info->coverage_count[uinfo->coverage]++;
#endif
	}

#ifdef VTE_DEBUG
	_vte_debug_print (VTE_DEBUG_PANGOCAIRO,
			  "vtepangocairo: %p cached %d ASCII letters\n",
			  info, info->coverage_count[0]);
#endif
}

static void
font_info_measure_font (struct font_info *info)
{
	PangoRectangle logical;

        /* Measure U+0021..U+007E individually instead of all together and then
         * averaging. For monospace fonts, the results should be the same, but
         * if the user (by design, or trough mis-configuration) uses a proportional
         * font, the latter method will greatly underestimate the required width,
         * leading to unreadable, overlapping characters.
         * https://gitlab.gnome.org/GNOME/vte/issues/138
         */
        int max_width{1};
        int max_height{1};
        for (char c = 0x21; c < 0x7f; ++c) {
                pango_layout_set_text(info->layout, &c, 1);
                pango_layout_get_extents (info->layout, NULL, &logical);
                max_width = std::max(max_width, PANGO_PIXELS_CEIL(logical.width));
                max_height = std::max(max_height, PANGO_PIXELS_CEIL(logical.height));
        }

        /* Use the sample text to get the baseline */
	pango_layout_set_text (info->layout, VTE_DRAW_SINGLE_WIDE_CHARACTERS, -1);
	pango_layout_get_extents (info->layout, NULL, &logical);
	/* We don't do CEIL for width since we are averaging;
	 * rounding is more accurate */
	info->ascent = PANGO_PIXELS_CEIL (pango_layout_get_baseline (info->layout));

        info->height = max_height;
        info->width = max_width;

	/* Now that we shaped the entire ASCII character string, cache glyph
	 * info for them */
	font_info_cache_ascii (info);

	if (info->height == 0) {
		info->height = PANGO_PIXELS_CEIL (logical.height);
	}
	if (info->ascent == 0) {
		info->ascent = PANGO_PIXELS_CEIL (pango_layout_get_baseline (info->layout));
	}

	_vte_debug_print (VTE_DEBUG_MISC,
			  "vtepangocairo: %p font metrics = %dx%d (%d)\n",
			  info, info->width, info->height, info->ascent);
}

static struct font_info *
font_info_allocate (PangoContext *context)
{
	struct font_info *info;
	PangoTabArray *tabs;

	info = g_slice_new0 (struct font_info);

	_vte_debug_print (VTE_DEBUG_PANGOCAIRO,
			  "vtepangocairo: %p allocating font_info\n",
			  info);

	info->layout = pango_layout_new (context);
	tabs = pango_tab_array_new_with_positions (1, FALSE, PANGO_TAB_LEFT, 1);
	pango_layout_set_tabs (info->layout, tabs);
	pango_tab_array_free (tabs);

	info->string = g_string_sized_new (VTE_UTF8_BPC+1);

	font_info_measure_font (info);

	return info;
}

static void
font_info_free (struct font_info *info)
{
	vteunistr i;

#ifdef VTE_DEBUG
	_vte_debug_print (VTE_DEBUG_PANGOCAIRO,
			  "vtepangocairo: %p freeing font_info.  coverages %d = %d + %d + %d\n",
			  info,
			  info->coverage_count[0],
			  info->coverage_count[1],
			  info->coverage_count[2],
			  info->coverage_count[3]);
#endif

	g_string_free (info->string, TRUE);
	g_object_unref (info->layout);

	for (i = 0; i < G_N_ELEMENTS (info->ascii_unistr_info); i++)
		unistr_info_finish (&info->ascii_unistr_info[i]);
		
	if (info->other_unistr_info) {
		g_hash_table_destroy (info->other_unistr_info);
	}

	g_slice_free (struct font_info, info);
}


static GHashTable *font_info_for_context;

static struct font_info *
font_info_register (struct font_info *info)
{
	g_hash_table_insert (font_info_for_context,
			     pango_layout_get_context (info->layout),
			     info);

	return info;
}

static void
font_info_unregister (struct font_info *info)
{
	g_hash_table_remove (font_info_for_context,
			     pango_layout_get_context (info->layout));
}


static struct font_info *
font_info_reference (struct font_info *info)
{
	if (!info)
		return info;

	g_return_val_if_fail (info->ref_count >= 0, info);

	if (info->destroy_timeout) {
		g_source_remove (info->destroy_timeout);
		info->destroy_timeout = 0;
	}

	info->ref_count++;

	return info;
}

static gboolean
font_info_destroy_delayed (struct font_info *info)
{
	info->destroy_timeout = 0;

	font_info_unregister (info);
	font_info_free (info);

	return FALSE;
}

static void
font_info_destroy (struct font_info *info)
{
	if (!info)
		return;

	g_return_if_fail (info->ref_count > 0);

	info->ref_count--;
	if (info->ref_count)
		return;

	/* Delay destruction by a few seconds, in case we need it again */
	info->destroy_timeout = gdk_threads_add_timeout_seconds (FONT_CACHE_TIMEOUT,
								 (GSourceFunc) font_info_destroy_delayed,
								 info);
}

static GQuark
fontconfig_timestamp_quark (void)
{
	static GQuark quark;

	if (G_UNLIKELY (!quark))
		quark = g_quark_from_static_string ("vte-fontconfig-timestamp");

	return quark;
}

static void
vte_pango_context_set_fontconfig_timestamp (PangoContext *context,
					    guint         fontconfig_timestamp)
{
	g_object_set_qdata ((GObject *) context,
			    fontconfig_timestamp_quark (),
			    GUINT_TO_POINTER (fontconfig_timestamp));
}

static guint
vte_pango_context_get_fontconfig_timestamp (PangoContext *context)
{
	return GPOINTER_TO_UINT (g_object_get_qdata ((GObject *) context,
						     fontconfig_timestamp_quark ()));
}

static guint
context_hash (PangoContext *context)
{
	return pango_units_from_double (pango_cairo_context_get_resolution (context))
	     ^ pango_font_description_hash (pango_context_get_font_description (context))
	     ^ cairo_font_options_hash (pango_cairo_context_get_font_options (context))
	     ^ GPOINTER_TO_UINT (pango_context_get_language (context))
	     ^ vte_pango_context_get_fontconfig_timestamp (context);
}

static gboolean
context_equal (PangoContext *a,
	       PangoContext *b)
{
	return _vte_double_equal(pango_cairo_context_get_resolution(a), pango_cairo_context_get_resolution (b))
	    && pango_font_description_equal (pango_context_get_font_description (a), pango_context_get_font_description (b))
	    && cairo_font_options_equal (pango_cairo_context_get_font_options (a), pango_cairo_context_get_font_options (b))
	    && pango_context_get_language (a) == pango_context_get_language (b)
	    && vte_pango_context_get_fontconfig_timestamp (a) == vte_pango_context_get_fontconfig_timestamp (b);
}

static struct font_info *
font_info_find_for_context (PangoContext *context)
{
	struct font_info *info;

	if (G_UNLIKELY (font_info_for_context == NULL))
		font_info_for_context = g_hash_table_new ((GHashFunc) context_hash, (GEqualFunc) context_equal);

	info = (struct font_info *)g_hash_table_lookup (font_info_for_context, context);
	if (G_LIKELY (info)) {
		_vte_debug_print (VTE_DEBUG_PANGOCAIRO,
				  "vtepangocairo: %p found font_info in cache\n",
				  info);
		info = font_info_reference (info);
	} else {
		info = font_info_allocate (context);
		info->ref_count = 1;
		font_info_register (info);
	}

	g_object_unref (context);

	return info;
}

/* assumes ownership/reference of context */
static struct font_info *
font_info_create_for_context (PangoContext               *context,
			      const PangoFontDescription *desc,
			      PangoLanguage              *language,
			      guint                       fontconfig_timestamp)
{
	if (!PANGO_IS_CAIRO_FONT_MAP (pango_context_get_font_map (context))) {
		/* Ouch, Gtk+ switched over to some drawing system?
		 * Lets just create one from the default font map.
		 */
		g_object_unref (context);
		context = pango_font_map_create_context (pango_cairo_font_map_get_default ());
	}

	vte_pango_context_set_fontconfig_timestamp (context, fontconfig_timestamp);

	pango_context_set_base_dir (context, PANGO_DIRECTION_LTR);

	if (desc)
		pango_context_set_font_description (context, desc);

	pango_context_set_language (context, language);

        /* Make sure our contexts have a font_options set.  We use
          * this invariant in our context hash and equal functions.
          */
        if (!pango_cairo_context_get_font_options (context)) {
                cairo_font_options_t *font_options;

                font_options = cairo_font_options_create ();
                pango_cairo_context_set_font_options (context, font_options);
                cairo_font_options_destroy (font_options);
        }

	return font_info_find_for_context (context);
}

static struct font_info *
font_info_create_for_screen (GdkScreen                  *screen,
			     const PangoFontDescription *desc,
			     PangoLanguage              *language)
{
	GtkSettings *settings = gtk_settings_get_for_screen (screen);
	int fontconfig_timestamp;
	g_object_get (settings, "gtk-fontconfig-timestamp", &fontconfig_timestamp, nullptr);
	return font_info_create_for_context (gdk_pango_context_get_for_screen (screen),
					     desc, language, fontconfig_timestamp);
}

static struct font_info *
font_info_create_for_widget (GtkWidget                  *widget,
			     const PangoFontDescription *desc)
{
	GdkScreen *screen = gtk_widget_get_screen (widget);
	PangoLanguage *language = pango_context_get_language (gtk_widget_get_pango_context (widget));

	return font_info_create_for_screen (screen, desc, language);
}

static struct unistr_info *
font_info_get_unistr_info (struct font_info *info,
			   vteunistr c)
{
	struct unistr_info *uinfo;
	union unistr_font_info *ufi;
	PangoRectangle logical;
	PangoLayoutLine *line;

	uinfo = font_info_find_unistr_info (info, c);
	if (G_LIKELY (uinfo->coverage != COVERAGE_UNKNOWN))
		return uinfo;

	ufi = &uinfo->ufi;

	g_string_set_size (info->string, 0);
	_vte_unistr_append_to_string (c, info->string);
	pango_layout_set_text (info->layout, info->string->str, info->string->len);
	pango_layout_get_extents (info->layout, NULL, &logical);

	uinfo->width = PANGO_PIXELS_CEIL (logical.width);

	line = pango_layout_get_line_readonly (info->layout, 0);

	uinfo->has_unknown_chars = pango_layout_get_unknown_glyphs_count (info->layout) != 0;
	/* we use PangoLayoutRun rendering unless there is exactly one run in the line. */
	if (G_UNLIKELY (!line || !line->runs || line->runs->next))
	{
		uinfo->coverage = COVERAGE_USE_PANGO_LAYOUT_LINE;

		ufi->using_pango_layout_line.line = pango_layout_line_ref (line);
		/* we hold a manual reference on layout.  pango currently
		 * doesn't work if line->layout is NULL.  ugh! */
		pango_layout_set_text (info->layout, "", -1); /* make layout disassociate from the line */
		ufi->using_pango_layout_line.line->layout = (PangoLayout *)g_object_ref (info->layout);

	} else {
		PangoGlyphItem *glyph_item = (PangoGlyphItem *)line->runs->data;
		PangoFont *pango_font = glyph_item->item->analysis.font;
		PangoGlyphString *glyph_string = glyph_item->glyphs;

		/* we use fast cairo path if glyph string has only one real
		 * glyph and at origin */
		if (!uinfo->has_unknown_chars &&
		    glyph_string->num_glyphs == 1 && glyph_string->glyphs[0].glyph <= 0xFFFF &&
		    (glyph_string->glyphs[0].geometry.x_offset |
		     glyph_string->glyphs[0].geometry.y_offset) == 0)
		{
			cairo_scaled_font_t *scaled_font = pango_cairo_font_get_scaled_font ((PangoCairoFont *) pango_font);

			if (scaled_font) {
				uinfo->coverage = COVERAGE_USE_CAIRO_GLYPH;

				ufi->using_cairo_glyph.scaled_font = cairo_scaled_font_reference (scaled_font);
				ufi->using_cairo_glyph.glyph_index = glyph_string->glyphs[0].glyph;
			}
		}

		/* use pango fast path otherwise */
		if (G_UNLIKELY (uinfo->coverage == COVERAGE_UNKNOWN)) {
			uinfo->coverage = COVERAGE_USE_PANGO_GLYPH_STRING;

			ufi->using_pango_glyph_string.font = pango_font ? (PangoFont *)g_object_ref (pango_font) : NULL;
			ufi->using_pango_glyph_string.glyph_string = pango_glyph_string_copy (glyph_string);
		}
	}

	/* release internal layout resources */
	pango_layout_set_text (info->layout, "", -1);

#ifdef VTE_DEBUG
	info->coverage_count[0]++;
	info->coverage_count[uinfo->coverage]++;
#endif

	return uinfo;
}

guint _vte_draw_get_style(gboolean bold, gboolean italic) {
	guint style = 0;
	if (bold)
		style |= VTE_DRAW_BOLD;
	if (italic)
		style |= VTE_DRAW_ITALIC;
	return style;
}

struct _vte_draw {
	struct font_info *fonts[4];
        /* cell metrics, already adjusted by cell_{width,height}_scale */
        int cell_width, cell_height;
        GtkBorder char_spacing;

	cairo_t *cr;

        /* Cache the undercurl's rendered look. */
        cairo_surface_t *undercurl_surface;
};

struct _vte_draw *
_vte_draw_new (void)
{
	struct _vte_draw *draw;

	/* Create the structure. */
	draw = g_slice_new0 (struct _vte_draw);

	_vte_debug_print (VTE_DEBUG_DRAW, "draw_new\n");

	return draw;
}

void
_vte_draw_free (struct _vte_draw *draw)
{
	gint style;
	_vte_debug_print (VTE_DEBUG_DRAW, "draw_free\n");

	/* Free all fonts (make sure to destroy every font only once)*/
	for (style = 3; style >= 0; style--) {
		if (draw->fonts[style] != NULL &&
			(style == 0 || draw->fonts[style] != draw->fonts[style-1])) {
			font_info_destroy (draw->fonts[style]);
			draw->fonts[style] = NULL;
		}
	}

        if (draw->undercurl_surface != NULL) {
                cairo_surface_destroy (draw->undercurl_surface);
                draw->undercurl_surface = NULL;
        }

	g_slice_free (struct _vte_draw, draw);
}

void
_vte_draw_set_cairo (struct _vte_draw *draw,
                     cairo_t *cr)
{
        _vte_debug_print (VTE_DEBUG_DRAW, "%s cairo context\n", cr ? "Settings" : "Unsetting");

        if (cr) {
                g_assert (draw->cr == NULL);
                draw->cr = cr;
        } else {
                g_assert (draw->cr != NULL);
                draw->cr = NULL;
        }
}

void
_vte_draw_clip(struct _vte_draw *draw,
               cairo_rectangle_int_t const* rect)
{
        cairo_save(draw->cr);
        cairo_rectangle(draw->cr,
                        rect->x, rect->y, rect->width, rect->height);
        cairo_clip(draw->cr);
}

void
_vte_draw_unclip(struct _vte_draw *draw)
{
        cairo_restore(draw->cr);
}

static void
_vte_draw_set_source_color_alpha (struct _vte_draw *draw,
                                  vte::color::rgb const* color,
                                  double alpha)
{
        g_assert(draw->cr);
	cairo_set_source_rgba (draw->cr,
			      color->red / 65535.,
			      color->green / 65535.,
			      color->blue / 65535.,
			      alpha);
}

void
_vte_draw_clear (struct _vte_draw *draw, gint x, gint y, gint width, gint height,
                 vte::color::rgb const* color, double alpha)
{
	_vte_debug_print (VTE_DEBUG_DRAW, "draw_clear (%d, %d, %d, %d)\n",
			  x,y,width, height);

        g_assert(draw->cr);
	cairo_rectangle (draw->cr, x, y, width, height);
	cairo_set_operator (draw->cr, CAIRO_OPERATOR_SOURCE);
	_vte_draw_set_source_color_alpha(draw, color, alpha);
	cairo_fill (draw->cr);
}

void
_vte_draw_set_text_font (struct _vte_draw *draw,
                         GtkWidget *widget,
                         const PangoFontDescription *fontdesc,
                         double cell_width_scale,
                         double cell_height_scale)
{
	PangoFontDescription *bolddesc   = NULL;
	PangoFontDescription *italicdesc = NULL;
	PangoFontDescription *bolditalicdesc = NULL;
	gint style, normal, bold, ratio;

	_vte_debug_print (VTE_DEBUG_DRAW, "draw_set_text_font\n");

	/* Free all fonts (make sure to destroy every font only once)*/
	for (style = 3; style >= 0; style--) {
		if (draw->fonts[style] != NULL &&
			(style == 0 || draw->fonts[style] != draw->fonts[style-1])) {
			font_info_destroy (draw->fonts[style]);
			draw->fonts[style] = NULL;
		}
	}

	/* calculate bold font desc */
	bolddesc = pango_font_description_copy (fontdesc);
	pango_font_description_set_weight (bolddesc, PANGO_WEIGHT_BOLD);

	/* calculate italic font desc */
	italicdesc = pango_font_description_copy (fontdesc);
	pango_font_description_set_style (italicdesc, PANGO_STYLE_ITALIC);

	/* calculate bold italic font desc */
	bolditalicdesc = pango_font_description_copy (bolddesc);
	pango_font_description_set_style (bolditalicdesc, PANGO_STYLE_ITALIC);

	draw->fonts[VTE_DRAW_NORMAL]  = font_info_create_for_widget (widget, fontdesc);
	draw->fonts[VTE_DRAW_BOLD]    = font_info_create_for_widget (widget, bolddesc);
	draw->fonts[VTE_DRAW_ITALIC]  = font_info_create_for_widget (widget, italicdesc);
	draw->fonts[VTE_DRAW_ITALIC | VTE_DRAW_BOLD] =
                font_info_create_for_widget (widget, bolditalicdesc);
	pango_font_description_free (bolddesc);
	pango_font_description_free (italicdesc);
	pango_font_description_free (bolditalicdesc);

	/* Decide if we should keep this bold font face, per bug 54926:
	 *  - reject bold font if it is not within 10% of normal font width
	 */
	normal = VTE_DRAW_NORMAL;
	bold   = normal | VTE_DRAW_BOLD;
	ratio = draw->fonts[bold]->width * 100 / draw->fonts[normal]->width;
	if (abs(ratio - 100) > 10) {
		_vte_debug_print (VTE_DEBUG_DRAW,
			"Rejecting bold font (%i%%).\n", ratio);
		font_info_destroy (draw->fonts[bold]);
		draw->fonts[bold] = draw->fonts[normal];
	}
	normal = VTE_DRAW_ITALIC;
	bold   = normal | VTE_DRAW_BOLD;
	ratio = draw->fonts[bold]->width * 100 / draw->fonts[normal]->width;
	if (abs(ratio - 100) > 10) {
		_vte_debug_print (VTE_DEBUG_DRAW,
			"Rejecting italic bold font (%i%%).\n", ratio);
		font_info_destroy (draw->fonts[bold]);
		draw->fonts[bold] = draw->fonts[normal];
	}

        /* Apply letter spacing and line spacing. */
        draw->cell_width = draw->fonts[VTE_DRAW_NORMAL]->width * cell_width_scale;
        draw->char_spacing.left = (draw->cell_width - draw->fonts[VTE_DRAW_NORMAL]->width) / 2;
        draw->char_spacing.right = (draw->cell_width - draw->fonts[VTE_DRAW_NORMAL]->width + 1) / 2;
        draw->cell_height = draw->fonts[VTE_DRAW_NORMAL]->height * cell_height_scale;
        draw->char_spacing.top = (draw->cell_height - draw->fonts[VTE_DRAW_NORMAL]->height + 1) / 2;
        draw->char_spacing.bottom = (draw->cell_height - draw->fonts[VTE_DRAW_NORMAL]->height) / 2;

        /* Drop the undercurl's cached look. Will recache on demand. */
        if (draw->undercurl_surface != NULL) {
                cairo_surface_destroy (draw->undercurl_surface);
                draw->undercurl_surface = NULL;
        }
}

void
_vte_draw_get_text_metrics(struct _vte_draw *draw,
                           int *cell_width, int *cell_height,
                           int *char_ascent, int *char_descent,
                           GtkBorder *char_spacing)
{
	g_return_if_fail (draw->fonts[VTE_DRAW_NORMAL] != NULL);

        if (cell_width)
                *cell_width = draw->cell_width;
        if (cell_height)
                *cell_height = draw->cell_height;
        if (char_ascent)
                *char_ascent = draw->fonts[VTE_DRAW_NORMAL]->ascent;
        if (char_descent)
                *char_descent = draw->fonts[VTE_DRAW_NORMAL]->height - draw->fonts[VTE_DRAW_NORMAL]->ascent;
        if (char_spacing)
                *char_spacing = draw->char_spacing;
}

/* Check if a unicode character is actually a graphic character we draw
 * ourselves to handle cases where fonts don't have glyphs for them. */
static gboolean
_vte_draw_unichar_is_local_graphic(vteunistr c)
{
        /* Box Drawing & Block Elements */
        return ((c >=  0x2500 && c <=  0x259f) ||
                (c >=  0x25e2 && c <=  0x25e5)
#ifdef WITH_UNICODE_NEXT
                || (c >= 0x1fb00 && c <= 0x1fbff)
#endif
                );
}

/* Stores the left and right edges of the given glyph, relative to the cell's left edge. */
void
_vte_draw_get_char_edges (struct _vte_draw *draw, vteunistr c, int columns, guint style,
                          int *left, int *right)
{
        if (G_UNLIKELY (_vte_draw_unichar_is_local_graphic (c))) {
                if (left)
                        *left = 0;
                if (right)
                        *right = draw->cell_width * columns;
                return;
        }

        int l, w, normal_width, fits_width;

        if (G_UNLIKELY (draw->fonts[VTE_DRAW_NORMAL] == NULL)) {
                if (left)
                        *left = 0;
                if (right)
                        *right = 0;
                return;
        }

        w = font_info_get_unistr_info (draw->fonts[style], c)->width;
        normal_width = draw->fonts[VTE_DRAW_NORMAL]->width * columns;
        fits_width = draw->cell_width * columns;

        if (G_LIKELY (w <= normal_width)) {
                /* The regular case: The glyph is not wider than one (CJK: two) regular character(s).
                 * Align to the left, after applying half (CJK: one) letter spacing. */
                l = draw->char_spacing.left + (columns == 2 ? draw->char_spacing.right : 0);
        } else if (G_UNLIKELY (w <= fits_width)) {
                /* Slightly wider glyph, but still fits in the cell (spacing included). This case can
                 * only happen with nonzero letter spacing. Center the glyph in the cell(s). */
                l = (fits_width - w) / 2;
        } else {
                /* Even wider glyph: doesn't fit in the cell. Align at left and overflow on the right. */
                l = 0;
        }

        if (left)
                *left = l;
        if (right)
                *right = l + w;
}

#ifdef WITH_UNICODE_NEXT

static bool
_vte_draw_is_separable_mosaic(vteunistr c)
{
        // FIXMEchpe check from T.101 which characters should be separable
        return ((c >= 0x1fb00 && c <= 0x1fb9f)||
                (c >= 0x25e2 && c <= 0x25e5) ||
                (c >= 0x2580 && c <= 0x259f));
}

/* Create separated mosaic pattern.
 * Transparent pixels will not be drawn; opaque pixels will draw that part of the
 * mosaic onto the target surface.
 */
static cairo_pattern_t*
create_mosaic_separation_pattern(int width,
                                 int height,
                                 int line_thickness)
{
        auto surface = cairo_image_surface_create(CAIRO_FORMAT_A1, width, height);
        // or CAIRO_FORMAT_A8, whichever is better/faster?

        auto cr = cairo_create(surface);

        /* It's not quite clear how the separated mosaics should be drawn.
         *
         * ITU-T T.101 Annex C, C.2.1.2, and Annex D, D.5.4, show the separation
         * being done by blanking a line on the left and bottom parts only of each
         * of the 3x2 blocks.
         * The minitel specification STUM 1B, Schéma 2.7 also shows them drawn that
         * way.
         *
         * On the other hand, ETS 300 706 §15.7.1, Table 47, shows the separation
         * being done by blanking a line around all four sides of each of the
         * 3x2 blocks.
         * That is also how ITU-T T.100 §5.4.2.1, Figure 6, shows the separation.
         *
         * Each of these has its own drawbacks. The T.101 way makes the 3x2 blocks
         * asymmetric, leaving differing amount of lit pixels for the smooth mosaics
         * comparing a mosaic with its corresponding vertically mirrored mosaic. It
         * keeps more lit pixels overall, which make it more suitable for low-resolution
         * display, which is probably why minitel uses that.
         * The ETS 300 706 way keeps symmetry, but removes even more lit pixels.
         *
         * Here we implement the T.101 way.
         */

        /* FIXMEchpe: Check that this fulfills [T.101 Appendix IV]:
         * "All separated and contiguous mosaics shall be uniquely presented for character
         * field sizes greater than or equal to dx = 6/256, dy = 8/256 [see D.8.3.3, item 7)]."
         */

        /* First, fill completely with transparent pixels */
        cairo_set_source_rgba(cr, 0., 0., 0., 0.);
        cairo_rectangle(cr, 0, 0, width, height);
        cairo_fill(cr);

        /* Now, fill the reduced blocks with opaque pixels */

        auto const pel = line_thickness; /* see T.101 D.5.3.2.2.6 for definition of 'logical pel' */

        if (width > 2 * pel && height > 3 * pel) {

                auto const width_half = width / 2;
                auto const height_thirds = height / 3;
                auto const remaining_height = height - 3 * height_thirds;

                int const y[4] = { 0, height_thirds, 2 * height_thirds + (remaining_height ? 1 : 0), height };
                int const x[3] = { 0, width_half, width };
                // FIXMEchpe: or use 2 * width_half instead of width, so that for width odd,
                // the extra row of pixels is unlit, and the lit blocks have equal width?

                cairo_set_source_rgba(cr, 0., 0., 0., 1.);
                for (auto yi = 0; yi < 3; ++yi) {
                        for (auto xi = 0; xi < 2; xi++) {
                                cairo_rectangle(cr, x[xi] + pel, y[yi], x[xi+1] - x[xi] - pel, y[yi+1] - y[yi] - pel);
                                cairo_fill(cr);
                        }
                }
        }

        cairo_destroy(cr);

        auto pattern = cairo_pattern_create_for_surface(surface);
        cairo_surface_destroy(surface);

        cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
        cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);

        return pattern;
}

/* pixman data must have stride 0 mod 4 */
static unsigned char const hatching_pattern_lr_data[16] = {
        0xff, 0x00, 0x00, 0x00,
        0x00, 0xff, 0x00, 0x00,
        0x00, 0x00, 0xff, 0x00,
        0x00, 0x00, 0x00, 0xff,
};
static unsigned char const hatching_pattern_rl_data[16] = {
        0x00, 0x00, 0x00, 0xff,
        0x00, 0x00, 0xff, 0x00,
        0x00, 0xff, 0x00, 0x00,
        0xff, 0x00, 0x00, 0x00,
};
static unsigned char const checkerboard_pattern_data[16] = {
        0xff, 0xff, 0x00, 0x00,
        0xff, 0xff, 0x00, 0x00,
        0x00, 0x00, 0xff, 0xff,
        0x00, 0x00, 0xff, 0xff,
};
static unsigned char const checkerboard_reverse_pattern_data[16] = {
        0x00, 0x00, 0xff, 0xff,
        0x00, 0x00, 0xff, 0xff,
        0xff, 0xff, 0x00, 0x00,
        0xff, 0xff, 0x00, 0x00,
};

#define DEFINE_STATIC_PATTERN_FUNC(name,data,width,height,stride) \
static cairo_pattern_t* \
name(void) \
{ \
        static cairo_pattern_t* pattern = nullptr; \
\
        if (pattern == nullptr) { \
                auto surface = cairo_image_surface_create_for_data(const_cast<unsigned char*>(data), \
                                                                   CAIRO_FORMAT_A8, \
                                                                   width, \
                                                                   height, \
                                                                   stride); \
                pattern = cairo_pattern_create_for_surface(surface); \
                cairo_surface_destroy(surface); \
\
                cairo_pattern_set_extend (pattern, CAIRO_EXTEND_REPEAT); \
                cairo_pattern_set_filter (pattern, CAIRO_FILTER_NEAREST); \
       } \
\
       return pattern; \
}

DEFINE_STATIC_PATTERN_FUNC(create_hatching_pattern_lr, hatching_pattern_lr_data, 4, 4, 4)
DEFINE_STATIC_PATTERN_FUNC(create_hatching_pattern_rl, hatching_pattern_rl_data, 4, 4, 4)
DEFINE_STATIC_PATTERN_FUNC(create_checkerboard_pattern, checkerboard_pattern_data, 4, 4, 4)
DEFINE_STATIC_PATTERN_FUNC(create_checkerboard_reverse_pattern, checkerboard_reverse_pattern_data, 4, 4, 4)

#undef DEFINE_STATIC_PATTERN_FUNC

#endif /* WITH_UNICODE_NEXT */

#include "box_drawing.h"

/* Draw the graphic representation of a line-drawing or special graphics
 * character. */
static void
_vte_draw_terminal_draw_graphic(struct _vte_draw *draw,
                                vteunistr c,
                                uint32_t attr,
                                vte::color::rgb const* fg,
                                gint x, gint y,
                                gint font_width, gint columns, gint font_height)
{
        gint width, height, xcenter, xright, ycenter, ybottom;
        int upper_half, left_half;
        int light_line_width, heavy_line_width;
        double adjust;
        cairo_t *cr = draw->cr;

        cairo_save (cr);

        width = draw->cell_width * columns;
        height = draw->cell_height;
        upper_half = height / 2;
        left_half = width / 2;

#define RECTANGLE(cr, x, y, w, h, xdenom, ydenom, xb1, yb1, xb2, yb2) \
        do { \
                int x1 = (w) * (xb1) / (xdenom); \
                int y1 = (h) * (yb1) / (ydenom); \
                int x2 = (w) * (xb2) / (xdenom); \
                int y2 = (h) * (yb2) / (ydenom); \
                cairo_rectangle ((cr), (x) + x1, (y) + y1, MAX(x2 - x1, 1), MAX(y2 - y1, 1)); \
                cairo_fill (cr); \
        } while (0)

#define POLYGON(cr, x, y, w, h, xdenom, ydenom, coords) \
        do { \
                const int *cc = coords; \
                int x1 = (w) * (cc[0]) / (xdenom); \
                int y1 = (h) * (cc[1]) / (ydenom); \
                cairo_move_to ((cr), (x) + x1, (y) + y1); \
                int i = 2; \
                while (cc[i] != -1) { \
                        x1 = (w) * (cc[i]) / (xdenom); \
                        y1 = (h) * (cc[i + 1]) / (ydenom); \
                        cairo_line_to ((cr), (x) + x1, (y) + y1); \
                        i += 2; \
                } \
                cairo_fill (cr); \
        } while (0)

#define PATTERN(cr, pattern, width, height) \
        do { \
                cairo_push_group(cr); \
                cairo_rectangle(cr, x, y, width, height); \
                cairo_fill(cr); \
                cairo_pop_group_to_source(cr); \
                cairo_mask(cr, pattern); \
        } while (0)

        /* Exclude the spacing for line width computation. */
        light_line_width = font_width / 5;
        light_line_width = MAX (light_line_width, 1);

        if (c >= 0x2550 && c <= 0x256c) {
                heavy_line_width = 3 * light_line_width;
        } else {
                heavy_line_width = light_line_width + 2;
        }

        xcenter = x + left_half;
        ycenter = y + upper_half;
        xright = x + width;
        ybottom = y + height;

#ifdef WITH_UNICODE_NEXT
        auto const separated = vte_attr_get_bool(attr, VTE_ATTR_SEPARATED_MOSAIC_SHIFT) &&_vte_draw_is_separable_mosaic(c);
        if (separated)
                cairo_push_group(cr);
#endif

        switch (c) {

        /* Box Drawing */
        case 0x2500: /* box drawings light horizontal */
        case 0x2501: /* box drawings heavy horizontal */
        case 0x2502: /* box drawings light vertical */
        case 0x2503: /* box drawings heavy vertical */
        case 0x250c: /* box drawings light down and right */
        case 0x250d: /* box drawings down light and right heavy */
        case 0x250e: /* box drawings down heavy and right light */
        case 0x250f: /* box drawings heavy down and right */
        case 0x2510: /* box drawings light down and left */
        case 0x2511: /* box drawings down light and left heavy */
        case 0x2512: /* box drawings down heavy and left light */
        case 0x2513: /* box drawings heavy down and left */
        case 0x2514: /* box drawings light up and right */
        case 0x2515: /* box drawings up light and right heavy */
        case 0x2516: /* box drawings up heavy and right light */
        case 0x2517: /* box drawings heavy up and right */
        case 0x2518: /* box drawings light up and left */
        case 0x2519: /* box drawings up light and left heavy */
        case 0x251a: /* box drawings up heavy and left light */
        case 0x251b: /* box drawings heavy up and left */
        case 0x251c: /* box drawings light vertical and right */
        case 0x251d: /* box drawings vertical light and right heavy */
        case 0x251e: /* box drawings up heavy and right down light */
        case 0x251f: /* box drawings down heavy and right up light */
        case 0x2520: /* box drawings vertical heavy and right light */
        case 0x2521: /* box drawings down light and right up heavy */
        case 0x2522: /* box drawings up light and right down heavy */
        case 0x2523: /* box drawings heavy vertical and right */
        case 0x2524: /* box drawings light vertical and left */
        case 0x2525: /* box drawings vertical light and left heavy */
        case 0x2526: /* box drawings up heavy and left down light */
        case 0x2527: /* box drawings down heavy and left up light */
        case 0x2528: /* box drawings vertical heavy and left light */
        case 0x2529: /* box drawings down light and left up heavy */
        case 0x252a: /* box drawings up light and left down heavy */
        case 0x252b: /* box drawings heavy vertical and left */
        case 0x252c: /* box drawings light down and horizontal */
        case 0x252d: /* box drawings left heavy and right down light */
        case 0x252e: /* box drawings right heavy and left down light */
        case 0x252f: /* box drawings down light and horizontal heavy */
        case 0x2530: /* box drawings down heavy and horizontal light */
        case 0x2531: /* box drawings right light and left down heavy */
        case 0x2532: /* box drawings left light and right down heavy */
        case 0x2533: /* box drawings heavy down and horizontal */
        case 0x2534: /* box drawings light up and horizontal */
        case 0x2535: /* box drawings left heavy and right up light */
        case 0x2536: /* box drawings right heavy and left up light */
        case 0x2537: /* box drawings up light and horizontal heavy */
        case 0x2538: /* box drawings up heavy and horizontal light */
        case 0x2539: /* box drawings right light and left up heavy */
        case 0x253a: /* box drawings left light and right up heavy */
        case 0x253b: /* box drawings heavy up and horizontal */
        case 0x253c: /* box drawings light vertical and horizontal */
        case 0x253d: /* box drawings left heavy and right vertical light */
        case 0x253e: /* box drawings right heavy and left vertical light */
        case 0x253f: /* box drawings vertical light and horizontal heavy */
        case 0x2540: /* box drawings up heavy and down horizontal light */
        case 0x2541: /* box drawings down heavy and up horizontal light */
        case 0x2542: /* box drawings vertical heavy and horizontal light */
        case 0x2543: /* box drawings left up heavy and right down light */
        case 0x2544: /* box drawings right up heavy and left down light */
        case 0x2545: /* box drawings left down heavy and right up light */
        case 0x2546: /* box drawings right down heavy and left up light */
        case 0x2547: /* box drawings down light and up horizontal heavy */
        case 0x2548: /* box drawings up light and down horizontal heavy */
        case 0x2549: /* box drawings right light and left vertical heavy */
        case 0x254a: /* box drawings left light and right vertical heavy */
        case 0x254b: /* box drawings heavy vertical and horizontal */
        case 0x2550: /* box drawings double horizontal */
        case 0x2551: /* box drawings double vertical */
        case 0x2552: /* box drawings down single and right double */
        case 0x2553: /* box drawings down double and right single */
        case 0x2554: /* box drawings double down and right */
        case 0x2555: /* box drawings down single and left double */
        case 0x2556: /* box drawings down double and left single */
        case 0x2557: /* box drawings double down and left */
        case 0x2558: /* box drawings up single and right double */
        case 0x2559: /* box drawings up double and right single */
        case 0x255a: /* box drawings double up and right */
        case 0x255b: /* box drawings up single and left double */
        case 0x255c: /* box drawings up double and left single */
        case 0x255d: /* box drawings double up and left */
        case 0x255e: /* box drawings vertical single and right double */
        case 0x255f: /* box drawings vertical double and right single */
        case 0x2560: /* box drawings double vertical and right */
        case 0x2561: /* box drawings vertical single and left double */
        case 0x2562: /* box drawings vertical double and left single */
        case 0x2563: /* box drawings double vertical and left */
        case 0x2564: /* box drawings down single and horizontal double */
        case 0x2565: /* box drawings down double and horizontal single */
        case 0x2566: /* box drawings double down and horizontal */
        case 0x2567: /* box drawings up single and horizontal double */
        case 0x2568: /* box drawings up double and horizontal single */
        case 0x2569: /* box drawings double up and horizontal */
        case 0x256a: /* box drawings vertical single and horizontal double */
        case 0x256b: /* box drawings vertical double and horizontal single */
        case 0x256c: /* box drawings double vertical and horizontal */
        case 0x2574: /* box drawings light left */
        case 0x2575: /* box drawings light up */
        case 0x2576: /* box drawings light right */
        case 0x2577: /* box drawings light down */
        case 0x2578: /* box drawings heavy left */
        case 0x2579: /* box drawings heavy up */
        case 0x257a: /* box drawings heavy right */
        case 0x257b: /* box drawings heavy down */
        case 0x257c: /* box drawings light left and heavy right */
        case 0x257d: /* box drawings light up and heavy down */
        case 0x257e: /* box drawings heavy left and light right */
        case 0x257f: /* box drawings heavy up and light down */
        {
                guint32 bitmap = _vte_draw_box_drawing_bitmaps[c - 0x2500];
                int xboundaries[6] = { 0,
                                       left_half - heavy_line_width / 2,
                                       left_half - light_line_width / 2,
                                       left_half - light_line_width / 2 + light_line_width,
                                       left_half - heavy_line_width / 2 + heavy_line_width,
                                       width};
                int yboundaries[6] = { 0,
                                       upper_half - heavy_line_width / 2,
                                       upper_half - light_line_width / 2,
                                       upper_half - light_line_width / 2 + light_line_width,
                                       upper_half - heavy_line_width / 2 + heavy_line_width,
                                       height};
                int xi, yi;
                cairo_set_line_width(cr, 0);
                for (yi = 4; yi >= 0; yi--) {
                        for (xi = 4; xi >= 0; xi--) {
                                if (bitmap & 1) {
                                        cairo_rectangle(cr,
                                                        x + xboundaries[xi],
                                                        y + yboundaries[yi],
                                                        xboundaries[xi + 1] - xboundaries[xi],
                                                        yboundaries[yi + 1] - yboundaries[yi]);
                                        cairo_fill(cr);
                                }
                                bitmap >>= 1;
                        }
                }
                break;
        }

        case 0x2504: /* box drawings light triple dash horizontal */
        case 0x2505: /* box drawings heavy triple dash horizontal */
        case 0x2506: /* box drawings light triple dash vertical */
        case 0x2507: /* box drawings heavy triple dash vertical */
        case 0x2508: /* box drawings light quadruple dash horizontal */
        case 0x2509: /* box drawings heavy quadruple dash horizontal */
        case 0x250a: /* box drawings light quadruple dash vertical */
        case 0x250b: /* box drawings heavy quadruple dash vertical */
        case 0x254c: /* box drawings light double dash horizontal */
        case 0x254d: /* box drawings heavy double dash horizontal */
        case 0x254e: /* box drawings light double dash vertical */
        case 0x254f: /* box drawings heavy double dash vertical */
        {
                const guint v = c - 0x2500;
                int size, line_width;

                size = (v & 2) ? height : width;

                switch (v >> 2) {
                case 1: /* triple dash */
                {
                        double segment = size / 8.;
                        double dashes[2] = { segment * 2., segment };
                        cairo_set_dash(cr, dashes, G_N_ELEMENTS(dashes), 0.);
                        break;
                }
                case 2: /* quadruple dash */
                {
                        double segment = size / 11.;
                        double dashes[2] = { segment * 2., segment };
                        cairo_set_dash(cr, dashes, G_N_ELEMENTS(dashes), 0.);
                        break;
                }
                case 19: /* double dash */
                {
                        double segment = size / 5.;
                        double dashes[2] = { segment * 2., segment };
                        cairo_set_dash(cr, dashes, G_N_ELEMENTS(dashes), 0.);
                        break;
                }
                }

                line_width = (v & 1) ? heavy_line_width : light_line_width;
                adjust = (line_width & 1) ? .5 : 0.;

                cairo_set_line_width(cr, line_width);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
                if (v & 2) {
                        cairo_move_to(cr, xcenter + adjust, y);
                        cairo_line_to(cr, xcenter + adjust, y + height);
                } else {
                        cairo_move_to(cr, x, ycenter + adjust);
                        cairo_line_to(cr, x + width, ycenter + adjust);
                }
                cairo_stroke(cr);
                break;
        }

        case 0x256d: /* box drawings light arc down and right */
        case 0x256e: /* box drawings light arc down and left */
        case 0x256f: /* box drawings light arc up and left */
        case 0x2570: /* box drawings light arc up and right */
        {
                const guint v = c - 0x256d;
                int line_width;
                int radius;

                cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);

                line_width = light_line_width;
                adjust = (line_width & 1) ? .5 : 0.;
                cairo_set_line_width(cr, line_width);

                radius = (font_width + 2) / 3;
                radius = MAX(radius, heavy_line_width);

                if (v & 2) {
                        cairo_move_to(cr, xcenter + adjust, y);
                        cairo_line_to(cr, xcenter + adjust, ycenter - radius + 2 * adjust);
                } else {
                        cairo_move_to(cr, xcenter + adjust, ybottom);
                        cairo_line_to(cr, xcenter + adjust, ycenter + radius);
                }
                cairo_stroke(cr);

                cairo_arc(cr,
                          (v == 1 || v == 2) ? xcenter - radius + 2 * adjust
                                             : xcenter + radius,
                          (v & 2) ? ycenter - radius + 2 * adjust
                                  : ycenter + radius,
                          radius - adjust,
                          (v + 2) * M_PI / 2.0, (v + 3) * M_PI / 2.0);
                cairo_stroke(cr);

                if (v == 1 || v == 2) {
                        cairo_move_to(cr, xcenter - radius + 2 * adjust, ycenter + adjust);
                        cairo_line_to(cr, x, ycenter + adjust);
                } else {
                        cairo_move_to(cr, xcenter + radius, ycenter + adjust);
                        cairo_line_to(cr, xright, ycenter + adjust);
                }

                cairo_stroke(cr);
                break;
        }

        case 0x2571: /* box drawings light diagonal upper right to lower left */
        case 0x2572: /* box drawings light diagonal upper left to lower right */
        case 0x2573: /* box drawings light diagonal cross */
        {
                auto const dx = (light_line_width + 1) / 2;
                cairo_rectangle(cr, x - dx, y, width + 2 * dx, height);
                cairo_clip(cr);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
                cairo_set_line_width(cr, light_line_width);
                if (c != 0x2571) {
                        cairo_move_to(cr, x, y);
                        cairo_line_to(cr, xright, ybottom);
                        cairo_stroke(cr);
                }
                if (c != 0x2572) {
                        cairo_move_to(cr, xright, y);
                        cairo_line_to(cr, x, ybottom);
                        cairo_stroke(cr);
                }
                break;
        }

        /* Block Elements */
        case 0x2580: /* upper half block */
                RECTANGLE(cr, x, y, width, height, 1, 2,  0, 0,  1, 1);
                break;

        case 0x2581: /* lower one eighth block */
        case 0x2582: /* lower one quarter block */
        case 0x2583: /* lower three eighths block */
        case 0x2584: /* lower half block */
        case 0x2585: /* lower five eighths block */
        case 0x2586: /* lower three quarters block */
        case 0x2587: /* lower seven eighths block */
        {
                const guint v = 0x2588 - c;
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, v,  1, 8);
                break;
        }

        case 0x2588: /* full block */
        case 0x2589: /* left seven eighths block */
        case 0x258a: /* left three quarters block */
        case 0x258b: /* left five eighths block */
        case 0x258c: /* left half block */
        case 0x258d: /* left three eighths block */
        case 0x258e: /* left one quarter block */
        case 0x258f: /* left one eighth block */
        {
                const guint v = 0x2590 - c;
                RECTANGLE(cr, x, y, width, height, 8, 1,  0, 0,  v, 1);
                break;
        }

        case 0x2590: /* right half block */
                RECTANGLE(cr, x, y, width, height, 2, 1,  1, 0,  2, 1);
                break;

        case 0x2591: /* light shade */
        case 0x2592: /* medium shade */
        case 0x2593: /* dark shade */
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       (c - 0x2590) / 4.);
                cairo_rectangle(cr, x, y, width, height);
                cairo_fill (cr);
                break;

        case 0x2594: /* upper one eighth block */
        {
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 0,  1, 1);
                break;
        }

        case 0x2595: /* right one eighth block */
        {
                RECTANGLE(cr, x, y, width, height, 8, 1,  7, 0,  8, 1);
                break;
        }

        case 0x2596: /* quadrant lower left */
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 1,  1, 2);
                break;

        case 0x2597: /* quadrant lower right */
                RECTANGLE(cr, x, y, width, height, 2, 2,  1, 1,  2, 2);
                break;

        case 0x2598: /* quadrant upper left */
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 0,  1, 1);
                break;

        case 0x2599: /* quadrant upper left and lower left and lower right */
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 0,  1, 1);
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 1,  2, 2);
                break;

        case 0x259a: /* quadrant upper left and lower right */
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 0,  1, 1);
                RECTANGLE(cr, x, y, width, height, 2, 2,  1, 1,  2, 2);
                break;

        case 0x259b: /* quadrant upper left and upper right and lower left */
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 0,  2, 1);
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 1,  1, 2);
                break;

        case 0x259c: /* quadrant upper left and upper right and lower right */
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 0,  2, 1);
                RECTANGLE(cr, x, y, width, height, 2, 2,  1, 1,  2, 2);
                break;

        case 0x259d: /* quadrant upper right */
                RECTANGLE(cr, x, y, width, height, 2, 2,  1, 0,  2, 1);
                break;

        case 0x259e: /* quadrant upper right and lower left */
                RECTANGLE(cr, x, y, width, height, 2, 2,  1, 0,  2, 1);
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 1,  1, 2);
                break;

        case 0x259f: /* quadrant upper right and lower left and lower right */
                RECTANGLE(cr, x, y, width, height, 2, 2,  1, 0,  2, 1);
                RECTANGLE(cr, x, y, width, height, 2, 2,  0, 1,  2, 2);
                break;

        case 0x25e2: /* black lower right triangle */
        {
                int coords[] = { 0, 1,  1, 0,  1, 1,  -1 };
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x25e3: /* black lower left triangle */
        {
                int coords[] = { 0, 0,  1, 1,  0, 1,  -1 };
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x25e4: /* black upper left triangle */
        {
                int coords[] = { 0, 0,  1, 0,  0, 1,  -1 };
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x25e5: /* black upper right triangle */
        {
                int coords[] = { 0, 0,  1, 0,  1, 1,  -1 };
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

#ifdef WITH_UNICODE_NEXT
        case 0x1fb00:
        case 0x1fb01:
        case 0x1fb02:
        case 0x1fb03:
        case 0x1fb04:
        case 0x1fb05:
        case 0x1fb06:
        case 0x1fb07:
        case 0x1fb08:
        case 0x1fb09:
        case 0x1fb0a:
        case 0x1fb0b:
        case 0x1fb0c:
        case 0x1fb0d:
        case 0x1fb0e:
        case 0x1fb0f:
        case 0x1fb10:
        case 0x1fb11:
        case 0x1fb12:
        case 0x1fb13:
        case 0x1fb14:
        case 0x1fb15:
        case 0x1fb16:
        case 0x1fb17:
        case 0x1fb18:
        case 0x1fb19:
        case 0x1fb1a:
        case 0x1fb1b:
        case 0x1fb1c:
        case 0x1fb1d:
        case 0x1fb1e:
        case 0x1fb1f:
        case 0x1fb20:
        case 0x1fb21:
        case 0x1fb22:
        case 0x1fb23:
        case 0x1fb24:
        case 0x1fb25:
        case 0x1fb26:
        case 0x1fb27:
        case 0x1fb28:
        case 0x1fb29:
        case 0x1fb2a:
        case 0x1fb2b:
        case 0x1fb2c:
        case 0x1fb2d:
        case 0x1fb2e:
        case 0x1fb2f:
        case 0x1fb30:
        case 0x1fb31:
        case 0x1fb32:
        case 0x1fb33:
        case 0x1fb34:
        case 0x1fb35:
        case 0x1fb36:
        case 0x1fb37:
        case 0x1fb38:
        case 0x1fb39:
        case 0x1fb3a:
        case 0x1fb3b:
        {
                guint32 bitmap = c - 0x1fb00 + 1;
                if (bitmap >= 0x15) bitmap++;
                if (bitmap >= 0x2a) bitmap++;
                int xi, yi;
                cairo_set_line_width(cr, 0);
                for (yi = 0; yi <= 2; yi++) {
                        for (xi = 0; xi <= 1; xi++) {
                                if (bitmap & 1) {
                                        RECTANGLE(cr, x, y, width, height, 2, 3,  xi, yi, xi + 1,  yi + 1);
                                }
                                bitmap >>= 1;
                        }
                }
                break;
        }

        case 0x1fb3c:
        case 0x1fb3d:
        case 0x1fb3e:
        case 0x1fb3f:
        case 0x1fb40:
        case 0x1fb41:
        case 0x1fb42:
        case 0x1fb43:
        case 0x1fb44:
        case 0x1fb45:
        case 0x1fb46:
        case 0x1fb47:
        case 0x1fb48:
        case 0x1fb49:
        case 0x1fb4a:
        case 0x1fb4b:
        case 0x1fb4c:
        case 0x1fb4d:
        case 0x1fb4e:
        case 0x1fb4f:
        case 0x1fb50:
        case 0x1fb51:
        case 0x1fb52:
        case 0x1fb53:
        case 0x1fb54:
        case 0x1fb55:
        case 0x1fb56:
        case 0x1fb57:
        case 0x1fb58:
        case 0x1fb59:
        case 0x1fb5a:
        case 0x1fb5b:
        case 0x1fb5c:
        case 0x1fb5d:
        case 0x1fb5e:
        case 0x1fb5f:
        case 0x1fb60:
        case 0x1fb61:
        case 0x1fb62:
        case 0x1fb63:
        case 0x1fb64:
        case 0x1fb65:
        case 0x1fb66:
        case 0x1fb67:
        {
                const int v = c - 0x1fb3c;
                const int coords[46][11] = {
                        { 0, 2,  1, 3,  0, 3,  -1 },                /* 3c */
                        { 0, 2,  2, 3,  0, 3,  -1 },                /* 3d */
                        { 0, 1,  1, 3,  0, 3,  -1 },                /* 3e */
                        { 0, 1,  2, 3,  0, 3,  -1 },                /* 3f */
                        { 0, 0,  1, 3,  0, 3,  -1 },                /* 40 */
                        { 0, 1,  1, 0,  2, 0,  2, 3,  0, 3,  -1 },  /* 41 */
                        { 0, 1,  2, 0,  2, 3,  0, 3,  -1 },         /* 42 */
                        { 0, 2,  1, 0,  2, 0,  2, 3,  0, 3,  -1 },  /* 43 */
                        { 0, 2,  2, 0,  2, 3,  0, 3,  -1 },         /* 44 */
                        { 0, 3,  1, 0,  2, 0,  2, 3,  -1 },         /* 45 */
                        { 0, 2,  2, 1,  2, 3,  0, 3,  -1 },         /* 46 */
                        { 1, 3,  2, 2,  2, 3,  -1 },                /* 47 */
                        { 0, 3,  2, 2,  2, 3,  -1 },                /* 48 */
                        { 1, 3,  2, 1,  2, 3,  -1 },                /* 49 */
                        { 0, 3,  2, 1,  2, 3,  -1 },                /* 4a */
                        { 1, 3,  2, 0,  2, 3,  -1 },                /* 4b */
                        { 0, 0,  1, 0,  2, 1,  2, 3,  0, 3,  -1 },  /* 4c */
                        { 0, 0,  2, 1,  2, 3,  0, 3,  -1 },         /* 4d */
                        { 0, 0,  1, 0,  2, 2,  2, 3,  0, 3,  -1 },  /* 4e */
                        { 0, 0,  2, 2,  2, 3,  0, 3,  -1 },         /* 4f */
                        { 0, 0,  1, 0,  2, 3,  0, 3,  -1 },         /* 50 */
                        { 0, 1,  2, 2,  2, 3,  0, 3,  -1 },         /* 51 */
                        { 0, 0,  2, 0,  2, 3,  1, 3,  0, 2,  -1 },  /* 52 */
                        { 0, 0,  2, 0,  2, 3,  0, 2,  -1 },         /* 53 */
                        { 0, 0,  2, 0,  2, 3,  1, 3,  0, 1,  -1 },  /* 54 */
                        { 0, 0,  2, 0,  2, 3,  0, 1,  -1 },         /* 55 */
                        { 0, 0,  2, 0,  2, 3,  1, 3,  -1 },         /* 56 */
                        { 0, 0,  1, 0,  0, 1,  -1 },                /* 57 */
                        { 0, 0,  2, 0,  0, 1,  -1 },                /* 58 */
                        { 0, 0,  1, 0,  0, 2,  -1 },                /* 59 */
                        { 0, 0,  2, 0,  0, 2,  -1 },                /* 5a */
                        { 0, 0,  1, 0,  0, 3,  -1 },                /* 5b */
                        { 0, 0,  2, 0,  2, 1,  0, 2,  -1 },         /* 5c */
                        { 0, 0,  2, 0,  2, 2,  1, 3,  0, 3,  -1 },  /* 5d */
                        { 0, 0,  2, 0,  2, 2,  0, 3,  -1 },         /* 5e */
                        { 0, 0,  2, 0,  2, 1,  1, 3,  0, 3,  -1 },  /* 5f */
                        { 0, 0,  2, 0,  2, 1,  0, 3,  -1 },         /* 60 */
                        { 0, 0,  2, 0,  1, 3,  0, 3,  -1 },         /* 61 */
                        { 1, 0,  2, 0,  2, 1,  -1 },                /* 62 */
                        { 0, 0,  2, 0,  2, 1,  -1 },                /* 63 */
                        { 1, 0,  2, 0,  2, 2,  -1 },                /* 64 */
                        { 0, 0,  2, 0,  2, 2,  -1 },                /* 65 */
                        { 1, 0,  2, 0,  2, 3,  -1 },                /* 66 */
                        { 0, 0,  2, 0,  2, 2,  0, 1,  -1 },         /* 67 */
                };
                POLYGON(cr, x, y, width, height, 2, 3, coords[v]);
                break;
        }

        case 0x1fb68:
        case 0x1fb69:
        case 0x1fb6a:
        case 0x1fb6b:
        case 0x1fb6c:
        case 0x1fb6d:
        case 0x1fb6e:
        case 0x1fb6f:
        {
                const int v = c - 0x1fb68;
                const int coords[8][11] = {
                        { 0, 0,  2, 0,  2, 2,  0, 2,  1, 1,  -1 },  /* 68 */
                        { 0, 0,  1, 1,  2, 0,  2, 2,  0, 2,  -1 },  /* 69 */
                        { 0, 0,  2, 0,  1, 1,  2, 2,  0, 2,  -1 },  /* 6a */
                        { 0, 0,  2, 0,  2, 2,  1, 1,  0, 2,  -1 },  /* 6b */
                        { 0, 0,  1, 1,  0, 2,  -1 },                /* 6c */
                        { 0, 0,  2, 0,  1, 1,  -1 },                /* 6d */
                        { 1, 1,  2, 0,  2, 2,  -1 },                /* 6e */
                        { 1, 1,  2, 2,  0, 2,  -1 },                /* 6f */
                };
                POLYGON(cr, x, y, width, height, 2, 2, coords[v]);
                break;
        }

        case 0x1fb70:
        case 0x1fb71:
        case 0x1fb72:
        case 0x1fb73:
        case 0x1fb74:
        case 0x1fb75:
        {
                const int v = c - 0x1fb70 + 1;
                RECTANGLE(cr, x, y, width, height, 8, 1,  v, 0,  v + 1, 1);
                break;
        }

        case 0x1fb76:
        case 0x1fb77:
        case 0x1fb78:
        case 0x1fb79:
        case 0x1fb7a:
        case 0x1fb7b:
        {
                const int v = c - 0x1fb76 + 1;
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, v,  1, v + 1);
                break;
        }

        case 0x1fb7c:
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 7,  1, 8);
                RECTANGLE(cr, x, y, width, height, 8, 1,  0, 0,  1, 1);
                break;

        case 0x1fb7d:
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 0,  1, 1);
                RECTANGLE(cr, x, y, width, height, 8, 1,  0, 0,  1, 1);
                break;

        case 0x1fb7e:
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 0,  1, 1);
                RECTANGLE(cr, x, y, width, height, 8, 1,  7, 0,  8, 1);
                break;

        case 0x1fb7f:
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 7,  1, 8);
                RECTANGLE(cr, x, y, width, height, 8, 1,  7, 0,  8, 1);
                break;

        case 0x1fb80:
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 0,  1, 1);
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 7,  1, 8);
                break;

        case 0x1fb81:
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 0,  1, 1);
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 2,  1, 3);
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 4,  1, 5);
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 7,  1, 8);
                break;

        case 0x1fb82:
        case 0x1fb83:
        case 0x1fb84:
        case 0x1fb85:
        case 0x1fb86:
        {
                int v = c - 0x1fb82 + 2;
                if (v >= 4) v++;
                RECTANGLE(cr, x, y, width, height, 1, 8,  0, 0,  1, v);
                break;
        }

        case 0x1fb87:
        case 0x1fb88:
        case 0x1fb89:
        case 0x1fb8a:
        case 0x1fb8b:
        {
                int v = c - 0x1fb87 + 2;
                if (v >= 4) v++;
                RECTANGLE(cr, x, y, width, height, 8, 1,  8 - v, 0,  8, 1);
                break;
        }

        case 0x1fb8c:
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 2, 1,  0, 0,  1, 1);
                break;

        case 0x1fb8d:
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 2, 1,  1, 0,  2, 1);
                break;

        case 0x1fb8e:
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 1, 2,  0, 0,  1, 1);
                break;

        case 0x1fb8f:
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 1, 2,  0, 1,  1, 2);
                break;

        case 0x1fb90:
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 1, 1,  0, 0,  1, 1);
                break;

        case 0x1fb91:
                RECTANGLE(cr, x, y, width, height, 1, 2,  0, 0,  1, 1);
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 1, 2,  0, 1,  1, 2);
                break;

        case 0x1fb92:
                RECTANGLE(cr, x, y, width, height, 1, 2,  0, 1,  1, 2);
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 1, 2,  0, 0,  1, 1);
                break;

        case 0x1fb93:
#if 0
                /* codepoint not assigned */
                RECTANGLE(cr, x, y, width, height, 2, 1,  0, 0,  1, 1);
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 2, 1,  1, 0,  2, 1);
#endif
                break;

        case 0x1fb94:
                RECTANGLE(cr, x, y, width, height, 2, 1,  1, 0,  2, 1);
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                RECTANGLE(cr, x, y, width, height, 2, 1,  0, 0,  1, 1);
                break;

        case 0x1fb95:
                PATTERN(cr, create_checkerboard_pattern(), width, height);
                break;

        case 0x1fb96:
                PATTERN(cr, create_checkerboard_reverse_pattern(), width, height);
                break;

        case 0x1fb97:
                RECTANGLE(cr, x, y, width, height, 1, 4,  0, 1,  1, 2);
                RECTANGLE(cr, x, y, width, height, 1, 4,  0, 3,  1, 4);
                break;

        case 0x1fb98:
                PATTERN(cr, create_hatching_pattern_lr(), width, height);
                break;

        case 0x1fb99:
                PATTERN(cr, create_hatching_pattern_rl(), width, height);
                break;

        case 0x1fb9a:
        {
                /* Self-intersecting polygon, is this officially allowed by cairo? */
                int coords[] = { 0, 0,  1, 0,  0, 1,  1, 1,  -1 };
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x1fb9b:
        {
                /* Self-intersecting polygon, is this officially allowed by cairo? */
                int coords[] = { 0, 0,  1, 1,  1, 0,  0, 1,  -1 };
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x1fb9c:
        {
                int coords[] = { 0, 0,  1, 0,  0, 1,  -1 };
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x1fb9d:
        {
                int coords[] = { 0, 0,  1, 0,  1, 1,  -1 };
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x1fb9e:
        {
                int coords[] = { 0, 1,  1, 0,  1, 1,  -1 };
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x1fb9f:
        {
                int coords[] = { 0, 0,  1, 1,  0, 1,  -1 };
                cairo_set_source_rgba (cr,
                                       fg->red / 65535.,
                                       fg->green / 65535.,
                                       fg->blue / 65535.,
                                       0.5);
                POLYGON(cr, x, y, width, height, 1, 1, coords);
                break;
        }

        case 0x1fba0:
        case 0x1fba1:
        case 0x1fba2:
        case 0x1fba3:
        case 0x1fba4:
        case 0x1fba5:
        case 0x1fba6:
        case 0x1fba7:
        case 0x1fba8:
        case 0x1fba9:
        case 0x1fbaa:
        case 0x1fbab:
        case 0x1fbac:
        case 0x1fbad:
        case 0x1fbae:
        {
                const int v = c - 0x1fba0;
                const int map[15] = { 0b0001, 0b0010, 0b0100, 0b1000, 0b0101, 0b1010, 0b1100, 0b0011,
                                      0b1001, 0b0110, 0b1110, 0b1101, 0b1011, 0b0111, 0b1111 };
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
                cairo_set_line_width(cr, light_line_width);
                adjust = (light_line_width & 1) ? .5 : 0.;
                double const dx = light_line_width / 2.;
                double const dy = light_line_width / 2.;
                if (map[v] & 1) {
                        /* upper left */
                        cairo_move_to(cr, x, ycenter + adjust);
                        cairo_line_to(cr, x + dx, ycenter + adjust);
                        cairo_line_to(cr, xcenter + adjust, y + dy);
                        cairo_line_to(cr, xcenter + adjust, y);
                        cairo_stroke(cr);
                }
                if (map[v] & 2) {
                        /* upper right */
                        cairo_move_to(cr, xright, ycenter + adjust);
                        cairo_line_to(cr, xright - dx, ycenter + adjust);
                        cairo_line_to(cr, xcenter + adjust, y + dy);
                        cairo_line_to(cr, xcenter + adjust, y);
                        cairo_stroke(cr);
                }
                if (map[v] & 4) {
                        /* lower left */
                        cairo_move_to(cr, x, ycenter + adjust);
                        cairo_line_to(cr, x + dx, ycenter + adjust);
                        cairo_line_to(cr, xcenter + adjust, ybottom - dy);
                        cairo_line_to(cr, xcenter + adjust, ybottom);
                        cairo_stroke(cr);
                }
                if (map[v] & 8) {
                        /* lower right */
                        cairo_move_to(cr, xright, ycenter + adjust);
                        cairo_line_to(cr, xright - dx, ycenter + adjust);
                        cairo_line_to(cr, xcenter + adjust, ybottom - dy);
                        cairo_line_to(cr, xcenter + adjust, ybottom);
                        cairo_stroke(cr);
                }
                break;
        }
#endif /* WITH_UNICODE_NEXT */

        default:
#ifdef WITH_UNICODE_NEXT
                break; /* FIXME temporary */
#endif
                g_assert_not_reached();
        }

#undef RECTANGLE
#undef POLYGON
#undef PATTERN

#ifdef WITH_UNICODE_NEXT
        if (separated) {
                cairo_pop_group_to_source(cr);
                auto pattern = create_mosaic_separation_pattern(width, height, light_line_width);
                cairo_mask(cr, pattern);
                cairo_pattern_destroy(pattern);
        }
#endif

        cairo_restore(cr);
}

static void
_vte_draw_text_internal (struct _vte_draw *draw,
			 struct _vte_draw_text_request *requests, gsize n_requests,
                         uint32_t attr,
			 vte::color::rgb const* color, double alpha, guint style)
{
	gsize i;
	cairo_scaled_font_t *last_scaled_font = NULL;
	int n_cr_glyphs = 0;
	cairo_glyph_t cr_glyphs[MAX_RUN_LENGTH];
	struct font_info *font = draw->fonts[style];

	g_return_if_fail (font != NULL);

        g_assert(draw->cr);
	_vte_draw_set_source_color_alpha (draw, color, alpha);
	cairo_set_operator (draw->cr, CAIRO_OPERATOR_OVER);

	for (i = 0; i < n_requests; i++) {
		vteunistr c = requests[i].c;

                if (G_UNLIKELY (requests[i].mirror)) {
                        vte_bidi_get_mirror_char (c, requests[i].box_mirror, &c);
                }

                if (_vte_draw_unichar_is_local_graphic(c)) {
                        _vte_draw_terminal_draw_graphic(draw, c,
                                                        attr,
                                                        color,
                                                        requests[i].x, requests[i].y,
                                                        font->width, requests[i].columns, font->height);
                        continue;
                }

		struct unistr_info *uinfo = font_info_get_unistr_info (font, c);
		union unistr_font_info *ufi = &uinfo->ufi;
                int x, y;

                _vte_draw_get_char_edges(draw, c, requests[i].columns, style, &x, NULL);
                x += requests[i].x;
                /* Bold/italic versions might have different ascents. In order to align their
                 * baselines, we offset by the normal font's ascent here. (Bug 137.) */
                y = requests[i].y + draw->char_spacing.top + draw->fonts[VTE_DRAW_NORMAL]->ascent;

		switch (uinfo->coverage) {
		default:
		case COVERAGE_UNKNOWN:
			g_assert_not_reached ();
			break;
		case COVERAGE_USE_PANGO_LAYOUT_LINE:
			cairo_move_to (draw->cr, x, y);
			pango_cairo_show_layout_line (draw->cr,
						      ufi->using_pango_layout_line.line);
			break;
		case COVERAGE_USE_PANGO_GLYPH_STRING:
			cairo_move_to (draw->cr, x, y);
			pango_cairo_show_glyph_string (draw->cr,
						       ufi->using_pango_glyph_string.font,
						       ufi->using_pango_glyph_string.glyph_string);
			break;
		case COVERAGE_USE_CAIRO_GLYPH:
			if (last_scaled_font != ufi->using_cairo_glyph.scaled_font || n_cr_glyphs == MAX_RUN_LENGTH) {
				if (n_cr_glyphs) {
					cairo_set_scaled_font (draw->cr, last_scaled_font);
					cairo_show_glyphs (draw->cr,
							   cr_glyphs,
							   n_cr_glyphs);
					n_cr_glyphs = 0;
				}
				last_scaled_font = ufi->using_cairo_glyph.scaled_font;
			}
			cr_glyphs[n_cr_glyphs].index = ufi->using_cairo_glyph.glyph_index;
			cr_glyphs[n_cr_glyphs].x = x;
			cr_glyphs[n_cr_glyphs].y = y;
			n_cr_glyphs++;
			break;
		}
	}
	if (n_cr_glyphs) {
		cairo_set_scaled_font (draw->cr, last_scaled_font);
		cairo_show_glyphs (draw->cr,
				   cr_glyphs,
				   n_cr_glyphs);
		n_cr_glyphs = 0;
	}
}

void
_vte_draw_text (struct _vte_draw *draw,
	       struct _vte_draw_text_request *requests, gsize n_requests,
                uint32_t attr,
	       vte::color::rgb const* color, double alpha, guint style)
{
        g_assert(draw->cr);

	if (_vte_debug_on (VTE_DEBUG_DRAW)) {
		GString *string = g_string_new ("");
		gchar *str;
		gsize n;
		for (n = 0; n < n_requests; n++) {
			g_string_append_unichar (string, requests[n].c);
		}
		str = g_string_free (string, FALSE);
		g_printerr ("draw_text (\"%s\", len=%" G_GSIZE_FORMAT ", color=(%d,%d,%d,%.3f), %s - %s)\n",
				str, n_requests, color->red, color->green, color->blue, alpha,
				(style & VTE_DRAW_BOLD)   ? "bold"   : "normal",
				(style & VTE_DRAW_ITALIC) ? "italic" : "regular");
		g_free (str);
	}

	_vte_draw_text_internal (draw, requests, n_requests, attr, color, alpha, style);
}

/* The following two functions are unused since commit 154abade902850afb44115cccf8fcac51fc082f0,
 * but let's keep them for now since they may become used again.
 */
gboolean
_vte_draw_has_char (struct _vte_draw *draw, vteunistr c, guint style)
{
	struct unistr_info *uinfo;

	_vte_debug_print (VTE_DEBUG_DRAW, "draw_has_char ('0x%04X', %s - %s)\n", c,
				(style & VTE_DRAW_BOLD)   ? "bold"   : "normal",
				(style & VTE_DRAW_ITALIC) ? "italic" : "regular");

	g_return_val_if_fail (draw->fonts[VTE_DRAW_NORMAL] != NULL, FALSE);

	uinfo = font_info_get_unistr_info (draw->fonts[style], c);
	return !uinfo->has_unknown_chars;
}

gboolean
_vte_draw_char (struct _vte_draw *draw,
	       struct _vte_draw_text_request *request,
                uint32_t attr,
	       vte::color::rgb const* color, double alpha, guint style)
{
	gboolean has_char;

	_vte_debug_print (VTE_DEBUG_DRAW,
			"draw_char ('%c', color=(%d,%d,%d,%.3f), %s, %s)\n",
			request->c,
			color->red, color->green, color->blue,
			alpha,
			(style & VTE_DRAW_BOLD)   ? "bold"   : "normal",
			(style & VTE_DRAW_ITALIC) ? "italic" : "regular");

	has_char =_vte_draw_has_char (draw, request->c, style);
	if (has_char)
		_vte_draw_text (draw, request, 1, attr, color, alpha, style);

	return has_char;
}

void
_vte_draw_draw_rectangle (struct _vte_draw *draw,
			 gint x, gint y, gint width, gint height,
			 vte::color::rgb const* color, double alpha)
{
        g_assert(draw->cr);

	_vte_debug_print (VTE_DEBUG_DRAW,
			"draw_rectangle (%d, %d, %d, %d, color=(%d,%d,%d,%.3f))\n",
			x,y,width,height,
			color->red, color->green, color->blue,
			alpha);

	cairo_set_operator (draw->cr, CAIRO_OPERATOR_OVER);
	cairo_rectangle (draw->cr, x+VTE_LINE_WIDTH/2., y+VTE_LINE_WIDTH/2., width-VTE_LINE_WIDTH, height-VTE_LINE_WIDTH);
	_vte_draw_set_source_color_alpha (draw, color, alpha);
	cairo_set_line_width (draw->cr, VTE_LINE_WIDTH);
	cairo_stroke (draw->cr);
}

void
_vte_draw_fill_rectangle (struct _vte_draw *draw,
			 gint x, gint y, gint width, gint height,
			 vte::color::rgb const* color, double alpha)
{
        g_assert(draw->cr);

	_vte_debug_print (VTE_DEBUG_DRAW,
			"draw_fill_rectangle (%d, %d, %d, %d, color=(%d,%d,%d,%.3f))\n",
			x,y,width,height,
			color->red, color->green, color->blue,
			alpha);

	cairo_set_operator (draw->cr, CAIRO_OPERATOR_OVER);
	cairo_rectangle (draw->cr, x, y, width, height);
	_vte_draw_set_source_color_alpha (draw, color, alpha);
	cairo_fill (draw->cr);
}


void
_vte_draw_draw_line(struct _vte_draw *draw,
                    gint x, gint y, gint xp, gint yp,
                    int line_width,
                    vte::color::rgb const *color, double alpha)
{
	_vte_draw_fill_rectangle(draw,
                                 x, y,
                                 MAX(line_width, xp - x + 1), MAX(line_width, yp - y + 1),
                                 color, alpha);
}

static inline double
_vte_draw_get_undercurl_rad(gint width)
{
        return width / 2. / sqrt(2);
}

static inline double
_vte_draw_get_undercurl_arc_height(gint width)
{
        return _vte_draw_get_undercurl_rad(width) * (1. - sqrt(2) / 2.);
}

double
_vte_draw_get_undercurl_height(gint width, double line_width)
{
        return 2. * _vte_draw_get_undercurl_arc_height(width) + line_width;
}

void
_vte_draw_draw_undercurl(struct _vte_draw *draw,
                         gint x, double y,
                         double line_width,
                         gint count,
                         vte::color::rgb const *color, double alpha)
{
        /* The end of the curly line slightly overflows to the next cell, so the canvas
         * caching the rendered look has to be wider not to chop this off. */
        gint x_padding = line_width + 1;  /* ceil, kind of */

        gint surface_top = y;  /* floor */

        g_assert(draw->cr);

        _vte_debug_print (VTE_DEBUG_DRAW,
                        "draw_undercurl (x=%d, y=%f, count=%d, color=(%d,%d,%d,%.3f))\n",
                        x, y, count,
                        color->red, color->green, color->blue,
                        alpha);

        if (G_UNLIKELY (draw->undercurl_surface == NULL)) {
                /* Cache the undercurl's look. The design assumes that until the cached look is
                 * invalidated (the font is changed), this method is always called with the "y"
                 * parameter having the same fractional part, and the same "line_width" parameter.
                 * For caching, only the fractional part of "y" is used. */
                cairo_t *undercurl_cr;

                double rad = _vte_draw_get_undercurl_rad(draw->cell_width);
                double y_bottom = y + _vte_draw_get_undercurl_height(draw->cell_width, line_width);
                double y_center = (y + y_bottom) / 2.;
                gint surface_bottom = y_bottom + 1;  /* ceil, kind of */

                _vte_debug_print (VTE_DEBUG_DRAW,
                                  "caching undercurl shape\n");

                /* Add a line_width of margin horizontally on both sides, for nice antialias overflowing. */
                draw->undercurl_surface = cairo_surface_create_similar (cairo_get_target (draw->cr),
                                                                        CAIRO_CONTENT_ALPHA,
                                                                        draw->cell_width + 2 * x_padding,
                                                                        surface_bottom - surface_top);
                undercurl_cr = cairo_create (draw->undercurl_surface);
                cairo_set_operator (undercurl_cr, CAIRO_OPERATOR_OVER);
                /* First quarter circle, similar to the left half of the tilde symbol. */
                cairo_arc (undercurl_cr, x_padding + draw->cell_width / 4., y_center - surface_top + draw->cell_width / 4., rad, M_PI * 5 / 4, M_PI * 7 / 4);
                /* Second quarter circle, similar to the right half of the tilde symbol. */
                cairo_arc_negative (undercurl_cr, x_padding + draw->cell_width * 3 / 4., y_center - surface_top - draw->cell_width / 4., rad, M_PI * 3 / 4, M_PI / 4);
                cairo_set_line_width (undercurl_cr, line_width);
                cairo_stroke (undercurl_cr);
                cairo_destroy (undercurl_cr);
        }

        /* Paint the cached look of the undercurl using the desired look.
         * The cached look takes the fractional part of "y" into account,
         * here we only offset by its integer part. */
        cairo_save (draw->cr);
        cairo_set_operator (draw->cr, CAIRO_OPERATOR_OVER);
        _vte_draw_set_source_color_alpha (draw, color, alpha);
        for (int i = 0; i < count; i++) {
                cairo_mask_surface (draw->cr, draw->undercurl_surface, x - x_padding + i * draw->cell_width, surface_top);
        }
        cairo_restore (draw->cr);
}
