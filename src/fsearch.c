/*
   FSearch - A fast file search utility
   Copyright © 2020 Christian Boxdörfer

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
   */

#define G_LOG_DOMAIN "fsearch-application"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "fsearch.h"
#include "fsearch_clipboard.h"
#include "fsearch_config.h"
#include "fsearch_database.h"
#include "fsearch_file_utils.h"
#include "fsearch_limits.h"
#include "fsearch_preferences_ui.h"
#include "fsearch_ui_utils.h"
#include "fsearch_window.h"
#include "icon_resources.h"
#include "ui_resources.h"
#include "fsearch_preview.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct _FsearchApplication {
    GtkApplication parent;
    FsearchDatabase *db;
    FsearchConfig *config;
    FsearchThreadPool *pool;

    GThreadPool *db_pool;

    char *option_search_term;
    bool new_window;

    guint file_manager_watch_id;
    bool has_file_manager_on_bus;

    FsearchDatabaseState db_state;
    guint db_timeout_id;

    GCancellable *db_thread_cancellable;
    int num_database_update_active;
    GMutex mutex;

    bool is_shutting_down;
};

static const char *fsearch_bus_name = "io.github.cboxdoerfer.FSearch";
static const char *fsearch_db_worker_bus_name = "io.github.cboxdoerfer.FSearchDatabaseWorker";
static const char *fsearch_object_path = "/io/github/cboxdoerfer/FSearch";

enum {
    FSEARCH_SIGNAL_DATABASE_SCAN_STARTED,
    FSEARCH_SIGNAL_DATABASE_UPDATE_FINISHED,
    FSEARCH_SIGNAL_DATABASE_LOAD_STARTED,
    NUM_FSEARCH_SIGNALS
};

typedef enum FsearchDatabaseActionType {
    FSEARCH_DATABASE_ACTION_SCAN,
    FSEARCH_DATABASE_ACTION_LOAD,
    NUM_FSEARCH_DATABASE_ACTION_TYPES,
} FsearchDatabaseActionType;

typedef struct {
    FsearchDatabaseActionType action;
    void (*update_func)(FsearchApplication *, FsearchDatabase *);
    void (*started_cb)(void *);
    void *started_cb_data;
    void (*finished_cb)(void *);
    void (*cancelled_cb)(void *);
    void *cancelled_cb_data;
} DatabaseUpdateContext;

static guint fsearch_signals[NUM_FSEARCH_SIGNALS];

G_DEFINE_TYPE(FsearchApplication, fsearch_application, GTK_TYPE_APPLICATION)

static void
action_set_enabled(const char *action_name, gboolean enabled);

static gboolean
on_database_scan_enqueue(gpointer data);

static void
set_accels_for_escape(GApplication *app);

static gboolean
on_database_auto_update(gpointer user_data) {
    FsearchApplication *self = FSEARCH_APPLICATION(user_data);
    g_debug("[app] scheduled database update started");
    g_action_group_activate_action(G_ACTION_GROUP(self), "update_database", NULL);
    return G_SOURCE_CONTINUE;
}

static void
database_auto_update_init(FsearchApplication *fsearch) {
    if (fsearch->db_timeout_id != 0) {
        g_source_remove(fsearch->db_timeout_id);
        fsearch->db_timeout_id = 0;
    }
    if (fsearch->config->update_database_every) {
        guint seconds = fsearch->config->update_database_every_hours * 3600
                      + fsearch->config->update_database_every_minutes * 60;
        if (seconds < 60) {
            seconds = 60;
        }

        g_debug("[app] update database every %d seconds", seconds);
        fsearch->db_timeout_id = g_timeout_add_seconds(seconds, on_database_auto_update, fsearch);
    }
}

static gboolean
on_database_notify_status(gpointer user_data) {
    g_autofree char *text = user_data;
    g_return_val_if_fail(text, G_SOURCE_REMOVE);

    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));

    for (; windows; windows = windows->next) {
        GtkWindow *window = windows->data;
        if (FSEARCH_IS_APPLICATION_WINDOW(window)) {
            fsearch_application_window_set_database_index_progress((FsearchApplicationWindow *)window, text);
        }
    }

    return G_SOURCE_REMOVE;
}

static void
database_notify_status_cb(const char *text) {
    if (text) {
        g_idle_add(on_database_notify_status, g_strdup(text));
    }
}

static void
prepare_windows_for_db_update(FsearchApplication *app) {
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));

    for (; windows; windows = windows->next) {
        FsearchApplicationWindow *window = windows->data;

        if (FSEARCH_IS_APPLICATION_WINDOW(window)) {
            fsearch_application_window_remove_model(window);
        }
    }
}

