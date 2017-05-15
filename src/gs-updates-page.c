/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2016 Richard Hughes <richard@hughsie.com>
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

#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gs-shell.h"
#include "gs-updates-page.h"
#include "gs-common.h"
#include "gs-app-row.h"
#include "gs-plugin-private.h"
#include "gs-removal-dialog.h"
#include "gs-update-dialog.h"
#include "gs-update-list.h"
#include "gs-update-monitor.h"
#include "gs-upgrade-banner.h"
#include "gs-application.h"

#ifdef HAVE_GNOME_DESKTOP
#include <gdesktop-enums.h>
#endif

#include <langinfo.h>

typedef enum {
	GS_UPDATES_PAGE_FLAG_NONE		= 0,
	GS_UPDATES_PAGE_FLAG_HAS_UPDATES	= 1 << 0,
	GS_UPDATES_PAGE_FLAG_HAS_UPGRADES	= 1 << 1,
	GS_UPDATES_PAGE_FLAG_LAST
} GsUpdatesPageFlags;

typedef enum {
	GS_UPDATES_PAGE_STATE_STARTUP,
	GS_UPDATES_PAGE_STATE_ACTION_REFRESH,
	GS_UPDATES_PAGE_STATE_ACTION_GET_UPDATES,
	GS_UPDATES_PAGE_STATE_MANAGED,
	GS_UPDATES_PAGE_STATE_IDLE,
	GS_UPDATES_PAGE_STATE_FAILED,
	GS_UPDATES_PAGE_STATE_LAST,
} GsUpdatesPageState;

struct _GsUpdatesPage
{
	GsPage			 parent_instance;

	GsPluginLoader		*plugin_loader;
	GtkBuilder		*builder;
	GCancellable		*cancellable;
	GCancellable		*cancellable_refresh;
	GCancellable		*cancellable_upgrade_download;
	GSettings		*settings;
	GSettings		*desktop_settings;
	gboolean		 cache_valid;
	guint			 action_cnt;
	gboolean		 all_updates_are_live;
	gboolean		 any_require_reboot;
	GsShell			*shell;
	GsPluginStatus		 last_status;
	GsUpdatesPageState	 state;
	GsUpdatesPageFlags	 result_flags;
	GtkWidget		*button_refresh;
	GtkWidget		*button_update_all;
	GtkWidget		*header_spinner_start;
	GtkWidget		*header_start_box;
	GtkWidget		*header_end_box;
	gboolean		 has_agreed_to_mobile_data;
	gboolean		 ampm_available;

	GtkWidget		*updates_box;
	GtkWidget		*button_updates_mobile;
	GtkWidget		*button_updates_offline;
	GtkWidget		*label_updates_failed;
	GtkWidget		*label_updates_last_checked;
	GtkWidget		*label_updates_spinner;
	GtkWidget		*list_box_updates;
	GtkWidget		*scrolledwindow_updates;
	GtkWidget		*spinner_updates;
	GtkWidget		*stack_updates;
	GtkWidget		*upgrade_banner;
	GtkWidget		*box_end_of_life;
	GtkWidget		*label_end_of_life;
};

enum {
	COLUMN_UPDATE_APP,
	COLUMN_UPDATE_NAME,
	COLUMN_UPDATE_VERSION,
	COLUMN_UPDATE_LAST
};

G_DEFINE_TYPE (GsUpdatesPage, gs_updates_page, GS_TYPE_PAGE)

static void
gs_updates_page_set_flag (GsUpdatesPage *self, GsUpdatesPageFlags flag)
{
	self->result_flags |= flag;
}

static void
gs_updates_page_clear_flag (GsUpdatesPage *self, GsUpdatesPageFlags flag)
{
	self->result_flags &= ~flag;
}

static const gchar *
gs_updates_page_state_to_string (GsUpdatesPageState state)
{
	if (state == GS_UPDATES_PAGE_STATE_STARTUP)
		return "startup";
	if (state == GS_UPDATES_PAGE_STATE_ACTION_REFRESH)
		return "action-refresh";
	if (state == GS_UPDATES_PAGE_STATE_ACTION_GET_UPDATES)
		return "action-get-updates";
	if (state == GS_UPDATES_PAGE_STATE_MANAGED)
		return "managed";
	if (state == GS_UPDATES_PAGE_STATE_IDLE)
		return "idle";
	if (state == GS_UPDATES_PAGE_STATE_FAILED)
		return "failed";
	return NULL;
}

static void
gs_updates_page_invalidate (GsUpdatesPage *self)
{
	self->cache_valid = FALSE;
}

static GDateTime *
time_next_midnight (void)
{
	GDateTime *next_midnight;
	GTimeSpan since_midnight;
	g_autoptr(GDateTime) now = NULL;

	now = g_date_time_new_now_local ();
	since_midnight = g_date_time_get_hour (now) * G_TIME_SPAN_HOUR +
			 g_date_time_get_minute (now) * G_TIME_SPAN_MINUTE +
			 g_date_time_get_second (now) * G_TIME_SPAN_SECOND +
			 g_date_time_get_microsecond (now);
	next_midnight = g_date_time_add (now, G_TIME_SPAN_DAY - since_midnight);

	return next_midnight;
}

static gchar *
gs_updates_page_last_checked_time_string (GsUpdatesPage *self)
{
#ifdef HAVE_GNOME_DESKTOP
	GDesktopClockFormat clock_format;
#endif
	const gchar *format_string;
	gchar *time_string;
	gboolean use_24h_time = FALSE;
	gint64 tmp;
	gint days_ago;
	g_autoptr(GDateTime) last_checked = NULL;
	g_autoptr(GDateTime) midnight = NULL;

	g_settings_get (self->settings, "check-timestamp", "x", &tmp);
	if (tmp == 0)
		return NULL;
	last_checked = g_date_time_new_from_unix_local (tmp);

	midnight = time_next_midnight ();
	days_ago = (gint) (g_date_time_difference (midnight, last_checked) / G_TIME_SPAN_DAY);

#ifdef HAVE_GNOME_DESKTOP
	clock_format = g_settings_get_enum (self->desktop_settings, "clock-format");
	use_24h_time = (clock_format == G_DESKTOP_CLOCK_FORMAT_24H || self->ampm_available == FALSE);
#endif

	if (days_ago < 1) { // today
		if (use_24h_time) {
			/* TRANSLATORS: Time in 24h format */
			format_string = _("%R");
		} else {
			/* TRANSLATORS: Time in 12h format */
			format_string = _("%l:%M %p");
		}
	} else if (days_ago < 2) { // yesterday
		if (use_24h_time) {
			/* TRANSLATORS: This is the word "Yesterday" followed by a
			   time string in 24h format. i.e. "Yesterday, 14:30" */
			format_string = _("Yesterday, %R");
		} else {
			/* TRANSLATORS: This is the word "Yesterday" followed by a
			   time string in 12h format. i.e. "Yesterday, 2:30 PM" */
			format_string = _("Yesterday, %l:%M %p");
		}
	} else if (days_ago < 3) {
		format_string = _("Two days ago");
	} else if (days_ago < 4) {
		format_string = _("Three days ago");
	} else if (days_ago < 5) {
		format_string = _("Four days ago");
	} else if (days_ago < 6) {
		format_string = _("Five days ago");
	} else if (days_ago < 7) {
		format_string = _("Six days ago");
	} else if (days_ago < 8) {
		format_string = _("One week ago");
	} else if (days_ago < 15) {
		format_string = _("Two weeks ago");
	} else {
		/* TRANSLATORS: This is the date string with: day number, month name, year.
		   i.e. "25 May 2012" */
		format_string = _("%e %B %Y");
	}

	time_string = g_date_time_format (last_checked, format_string);

	return time_string;
}

