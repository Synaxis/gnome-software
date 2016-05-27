/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015-2016 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <gnome-software.h>

#include "gs-appstream.h"

#define	GS_APPSTREAM_MAX_SCREENSHOTS	5

static void
gs_refine_item_pixbuf (GsPlugin *plugin, GsApp *app, AsApp *item)
{
	AsIcon *icon;
	g_autoptr(GError) error = NULL;

	icon = as_app_get_icon_default (item);
	switch (as_icon_get_kind (icon)) {
	case AS_ICON_KIND_REMOTE:
		if (as_icon_get_filename (icon) == NULL) {
			g_autofree gchar *fn = NULL;
			g_autofree gchar *cachedir = NULL;
			fn = gs_utils_get_cache_filename ("icons",
							  as_icon_get_name (icon),
							  GS_UTILS_CACHE_FLAG_WRITEABLE,
							  NULL);
			as_icon_set_filename (icon, fn);
			cachedir = g_path_get_basename (fn);
			as_icon_set_prefix (icon, cachedir);
		}
		gs_app_set_icon (app, icon);
		break;
	case AS_ICON_KIND_STOCK:
		gs_app_set_icon (app, icon);
		break;
	case AS_ICON_KIND_LOCAL:
		/* does not exist, so try to find using the icon theme */
		if (as_icon_get_kind (icon) == AS_ICON_KIND_LOCAL &&
		    as_icon_get_filename (icon) == NULL) {
			g_debug ("converting missing LOCAL icon %s to STOCK",
				 as_icon_get_name (icon));
			as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
		}
		gs_app_set_icon (app, icon);
		break;
	case AS_ICON_KIND_CACHED:
		if (gs_plugin_get_scale (plugin) == 2)
			icon = as_app_get_icon_for_size (item, 128, 128);
		if (icon == NULL)
			icon = as_app_get_icon_for_size (item, 64, 64);
		if (icon == NULL) {
			g_warning ("failed to find cached icon %s",
				   as_app_get_id (item));
			return;
		}
		gs_app_set_icon (app, icon);
		break;
	default:
		g_warning ("icon kind unknown for %s", as_app_get_id (item));
		break;
	}
}

static void
gs_appstream_refine_add_addons (GsPlugin *plugin, GsApp *app, AsApp *item)
{
	GPtrArray *addons;
	guint i;

	addons = as_app_get_addons (item);
	if (addons == NULL)
		return;

	for (i = 0; i < addons->len; i++) {
		AsApp *as_addon = g_ptr_array_index (addons, i);
		g_autoptr(GError) error = NULL;
		g_autoptr(GsApp) addon = NULL;

		addon = gs_app_new (as_app_get_id (as_addon));

		/* add all the data we can */
		if (!gs_appstream_refine_app (plugin, addon, as_addon, &error)) {
			g_warning ("failed to refine addon: %s", error->message);
			continue;
		}
		gs_app_add_addon (app, addon);
	}
}

static void
gs_appstream_refine_add_screenshots (GsApp *app, AsApp *item)
{
	AsScreenshot *ss;
	GPtrArray *images_as;
	GPtrArray *screenshots_as;
	guint i;

	/* do we have any to add */
	screenshots_as = as_app_get_screenshots (item);
	if (screenshots_as->len == 0)
		return;

	/* does the app already have some */
	gs_app_add_kudo (app, GS_APP_KUDO_HAS_SCREENSHOTS);
	if (gs_app_get_screenshots(app)->len > 0)
		return;

	/* add any we know */
	for (i = 0; i < screenshots_as->len &&
		    i < GS_APPSTREAM_MAX_SCREENSHOTS; i++) {
		ss = g_ptr_array_index (screenshots_as, i);
		images_as = as_screenshot_get_images (ss);
		if (images_as->len == 0)
			continue;
		if (as_screenshot_get_kind (ss) == AS_SCREENSHOT_KIND_UNKNOWN)
			continue;
		gs_app_add_screenshot (app, ss);
	}
}