static gboolean
on_database_update_finished(gpointer user_data) {
    FsearchApplication *self = FSEARCH_APPLICATION_DEFAULT;
    FsearchDatabase *db = user_data;
    if (self->is_shutting_down) {
        g_debug("[app] update finished, but app is shutting down");
        g_clear_pointer(&db, db_unref);
        return G_SOURCE_REMOVE;
    }
    fsearch_application_state_lock(self);
    if (!g_cancellable_is_cancelled(self->db_thread_cancellable)) {
        prepare_windows_for_db_update(self);
        g_clear_pointer(&self->db, db_unref);
        self->db = g_steal_pointer(&db);
    }
    else if (db) {
        g_clear_pointer(&db, db_unref);
    }
    g_cancellable_reset(self->db_thread_cancellable);
    self->num_database_update_active--;
    if (self->num_database_update_active == 0) {
        action_set_enabled("update_database", TRUE);
        action_set_enabled("cancel_update_database", FALSE);
    }
    fsearch_application_state_unlock(self);
    g_signal_emit(self, fsearch_signals[FSEARCH_SIGNAL_DATABASE_UPDATE_FINISHED], 0);
    return G_SOURCE_REMOVE;
}

static void
database_update_finished_cb(gpointer user_data) {
    FsearchApplication *self = FSEARCH_APPLICATION_DEFAULT;
    self->db_state = FSEARCH_DATABASE_STATE_IDLE;
    g_idle_add(on_database_update_finished, user_data);
}

static gboolean
on_database_load_started(gpointer user_data) {
    FsearchApplication *self = FSEARCH_APPLICATION(user_data);
    g_signal_emit(self, fsearch_signals[FSEARCH_SIGNAL_DATABASE_LOAD_STARTED], 0);
    return G_SOURCE_REMOVE;
}

static gboolean
on_database_scan_started(gpointer user_data) {
    FsearchApplication *self = FSEARCH_APPLICATION(user_data);
    g_signal_emit(self, fsearch_signals[FSEARCH_SIGNAL_DATABASE_SCAN_STARTED], 0);
    return G_SOURCE_REMOVE;
}

static void
database_load_started_cb(gpointer user_data) {
    FsearchApplication *self = FSEARCH_APPLICATION(user_data);
    self->db_state = FSEARCH_DATABASE_STATE_LOADING;
    g_idle_add(on_database_load_started, self);
}

static void
database_scan_started_cb(gpointer user_data) {
    FsearchApplication *self = FSEARCH_APPLICATION(user_data);
    self->db_state = FSEARCH_DATABASE_STATE_SCANNING;
    g_idle_add(on_database_scan_started, self);
}

static void
database_scan_and_save(FsearchApplication *app, FsearchDatabase *db) {
    const bool scan_successful =
        db_scan(db, app->db_thread_cancellable, app->config->show_indexing_status ? database_notify_status_cb : NULL);
    if (scan_successful && !g_cancellable_is_cancelled(app->db_thread_cancellable)) {
        g_autofree gchar *db_path = fsearch_application_get_database_dir();
        if (db_path) {
            if (app->config->show_indexing_status) {
                database_notify_status_cb(_("Saving…"));
            }
            db_save(db, db_path);
        }
    }
}

static void
database_load(FsearchApplication *app, FsearchDatabase *db) {
    g_autofree char *db_file_path = fsearch_application_get_database_file_path();
    if (!db_file_path) {
        return;
    }
    if (!db_load(db, db_file_path, app->config->show_indexing_status ? database_notify_status_cb : NULL)
        && !app->config->update_database_on_launch) {
        // load failed -> trigger rescan
        g_idle_add(on_database_scan_enqueue, NULL);
    }
}

static void
database_scan_or_load_enqueue(FsearchDatabaseActionType action) {
    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;
    action_set_enabled("update_database", FALSE);
    action_set_enabled("cancel_update_database", TRUE);

    g_cancellable_reset(app->db_thread_cancellable);
    app->num_database_update_active++;

    DatabaseUpdateContext *ctx = calloc(1, sizeof(DatabaseUpdateContext));
    g_assert(ctx);

    switch (action) {
    case FSEARCH_DATABASE_ACTION_SCAN:
        ctx->update_func = database_scan_and_save;
        ctx->started_cb = database_scan_started_cb;
        break;
    case FSEARCH_DATABASE_ACTION_LOAD:
        ctx->update_func = database_load;
        ctx->started_cb = database_load_started_cb;
        break;
    default:
        g_assert_not_reached();
    }

    ctx->started_cb_data = app;
    ctx->finished_cb = database_update_finished_cb;

    g_thread_pool_push(app->db_pool, g_steal_pointer(&ctx), NULL);
}

