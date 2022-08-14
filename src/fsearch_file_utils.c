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

#define G_LOG_DOMAIN "fsearch-utils"

#define _GNU_SOURCE

#include "fsearch_file_utils.h"
#include "fsearch_limits.h"
#include "fsearch_string_utils.h"
#include "fsearch_ui_utils.h"
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

const char *data_folder_name = "fsearch";

static void
add_error_message_with_format(GString *error_messages, const char *description, const char *item_name, const char *reason) {
    if (!error_messages || !description || !item_name || !reason) {
        return;
    }
    g_autoptr(GString) error_message = g_string_new(NULL);
    g_string_printf(error_message, "%s \"%s\": %s", description, item_name, reason);
    g_string_append(error_messages, error_message->str);
    g_string_append_c(error_messages, '\n');
}

static void
add_error_message(GString *error_messages, const char *error_message) {
    if (!error_messages || !error_message) {
        return;
    }
    g_string_append(error_messages, error_message);
    g_string_append_c(error_messages, '\n');
}

void
fsearch_file_utils_init_data_dir_path(char *path, size_t len) {
    g_assert(path);
    g_assert(len >= 0);

    const gchar *xdg_data_dir = g_get_user_data_dir();
    snprintf(path, len, "%s/%s", xdg_data_dir, data_folder_name);
}

bool
fsearch_file_utils_create_dir(const char *path) {
    return !g_mkdir_with_parents(path, 0700);
}

bool
fsearch_file_utils_is_desktop_file(const char *path) {
    const char *uri_extension = fsearch_string_get_extension(path);
    if (uri_extension && !strcmp(uri_extension, "desktop")) {
        return true;
    }
    return false;
}

static gboolean
keyword_eval_cb(const GMatchInfo *info, GString *res, gpointer data) {
    g_autofree gchar *match = g_match_info_fetch(info, 0);
    if (!match) {
        return FALSE;
    }
    gchar *r = g_hash_table_lookup((GHashTable *)data, match);
    if (r) {
        g_string_append(res, r);
    }

    return FALSE;
}

static char *
build_folder_open_cmd(const char *path, const char *path_full, const char *cmd) {
    if (!path || !path_full) {
        return NULL;
    }
    g_autofree char *path_quoted = g_shell_quote(path);
    g_autofree char *path_full_quoted = g_shell_quote(path_full);

    // The following code is mostly based on the example code found here:
    // https://developer.gnome.org/glib/stable/glib-Perl-compatible-regular-expressions.html#g-regex-replace-eval
    //
    // Create hash table which hold all valid keywords as keys
    // and their replacements as values
    // All valid keywords are:
    // - {path_raw}
    //     The raw path of a file or folder. E.g. the path of /foo/bar is /foo
    // - {path_full_raw}
    //     The raw full path of a file or folder. E.g. the full path of /foo/bar
    //     is /foo/bar
    // - {path} and {path_full}
    //     Those are the same as {path_raw} and {path_full_raw} but they get
    //     properly escaped and quoted for the usage in shells. E.g. /foo/'bar
    //     becomes '/foo/'\''bar'

    g_autoptr(GHashTable) keywords = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(keywords, "{path_raw}", (char *)path);
    g_hash_table_insert(keywords, "{path_full_raw}", (char *)path_full);
    g_hash_table_insert(keywords, "{path}", path_quoted);
    g_hash_table_insert(keywords, "{path_full}", path_full_quoted);

    // Regular expression which matches multiple words (and underscores)
    // surrounded with {}
    g_autoptr(GRegex) reg = g_regex_new("{[\\w]+}", 0, 0, NULL);
    // Replace all the matched keywords
    return g_regex_replace_eval(reg, cmd, -1, 0, 0, keyword_eval_cb, keywords, NULL);
}

static bool
open_with_cmd(const char *path, const char *path_full, const char *cmd, GString *error_message) {
    const char *error_description = C_("Will be followed by the path of the folder.", "Error while opening folder");
    g_autofree char *cmd_res = build_folder_open_cmd(path, path_full, cmd);
    if (!cmd_res) {
        add_error_message_with_format(error_message, error_description, path_full, _("Failed to build open command"));
        return false;
    }

    g_autoptr(GError) error = NULL;
    if (!g_spawn_command_line_async(cmd_res, &error)) {
        add_error_message(error_message, error->message);
        return false;
    }

    return true;
}