static const gchar *
gs_updates_page_get_state_string (GsPluginStatus status)
{
	if (status == GS_PLUGIN_STATUS_DOWNLOADING) {
		/* TRANSLATORS: the updates are being downloaded */
		return _("Downloading new updates…");
	}

	/* TRANSLATORS: the update panel is doing *something* vague */
	return _("Looking for new updates…");
}

static void
gs_updates_page_update_ui_state (GsUpdatesPage *self)
{
	GsUpdateList *update_list;
	gboolean allow_mobile_refresh = TRUE;
	g_autofree gchar *checked_str = NULL;
	g_autofree gchar *spinner_str = NULL;

	if (gs_shell_get_mode (self->shell) != GS_SHELL_MODE_UPDATES)
		return;

	/* spinners */
	switch (self->state) {
	case GS_UPDATES_PAGE_STATE_STARTUP:
	case GS_UPDATES_PAGE_STATE_ACTION_GET_UPDATES:
	case GS_UPDATES_PAGE_STATE_ACTION_REFRESH:
		/* if we have updates, avoid clearing the page with a spinner */
		if (self->result_flags != GS_UPDATES_PAGE_FLAG_NONE) {
			gs_stop_spinner (GTK_SPINNER (self->spinner_updates));
			gtk_spinner_start (GTK_SPINNER (self->header_spinner_start));
			gtk_widget_show (self->header_spinner_start);
		} else {
			gs_start_spinner (GTK_SPINNER (self->spinner_updates));
		}
		break;
	default:
		gs_stop_spinner (GTK_SPINNER (self->spinner_updates));
		gtk_spinner_stop (GTK_SPINNER (self->header_spinner_start));
		gtk_widget_hide (self->header_spinner_start);
		break;
	}

	/* spinner text */
	switch (self->state) {
	case GS_UPDATES_PAGE_STATE_STARTUP:
		spinner_str = g_strdup_printf ("%s\n%s",
				       /* TRANSLATORS: the updates panel is starting up */
				       _("Setting up updates…"),
				       _("(This could take a while)"));
		gtk_label_set_label (GTK_LABEL (self->label_updates_spinner), spinner_str);
		break;
	case GS_UPDATES_PAGE_STATE_ACTION_REFRESH:
		spinner_str = g_strdup_printf ("%s\n%s",
				       gs_updates_page_get_state_string (self->last_status),
				       /* TRANSLATORS: the updates panel is starting up */
				       _("(This could take a while)"));
		gtk_label_set_label (GTK_LABEL (self->label_updates_spinner), spinner_str);
		break;
	default:
		break;
	}

	/* headerbar refresh icon */
	switch (self->state) {
	case GS_UPDATES_PAGE_STATE_ACTION_REFRESH:
	case GS_UPDATES_PAGE_STATE_ACTION_GET_UPDATES:
		gtk_image_set_from_icon_name (GTK_IMAGE (gtk_button_get_image (GTK_BUTTON (self->button_refresh))),
					      "media-playback-stop-symbolic", GTK_ICON_SIZE_MENU);
		gtk_widget_show (self->button_refresh);
		break;
	case GS_UPDATES_PAGE_STATE_STARTUP:
	case GS_UPDATES_PAGE_STATE_MANAGED:
		gtk_widget_hide (self->button_refresh);
		break;
	default:
		gtk_image_set_from_icon_name (GTK_IMAGE (gtk_button_get_image (GTK_BUTTON (self->button_refresh))),
					      "view-refresh-symbolic", GTK_ICON_SIZE_MENU);
		if (self->result_flags != GS_UPDATES_PAGE_FLAG_NONE) {
			gtk_widget_show (self->button_refresh);
		} else {
			if (gs_plugin_loader_get_network_metered (self->plugin_loader) &&
			    !self->has_agreed_to_mobile_data)
				allow_mobile_refresh = FALSE;
			gtk_widget_set_visible (self->button_refresh, allow_mobile_refresh);
		}
		break;
	}
	gtk_widget_set_sensitive (self->button_refresh,
				  gs_plugin_loader_get_network_available (self->plugin_loader));

	/* headerbar update button */
	update_list = GS_UPDATE_LIST (self->list_box_updates);
	gs_update_list_set_force_headers (update_list,
			self->result_flags & GS_UPDATES_PAGE_FLAG_HAS_UPGRADES);
	gtk_widget_set_visible (self->button_update_all,
				!gs_update_list_has_headers (update_list));

	/* stack */
	switch (self->state) {
	case GS_UPDATES_PAGE_STATE_MANAGED:
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "managed");
		break;
	case GS_UPDATES_PAGE_STATE_FAILED:
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "failed");
		break;
	case GS_UPDATES_PAGE_STATE_ACTION_GET_UPDATES:
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates),
						  "spinner");
		break;
	case GS_UPDATES_PAGE_STATE_ACTION_REFRESH:
		if (self->result_flags != GS_UPDATES_PAGE_FLAG_NONE) {
			gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "view");
		} else {
			gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "spinner");
		}
		break;
	case GS_UPDATES_PAGE_STATE_STARTUP:
	case GS_UPDATES_PAGE_STATE_IDLE:

		/* if have updates, just show the view, otherwise show network */
		if (self->result_flags != GS_UPDATES_PAGE_FLAG_NONE) {
			gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "view");
			break;
		}

		/* check we have a "free" network connection */
		if (gs_plugin_loader_get_network_available (self->plugin_loader) &&
		    !gs_plugin_loader_get_network_metered (self->plugin_loader)) {
			gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "uptodate");
			break;
		}

		/* expensive network connection */
		if (gs_plugin_loader_get_network_metered (self->plugin_loader)) {
			if (self->has_agreed_to_mobile_data) {
				gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "uptodate");
			} else {
				gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "mobile");
			}
			break;
		}

		/* no network connection */
		gtk_stack_set_visible_child_name (GTK_STACK (self->stack_updates), "offline");
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	/* any updates? */
	gtk_widget_set_visible (self->updates_box,
				self->result_flags & GS_UPDATES_PAGE_FLAG_HAS_UPDATES);

	/* last checked label */
	if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (self->stack_updates)), "uptodate") == 0) {
		checked_str = gs_updates_page_last_checked_time_string (self);
		if (checked_str != NULL) {
			g_autofree gchar *last_checked = NULL;

			/* TRANSLATORS: This is the time when we last checked for updates */
			last_checked = g_strdup_printf (_("Last checked: %s"), checked_str);
			gtk_label_set_label (GTK_LABEL (self->label_updates_last_checked),
					     last_checked);
		}
		gtk_widget_set_visible (self->label_updates_last_checked, checked_str != NULL);
	}
}