static gboolean
on_database_scan_enqueue(gpointer data) {
    database_scan_or_load_enqueue(FSEARCH_DATABASE_ACTION_SCAN);
    return G_SOURCE_REMOVE;
}

static void
database_pool_func(gpointer data, gpointer user_data) {
    FsearchApplication *app = FSEARCH_APPLICATION(user_data);
    g_autofree DatabaseUpdateContext *ctx = data;
    g_return_if_fail(ctx);

    if (ctx->started_cb) {
        ctx->started_cb(ctx->started_cb_data);
    }

    g_autoptr(GTimer) timer = g_timer_new();
    g_timer_start(timer);

    fsearch_application_state_lock(app);
    FsearchDatabase *db = db_new(app->config->indexes,
                                 app->config->exclude_locations,
                                 app->config->exclude_files,
                                 app->config->exclude_hidden_items);
    fsearch_application_state_unlock(app);

    ctx->update_func(app, db);

    g_timer_stop(timer);
    const double seconds = g_timer_elapsed(timer, NULL);

    g_debug("[app] database update finished in %.2f ms", seconds * 1000);

    if (ctx->finished_cb) {
        ctx->finished_cb(db);
    }
}

static void
move_search_term_to_window(FsearchApplication *app, FsearchApplicationWindow *win) {
    if (!app->option_search_term) {
        return;
    }
    GtkEntry *entry = fsearch_application_window_get_search_entry(win);
    g_return_if_fail(entry);

    GtkEntryBuffer *buffer = gtk_entry_get_buffer(entry);
    gtk_entry_buffer_set_text(buffer, app->option_search_term, -1);

    g_clear_pointer(&app->option_search_term, g_free);
}

static FsearchApplicationWindow *
get_first_application_window(FsearchApplication *app) {
    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));

    if (!windows || !FSEARCH_IS_APPLICATION_WINDOW(windows->data)) {
        return NULL;
    }

    return FSEARCH_APPLICATION_WINDOW(windows->data);
}

static GString *
get_application_version(void) {
    GString *version = g_string_new(PACKAGE_VERSION);
#ifdef BUILD_CHANNEL
    if (g_strcmp0(BUILD_CHANNEL, "other") != 0) {
        g_string_append(version, " (");
        g_string_append(version, BUILD_CHANNEL);
        g_string_append(version, ")");
    }
#endif
    return version;
}

static void
show_url(FsearchApplication *app, const char *url) {
    g_return_if_fail(url);
    g_return_if_fail(FSEARCH_IS_APPLICATION(app));

    FsearchApplicationWindow *window = get_first_application_window(app);
    if (!window) {
        return;
    }

    gtk_show_uri(GTK_WINDOW(window), url, GDK_CURRENT_TIME);
}

static void
action_forum_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    show_url(app, "https://github.com/cboxdoerfer/fsearch/discussions/");
}

static void
action_bug_report_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    show_url(app, "https://github.com/cboxdoerfer/fsearch/issues/");
}

static void
action_donate_github_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    show_url(app, "https://github.com/sponsors/cboxdoerfer");
}

static void
action_donate_paypal_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    show_url(app, "https://www.paypal.com/donate/?hosted_button_id=TTXBUD7PMZXN2");
}

static void
action_online_help_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    show_url(app, "https://github.com/cboxdoerfer/fsearch/wiki/");
}

static void
action_help_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    show_url(app, "help:fsearch");
}

static void
action_about_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    g_assert(FSEARCH_IS_APPLICATION(app));
    FsearchApplicationWindow *window = get_first_application_window(app);
    if (!window) {
        return;
    }

    g_autoptr(GString) version = get_application_version();

    gtk_show_about_dialog(GTK_WINDOW(window),
                          "program-name",
                          PACKAGE_NAME,
                          "logo-icon-name",
                          "io.github.cboxdoerfer.FSearch",
                          "license-type",
                          GTK_LICENSE_GPL_2_0,
                          "copyright",
                          "Christian Boxdörfer",
                          "website",
                          "https://github.com/cboxdoerfer/fsearch",
                          "version",
                          version->str,
                          "translator-credits",
                          _("translator-credits"),
                          "comments",
                          _("A search utility focusing on performance and advanced features"),
                          NULL);
}