static bool
open_application_for_path(const char *path, GString *error_messages) {
    const char *error_description = C_("Will be followed by the path of the desktop file.",
                                       "Error when launching desktop file");
    g_autoptr(GAppInfo) info = (GAppInfo *)g_desktop_app_info_new_from_filename(path);
    if (!info) {
        add_error_message_with_format(error_messages, error_description, path, _("Failed to get application information"));
        return false;
    }

    GdkDisplay *display = gdk_display_get_default();
    if (!display) {
        add_error_message_with_format(error_messages, error_description, path, _("Failed to get default display"));
        return false;
    }

    g_autoptr(GAppLaunchContext) context = (GAppLaunchContext *)gdk_display_get_app_launch_context(display);

    g_autoptr(GError) error = NULL;
    g_app_info_launch(info, NULL, context, &error);

    if (error) {
        add_error_message(error_messages, error->message);
        return false;
    }

    return true;
}

static bool
file_remove_or_trash(const char *path, bool delete, GString *error_messages) {
    g_autoptr(GFile) file = g_file_new_for_path(path);
    if (!file) {
        add_error_message_with_format(error_messages,
                                      C_("Will be followed by the path of the file.", "Error when removing file"),
                                      path,
                                      _("Failed to get path"));
        return false;
    }
    g_autoptr(GError) error = NULL;
    if (delete) {
        g_file_delete(file, NULL, &error);
    }
    else {
        g_file_trash(file, NULL, &error);
    }

    if (error) {
        add_error_message(error_messages, error->message);
        return false;
    }
    return true;
}

bool
fsearch_file_utils_remove(const char *path, GString *error_messages) {
    return file_remove_or_trash(path, true, error_messages);
}

bool
fsearch_file_utils_trash(const char *path, GString *error_messages) {
    return file_remove_or_trash(path, false, error_messages);
}

bool
fsearch_file_utils_open_path(const char *path,
                             bool launch_desktop_files,
                             GAppLaunchContext *launch_context,
                             GString *error_message) {
    g_return_val_if_fail(path, false);

    GList *list = g_list_append(NULL, g_strdup(path));
    const bool res = fsearch_file_utils_open_path_list(list, launch_desktop_files, launch_context, error_message);
    g_list_free_full(list, g_free);
    return res;
}

typedef struct {
    GHashTable *content_types;
    GString *error_messages;
    GAppLaunchContext *launch_context;
    bool launch_desktop_applications;
} FsearchFileUtilsLaunchContext;

static void
launch_for_content_type(const char *content_type, GPtrArray *files, FsearchFileUtilsLaunchContext *ctx) {
    g_return_if_fail(content_type);
    g_return_if_fail(files);

    if (ctx->launch_desktop_applications && g_strcmp0(content_type, "application/x-desktop") == 0) {
        // If requested, desktop files will launch their corresponding applications
        for (uint32_t i = 0; i < files->len; ++i) {
            GFile *file = g_ptr_array_index(files, i);
            g_autofree char *path = g_file_get_path(file);
            if (!path) {
                add_error_message_with_format(ctx->error_messages,
                                              C_("Will be followed by the path of the desktop file",
                                                 "Error when launching desktop file"),
                                              path,
                                              _("Failed to get path"));
                continue;
            }
            open_application_for_path(path, ctx->error_messages);
        }
        return;
    }

    g_autoptr(GAppInfo) info = g_app_info_get_default_for_type(content_type, FALSE);
    if (!info) {
        add_error_message_with_format(ctx->error_messages,
                                      C_("Will be followed by the content type string.",
                                         "Error when getting information for content type"),
                                      content_type,
                                      _("No default application registered"));
        return;
    }

    GList *uri_list = NULL;
    for (uint32_t i = 0; i < files->len; ++i) {
        GFile *file = g_ptr_array_index(files, i);
        char *uri = g_file_get_uri(file);
        if (uri) {
            uri_list = g_list_append(uri_list, uri);
            g_debug("[open] opening file/folder:\nuri: \"%s\"\ncontent type: \"%s\"\napplication: \"%s\"\ncmd: "
                    "\"%s\"",
                    uri,
                    content_type,
                    g_app_info_get_display_name(info),
                    g_app_info_get_commandline(info));
        }
    }

    g_autoptr(GError) error = NULL;
    if (!g_app_info_launch_uris(info, uri_list, ctx->launch_context, &error)) {
        add_error_message(ctx->error_messages, error->message);
    }
    g_list_free_full(uri_list, g_free);
}