static gboolean
gs_appstream_is_recent_release (AsApp *app)
{
	AsRelease *release;
	GPtrArray *releases;
	guint secs;

	/* get newest release */
	releases = as_app_get_releases (app);
	if (releases->len == 0)
		return FALSE;
	release = g_ptr_array_index (releases, 0);

	/* is last build less than one year ago? */
	secs = (g_get_real_time () / G_USEC_PER_SEC) -
		as_release_get_timestamp (release);
	return secs / (60 * 60 * 24) < 365;
}

static gboolean
gs_appstream_are_screenshots_perfect (AsApp *app)
{
	AsImage *image;
	AsScreenshot *screenshot;
	GPtrArray *screenshots;
	guint height;
	guint i;
	guint width;

	screenshots = as_app_get_screenshots (app);
	if (screenshots->len == 0)
		return FALSE;
	for (i = 0; i < screenshots->len; i++) {

		/* get the source image as the thumbs will be resized & padded */
		screenshot = g_ptr_array_index (screenshots, i);
		image = as_screenshot_get_source (screenshot);
		if (image == NULL)
			return FALSE;

		width = as_image_get_width (image);
		height = as_image_get_height (image);

		/* too small */
		if (width < AS_IMAGE_LARGE_WIDTH || height < AS_IMAGE_LARGE_HEIGHT)
			return FALSE;

		/* too large */
		if (width > AS_IMAGE_LARGE_WIDTH * 2 || height > AS_IMAGE_LARGE_HEIGHT * 2)
			return FALSE;

		/* not 16:9 */
		if ((width / 16) * 9 != height)
			return FALSE;
	}
	return TRUE;
}

static void
gs_appstream_copy_metadata (GsApp *app, AsApp *item)
{
	GHashTable *hash;
	GList *l;
	g_autoptr(GList) keys = NULL;

	hash = as_app_get_metadata (item);
	keys = g_hash_table_get_keys (hash);
	for (l = keys; l != NULL; l = l->next) {
		const gchar *key = l->data;
		const gchar *value = g_hash_table_lookup (hash, key);
		if (gs_app_get_metadata_item (app, key) != NULL)
			continue;
		gs_app_set_metadata (app, key, value);
	}
}

GsApp *
gs_appstream_create_runtime (GsPlugin *plugin,
			     GsApp *parent,
			     const gchar *runtime)
{
	const gchar *id_parent;
	g_autofree gchar *id = NULL;
	g_autofree gchar *source = NULL;
	g_auto(GStrv) id_split = NULL;
	g_auto(GStrv) runtime_split = NULL;
	g_autoptr(GsApp) app = NULL;

	/* get the name/arch/branch */
	runtime_split = g_strsplit (runtime, "/", -1);
	if (g_strv_length (runtime_split) != 3)
		return NULL;

	/* find the parent app ID prefix */
	id_parent = gs_app_get_id (parent);
	if (id_parent == NULL)
		return NULL;
	id_split = g_strsplit (id_parent, ":", 2);
	if (g_strv_length (id_split) == 2) {
		id = g_strdup_printf ("%s:%s.runtime",
				      id_split[0],
				      runtime_split[0]);
	} else {
		id = g_strdup_printf ("%s.runtime", runtime_split[0]);
	}

	/* search in the cache */
	app = gs_plugin_cache_lookup (plugin, id);
	if (app != NULL)
		return g_steal_pointer (&app);

	/* create the complete GsApp from the single string */
	app = gs_app_new (id);
	source = g_strdup_printf ("runtime/%s", runtime);
	gs_app_add_source (app, source);
	gs_app_set_kind (app, AS_APP_KIND_RUNTIME);
	gs_app_set_version (app, runtime_split[2]);

	/* save in the cache */
	gs_plugin_cache_add (plugin, id, app);

	return g_steal_pointer (&app);
}