static void
action_quit_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    // TODO: windows need to be cleaned up manually here
    g_application_quit(G_APPLICATION(app));
}

static void
on_preferences_ui_finished(FsearchConfig *new_config) {
    if (!new_config) {
        return;
    }

    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;

    FsearchConfigCompareResult config_diff = {.database_config_changed = true,
                                              .listview_config_changed = true,
                                              .search_config_changed = true};

    if (app->config) {
        config_diff = config_cmp(app->config, new_config);
        g_clear_pointer(&app->config, config_free);
    }
    app->config = new_config;
    config_save(app->config);

    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", new_config->enable_dark_theme, NULL);
    database_auto_update_init(app);

    if (config_diff.database_config_changed) {
        database_scan_or_load_enqueue(FSEARCH_DATABASE_ACTION_SCAN);
    }

    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
    for (GList *w = windows; w; w = w->next) {
        FsearchApplicationWindow *window = w->data;
        if (config_diff.search_config_changed) {
            fsearch_application_window_update_query_flags(window);
        }
        if (config_diff.listview_config_changed) {
            fsearch_application_window_update_listview_config(window);
        }
    }

    set_accels_for_escape(G_APPLICATION(app));
}

static void
action_preferences_activated(GSimpleAction *action, GVariant *parameter, gpointer gapp) {
    g_assert(FSEARCH_IS_APPLICATION(gapp));
    FsearchApplication *app = FSEARCH_APPLICATION(gapp);

    const FsearchPreferencesPage page = g_variant_get_uint32(parameter);

    GtkWindow *win_active = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (!win_active) {
        return;
    }
    FsearchConfig *copy_config = config_copy(app->config);
    preferences_ui_launch(copy_config, win_active, page, on_preferences_ui_finished);
}

static void
action_cancel_update_database_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;
    g_cancellable_cancel(app->db_thread_cancellable);
}

static void
action_update_database_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    database_scan_or_load_enqueue(FSEARCH_DATABASE_ACTION_SCAN);
}

static void
action_new_window_activated(GSimpleAction *action, GVariant *parameter, gpointer app) {
    GtkWindow *window = GTK_WINDOW(fsearch_application_window_new(FSEARCH_APPLICATION(app)));
    FsearchApplication *self = FSEARCH_APPLICATION(app);

    move_search_term_to_window(self, FSEARCH_APPLICATION_WINDOW(window));
    fsearch_application_window_focus_search_entry(FSEARCH_APPLICATION_WINDOW(window));

    gtk_window_present(window);
}

static void
action_set_enabled(const char *action_name, gboolean enabled) {
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(FSEARCH_APPLICATION_DEFAULT), action_name);
    g_return_if_fail(action);

    g_debug(enabled ? "[app] enabled action: %s" : "[app] disabled action: %s", action_name);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

static void
fsearch_application_shutdown(GApplication *app) {
    g_assert(FSEARCH_IS_APPLICATION(app));
    FsearchApplication *fsearch = FSEARCH_APPLICATION(app);

    GList *windows = gtk_application_get_windows(GTK_APPLICATION(app));
    for (; windows; windows = windows->next) {
        GtkWindow *window = windows->data;
        if (FSEARCH_IS_APPLICATION_WINDOW(window)) {
            fsearch_application_window_prepare_shutdown(window);
        }
    }

    if (fsearch->file_manager_watch_id) {
        g_bus_unwatch_name(fsearch->file_manager_watch_id);
        fsearch->file_manager_watch_id = 0;
    }

    if (fsearch->db_pool) {
        g_debug("[app] waiting for database thread to exit...");
        fsearch->is_shutting_down = true;
        g_cancellable_cancel(fsearch->db_thread_cancellable);
        g_thread_pool_free(g_steal_pointer(&fsearch->db_pool), FALSE, TRUE);
        g_debug("[app] database thread finished.");
    }

    // close the preview
    fsearch_preview_call_close();

    g_clear_pointer(&fsearch->db, db_unref);
    g_clear_object(&fsearch->db_thread_cancellable);

    g_clear_pointer(&fsearch->option_search_term, g_free);

    config_save(fsearch->config);
    g_clear_pointer(&fsearch->config, config_free);

    g_mutex_clear(&fsearch->mutex);

    G_APPLICATION_CLASS(fsearch_application_parent_class)->shutdown(app);
}

static void
fsearch_application_finalize(GObject *object) {
    G_OBJECT_CLASS(fsearch_application_parent_class)->finalize(object);
}