static void
collect_for_content_type(GHashTable *content_types, const char *path_full, GString *error_message) {
    g_return_if_fail(path_full);
    g_return_if_fail(content_types);

    g_autoptr(GError) error = NULL;
    g_autofree char *content_type = fsearch_file_utils_get_content_type(path_full, &error);
    if (!content_type) {
        add_error_message(error_message, error->message);
        return;
    }

    if (g_hash_table_contains(content_types, content_type)) {
        // This content type was already added to the hash table.
        // Add this file to the corresponding array.
        GPtrArray *uris = g_hash_table_lookup(content_types, content_type);
        if (uris) {
            g_ptr_array_add(uris, g_file_new_for_path(path_full));
        }
    }
    else {
        // This content type hasn't been handled before.
        // We create a new array to hold its files and add it to the hash table.
        GPtrArray *uris = g_ptr_array_new_with_free_func(g_object_unref);
        g_ptr_array_add(uris, g_file_new_for_path(path_full));
        g_hash_table_insert(content_types, g_strdup(content_type), uris);
    }
}

static bool
app_is_sandboxed(void) {
    static bool is_sandboxed = false;
    static gsize initialization_value = 0;

    if (g_once_init_enter(&initialization_value)) {
        g_auto(GStrv) env = g_get_environ();
        if (g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS)) {
            is_sandboxed = true;
        }
        else if (g_environ_getenv(env, "SNAP")) {
            is_sandboxed = true;
        }

        g_once_init_leave(&initialization_value, 1);
    }
    return is_sandboxed;
}

static bool
launch_default_for_path(GList *paths, GAppLaunchContext *launch_context, GString *error_message) {
    for (GList *p = paths; p != NULL; p = p->next) {
        g_autoptr(GFile) file = g_file_new_for_path(p->data);
        g_autofree char *uri = g_file_get_uri(file);
        if (uri) {
            g_autoptr(GError) error = NULL;
            g_app_info_launch_default_for_uri(uri, launch_context, &error);
            if (error) {
                add_error_message(error_message, error->message);
            }
        }
    }
    return error_message->len > 0 ? false : true;
}

bool
fsearch_file_utils_open_path_list(GList *paths,
                                  bool launch_desktop_files,
                                  GAppLaunchContext *launch_context,
                                  GString *error_message) {
    g_return_val_if_fail(paths, false);

    if (app_is_sandboxed()) {
        g_debug("[open_path_list] FSearch is sandboxed. Ask the system to open the files for us.");
        return launch_default_for_path(paths, launch_context, error_message);
    }

    g_autoptr(GHashTable)
        content_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);

    for (GList *p = paths; p != NULL; p = p->next) {
        char *path = p->data;
        collect_for_content_type(content_types, path, error_message);
    }
    FsearchFileUtilsLaunchContext ctx = {.launch_context = launch_context,
                                         .error_messages = error_message,
                                         .launch_desktop_applications = launch_desktop_files};
    g_hash_table_foreach(content_types, (GHFunc)launch_for_content_type, &ctx);
    return error_message->len > 0 ? false : true;
}

bool
fsearch_file_utils_open_parent_folder_with_optional_command(const char *path,
                                                            const char *cmd,
                                                            GAppLaunchContext *launch_context,
                                                            GString *error_message) {
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFile) parent = g_file_get_parent(file);
    // If file has no parent it means it's the root directory.
    // We still want to open a folder, so we consider the root directory to be its own parent
    // and open it, instead of doing nothing or failing.
    g_autofree char *parent_path = g_file_get_path(parent ? parent : file);

    if (!parent_path) {
        add_error_message_with_format(error_message,
                                      C_("Will be followed by the path of the folder.",
                                         "Error when opening parent folder"),
                                      path,
                                      _("Failed to get parent path"));
        return false;
    }

    if (cmd) {
        return open_with_cmd(parent_path, path, cmd, error_message);
    }
    else {
        return fsearch_file_utils_open_path(parent_path, false, launch_context, error_message);
    }
}