static void
gs_updates_page_set_state (GsUpdatesPage *self, GsUpdatesPageState state)
{
	g_debug ("setting state from %s to %s (has-update:%i, has-upgrade:%i)",
		 gs_updates_page_state_to_string (self->state),
		 gs_updates_page_state_to_string (state),
		 (self->result_flags & GS_UPDATES_PAGE_FLAG_HAS_UPDATES) > 0,
		 (self->result_flags & GS_UPDATES_PAGE_FLAG_HAS_UPGRADES) > 0);
	self->state = state;
	gs_updates_page_update_ui_state (self);
}

static void
gs_updates_page_decrement_refresh_count (GsUpdatesPage *self)
{
	/* every job increcements this */
	if (self->action_cnt == 0) {
		g_warning ("action_cnt already zero!");
		return;
	}
	if (--self->action_cnt > 0)
		return;

	/* all done */
	gs_updates_page_set_state (self, GS_UPDATES_PAGE_STATE_IDLE);

	/* seems a good place */
	gs_shell_profile_dump (self->shell);
}

static void
gs_updates_page_network_available_notify_cb (GsPluginLoader *plugin_loader,
                                             GParamSpec *pspec,
                                             GsUpdatesPage *self)
{
	gs_updates_page_update_ui_state (self);
}

static void
gs_updates_page_get_updates_cb (GsPluginLoader *plugin_loader,
                                GAsyncResult *res,
                                GsUpdatesPage *self)
{
	guint i;
	GtkWidget *widget;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	self->cache_valid = TRUE;

	/* get the results */
	list = gs_plugin_loader_get_updates_finish (plugin_loader, res, &error);
	if (list == NULL) {
		gs_updates_page_clear_flag (self, GS_UPDATES_PAGE_FLAG_HAS_UPDATES);
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("updates-shell: failed to get updates: %s", error->message);
		gs_utils_error_strip_unique_id (error);
		gtk_label_set_label (GTK_LABEL (self->label_updates_failed),
				     error->message);
		gs_updates_page_set_state (self, GS_UPDATES_PAGE_STATE_FAILED);
		widget = GTK_WIDGET (gtk_builder_get_object (self->builder,
							     "button_updates_counter"));
		gtk_widget_hide (widget);
		return;
	}

	/* add the results */
	self->all_updates_are_live = TRUE;
	self->any_require_reboot = FALSE;
	for (i = 0; list != NULL && i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		if (gs_app_get_state (app) != AS_APP_STATE_UPDATABLE_LIVE)
			self->all_updates_are_live = FALSE;
		if (gs_app_has_quirk (app, AS_APP_QUIRK_NEEDS_REBOOT))
			self->any_require_reboot = TRUE;
		gs_update_list_add_app (GS_UPDATE_LIST (self->list_box_updates), app);
	}

	/* change the button as to whether a reboot is required to
	 * apply all the updates */
	if (self->all_updates_are_live) {
		gtk_button_set_label (GTK_BUTTON (self->button_update_all),
				      /* TRANSLATORS: all updates will be installed */
				      _("U_pdate All"));
	} else {
		gtk_button_set_label (GTK_BUTTON (self->button_update_all),
				      /* TRANSLATORS: this is an offline update */
				      _("_Restart & Update"));
	}

	/* update the counter */
	widget = GTK_WIDGET (gtk_builder_get_object (self->builder,
						     "button_updates_counter"));
	if (gs_app_list_length (list) > 0 &&
	    gs_plugin_loader_get_allow_updates (self->plugin_loader)) {
		g_autofree gchar *text = NULL;
		text = g_strdup_printf ("%u", gs_app_list_length (list));
		gtk_label_set_label (GTK_LABEL (widget), text);
		gtk_widget_show (widget);
	} else {
		gtk_widget_hide (widget);
	}

	/* update the tab style */
	if (gs_app_list_length (list) > 0 &&
	    gs_shell_get_mode (self->shell) != GS_SHELL_MODE_UPDATES)
		gtk_style_context_add_class (gtk_widget_get_style_context (widget), "needs-attention");
	else
		gtk_style_context_remove_class (gtk_widget_get_style_context (widget), "needs-attention");

	/* no results */
	if (gs_app_list_length (list) == 0) {
		g_debug ("updates-shell: no updates to show");
		gs_updates_page_clear_flag (self, GS_UPDATES_PAGE_FLAG_HAS_UPDATES);
	} else {
		gs_updates_page_set_flag (self, GS_UPDATES_PAGE_FLAG_HAS_UPDATES);
	}

	/* only when both set */
	gs_updates_page_decrement_refresh_count (self);
}

static void
gs_updates_page_get_upgrades_cb (GObject *source_object,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	GsUpdatesPage *self = GS_UPDATES_PAGE (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get the results */
	list = gs_plugin_loader_get_distro_upgrades_finish (plugin_loader, res, &error);
	if (list == NULL) {
		gs_updates_page_clear_flag (self, GS_UPDATES_PAGE_FLAG_HAS_UPGRADES);
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED)) {
			g_warning ("updates-shell: failed to get upgrades: %s",
				   error->message);
		}
	} else if (gs_app_list_length (list) == 0) {
		g_debug ("updates-shell: no upgrades to show");
		gs_updates_page_clear_flag (self, GS_UPDATES_PAGE_FLAG_HAS_UPGRADES);
		gtk_widget_set_visible (self->upgrade_banner, FALSE);
	} else {
		GsApp *app = gs_app_list_index (list, 0);
		g_debug ("got upgrade %s", gs_app_get_id (app));
		gs_upgrade_banner_set_app (GS_UPGRADE_BANNER (self->upgrade_banner), app);
		gs_updates_page_set_flag (self, GS_UPDATES_PAGE_FLAG_HAS_UPGRADES);
		gtk_widget_set_visible (self->upgrade_banner, TRUE);
	}

	/* only when both set */
	gs_updates_page_decrement_refresh_count (self);
}