static void
gs_refine_item_management_plugin (GsPlugin *plugin, GsApp *app, AsApp *item)
{
	GPtrArray *bundles;
	const gchar *management_plugin = NULL;
	const gchar *runtime = NULL;
	guint i;

	/* allow override */
	management_plugin = as_app_get_metadata_item (item, "GnomeSoftware::Plugin");
	if (management_plugin != NULL) {
		gs_app_set_management_plugin (app, management_plugin);
		return;
	}

	/* find the default bundle kind */
	bundles = as_app_get_bundles (item);
	for (i = 0; i < bundles->len; i++) {
		AsBundle *bundle = g_ptr_array_index (bundles, i);
		AsBundleKind kind = as_bundle_get_kind (bundle);

		/* common to all bundle formats */
		gs_app_set_management_plugin (app, as_bundle_kind_to_string (kind));
		gs_app_add_source (app, as_bundle_get_id (bundle));

		/* automatically add runtime */
		if (kind == AS_BUNDLE_KIND_FLATPAK) {
			runtime = as_bundle_get_runtime (bundle);
			if (runtime != NULL) {
				g_autoptr(GsApp) app2 = NULL;
				app2 = gs_appstream_create_runtime (plugin, app, runtime);
				if (app2 != NULL) {
					g_debug ("runtime for %s is %s",
						 gs_app_get_id (app), runtime);
					gs_app_set_runtime (app, app2);
				}
			}
			break;
		}
	}
}

static gboolean
gs_appstream_refine_app_updates (GsPlugin *plugin,
				 GsApp *app,
				 AsApp *item,
				 GError **error)
{
	AsRelease *rel;
	AsUrgencyKind urgency_best = AS_URGENCY_KIND_UNKNOWN;
	GPtrArray *releases;
	guint i;
	g_autoptr(GPtrArray) updates_list = NULL;

	/* not enough data to make sense */
	if (gs_app_get_version (app) == NULL) {
		g_debug ("no installed version for %s, "
			 "so unable to add AppStream releases",
			 gs_app_get_id (app));
		return TRUE;
	}

	/* make a list of valid updates */
	updates_list = g_ptr_array_new ();
	releases = as_app_get_releases (item);
	for (i = 0; i < releases->len; i++) {
		rel = g_ptr_array_index (releases, i);

		/* is newer than what's installed */
		g_debug ("installed %s update is %s [%i]",
			 gs_app_get_version (app),
			 as_release_get_version (rel),
			 as_release_get_state (rel));
		if (as_utils_vercmp (as_release_get_version (rel),
				     gs_app_get_version (app)) < 0)
			continue;

		/* use the 'worst' urgency, e.g. critical over enhancement */
		if (as_release_get_urgency (rel) > urgency_best)
			urgency_best = as_release_get_urgency (rel);

		/* add updates with a description */
		if (as_release_get_description (rel, NULL) == NULL)
			continue;
		g_ptr_array_add (updates_list, rel);
	}

	/* only set if known */
	if (urgency_best != AS_URGENCY_KIND_UNKNOWN)
		gs_app_set_update_urgency (app, urgency_best);

	/* no prefix on each release */
	if (updates_list->len == 1) {
		g_autofree gchar *desc = NULL;
		rel = g_ptr_array_index (updates_list, 0);
		desc = as_markup_convert (as_release_get_description (rel, NULL),
					  AS_MARKUP_CONVERT_FORMAT_SIMPLE,
					  error);
		if (desc == NULL)
			return FALSE;
		gs_app_set_update_details (app, desc);

	/* get the descriptions with a version prefix */
	} else if (updates_list->len > 1) {
		g_autoptr(GString) update_desc = g_string_new ("");
		for (i = 0; i < updates_list->len; i++) {
			g_autofree gchar *desc = NULL;
			rel = g_ptr_array_index (updates_list, i);
			desc = as_markup_convert (as_release_get_description (rel, NULL),
						  AS_MARKUP_CONVERT_FORMAT_SIMPLE,
						  error);
			if (desc == NULL)
				return FALSE;
			g_string_append_printf (update_desc,
						"<b>Version %s:</b>\n%s\n\n",
						as_release_get_version (rel),
						desc);
		}

		/* remove trailing newlines */
		if (update_desc->len > 2)
			g_string_truncate (update_desc, update_desc->len - 2);
		gs_app_set_update_details (app, update_desc->str);
	}

	/* if there is no already set update version use the newest */
	if (gs_app_get_update_version (app) == NULL) {
		rel = as_app_get_release_default (item);
		if (rel != NULL)
			gs_app_set_update_version (app, as_release_get_version (rel));
	}

	/* success */
	return TRUE;
}