static void
on_file_manager_name_appeared(GDBusConnection *connection, const gchar *name, const gchar *name_owner, gpointer user_data) {
    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;
    g_return_if_fail(app);
    app->has_file_manager_on_bus = true;
}

static void
on_file_manager_name_vanished(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    FsearchApplication *app = FSEARCH_APPLICATION_DEFAULT;
    g_return_if_fail(app);
    app->has_file_manager_on_bus = false;
}

static void
set_accel_for_action(GApplication *app, const char *action, const char *accel) {
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), action, (const gchar *const[]){accel, NULL});
}

static void
set_accels_for_action(GApplication *app, const char *action, const gchar *const *accels) {
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), action, accels);
}

static void
set_accels_for_escape(GApplication *app) {
    FsearchApplication *fsearch = FSEARCH_APPLICATION(app);

    if (fsearch->config->exit_on_escape) {
        set_accels_for_action(app, "win.hide_window", (const gchar *const[]){NULL});
        set_accels_for_action(app, "app.quit", (const gchar *const[]){"<control>q", "Escape", NULL});
    }
    else {
        set_accel_for_action(app, "win.hide_window", "Escape");
        set_accel_for_action(app, "app.quit", "<control>q");
    }
}

static void
fsearch_application_startup(GApplication *app) {
    g_assert(FSEARCH_IS_APPLICATION(app));
    G_APPLICATION_CLASS(fsearch_application_parent_class)->startup(app);

    FsearchApplication *fsearch = FSEARCH_APPLICATION(app);

    g_mutex_init(&fsearch->mutex);
    config_make_dir();

    char data_dir[PATH_MAX] = "";
    fsearch_file_utils_init_data_dir_path(data_dir, sizeof(data_dir));
    fsearch_file_utils_create_dir(data_dir);

    fsearch->db_thread_cancellable = g_cancellable_new();
    fsearch->config = calloc(1, sizeof(FsearchConfig));
    g_assert(fsearch->config);
    if (!config_load(fsearch->config)) {
        config_load_default(fsearch->config);
    }
    fsearch->db = NULL;
    fsearch->db_state = FSEARCH_DATABASE_STATE_IDLE;

    fsearch->file_manager_watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION,
                                                      "org.freedesktop.FileManager1",
                                                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                      on_file_manager_name_appeared,
                                                      on_file_manager_name_vanished,
                                                      NULL,
                                                      NULL);

    g_autoptr(GtkCssProvider) provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, "/io/github/cboxdoerfer/FSearch/ui/shared.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", fsearch->config->enable_dark_theme, NULL);

    if (fsearch->config->show_menubar) {
        g_autoptr(GtkBuilder) menu_builder = gtk_builder_new_from_resource("/io/github/cboxdoerfer/FSearch/ui/"
                                                                           "menus.ui");
        GMenuModel *menu_model = G_MENU_MODEL(gtk_builder_get_object(menu_builder, "fsearch_main_menu"));
        gtk_application_set_menubar(GTK_APPLICATION(app), menu_model);
    }

    if (!fsearch->config->show_menubar) {
        // When the menubar is shown, F10 is already set to open the first menu in the menubar.
        // So we only want to override the F10 action when the menu bar is hidden.
        set_accel_for_action(app, "win.toggle_app_menu", "F10");
    }
    set_accel_for_action(app, "win.toggle_focus", "Tab");
    set_accel_for_action(app, "win.focus_search", "<control>f");
    set_accel_for_action(app, "app.new_window", "<control>n");
    set_accel_for_action(app, "win.select_all", "<control>a");
    set_accel_for_action(app, "win.match_case", "<control>i");
    set_accel_for_action(app, "win.search_mode", "<control>r");
    set_accel_for_action(app, "win.search_in_path", "<control>u");
    set_accel_for_action(app, "app.update_database", "<control><shift>r");
    set_accel_for_action(app, "app.preferences(uint32 0)", "<control>p");
    set_accel_for_action(app, "win.close_window", "<control>w");
    set_accel_for_action(app, "app.help", "F1");
    set_accels_for_escape(app);

    fsearch->db_pool = g_thread_pool_new(database_pool_func, app, 1, TRUE, NULL);
    fsearch->is_shutting_down = false;
}