static void
gs_updates_page_get_system_finished_cb (GObject *source_object,
					GAsyncResult *res,
					gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	GsUpdatesPage *self = GS_UPDATES_PAGE (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsApp) app = NULL;
	g_autoptr(GString) str = g_string_new (NULL);

	/* get result */
	if (!gs_plugin_loader_app_refine_finish (plugin_loader, res, &error)) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get system: %s", error->message);
		return;
	}

	/* show or hide the end of life notification */
	app = gs_plugin_loader_get_system_app (plugin_loader);
	if (gs_app_get_state (app) != AS_APP_STATE_UNAVAILABLE) {
		gtk_widget_set_visible (self->box_end_of_life, FALSE);
		return;
	}

	/* construct a sufficiently scary message */
	if (gs_app_get_name (app) != NULL && gs_app_get_version (app) != NULL) {
		/* TRANSLATORS:  the first %s is the distro name, e.g. 'Fedora'
		 * and the second %s is the distro version, e.g. '25' */
		g_string_append_printf (str, _("%s %s is no longer supported."),
					gs_app_get_name (app),
					gs_app_get_version (app));
	} else {
		/* TRANSLATORS: OS refers to operating system, e.g. Fedora */
		g_string_append (str, _("Your OS is no longer supported."));
	}
	g_string_append (str, " ");

	/* TRANSLATORS: EOL distros do not get important updates */
	g_string_append (str, _("This means that it does not receive security updates."));
	g_string_append (str, " ");

	/* TRANSLATORS: upgrade refers to a major update, e.g. Fedora 25 to 26 */
	g_string_append (str, _("It is recommended that you upgrade to a more recent version."));

	gtk_label_set_label (GTK_LABEL (self->label_end_of_life), str->str);
	gtk_widget_set_visible (self->box_end_of_life, TRUE);

}

static void
gs_updates_page_load (GsUpdatesPage *self)
{
	guint64 refine_flags;
	g_autoptr(GsApp) app = NULL;

	if (self->action_cnt > 0)
		return;
	gs_update_list_remove_all (GS_UPDATE_LIST (self->list_box_updates));
	refine_flags = GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON |
		       GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS |
		       GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROVENANCE |
		       GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION;
	gs_updates_page_set_state (self, GS_UPDATES_PAGE_STATE_ACTION_GET_UPDATES);
	self->action_cnt++;
	gs_plugin_loader_get_updates_async (self->plugin_loader,
					    refine_flags,
					    GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
					    self->cancellable,
					    (GAsyncReadyCallback) gs_updates_page_get_updates_cb,
					    self);

	/* get the system state */
	app = gs_plugin_loader_get_system_app (self->plugin_loader);
	gs_plugin_loader_app_refine_async (self->plugin_loader, app,
					   GS_PLUGIN_REFINE_FLAGS_DEFAULT,
					   GS_PLUGIN_FAILURE_FLAGS_NONE,
					   self->cancellable,
					   gs_updates_page_get_system_finished_cb,
					   self);

	/* don't refresh every each time */
	if ((self->result_flags & GS_UPDATES_PAGE_FLAG_HAS_UPGRADES) == 0) {
		refine_flags |= GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPGRADE_REMOVED;
		gs_plugin_loader_get_distro_upgrades_async (self->plugin_loader,
							    refine_flags,
							    GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
							    self->cancellable,
							    gs_updates_page_get_upgrades_cb,
							    self);
		self->action_cnt++;
	}
}

static void
gs_updates_page_reload (GsPage *page)
{
	GsUpdatesPage *self = GS_UPDATES_PAGE (page);
	gs_updates_page_invalidate (self);
	gs_updates_page_load (self);
}

static void
gs_updates_page_switch_to (GsPage *page,
                           gboolean scroll_up)
{
	GsUpdatesPage *self = GS_UPDATES_PAGE (page);
	GtkWidget *widget;

	if (gs_shell_get_mode (self->shell) != GS_SHELL_MODE_UPDATES) {
		g_warning ("Called switch_to(updates) when in mode %s",
			   gs_shell_get_mode_string (self->shell));
		return;
	}

	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "buttonbox_main"));
	gtk_widget_show (widget);

	gtk_widget_set_visible (self->button_refresh, TRUE);

	if (scroll_up) {
		GtkAdjustment *adj;
		adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolledwindow_updates));
		gtk_adjustment_set_value (adj, gtk_adjustment_get_lower (adj));
	}

	/* no need to refresh */
	if (self->cache_valid) {
		gs_updates_page_update_ui_state (self);
		return;
	}

	if (self->state == GS_UPDATES_PAGE_STATE_ACTION_GET_UPDATES) {
		gs_updates_page_update_ui_state (self);
		return;
	}
	gs_updates_page_load (self);
}

static void
show_update_details (GsApp *app, GsUpdatesPage *self)
{
	GtkWidget *dialog;
	g_autofree gchar *str = NULL;

	/* debug */
	str = gs_app_to_string (app);
	g_debug ("%s", str);

	dialog = gs_update_dialog_new (self->plugin_loader);
	gs_update_dialog_show_update_details (GS_UPDATE_DIALOG (dialog), app);
	gs_shell_modal_dialog_present (self->shell, GTK_DIALOG (dialog));

	/* just destroy */
	g_signal_connect_swapped (dialog, "response",
				  G_CALLBACK (gtk_widget_destroy), dialog);
}

static void
gs_updates_page_activated_cb (GtkListBox *list_box,
                              GtkListBoxRow *row,
                              GsUpdatesPage *self)
{
	GsApp *app;

	app = gs_app_row_get_app (GS_APP_ROW (row));

	show_update_details (app, self);
}

static void
gs_updates_page_button_clicked_cb (GsUpdateList *update_list,
                                   GsApp *app,
                                   GsUpdatesPage *self)
{
	g_autoptr(GCancellable) cancellable = g_cancellable_new ();
	if (gs_app_get_state (app) != AS_APP_STATE_UPDATABLE_LIVE)
		return;
	g_set_object (&self->cancellable, cancellable);
	gs_page_update_app (GS_PAGE (self), app, self->cancellable);
}