gboolean
gs_appstream_refine_app (GsPlugin *plugin,
			 GsApp *app,
			 AsApp *item,
			 GError **error)
{
	GHashTable *urls;
	GPtrArray *pkgnames;
	GPtrArray *kudos;
	const gchar *tmp;
	guint i;

	/* set the kind to be more precise */
	if (gs_app_get_kind (app) == AS_APP_KIND_UNKNOWN ||
	    gs_app_get_kind (app) == AS_APP_KIND_GENERIC) {
		gs_app_set_kind (app, as_app_get_kind (item));
	}

	/* is installed already */
	if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN) {
		switch (as_app_get_source_kind (item)) {
		case AS_APP_SOURCE_KIND_APPDATA:
		case AS_APP_SOURCE_KIND_DESKTOP:
		case AS_APP_SOURCE_KIND_METAINFO:
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);
			break;
		default:
			break;
		}
	}

	/* types we can never launch */
	switch (gs_app_get_kind (app)) {
	case AS_APP_KIND_ADDON:
	case AS_APP_KIND_CODEC:
	case AS_APP_KIND_FIRMWARE:
	case AS_APP_KIND_FONT:
	case AS_APP_KIND_GENERIC:
	case AS_APP_KIND_INPUT_METHOD:
	case AS_APP_KIND_LOCALIZATION:
	case AS_APP_KIND_OS_UPDATE:
	case AS_APP_KIND_OS_UPGRADE:
	case AS_APP_KIND_RUNTIME:
	case AS_APP_KIND_SOURCE:
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);
		break;
	default:
		break;
	}

	/* set management plugin automatically */
	gs_refine_item_management_plugin (plugin, app, item);

	/* set id */
	if (as_app_get_id (item) != NULL && gs_app_get_id (app) == NULL)
		gs_app_set_id (app, as_app_get_id (item));

	/* set source */
	gs_app_set_metadata (app, "appstream::source-file", as_app_get_source_file (item));

	/* set name */
	tmp = as_app_get_name (item, NULL);
	if (tmp != NULL) {
		if (g_str_has_prefix (tmp, "(Nightly) ")) {
			tmp += 10;
			if (gs_app_get_metadata_item (app, "X-XdgApp-Tags") == NULL)
				gs_app_set_metadata (app, "X-XdgApp-Tags", "nightly");
		}
		gs_app_set_name (app, GS_APP_QUALITY_HIGHEST, tmp);
	}

	/* set summary */
	tmp = as_app_get_comment (item, NULL);
	if (tmp != NULL) {
		gs_app_set_summary (app, GS_APP_QUALITY_HIGHEST, tmp);
	}

	/* add urls */
	urls = as_app_get_urls (item);
	if (g_hash_table_size (urls) > 0 &&
	    gs_app_get_url (app, AS_URL_KIND_HOMEPAGE) == NULL) {
		GList *l;
		g_autoptr(GList) keys = NULL;
		keys = g_hash_table_get_keys (urls);
		for (l = keys; l != NULL; l = l->next) {
			gs_app_set_url (app,
					as_url_kind_from_string (l->data),
					g_hash_table_lookup (urls, l->data));
		}
	}

	/* set license */
	if (as_app_get_project_license (item) != NULL && gs_app_get_license (app) == NULL)
		gs_app_set_license (app,
				    GS_APP_QUALITY_HIGHEST,
				    as_app_get_project_license (item));

	/* set keywords */
	if (as_app_get_keywords (item, NULL) != NULL &&
	    gs_app_get_keywords (app) == NULL) {
		gs_app_set_keywords (app, as_app_get_keywords (item, NULL));
		gs_app_add_kudo (app, GS_APP_KUDO_HAS_KEYWORDS);
	}

	/* set origin */
	if (as_app_get_origin (item) != NULL &&
	    gs_app_get_origin (app) == NULL) {
		gs_app_set_origin (app, as_app_get_origin (item));
	}

	/* set description */
	tmp = as_app_get_description (item, NULL);
	if (tmp != NULL) {
		g_autofree gchar *from_xml = NULL;
		from_xml = as_markup_convert_simple (tmp, error);
		if (from_xml == NULL) {
			g_prefix_error (error, "trying to parse '%s': ", tmp);
			return FALSE;
		}
		gs_app_set_description (app, GS_APP_QUALITY_HIGHEST, from_xml);
	}

	/* set icon */
	if (as_app_get_icon_default (item) != NULL && gs_app_get_pixbuf (app) == NULL)
		gs_refine_item_pixbuf (plugin, app, item);

	/* set categories */
	if (as_app_get_categories (item) != NULL &&
	    gs_app_get_categories (app)->len == 0)
		gs_app_set_categories (app, as_app_get_categories (item));

	/* set project group */
	if (as_app_get_project_group (item) != NULL &&
	    gs_app_get_project_group (app) == NULL)
		gs_app_set_project_group (app, as_app_get_project_group (item));

	/* this is a core application for the desktop and cannot be removed */
	if (as_app_has_compulsory_for_desktop (item, "GNOME") &&
	    gs_app_get_kind (app) == AS_APP_KIND_DESKTOP)
		gs_app_add_quirk (app, AS_APP_QUIRK_COMPULSORY);

	/* set id kind */
	if (gs_app_get_kind (app) == AS_APP_KIND_UNKNOWN)
		gs_app_set_kind (app, as_app_get_kind (item));

	/* copy all the metadata */
	gs_appstream_copy_metadata (app, item);

	/* set package names */
	pkgnames = as_app_get_pkgnames (item);
	if (pkgnames->len > 0 && gs_app_get_sources(app)->len == 0)
		gs_app_set_sources (app, pkgnames);

	/* set addons */
	gs_appstream_refine_add_addons (plugin, app, item);

	/* set screenshots */
	gs_appstream_refine_add_screenshots (app, item);

	/* are the screenshots perfect */
	if (gs_appstream_are_screenshots_perfect (item))
		gs_app_add_kudo (app, GS_APP_KUDO_PERFECT_SCREENSHOTS);

	/* was this application released recently */
	if (gs_appstream_is_recent_release (item))
		gs_app_add_kudo (app, GS_APP_KUDO_RECENT_RELEASE);

	/* add kudos */
	if (as_app_get_language (item, gs_plugin_get_locale (plugin)) > 50)
		gs_app_add_kudo (app, GS_APP_KUDO_MY_LANGUAGE);

	/* add a kudo to featured and popular apps */
	if (as_app_has_kudo (item, "GnomeSoftware::popular"))
		gs_app_add_kudo (app, GS_APP_KUDO_FEATURED_RECOMMENDED);
	if (as_app_has_category (item, "featured"))
		gs_app_add_kudo (app, GS_APP_KUDO_FEATURED_RECOMMENDED);

	/* add new-style kudos */
	kudos = as_app_get_kudos (item);
	for (i = 0; i < kudos->len; i++) {
		tmp = g_ptr_array_index (kudos, i);
		switch (as_kudo_kind_from_string (tmp)) {
		case AS_KUDO_KIND_SEARCH_PROVIDER:
			gs_app_add_kudo (app, GS_APP_KUDO_SEARCH_PROVIDER);
			break;
		case AS_KUDO_KIND_USER_DOCS:
			gs_app_add_kudo (app, GS_APP_KUDO_INSTALLS_USER_DOCS);
			break;
		case AS_KUDO_KIND_APP_MENU:
			gs_app_add_kudo (app, GS_APP_KUDO_USES_APP_MENU);
			break;
		case AS_KUDO_KIND_MODERN_TOOLKIT:
			gs_app_add_kudo (app, GS_APP_KUDO_MODERN_TOOLKIT);
			break;
		case AS_KUDO_KIND_NOTIFICATIONS:
			gs_app_add_kudo (app, GS_APP_KUDO_USES_NOTIFICATIONS);
			break;
		case AS_KUDO_KIND_HIGH_CONTRAST:
			gs_app_add_kudo (app, GS_APP_KUDO_HIGH_CONTRAST);
			break;
		case AS_KUDO_KIND_HI_DPI_ICON:
			gs_app_add_kudo (app, GS_APP_KUDO_HI_DPI_ICON);
			break;
		default:
			break;
		}
	}

	/* is there any update information */
	if (!gs_appstream_refine_app_updates (plugin, app, item, error))
		return FALSE;

	return TRUE;
}