static GActionEntry fsearch_app_entries[] = {
    {"new_window", action_new_window_activated, NULL, NULL, NULL},
    {"about", action_about_activated, NULL, NULL, NULL},
    {"online_help", action_online_help_activated, NULL, NULL, NULL},
    {"help", action_help_activated, NULL, NULL, NULL},
    {"donate_paypal", action_donate_paypal_activated, NULL, NULL, NULL},
    {"donate_github", action_donate_github_activated, NULL, NULL, NULL},
    {"bug_report", action_bug_report_activated, NULL, NULL, NULL},
    {"forum", action_forum_activated, NULL, NULL, NULL},
    {"update_database", action_update_database_activated, NULL, NULL, NULL},
    {"cancel_update_database", action_cancel_update_database_activated, NULL, NULL, NULL},
    {"preferences", action_preferences_activated, "u", NULL, NULL},
    {"quit", action_quit_activated, NULL, NULL, NULL}};

static void
fsearch_application_init(FsearchApplication *app) {
    g_action_map_add_action_entries(G_ACTION_MAP(app), fsearch_app_entries, G_N_ELEMENTS(fsearch_app_entries), app);
}

static void
fsearch_application_activate(GApplication *app) {
    g_assert(FSEARCH_IS_APPLICATION(app));

    FsearchApplication *self = FSEARCH_APPLICATION(app);

    if (!self->new_window) {
        // If there's already a window make it visible
        FsearchApplicationWindow *window = get_first_application_window(FSEARCH_APPLICATION(app));
        if (window) {
            move_search_term_to_window(self, window);
            fsearch_application_window_focus_search_entry(FSEARCH_APPLICATION_WINDOW(window));
            gtk_window_present(GTK_WINDOW(window));
            return;
        }
    }

    g_action_group_activate_action(G_ACTION_GROUP(self), "new_window", NULL);

    database_auto_update_init(self);

    g_cancellable_reset(self->db_thread_cancellable);
    database_scan_or_load_enqueue(FSEARCH_DATABASE_ACTION_LOAD);
    if (self->config->update_database_on_launch) {
        database_scan_or_load_enqueue(FSEARCH_DATABASE_ACTION_SCAN);
    }
}

static gint
fsearch_application_command_line(GApplication *app, GApplicationCommandLine *cmdline) {
    FsearchApplication *self = FSEARCH_APPLICATION(app);
    g_assert(FSEARCH_IS_APPLICATION(self));
    g_assert(G_IS_APPLICATION_COMMAND_LINE(cmdline));

    GVariantDict *dict = g_application_command_line_get_options_dict(cmdline);

    if (g_variant_dict_contains(dict, "new-window")) {
        self->new_window = true;
    }

    if (g_variant_dict_contains(dict, "preferences")) {
        g_action_group_activate_action(G_ACTION_GROUP(self), "preferences", g_variant_new_uint32(0));
        return 0;
    }

    if (g_variant_dict_contains(dict, "update-database")) {
        g_action_group_activate_action(G_ACTION_GROUP(self), "update_database", NULL);
        return 0;
    }

    const gchar *search_term = NULL;
    if (g_variant_dict_lookup(dict, "search", "&s", &search_term)) {
        g_clear_pointer(&self->option_search_term, g_free);
        self->option_search_term = g_strdup(search_term);
    }

    g_application_activate(G_APPLICATION(self));
    self->new_window = false;

    return G_APPLICATION_CLASS(fsearch_application_parent_class)->command_line(app, cmdline);
}

typedef struct {
    GMainLoop *loop;
    bool update_called_on_primary;
} FsearchApplicationDatabaseWorker;

static void
on_action_group_changed(GDBusConnection *connection,
                        const gchar *sender_name,
                        const gchar *object_path,
                        const gchar *interface_name,
                        const gchar *signal_name,
                        GVariant *parameters,
                        gpointer user_data) {
    return;
}

static void
on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    FsearchApplicationDatabaseWorker *worker_ctx = user_data;

    g_autoptr(GDBusActionGroup) dbus_group = g_dbus_action_group_get(connection, fsearch_bus_name, fsearch_object_path);

    const guint signal_id = g_dbus_connection_signal_subscribe(connection,
                                                               fsearch_bus_name,
                                                               "org.gtk.Actions",
                                                               "Changed",
                                                               fsearch_object_path,
                                                               NULL,
                                                               G_DBUS_SIGNAL_FLAGS_NONE,
                                                               on_action_group_changed,
                                                               NULL,
                                                               NULL);

    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(connection,
                                                            fsearch_bus_name,
                                                            fsearch_object_path,
                                                            "org.gtk.Actions",
                                                            "DescribeAll",
                                                            NULL,
                                                            G_VARIANT_TYPE("(a{s(bgav)})"),
                                                            G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                            -1,
                                                            NULL,
                                                            NULL);
    g_dbus_connection_signal_unsubscribe(connection, signal_id);

    if (dbus_group && reply) {
        g_debug("[app] trigger database update in primary instance");
        g_action_group_activate_action(G_ACTION_GROUP(dbus_group), "update_database", NULL);

        worker_ctx->update_called_on_primary = true;
    }
    if (worker_ctx) {
        g_clear_pointer(&worker_ctx->loop, g_main_loop_quit);
    }
}