static void
gs_updates_page_refresh_cb (GsPluginLoader *plugin_loader,
                            GAsyncResult *res,
                            GsUpdatesPage *self)
{
	gboolean ret;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;

	/* get the results */
	ret = gs_plugin_loader_refresh_finish (plugin_loader, res, &error);
	if (!ret) {
		/* user cancel */
		if (g_error_matches (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_CANCELLED)) {
			gs_updates_page_set_state (self, GS_UPDATES_PAGE_STATE_IDLE);
			return;
		}
		g_warning ("failed to refresh: %s", error->message);
		gs_utils_error_strip_unique_id (error);
		gtk_label_set_label (GTK_LABEL (self->label_updates_failed),
				     error->message);
		gs_updates_page_set_state (self, GS_UPDATES_PAGE_STATE_FAILED);
		return;
	}

	/* update the last checked timestamp */
	now = g_date_time_new_now_local ();
	g_settings_set (self->settings, "check-timestamp", "x",
			g_date_time_to_unix (now));

	/* get the new list */
	gs_updates_page_invalidate (self);
	gs_page_switch_to (GS_PAGE (self), TRUE);
}

static void
gs_updates_page_get_new_updates (GsUpdatesPage *self)
{
	GsPluginRefreshFlags refresh_flags = GS_PLUGIN_REFRESH_FLAGS_NONE;

	/* force a check for updates and download */
	gs_updates_page_set_state (self, GS_UPDATES_PAGE_STATE_ACTION_REFRESH);

	if (self->cancellable_refresh != NULL) {
		g_cancellable_cancel (self->cancellable_refresh);
		g_object_unref (self->cancellable_refresh);
	}
	self->cancellable_refresh = g_cancellable_new ();

	refresh_flags |= GS_PLUGIN_REFRESH_FLAGS_INTERACTIVE;
	refresh_flags |= GS_PLUGIN_REFRESH_FLAGS_METADATA;
	if (g_settings_get_boolean (self->settings, "download-updates"))
		refresh_flags |= GS_PLUGIN_REFRESH_FLAGS_PAYLOAD;
	gs_plugin_loader_refresh_async (self->plugin_loader,
					10 * 60,
					refresh_flags,
					GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
					self->cancellable_refresh,
					(GAsyncReadyCallback) gs_updates_page_refresh_cb,
					self);
}

static void
gs_updates_page_show_network_settings (GsUpdatesPage *self)
{
	g_autoptr(GError) error = NULL;
	if (!g_spawn_command_line_async ("gnome-control-center network", &error))
		g_warning ("Failed to open the control center: %s", error->message);
}

static void
gs_updates_page_refresh_confirm_cb (GtkDialog *dialog,
                                    GtkResponseType response_type,
                                    GsUpdatesPage *self)
{
	/* unmap the dialog */
	gtk_widget_destroy (GTK_WIDGET (dialog));

	switch (response_type) {
	case GTK_RESPONSE_REJECT:
		/* open the control center */
		gs_updates_page_show_network_settings (self);
		break;
	case GTK_RESPONSE_ACCEPT:
		self->has_agreed_to_mobile_data = TRUE;
		gs_updates_page_get_new_updates (self);
		break;
	case GTK_RESPONSE_CANCEL:
	case GTK_RESPONSE_DELETE_EVENT:
		break;
	default:
		g_assert_not_reached ();
	}
}

static void
gs_updates_page_button_network_settings_cb (GtkWidget *widget,
                                            GsUpdatesPage *self)
{
	gs_updates_page_show_network_settings (self);
}

static void
gs_updates_page_button_mobile_refresh_cb (GtkWidget *widget,
                                          GsUpdatesPage *self)
{
	self->has_agreed_to_mobile_data = TRUE;
	gs_updates_page_get_new_updates (self);
}

static void
gs_updates_page_button_refresh_cb (GtkWidget *widget,
                                   GsUpdatesPage *self)
{
	GtkWidget *dialog;

	/* cancel existing action? */
	if (self->state == GS_UPDATES_PAGE_STATE_ACTION_REFRESH) {
		g_cancellable_cancel (self->cancellable_refresh);
		g_clear_object (&self->cancellable_refresh);
		return;
	}

	/* check we have a "free" network connection */
	if (gs_plugin_loader_get_network_available (self->plugin_loader) &&
	    !gs_plugin_loader_get_network_metered (self->plugin_loader)) {
		gs_updates_page_get_new_updates (self);

	/* expensive network connection */
	} else if (gs_plugin_loader_get_network_metered (self->plugin_loader)) {
		if (self->has_agreed_to_mobile_data) {
			gs_updates_page_get_new_updates (self);
			return;
		}
		dialog = gtk_message_dialog_new (gs_shell_get_window (self->shell),
						 GTK_DIALOG_MODAL |
						 GTK_DIALOG_USE_HEADER_BAR |
						 GTK_DIALOG_DESTROY_WITH_PARENT,
						 GTK_MESSAGE_ERROR,
						 GTK_BUTTONS_CANCEL,
						 /* TRANSLATORS: this is to explain that downloading updates may cost money */
						 _("Charges may apply"));
		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
							  /* TRANSLATORS: we need network
							   * to do the updates check */
							  _("Checking for updates while using mobile broadband could cause you to incur charges."));
		gtk_dialog_add_button (GTK_DIALOG (dialog),
				       /* TRANSLATORS: this is a link to the
					* control-center network panel */
				       _("Check Anyway"),
				       GTK_RESPONSE_ACCEPT);
		g_signal_connect (dialog, "response",
				  G_CALLBACK (gs_updates_page_refresh_confirm_cb),
				  self);
		gs_shell_modal_dialog_present (self->shell, GTK_DIALOG (dialog));

	/* no network connection */
	} else {
		dialog = gtk_message_dialog_new (gs_shell_get_window (self->shell),
						 GTK_DIALOG_MODAL |
						 GTK_DIALOG_USE_HEADER_BAR |
						 GTK_DIALOG_DESTROY_WITH_PARENT,
						 GTK_MESSAGE_ERROR,
						 GTK_BUTTONS_CANCEL,
						 /* TRANSLATORS: can't do updates check */
						 _("No Network"));
		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
							  /* TRANSLATORS: we need network
							   * to do the updates check */
							  _("Internet access is required to check for updates."));
		gtk_dialog_add_button (GTK_DIALOG (dialog),
				       /* TRANSLATORS: this is a link to the
					* control-center network panel */
				       _("Network Settings"),
				       GTK_RESPONSE_REJECT);
		g_signal_connect (dialog, "response",
				  G_CALLBACK (gs_updates_page_refresh_confirm_cb),
				  self);
		gs_shell_modal_dialog_present (self->shell, GTK_DIALOG (dialog));
	}
}

static void
gs_updates_page_pending_apps_changed_cb (GsPluginLoader *plugin_loader,
                                         GsUpdatesPage *self)
{
	gs_updates_page_invalidate (self);
}

static void
cancel_trigger_failed_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
	GsUpdatesPage *self = GS_UPDATES_PAGE (user_data);
	g_autoptr(GError) error = NULL;
	if (!gs_plugin_loader_app_action_finish (self->plugin_loader, res, &error)) {
		g_warning ("failed to cancel trigger: %s", error->message);
		return;
	}
}