bool
fsearch_file_utils_open_parent_folder_with_optional_command_from_path_list(GList *paths,
                                                                           const char *cmd,
                                                                           GAppLaunchContext *launchContext,
                                                                           GString *error_message) {
    g_return_val_if_fail(paths, false);

    for (GList *p = paths; p != NULL; p = p->next) {
        const char *path = p->data;
        fsearch_file_utils_open_parent_folder_with_optional_command(path, cmd, launchContext, error_message);
    }
    return true;
}

static gchar *
get_content_type_description(const gchar *name) {
    if (!name) {
        return NULL;
    }
    g_autofree gchar *content_type = g_content_type_guess(name, NULL, 0, NULL);
    if (!content_type) {
        return NULL;
    }
    return g_content_type_get_description(content_type);
}

gchar *
fsearch_file_utils_get_file_type_non_localized(const char *name, gboolean is_dir) {
    gchar *type = NULL;
    if (is_dir) {
        type = g_strdup("Folder");
    }
    else {
        type = get_content_type_description(name);
    }
    if (type == NULL) {
        type = g_strdup("Unknown Type");
    }
    return type;
}

gchar *
fsearch_file_utils_get_file_type(const char *name, gboolean is_dir) {
    gchar *type = NULL;
    if (is_dir) {
        type = g_strdup(_("Folder"));
    }
    else {
        type = get_content_type_description(name);
    }
    if (type == NULL) {
        type = g_strdup(_("Unknown Type"));
    }
    return type;
}

#define DEFAULT_FILE_ICON_NAME "application-octet-stream"

GIcon *
fsearch_file_utils_get_desktop_file_icon(const char *path) {
    g_autoptr(GAppInfo) info = (GAppInfo *)g_desktop_app_info_new_from_filename(path);
    if (!info) {
        goto default_icon;
    }

    GIcon *icon = g_app_info_get_icon(info);
    if (!icon) {
        goto default_icon;
    }

    return g_object_ref(icon);

default_icon:
    return g_themed_icon_new("application-x-executable");
}

GIcon *
fsearch_file_utils_guess_icon(const char *name, const char *path, bool is_dir) {
    if (is_dir) {
        return g_themed_icon_new("folder");
    }

    if (fsearch_file_utils_is_desktop_file(name)) {
        return fsearch_file_utils_get_desktop_file_icon(path);
    }

    g_autofree gchar *content_type = g_content_type_guess(name, NULL, 0, NULL);
    if (!content_type) {
        return g_themed_icon_new(DEFAULT_FILE_ICON_NAME);
    }

    GIcon *icon = g_content_type_get_icon(content_type);

    return icon ? icon : g_themed_icon_new(DEFAULT_FILE_ICON_NAME);
}

GIcon *
fsearch_file_utils_get_icon_for_path(const char *path) {
    g_autoptr(GFile) g_file = g_file_new_for_path(path);
    if (!g_file) {
        return g_themed_icon_new("edit-delete");
    }

    g_autoptr(GFileInfo) file_info = g_file_query_info(g_file, "standard::icon", 0, NULL, NULL);
    if (!file_info) {
        return g_themed_icon_new("edit-delete");
    }

    GIcon *icon = g_file_info_get_icon(file_info);
    return g_object_ref(icon);
}

char *
fsearch_file_utils_get_size_formatted(off_t size, bool show_base_2_units) {
    if (show_base_2_units) {
        return g_format_size_full(size, G_FORMAT_SIZE_IEC_UNITS);
    }
    else {
        return g_format_size_full(size, G_FORMAT_SIZE_DEFAULT);
    }
}

char *
fsearch_file_utils_get_content_type(const char *path, GError **error) {
    g_assert(path);
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo)
        info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_QUERY_INFO_NONE, NULL, error);
    if (!info) {
        return NULL;
    }
    const char *content_type = g_file_info_get_content_type(info);
    return content_type ? g_strdup(content_type) : NULL;
}