static void
on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    FsearchApplicationDatabaseWorker *worker_ctx = user_data;
    if (worker_ctx && worker_ctx->loop) {
        g_main_loop_quit(worker_ctx->loop);
    }
}

static int
database_scan_in_local_instance() {
    FsearchConfig *config = calloc(1, sizeof(FsearchConfig));
    g_assert(config);

    if (!config_load(config)) {
        if (!config_load_default(config)) {
            g_printerr("[fsearch] failed to load config\n");
            g_clear_pointer(&config, config_free);
            return EXIT_FAILURE;
        }
    }

    g_autoptr(GTimer) timer = g_timer_new();
    g_timer_start(timer);

    FsearchDatabase *db =
        db_new(config->indexes, config->exclude_locations, config->exclude_files, config->exclude_hidden_items);

    int res = EXIT_FAILURE;
    if (db_scan(db, NULL, NULL)) {
        g_autofree char *db_path = fsearch_application_get_database_dir();
        if (db_path) {
            res = db_save(db, db_path) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    g_clear_pointer(&db, db_unref);
    g_clear_pointer(&config, config_free);

    g_timer_stop(timer);
    const double seconds = g_timer_elapsed(timer, NULL);

    if (res == EXIT_SUCCESS) {
        g_print("[fsearch] database update finished successfully in %.2f seconds\n", seconds);
    }
    else {
        g_printerr("[fsearch] database update failed\n");
    }

    return res;
}

static int
fsearch_application_local_database_scan() {
    // First detect if the another instance of fsearch is already registered
    // If yes, trigger update there, so the UI is aware of the update and can display its progress
    FsearchApplicationDatabaseWorker worker_ctx = {};
    worker_ctx.loop = g_main_loop_new(NULL, FALSE);
    const guint owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                          fsearch_db_worker_bus_name,
                                          G_BUS_NAME_OWNER_FLAGS_NONE,
                                          NULL,
                                          on_name_acquired,
                                          on_name_lost,
                                          &worker_ctx,
                                          NULL);
    g_main_loop_run(worker_ctx.loop);
    g_bus_unown_name(owner_id);

    if (worker_ctx.update_called_on_primary) {
        // triggered update in primary instance, we're done here
        return 0;
    }
    else {
        // no primary instance found, perform update
        return database_scan_in_local_instance();
    }
}

static gint
fsearch_application_handle_local_options(GApplication *application, GVariantDict *options) {
    if (g_variant_dict_contains(options, "update-database")) {
        return fsearch_application_local_database_scan();
    }
    if (g_variant_dict_contains(options, "version")) {
        g_autoptr(GString) version = get_application_version();
        g_print("FSearch %s\n", version->str);
        return 0;
    }

    return -1;
}

static void
fsearch_application_add_option_entries(FsearchApplication *self) {
    static const GOptionEntry main_entries[] = {
        {"new-window", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Open a new application window")},
        {"preferences", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Show the application preferences")},
        {"search", 's', 0, G_OPTION_ARG_STRING, NULL, N_("Set the search pattern"), "PATTERN"},
        {"update-database", 'u', 0, G_OPTION_ARG_NONE, NULL, N_("Update the database and exit")},
        {"version", 'v', 0, G_OPTION_ARG_NONE, NULL, N_("Print version information and exit")},
        {NULL}};

    g_assert(FSEARCH_IS_APPLICATION(self));

    g_application_add_main_option_entries(G_APPLICATION(self), main_entries);
}

static void
fsearch_application_win_added(GtkApplication *app, GtkWindow *win) {
    GTK_APPLICATION_CLASS(fsearch_application_parent_class)->window_added(app, win);
    fsearch_application_window_added(FSEARCH_APPLICATION_WINDOW(win), FSEARCH_APPLICATION(app));
}

static void
fsearch_application_win_removed(GtkApplication *app, GtkWindow *win) {
    fsearch_application_window_removed(FSEARCH_APPLICATION_WINDOW(win), FSEARCH_APPLICATION(app));
    GTK_APPLICATION_CLASS(fsearch_application_parent_class)->window_removed(app, win);
}

static void
fsearch_application_class_init(FsearchApplicationClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GApplicationClass *g_app_class = G_APPLICATION_CLASS(klass);
    GtkApplicationClass *gtk_app_class = GTK_APPLICATION_CLASS(klass);

    object_class->finalize = fsearch_application_finalize;

    g_app_class->activate = fsearch_application_activate;
    g_app_class->startup = fsearch_application_startup;
    g_app_class->shutdown = fsearch_application_shutdown;
    g_app_class->command_line = fsearch_application_command_line;
    g_app_class->handle_local_options = fsearch_application_handle_local_options;

    gtk_app_class->window_added = fsearch_application_win_added;
    gtk_app_class->window_removed = fsearch_application_win_removed;

    fsearch_signals[FSEARCH_SIGNAL_DATABASE_SCAN_STARTED] = g_signal_new("database-scan-started",
                                                                         G_TYPE_FROM_CLASS(klass),
                                                                         G_SIGNAL_RUN_LAST,
                                                                         0,
                                                                         NULL,
                                                                         NULL,
                                                                         NULL,
                                                                         G_TYPE_NONE,
                                                                         0);

    fsearch_signals[FSEARCH_SIGNAL_DATABASE_UPDATE_FINISHED] = g_signal_new("database-update-finished",
                                                                            G_TYPE_FROM_CLASS(klass),
                                                                            G_SIGNAL_RUN_LAST,
                                                                            0,
                                                                            NULL,
                                                                            NULL,
                                                                            NULL,
                                                                            G_TYPE_NONE,
                                                                            0);
    fsearch_signals[FSEARCH_SIGNAL_DATABASE_LOAD_STARTED] = g_signal_new("database-load-started",
                                                                         G_TYPE_FROM_CLASS(klass),
                                                                         G_SIGNAL_RUN_LAST,
                                                                         0,
                                                                         NULL,
                                                                         NULL,
                                                                         NULL,
                                                                         G_TYPE_NONE,
                                                                         0);
}

// Public functions

void
fsearch_application_state_lock(FsearchApplication *fsearch) {
    g_assert(FSEARCH_IS_APPLICATION(fsearch));
    g_mutex_lock(&fsearch->mutex);
}

void
fsearch_application_state_unlock(FsearchApplication *fsearch) {
    g_assert(FSEARCH_IS_APPLICATION(fsearch));
    g_mutex_unlock(&fsearch->mutex);
}

FsearchDatabaseState
fsearch_application_get_db_state(FsearchApplication *fsearch) {
    g_assert(FSEARCH_IS_APPLICATION(fsearch));
    return fsearch->db_state;
}

uint32_t
fsearch_application_get_num_db_entries(FsearchApplication *fsearch) {
    g_assert(FSEARCH_IS_APPLICATION(fsearch));
    return fsearch->db ? db_get_num_entries(fsearch->db) : 0;
}

FsearchDatabase *
fsearch_application_get_db(FsearchApplication *fsearch) {
    g_assert(FSEARCH_IS_APPLICATION(fsearch));
    return db_ref(fsearch->db);
}

FsearchConfig *
fsearch_application_get_config(FsearchApplication *fsearch) {
    g_assert(FSEARCH_IS_APPLICATION(fsearch));
    return fsearch->config;
}

char *
fsearch_application_get_database_file_path() {
    GString *file_path = g_string_new(g_get_user_data_dir());
    g_string_append_c(file_path, G_DIR_SEPARATOR);
    g_string_append(file_path, "fsearch");
    g_string_append_c(file_path, G_DIR_SEPARATOR);
    g_string_append(file_path, "fsearch.db");

    return g_string_free(file_path, FALSE);
}

char *
fsearch_application_get_database_dir() {
    GString *db_dir = g_string_new(g_get_user_data_dir());
    g_string_append_c(db_dir, G_DIR_SEPARATOR);
    g_string_append(db_dir, "fsearch");
    return g_string_free(db_dir, FALSE);
}

gboolean
fsearch_application_has_file_manager_on_bus(FsearchApplication *fsearch) {
    g_assert(FSEARCH_IS_APPLICATION(fsearch));
    return fsearch->has_file_manager_on_bus;
}

FsearchApplication *
fsearch_application_new(void) {
    FsearchApplication *self = g_object_new(FSEARCH_APPLICATION_TYPE,
                                            "application-id",
                                            fsearch_bus_name,
                                            "flags",
                                            G_APPLICATION_HANDLES_COMMAND_LINE,
                                            NULL);
    fsearch_application_add_option_entries(self);
    return self;
}