static void
gs_updates_page_reboot_failed_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
	GsUpdatesPage *self = GS_UPDATES_PAGE (user_data);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) apps = NULL;
	g_autoptr(GVariant) retval = NULL;

	/* get result */
	retval = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
	if (retval != NULL)
		return;

	if (error != NULL) {
		g_warning ("Calling org.gnome.SessionManager.Reboot failed: %s",
			   error->message);
	}

	/* cancel trigger */
	apps = gs_update_list_get_apps (GS_UPDATE_LIST (self->list_box_updates));
	gs_plugin_loader_app_action_async (self->plugin_loader,
					   gs_app_list_index (apps, 0),
					   GS_PLUGIN_ACTION_UPDATE_CANCEL,
					   GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
					   self->cancellable,
					   cancel_trigger_failed_cb,
					   self);
}

static void
gs_updates_page_perform_update_cb (GsPluginLoader *plugin_loader,
                                   GAsyncResult *res,
                                   GsUpdatesPage *self)
{
	g_autoptr(GError) error = NULL;

	/* unconditionally re-enable this */
	gtk_widget_set_sensitive (GTK_WIDGET (self->button_update_all), TRUE);

	/* get the results */
	if (!gs_plugin_loader_update_finish (plugin_loader, res, &error)) {
		g_warning ("Failed to perform update: %s", error->message);
		return;
	}

	/* trigger reboot if any application was not updatable live */
	if (!self->all_updates_are_live) {
		g_autoptr(GDBusConnection) bus = NULL;
		bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
		g_dbus_connection_call (bus,
					"org.gnome.SessionManager",
					"/org/gnome/SessionManager",
					"org.gnome.SessionManager",
					"Reboot",
					NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
					G_MAXINT, NULL,
					gs_updates_page_reboot_failed_cb,
					self);

	/* when we are not doing an offline update, show a notification
	 * if any application requires a reboot */
	} else if (self->any_require_reboot) {
		g_autoptr(GNotification) n = NULL;
		/* TRANSLATORS: we've just live-updated some apps */
		n = g_notification_new (_("Updates have been installed"));
		/* TRANSLATORS: the new apps will not be run until we restart */
		g_notification_set_body (n, _("A restart is required for them to take effect."));
		/* TRANSLATORS: button text */
		g_notification_add_button (n, _("Not Now"), "app.nop");
		/* TRANSLATORS: button text */
		g_notification_add_button_with_target (n, _("Restart"), "app.reboot", NULL);
		g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
		g_application_send_notification (g_application_get_default (), "restart-required", n);
	}
}

static void
update_all (GsUpdatesPage *self, GsAppList *apps)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GCancellable) cancellable = g_cancellable_new ();

	g_set_object (&self->cancellable, cancellable);
	gs_plugin_loader_update_async (self->plugin_loader,
				       apps,
				       GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
				       self->cancellable,
				       (GAsyncReadyCallback) gs_updates_page_perform_update_cb,
				       self);
}

static void
gs_updates_page_header_update_all_cb (GtkButton     *button,
                                      GsUpdatesPage *self)
{
	g_autoptr(GsAppList) apps = NULL;

	apps = gs_update_list_get_apps (GS_UPDATE_LIST (self->list_box_updates));
	update_all (self, apps);
	gtk_widget_set_sensitive (GTK_WIDGET (self->button_update_all), FALSE);
}

static void
gs_updates_page_update_all_cb (GsUpdateList *update_list,
                               GsAppList *apps,
                               GsUpdatesPage *self)
{
	update_all (self, apps);
}

typedef struct {
	GsApp		*app;
	GsUpdatesPage	*self;
} GsPageHelper;

static void
gs_page_helper_free (GsPageHelper *helper)
{
	if (helper->app != NULL)
		g_object_unref (helper->app);
	if (helper->self != NULL)
		g_object_unref (helper->self);
	g_slice_free (GsPageHelper, helper);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsPageHelper, gs_page_helper_free);

static void
upgrade_download_finished_cb (GObject *source,
                              GAsyncResult *res,
                              gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsPageHelper) helper = (GsPageHelper *) user_data;

	if (!gs_plugin_loader_app_action_finish (plugin_loader, res, &error)) {
		if (g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			return;
		g_warning ("failed to upgrade-download: %s", error->message);
	}
}

static void
gs_updates_page_upgrade_download_cb (GsUpgradeBanner *upgrade_banner,
                                     GsUpdatesPage *self)
{
	GsApp *app;
	GsPageHelper *helper;

	app = gs_upgrade_banner_get_app (upgrade_banner);
	if (app == NULL) {
		g_warning ("no upgrade available to download");
		return;
	}

	helper = g_slice_new0 (GsPageHelper);
	helper->app = g_object_ref (app);
	helper->self = g_object_ref (self);

	if (self->cancellable_upgrade_download != NULL)
		g_object_unref (self->cancellable_upgrade_download);
	self->cancellable_upgrade_download = g_cancellable_new ();
	gs_plugin_loader_app_action_async (self->plugin_loader,
					   app,
					   GS_PLUGIN_ACTION_UPGRADE_DOWNLOAD,
					   GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
					   self->cancellable_upgrade_download,
					   upgrade_download_finished_cb,
					   helper);
}

static void
upgrade_reboot_failed_cb (GObject *source,
                          GAsyncResult *res,
                          gpointer user_data)
{
	GsUpdatesPage *self = (GsUpdatesPage *) user_data;
	GsApp *app;
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) retval = NULL;

	/* get result */
	retval = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
	if (retval != NULL)
		return;

	if (error != NULL) {
		g_warning ("Calling org.gnome.SessionManager.Reboot failed: %s",
			   error->message);
	}

	app = gs_upgrade_banner_get_app (GS_UPGRADE_BANNER (self->upgrade_banner));
	if (app == NULL) {
		g_warning ("no upgrade to cancel");
		return;
	}

	/* cancel trigger */
	gs_plugin_loader_app_action_async (self->plugin_loader,
					   app,
					   GS_PLUGIN_ACTION_UPDATE_CANCEL,
					   GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
					   self->cancellable,
					   cancel_trigger_failed_cb,
					   self);
}

static void
upgrade_trigger_finished_cb (GObject *source,
                             GAsyncResult *res,
                             gpointer user_data)
{
	GsUpdatesPage *self = (GsUpdatesPage *) user_data;
	g_autoptr(GDBusConnection) bus = NULL;
	g_autoptr(GError) error = NULL;

	/* get the results */
	if (!gs_plugin_loader_update_finish (self->plugin_loader, res, &error)) {
		g_warning ("Failed to trigger offline update: %s", error->message);
		return;
	}

	/* trigger reboot */
	bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
	g_dbus_connection_call (bus,
				"org.gnome.SessionManager",
				"/org/gnome/SessionManager",
				"org.gnome.SessionManager",
				"Reboot",
				NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
				G_MAXINT, NULL,
				upgrade_reboot_failed_cb,
				self);
}

static void
trigger_upgrade (GsUpdatesPage *self)
{
	GsApp *upgrade;

	upgrade = gs_upgrade_banner_get_app (GS_UPGRADE_BANNER (self->upgrade_banner));
	if (upgrade == NULL) {
		g_warning ("no upgrade available to install");
		return;
	}

	gs_plugin_loader_app_action_async (self->plugin_loader,
	                                   upgrade,
	                                   GS_PLUGIN_ACTION_UPGRADE_TRIGGER,
	                                   GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
	                                   self->cancellable,
	                                   upgrade_trigger_finished_cb,
	                                   self);
}

static void
gs_updates_page_upgrade_confirm_cb (GtkDialog *dialog,
                                    GtkResponseType response_type,
                                    GsUpdatesPage *self)
{
	/* unmap the dialog */
	gtk_widget_destroy (GTK_WIDGET (dialog));

	switch (response_type) {
	case GTK_RESPONSE_ACCEPT:
		g_debug ("agreed to upgrade removing apps");
		trigger_upgrade (self);
		break;
	case GTK_RESPONSE_CANCEL:
		g_debug ("cancelled removal dialog");
		break;
	case GTK_RESPONSE_DELETE_EVENT:
		break;
	default:
		g_assert_not_reached ();
	}
}

static void
gs_updates_page_upgrade_install_cb (GsUpgradeBanner *upgrade_banner,
                                    GsUpdatesPage *self)
{
	GPtrArray *removals;
	GsApp *upgrade;
	GtkWidget *dialog;
	guint cnt = 0;
	guint i;

	upgrade = gs_upgrade_banner_get_app (GS_UPGRADE_BANNER (self->upgrade_banner));
	if (upgrade == NULL) {
		g_warning ("no upgrade available to install");
		return;
	}

	/* count the removals */
	removals = gs_app_get_related (upgrade);
	for (i = 0; i < removals->len; i++) {
		GsApp *app = g_ptr_array_index (removals, i);
		if (gs_app_get_state (app) != AS_APP_STATE_UNAVAILABLE)
			continue;
		cnt++;
	}

	if (cnt == 0) {
		/* no need for a removal confirmation dialog */
		trigger_upgrade (self);
		return;
	}

	dialog = gs_removal_dialog_new ();
	g_signal_connect (dialog, "response",
	                  G_CALLBACK (gs_updates_page_upgrade_confirm_cb),
	                  self);
	gs_removal_dialog_show_upgrade_removals (GS_REMOVAL_DIALOG (dialog),
	                                         upgrade);
	gs_shell_modal_dialog_present (self->shell, GTK_DIALOG (dialog));
}

static void
gs_updates_page_upgrade_help_cb (GsUpgradeBanner *upgrade_banner,
                                 GsUpdatesPage *self)
{
	GsApp *app;
	const gchar *uri;
	g_autoptr(GError) error = NULL;

	app = gs_upgrade_banner_get_app (upgrade_banner);
	if (app == NULL) {
		g_warning ("no upgrade available to launch");
		return;
	}

	/* open the link */
	uri = gs_app_get_url (app, AS_URL_KIND_HOMEPAGE);
	gs_shell_show_uri (self->shell, uri);
}

static void
gs_updates_page_invalidate_downloaded_upgrade (GsUpdatesPage *self)
{
	GsApp *app;
	app = gs_upgrade_banner_get_app (GS_UPGRADE_BANNER (self->upgrade_banner));
	if (app == NULL)
		return;
	if (gs_app_get_state (app) != AS_APP_STATE_UPDATABLE)
		return;
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	g_debug ("resetting %s to AVAILABLE as the updates have changed",
		 gs_app_get_id (app));
}

static gboolean
gs_shell_update_are_updates_in_progress (GsUpdatesPage *self)
{
	GsUpdateList *update_list = GS_UPDATE_LIST (self->list_box_updates);
	g_autoptr(GsAppList) list = gs_update_list_get_apps (update_list);
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		switch (gs_app_get_state (app)) {
		case AS_APP_STATE_INSTALLING:
		case AS_APP_STATE_REMOVING:
		case AS_APP_STATE_PURCHASING:
			return TRUE;
			break;
		default:
			break;
		}
	}
	return FALSE;
}

static void
gs_updates_page_changed_cb (GsPluginLoader *plugin_loader,
                            GsUpdatesPage *self)
{
	/* if we do a live update and the upgrade is waiting to be deployed
	 * then make sure all new packages are downloaded */
	gs_updates_page_invalidate_downloaded_upgrade (self);

	/* check to see if any apps in the app list are in a processing state */
	if (gs_shell_update_are_updates_in_progress (self)) {
		g_debug ("ignoring updates-changed as updates in progress");
		return;
	}

	/* refresh updates list */
	gs_updates_page_reload (GS_PAGE (self));
}

static void
gs_updates_page_status_changed_cb (GsPluginLoader *plugin_loader,
                                   GsApp *app,
                                   GsPluginStatus status,
                                   GsUpdatesPage *self)
{
	switch (status) {
	case GS_PLUGIN_STATUS_INSTALLING:
	case GS_PLUGIN_STATUS_REMOVING:
		if (gs_app_get_kind (app) != AS_APP_KIND_OS_UPGRADE &&
		    gs_app_get_id (app) != NULL) {
			/* if we do a install or remove then make sure all new
			 * packages are downloaded */
			gs_updates_page_invalidate_downloaded_upgrade (self);
		}
		break;
	default:
		break;
	}

	self->last_status = status;
	gs_updates_page_update_ui_state (self);
}

static void
gs_updates_page_allow_updates_notify_cb (GsPluginLoader *plugin_loader,
                                         GParamSpec *pspec,
                                         GsUpdatesPage *self)
{
	if (!gs_plugin_loader_get_allow_updates (plugin_loader)) {
		gs_updates_page_set_state (self, GS_UPDATES_PAGE_STATE_MANAGED);
		return;
	}
	gs_updates_page_set_state (self, GS_UPDATES_PAGE_STATE_IDLE);
}

static void
gs_updates_page_upgrade_cancel_cb (GsUpgradeBanner *upgrade_banner,
                                   GsUpdatesPage *self)
{
	g_cancellable_cancel (self->cancellable_upgrade_download);
}

static gboolean
gs_updates_page_setup (GsPage *page,
                       GsShell *shell,
                       GsPluginLoader *plugin_loader,
                       GtkBuilder *builder,
                       GCancellable *cancellable,
                       GError **error)
{
	GsUpdatesPage *self = GS_UPDATES_PAGE (page);
	AtkObject *accessible;

	g_return_val_if_fail (GS_IS_UPDATES_PAGE (self), TRUE);

	self->shell = shell;

	self->plugin_loader = g_object_ref (plugin_loader);
	g_signal_connect (self->plugin_loader, "pending-apps-changed",
			  G_CALLBACK (gs_updates_page_pending_apps_changed_cb),
			  self);
	g_signal_connect (self->plugin_loader, "status-changed",
			  G_CALLBACK (gs_updates_page_status_changed_cb),
			  self);
	g_signal_connect (self->plugin_loader, "updates-changed",
			  G_CALLBACK (gs_updates_page_changed_cb),
			  self);
	g_signal_connect_object (self->plugin_loader, "notify::allow-updates",
				 G_CALLBACK (gs_updates_page_allow_updates_notify_cb),
				 self, 0);
	g_signal_connect_object (self->plugin_loader, "notify::network-available",
				 G_CALLBACK (gs_updates_page_network_available_notify_cb),
				 self, 0);
	self->builder = g_object_ref (builder);
	self->cancellable = g_object_ref (cancellable);

	/* setup updates */
	g_signal_connect (self->list_box_updates, "row-activated",
			  G_CALLBACK (gs_updates_page_activated_cb), self);
	g_signal_connect (self->list_box_updates, "button-clicked",
			  G_CALLBACK (gs_updates_page_button_clicked_cb), self);
	g_signal_connect (self->list_box_updates, "update-all",
			  G_CALLBACK (gs_updates_page_update_all_cb), self);

	/* setup system upgrades */
	g_signal_connect (self->upgrade_banner, "download-clicked",
			  G_CALLBACK (gs_updates_page_upgrade_download_cb), self);
	g_signal_connect (self->upgrade_banner, "install-clicked",
			  G_CALLBACK (gs_updates_page_upgrade_install_cb), self);
	g_signal_connect (self->upgrade_banner, "cancel-clicked",
			  G_CALLBACK (gs_updates_page_upgrade_cancel_cb), self);
	g_signal_connect (self->upgrade_banner, "help-clicked",
			  G_CALLBACK (gs_updates_page_upgrade_help_cb), self);

	self->header_end_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_visible (self->header_end_box, TRUE);
	gs_page_set_header_end_widget (GS_PAGE (self), self->header_end_box);

	self->button_update_all = gtk_button_new_with_mnemonic (_("Restart & _Install"));
	gtk_widget_set_valign (self->button_update_all, GTK_ALIGN_CENTER);
	gtk_widget_set_visible (self->button_update_all, FALSE);
	gtk_style_context_add_class (gtk_widget_get_style_context (self->button_update_all), "suggested-action");
	gtk_container_add (GTK_CONTAINER (self->header_end_box), self->button_update_all);
	g_signal_connect (self->button_update_all, "clicked", G_CALLBACK (gs_updates_page_header_update_all_cb), self);

	self->header_start_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_visible (self->header_start_box, TRUE);
	gs_page_set_header_start_widget (GS_PAGE (self), self->header_start_box);

	self->header_spinner_start = gtk_spinner_new ();
	gtk_box_pack_end (GTK_BOX (self->header_start_box), self->header_spinner_start, FALSE, FALSE, 0);

	/* setup update details window */
	self->button_refresh = gtk_button_new_from_icon_name ("view-refresh-symbolic", GTK_ICON_SIZE_MENU);
	accessible = gtk_widget_get_accessible (self->button_refresh);
	if (accessible != NULL)
		atk_object_set_name (accessible, _("Check for updates"));
	gtk_box_pack_start (GTK_BOX (self->header_start_box), self->button_refresh, FALSE, FALSE, 0);
	g_signal_connect (self->button_refresh, "clicked",
			  G_CALLBACK (gs_updates_page_button_refresh_cb),
			  self);

	g_signal_connect (self->button_updates_mobile, "clicked",
			  G_CALLBACK (gs_updates_page_button_mobile_refresh_cb),
			  self);
	g_signal_connect (self->button_updates_offline, "clicked",
			  G_CALLBACK (gs_updates_page_button_network_settings_cb),
			  self);

	/* set initial state */
	if (!gs_plugin_loader_get_allow_updates (self->plugin_loader))
		self->state = GS_UPDATES_PAGE_STATE_MANAGED;
	return TRUE;
}

static void
gs_updates_page_dispose (GObject *object)
{
	GsUpdatesPage *self = GS_UPDATES_PAGE (object);

	if (self->cancellable_refresh != NULL) {
		g_cancellable_cancel (self->cancellable_refresh);
		g_clear_object (&self->cancellable_refresh);
	}
	if (self->cancellable_upgrade_download != NULL) {
		g_cancellable_cancel (self->cancellable_upgrade_download);
		g_clear_object (&self->cancellable_upgrade_download);
	}

	g_clear_object (&self->builder);
	g_clear_object (&self->plugin_loader);
	g_clear_object (&self->cancellable);
	g_clear_object (&self->settings);
	g_clear_object (&self->desktop_settings);

	G_OBJECT_CLASS (gs_updates_page_parent_class)->dispose (object);
}

static void
gs_updates_page_class_init (GsUpdatesPageClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPageClass *page_class = GS_PAGE_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_updates_page_dispose;
	page_class->switch_to = gs_updates_page_switch_to;
	page_class->reload = gs_updates_page_reload;
	page_class->setup = gs_updates_page_setup;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-updates-page.ui");

	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, updates_box);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, button_updates_mobile);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, button_updates_offline);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, label_updates_failed);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, label_updates_last_checked);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, label_updates_spinner);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, list_box_updates);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, scrolledwindow_updates);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, spinner_updates);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, stack_updates);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, upgrade_banner);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, box_end_of_life);
	gtk_widget_class_bind_template_child (widget_class, GsUpdatesPage, label_end_of_life);
}

static void
gs_updates_page_init (GsUpdatesPage *self)
{
	const char *ampm;

	gtk_widget_init_template (GTK_WIDGET (self));

	self->state = GS_UPDATES_PAGE_STATE_STARTUP;
	self->settings = g_settings_new ("org.gnome.software");
	self->desktop_settings = g_settings_new ("org.gnome.desktop.interface");

	ampm = nl_langinfo (AM_STR);
	if (ampm != NULL && *ampm != '\0')
		self->ampm_available = TRUE;
}

GsUpdatesPage *
gs_updates_page_new (void)
{
	GsUpdatesPage *self;
	self = g_object_new (GS_TYPE_UPDATES_PAGE, NULL);
	return GS_UPDATES_PAGE (self);
}

/* vim: set noexpandtab: */
