/*
 * Copyright (c) 2021-2022  Martin Lund
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <lxi.h>
#include <ctype.h>
#include "config.h"
#include "lxi_gui-window.h"
#include "screenshot.h"
#include "benchmark.h"
#include "misc.h"
#include "lxilua.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <locale.h>
#include <gtksourceview/gtksource.h>
#include <adwaita.h>
#include "gtkchart.h"
#include "lxi_gui-resources.h"

static lxi_info_t info;

struct _LxiGuiWindow
{
    GtkApplicationWindow  parent_instance;

    /* Template widgets */
    GSettings           *settings;
    GtkListBox          *list_instruments;
    GMenuModel          *list_widget_menu_model;
    GtkWidget           *list_widget_popover_menu;
    GtkViewport         *list_viewport;
    GdkClipboard        *clipboard;
    GtkEntry            *entry_scpi;
    GtkTextView         *text_view_scpi;
    GtkToggleButton     *toggle_button_scpi_send;
    GtkPicture          *picture_screenshot;
    GtkToggleButton     *toggle_button_screenshot_grab;
    GtkButton           *button_screenshot_save;
    GThread             *screenshot_worker_thread;
    GThread             *screenshot_grab_worker_thread;
    GThread             *search_worker_thread;
    GThread             *send_worker_thread;
    GtkProgressBar      *progress_bar_benchmark;
    GThread             *benchmark_worker_thread;
    GtkToggleButton     *toggle_button_benchmark_start;
    GtkToggleButton     *toggle_button_search;
    GtkSpinButton       *spin_button_benchmark_requests;
    GtkLabel            *label_benchmark_result;
    GtkImage            *image_benchmark;
    GdkPixbuf           *pixbuf_screenshot;
    GtkSourceView       *source_view_script;
    GtkTextView         *text_view_script_status;
    GThread             *script_run_worker_thread;
    GtkInfoBar          *info_bar;
    GtkLabel            *label_info_bar;
    GtkViewport         *viewport_screenshot;
    GtkToggleButton     *toggle_button_script_run;
    AdwFlap             *flap;
    AdwStatusPage       *status_page_instruments;
    unsigned int        benchmark_requests_count;
    const char          *id;
    const char          *ip;
    char                *image_buffer;
    int                 image_size;
    char                image_format[10];
    char                image_filename[1000];
    GFile               *script_file;
    lua_State           *L;
    gboolean            screenshot_ready;
    gboolean            screenshot_loaded;
    int                 screenshot_size;
    double              progress_bar_fraction;
    char                *benchmark_result_text;
    gboolean            lua_stop_requested;
    GMutex              mutex_gui_chart;
    GMutex              mutex_discover;
    GMutex              mutex_save_png;
    GMutex              mutex_save_csv;
    bool                no_instruments;
};

G_DEFINE_TYPE (LxiGuiWindow, lxi_gui_window, GTK_TYPE_APPLICATION_WINDOW)

static LxiGuiWindow *self_global;

#define CHARTS_MAX 1024

struct chart_t
{
    bool allocated;
    int handle;
    GtkChartType type;
    char *title;
    char *label;
    char *x_label;
    char *y_label;
    double x;
    double y;
    double x_max;
    double y_max;
    double value;
    double value_min;
    double value_max;
    int width;
    bool autoscale;
    bool no_csv;
    GtkWidget *widget;
    GtkWindow *window;
    char *filename_png;
    char *filename_csv;
};

static struct chart_t gui_chart[CHARTS_MAX];

struct show_data_t
{
    LxiGuiWindow *self;
    gchar *buffer;
};

static gboolean show_error_thread(gpointer user_data)
{
    struct show_data_t *data = user_data;

    // Show error message
    gtk_label_set_text (GTK_LABEL(data->self->label_info_bar), data->buffer);
    gtk_info_bar_set_message_type(data->self->info_bar, GTK_MESSAGE_ERROR);
    gtk_info_bar_set_show_close_button(data->self->info_bar, true);
    gtk_widget_show (GTK_WIDGET(data->self->info_bar));

    g_free(data->buffer);
    return G_SOURCE_REMOVE;
}

static void show_error(LxiGuiWindow *self, const char *buffer)
{
    struct show_data_t *data = g_new0(struct show_data_t, 1);
    data->self = self;
    data->buffer = g_strdup(buffer);
    g_idle_add(show_error_thread, data);
}

static gboolean show_info_thread(gpointer user_data)
{
    struct show_data_t *data = user_data;

    // Show info message
    gtk_label_set_text (GTK_LABEL(data->self->label_info_bar), data->buffer);
    gtk_info_bar_set_message_type(data->self->info_bar, GTK_MESSAGE_INFO);
    gtk_info_bar_set_show_close_button(data->self->info_bar, false);
    gtk_widget_show (GTK_WIDGET(data->self->info_bar));

    g_free(data->buffer);
    return G_SOURCE_REMOVE;
}

static void show_info(LxiGuiWindow *self, const char *buffer)
{
    struct show_data_t *data = g_new0(struct show_data_t, 1);
    data->self = self;
    data->buffer = g_strdup(buffer);
    g_idle_add(show_info_thread, data);
}

static void hide_info_bar(LxiGuiWindow *self)
{
    gtk_widget_hide(GTK_WIDGET(self->info_bar));
}

static GtkWidget* find_child_by_name(GtkWidget* parent, const gchar* name)
{
    GtkWidget *child;
    GList *children = NULL;

    g_assert(GTK_IS_WIDGET(parent) == true);

    if (g_strcmp0(gtk_widget_get_name(parent), name) == 0)
        return parent;

    for (child = gtk_widget_get_first_child(parent);
            child != NULL;
            child = gtk_widget_get_next_sibling(child))
    {
        children = g_list_append(children, child);
    }

    while (children != NULL)
    {
        GtkWidget* widget = find_child_by_name(children->data, name);

        if (widget != NULL)
            return widget;

        children = children->next;
    }

    return NULL;
}

static void pressed_cb (GtkGestureClick *gesture,
        guint            n_press,
        double           x,
        double           y,
        LxiGuiWindow     *self)
{
    GtkWidget *child;
    GtkListBoxRow *row;
    GtkAdjustment* adjustment;
    double y_adjustment, y_adjusted;

    UNUSED(gesture);
    UNUSED(n_press);

    // Adjust y value to account for scrolling offset so we can pick the right row
    adjustment = gtk_list_box_get_adjustment(self->list_instruments);
    y_adjustment = gtk_adjustment_get_value(adjustment);
    y_adjusted = y + y_adjustment;
    row = gtk_list_box_get_row_at_y(self->list_instruments, y_adjusted);

    if (row != NULL)
    {
        child = find_child_by_name(GTK_WIDGET(row), "list-title");
        if (child != NULL)
        {
            // Save IP selected via GUI
            self->ip = gtk_label_get_text(GTK_LABEL(child));
        }

        child = find_child_by_name(GTK_WIDGET(row), "list-subtitle");
        if (child != NULL)
        {
            // Save ID selected via GUI
            self->id = gtk_label_get_text(GTK_LABEL(child));
        }

        // If right click
        if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) == GDK_BUTTON_SECONDARY)
        {
            /* Place our popup menu at the point where
             * the click happened, before popping it up.
             */
            gtk_popover_set_pointing_to (GTK_POPOVER (self->list_widget_popover_menu),
                    &(const GdkRectangle){ x, y, 1, 1 });
            gtk_popover_popup (GTK_POPOVER (self->list_widget_popover_menu));
        }
    }
}

static void action_cb (GtkWidget  *widget,
        const char *action_name,
        GVariant   *parameter)
{
    LxiGuiWindow *self = LXI_GUI_WINDOW (widget);

    UNUSED(parameter);

    if (g_str_equal (action_name, "action.copy_ip"))
    {
        gdk_clipboard_set (self->clipboard, G_TYPE_STRING, self->ip);
    }

    if (g_str_equal (action_name, "action.copy_id"))
    {
        gdk_clipboard_set (self->clipboard, G_TYPE_STRING, self->id);
    }

    if (g_str_equal (action_name, "action.open_browser") && self->ip != NULL)
    {
#ifndef __APPLE__
        gchar *uri = g_strconcat("http://", self->ip, NULL);
        gtk_show_uri(GTK_WINDOW(self), uri, GDK_CURRENT_TIME);
#else
        gchar *uri = g_strconcat("open http://", self->ip, NULL);
        system(uri);
#endif
        g_free(uri);
    }
}

static gboolean gui_update_search_add_instrument_thread(gpointer data)
{
    GtkWidget *list_box = data;

    // Add list box to list (GtkListBoxRow automatically inserted inbetween)
    gtk_list_box_append(self_global->list_instruments, list_box);

    g_mutex_unlock(&self_global->mutex_discover);

    return G_SOURCE_REMOVE;
}

/* Add instrument to list */
static void list_add_instrument (LxiGuiWindow *self, const char *ip, const char *id)
{
    UNUSED(self);

    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *list_text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *list_title = gtk_label_new(ip);
    GtkWidget *list_subtitle = gtk_label_new (id);

    // Set properties of list box
    gtk_widget_set_size_request(list_box, -1, 60);

    // Set properties of list text box
    gtk_widget_set_hexpand(list_text_box, true);
    gtk_widget_set_hexpand_set(list_text_box, true);
    gtk_widget_set_margin_top(list_text_box, 8);
    gtk_widget_set_margin_end(list_text_box, 5);
    gtk_widget_set_halign(list_text_box, GTK_ALIGN_START);

    // Add image to list box
    GtkWidget *image = gtk_image_new_from_resource("/io/github/lxi-tools/lxi-gui/icons/lxi-instrument.png");
    gtk_widget_set_margin_start(image, 2);
    gtk_widget_set_margin_end(image, 2);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 50);
    gtk_box_append(GTK_BOX(list_box), image);

    // Add title to list text box
    gtk_widget_set_name(list_title, "list-title");
    gtk_widget_set_halign(list_title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(list_text_box), list_title);

    // Add subtitle to list text box
    gtk_widget_set_name(list_subtitle, "list-subtitle");
    gtk_widget_add_css_class(list_subtitle, "subtitle");
    GtkStyleContext *context = gtk_widget_get_style_context (GTK_WIDGET (list_subtitle));
    GtkCssProvider *provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (provider, "label.subtitle {opacity: 1; font-size: x-small;}", -1);
    gtk_style_context_add_provider (context, GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
    gtk_widget_set_vexpand(list_subtitle, true);
    gtk_widget_set_vexpand_set(list_subtitle, true);
    gtk_widget_set_valign(list_subtitle, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(list_subtitle), true);
    //  gtk_label_set_natural_wrap_mode(GTK_LABEL(list_subtitle), GTK_NATURAL_WRAP_NONE);
    gtk_label_set_wrap_mode(GTK_LABEL(list_subtitle), PANGO_WRAP_CHAR);
    gtk_box_append(GTK_BOX(list_text_box), list_subtitle);

    // Add text box to list box
    gtk_box_append(GTK_BOX(list_box), list_text_box);

    // Add list box to instrument list
    g_idle_add(gui_update_search_add_instrument_thread, list_box);

    // Mark instrument list populated
    self->no_instruments = false;
}

static void mdns_service(const char *address, const char *id, const char *service, int port)
{
    UNUSED(service);
    UNUSED(port);

    GtkWidget *child, *subtitle_child;

    g_mutex_lock(&self_global->mutex_discover);

    // Traverse list of instruments
    for (child = gtk_widget_get_first_child(GTK_WIDGET(self_global->list_instruments));
            child != NULL;
            child = gtk_widget_get_next_sibling(child))
    {
        subtitle_child = find_child_by_name(GTK_WIDGET(child), "list-subtitle");
        if (subtitle_child != NULL)
        {
            if (strcmp(id, gtk_label_get_text(GTK_LABEL(subtitle_child))) == 0)
            {
                // Instruments already exists, do not add
                g_mutex_unlock(&self_global->mutex_discover);
                return;
            }
        }
    }

    // No match found, add instrument to list box
    list_add_instrument(self_global, address, id);
}

static void vxi11_broadcast(const char *address, const char *interface)
{
    UNUSED(address);

    char *text = g_strdup_printf ("Broadcasting on interface %s", interface);
    show_info(self_global, text);
    g_free(text);
}

static void vxi11_device(const char *address, const char *id)
{
    g_mutex_lock(&self_global->mutex_discover);

    list_add_instrument(self_global, address, id);
}

static gboolean gui_update_script_run_worker_function_finished_thread(gpointer data)
{
    LxiGuiWindow *self = data;

    // Restore search button
    gtk_toggle_button_set_active(self->toggle_button_script_run, false);
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_script_run), true);

    return G_SOURCE_REMOVE;
}

static gboolean gui_update_search_finished_thread(gpointer data)
{
    LxiGuiWindow *self = data;

    // Restore search button
    gtk_toggle_button_set_active(self->toggle_button_search, false);
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_search), true);

    // Hide broadcasting info bar
    hide_info_bar(self);

    // Manage instruments status page
    if (self->no_instruments)
    {
        gtk_widget_set_visible(GTK_WIDGET(self->status_page_instruments), true);
    }

    // Reenable search shortcut
    gtk_widget_action_set_enabled (GTK_WIDGET (self), "action.search", true);

    return G_SOURCE_REMOVE;
}

static gboolean gui_update_send_worker_finished_thread(gpointer data)
{
    LxiGuiWindow *self = data;

    // Restore search button
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_scpi_send), true);
    gtk_toggle_button_set_active(self->toggle_button_scpi_send, false);

    return G_SOURCE_REMOVE;
}

static gpointer search_worker_thread(gpointer data)
{
    LxiGuiWindow *self = data;
    unsigned int timeout = g_settings_get_uint(self->settings, "timeout-discover");
    bool use_mdns_discovery = g_settings_get_boolean(self->settings, "use-mdns-discovery");

    // Reset selected IP and ID
    self->ip = NULL;
    self->id = NULL;

    // Search for LXI devices
    if (use_mdns_discovery)
        lxi_discover(&info, timeout, DISCOVER_MDNS);
    else
        lxi_discover(&info, timeout, DISCOVER_VXI11);

    g_idle_add(gui_update_search_finished_thread, self);

    return NULL;
}

static gboolean gui_update_search_start_thread(gpointer data)
{
    LxiGuiWindow *self = data;
    GtkWidget *child;

    // Hide instruments status page
    gtk_widget_set_visible(GTK_WIDGET(self->status_page_instruments), false);
    self->no_instruments = true;

    // Reveal flap
    adw_flap_set_reveal_flap(self->flap, true);

    // Only allow one search activity at a time
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_search), false);

    // Clear instrument list
    child = gtk_widget_get_first_child (GTK_WIDGET(self->list_instruments));
    while (child != NULL)
    {
        gtk_list_box_remove (GTK_LIST_BOX (self->list_instruments), child);
        child = gtk_widget_get_first_child (GTK_WIDGET(self->list_instruments));
    }

    // Start thread which searches for LXI instruments
    self->search_worker_thread = g_thread_new("search_worker", search_worker_thread, (gpointer)self);

    return G_SOURCE_REMOVE;
}

static void button_clicked_search(LxiGuiWindow *self, GtkToggleButton *button)
{
    UNUSED(button);

    g_idle_add(gui_update_search_start_thread, self);
}

struct dispatch_data_t
{
    GtkTextView *text_view;
    gchar *buffer;
};

static gboolean text_view_add_buffer_thread(gpointer user_data)
{
    struct dispatch_data_t *data = user_data;
    GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(data->text_view);
    GtkTextIter iter;

    // Append text
    gtk_text_buffer_get_end_iter(text_buffer, &iter);
    gtk_text_buffer_insert (text_buffer, &iter, data->buffer, -1);

    // Scroll down
    gtk_text_buffer_get_end_iter(text_buffer, &iter);
    gtk_text_iter_backward_line(&iter);
    GtkTextMark *end_mark = gtk_text_buffer_create_mark(text_buffer, NULL, &iter, FALSE);
    gtk_text_view_scroll_mark_onscreen(data->text_view, end_mark);
    gtk_text_buffer_delete_mark(text_buffer, end_mark);

    // Cleanup
    g_free(data->buffer);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void text_view_add_buffer(GtkTextView *view, const char *buffer)
{
    struct dispatch_data_t *data = g_new0(struct dispatch_data_t, 1);
    data->text_view = view;
    data->buffer = g_strdup(buffer);
    g_idle_add(text_view_add_buffer_thread, data);
}

static gboolean text_view_add_markup_buffer_thread(gpointer user_data)
{
    struct dispatch_data_t *data = user_data;
    GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(data->text_view);
    GtkTextIter iter;

    // Append text
    gtk_text_buffer_get_end_iter(text_buffer, &iter);
    gtk_text_buffer_insert_markup(text_buffer, &iter, data->buffer, -1);

    // Scroll down
    gtk_text_buffer_get_end_iter(text_buffer, &iter);
    GtkTextMark *end_mark = gtk_text_buffer_create_mark(text_buffer, NULL, &iter, FALSE);
    gtk_text_view_scroll_mark_onscreen(data->text_view, end_mark);
    gtk_text_buffer_delete_mark(text_buffer, end_mark);

    // Cleanup
    g_free(data->buffer);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void text_view_add_buffer_in_dimgray(GtkTextView *view, const char *buffer)
{
    struct dispatch_data_t *data = g_new0(struct dispatch_data_t, 1);
    data->text_view = view;
    data->buffer = g_strdup_printf("<span foreground=\"dimgray\">%s</span>", buffer);
    g_idle_add(text_view_add_markup_buffer_thread, data);
}

static gboolean text_view_clear_buffer_thread(gpointer data)
{
    GtkTextView *view = data;

    GtkTextIter start, end;
    GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(view);
    gtk_text_buffer_get_bounds(text_buffer, &start, &end);
    gtk_text_buffer_delete(text_buffer, &start, &end);

    return G_SOURCE_REMOVE;
}

static void text_view_clear_buffer(GtkTextView *view)
{
    g_idle_add(text_view_clear_buffer_thread, view);
}

static void save_text_buffer_to_file(GFile *file, GtkTextBuffer *text_buffer)
{
    GtkTextIter start, end;
    gboolean status = true;
    GError *error = NULL;
    char *buffer;

    // Get buffer of script text view
    gtk_text_buffer_get_bounds(text_buffer, &start, &end);
    buffer = gtk_text_buffer_get_text(text_buffer, &start, &end, true);

    // Write output stream to file
    status = g_file_replace_contents (file, buffer, strlen(buffer), NULL, false, 0, NULL, NULL, &error);
    if (status != true)
    {
        g_print("Could not write output stream: %s\n", error->message);
        g_error_free(error);
        return;
    }

    // TODO: Report errors to GUI
}

static void on_scpi_save_as_response(GtkDialog *dialog,
        int        response,
        gpointer   user_data)
{
    LxiGuiWindow *self = user_data;

    if (response == GTK_RESPONSE_ACCEPT)
    {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
        GFile *file = gtk_file_chooser_get_file (chooser);

        GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(self->text_view_scpi);
        save_text_buffer_to_file(file, text_buffer);
    }

    gtk_window_destroy (GTK_WINDOW (dialog));
}

static void scpi_save_as(LxiGuiWindow *self)
{
    GtkWidget *dialog;

    // Show file save dialog
    dialog = gtk_file_chooser_dialog_new ("Select file",
            GTK_WINDOW (self),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Save", GTK_RESPONSE_ACCEPT,
            NULL);
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_current_name (chooser, "Untitled.txt");

    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
    gtk_widget_show (dialog);

    g_signal_connect (dialog, "response",
            G_CALLBACK (on_scpi_save_as_response),
            (gpointer *) self);
}

static void scpi_action_cb(GtkWidget  *widget,
        const char *action_name,
        GVariant   *parameter)
{
    LxiGuiWindow *self = LXI_GUI_WINDOW (widget);

    UNUSED(parameter);

    if (g_str_equal (action_name, "scpi_clear_all"))
    {
        text_view_clear_buffer(self->text_view_scpi);
    }

    if (g_str_equal (action_name, "scpi_save_as"))
    {
        scpi_save_as(self);
    }
}

static void scpi_print(LxiGuiWindow *self,
        const char *text,
        bool sent,
        const char *ip,
        const char *timestamp)
{
    bool show_ip = g_settings_get_boolean(self->settings, "scpi-show-message-ip");
    bool show_type = g_settings_get_boolean(self->settings, "scpi-show-message-type");
    bool show_timestamp = g_settings_get_boolean(self->settings, "scpi-show-message-timestamp");
    GString *string = g_string_new(NULL);

    // Build text string
    if ((show_timestamp) || (show_ip) || (show_type))
    {
        g_string_append(string, "[");
    }

    if (show_timestamp)
    {
        // Add timestamp
        g_string_append(string, timestamp);
    }

    if (show_ip)
    {
        // Add IP
        if (show_timestamp)
        {
            g_string_append(string, " ");
        }

        g_string_append(string, ip);
    }

    if (show_type)
    {
        // Add type
        const char *type;

        if ((show_timestamp) || (show_ip))
        {
            g_string_append(string, " ");
        }

        if (sent)
        {
            type = "REQ";
        } else
        {
            type = "RSP";
        }
        g_string_append(string, type);
    }

    if ((show_timestamp) || (show_ip) || (show_type))
    {
        g_string_append(string, "] ");
    }

    g_string_append(string, text);

    if (sent)
    {
        text_view_add_buffer_in_dimgray(self->text_view_scpi, string->str);
        text_view_add_buffer(self->text_view_scpi, "\n");
    }
    else
    {
        text_view_add_buffer(self->text_view_scpi, string->str);
    }

    // Cleanup
    g_string_free(string, true);
}

static gpointer send_worker_thread(gpointer data)
{
    LxiGuiWindow *self = data;
    int device = 0;
    const char *input_buffer;
    GString *tx_buffer;
    char rx_buffer[65536];
    int rx_bytes;
    unsigned int timeout = g_settings_get_uint(self->settings, "timeout-scpi");
    bool show_sent_scpi = g_settings_get_boolean(self->settings, "show-sent-scpi");
    unsigned int com_protocol = g_settings_get_uint(self->settings, "com-protocol");
    unsigned int raw_port = g_settings_get_uint(self->settings, "raw-port");

    if (self->ip == NULL)
    {
        show_error(self, "No instrument selected");
        goto error_no_instrument;
    }

    // Prepare buffer to send
    GtkEntryBuffer *entry_buffer = gtk_entry_get_buffer(GTK_ENTRY(self->entry_scpi));
    input_buffer = gtk_entry_buffer_get_text(entry_buffer);

    if (strlen(input_buffer) == 0)
        goto error_no_input;

    tx_buffer = g_string_new_len(input_buffer, strlen(input_buffer));
    strip_trailing_space(tx_buffer->str);

    if (com_protocol == VXI11)
    {
        device = lxi_connect(self->ip, 0, NULL, timeout, VXI11);
    }
    if (com_protocol == RAW)
    {
        tx_buffer = g_string_append(tx_buffer, "\n");
        device = lxi_connect(self->ip, raw_port, NULL, timeout, RAW);
    }
    if (device == LXI_ERROR)
    {
        show_error(self, "Error connecting");
        goto error_connect;
    }

    if (lxi_send(device, tx_buffer->str, tx_buffer->len, timeout) == LXI_ERROR)
    {
        show_error(self, "Error sending");
        goto error_send;
    }

    if (show_sent_scpi)
    {
        GDateTime* date_time = g_date_time_new_now_local();
        char *timestamp = g_strdup_printf("%02d:%02d:%02d:%03d",
                g_date_time_get_hour(date_time),
                g_date_time_get_minute(date_time),
                g_date_time_get_second(date_time),
                g_date_time_get_microsecond(date_time)/1000);
        g_date_time_unref(date_time);

        // Print sent command to output view
        if (com_protocol == RAW)
        {
            // Remove newline
            g_string_erase(tx_buffer, tx_buffer->len - 1, 1);
        }
        scpi_print(self, tx_buffer->str, true, self->ip, timestamp);

        g_free(timestamp);
    }

    if (question(tx_buffer->str))
    {
        rx_bytes = lxi_receive(device, rx_buffer, sizeof(rx_buffer), timeout);
        if (rx_bytes == LXI_ERROR)
        {
            show_error(self, "No response received");
            goto error_receive;
        }

        // Terminate received string/data
        rx_buffer[rx_bytes] = 0;

        GDateTime* date_time = g_date_time_new_now_local();
        char *timestamp = g_strdup_printf("%02d:%02d:%02d:%03d",
                g_date_time_get_hour(date_time),
                g_date_time_get_minute(date_time),
                g_date_time_get_second(date_time),
                g_date_time_get_microsecond(date_time)/1000);
        g_date_time_unref(date_time);

        // Print received response to text view
        scpi_print(self, rx_buffer, false, self->ip, timestamp);
        g_free(timestamp);
    }

    // Clear text in text input entry
    gtk_entry_buffer_delete_text(entry_buffer, 0, -1);

error_send:
error_receive:
    lxi_disconnect(device);
error_connect:
    g_string_free(tx_buffer, true);
error_no_instrument:
error_no_input:
    // Restore send button state
    // Defer!
    g_idle_add(gui_update_send_worker_finished_thread, self);

    return NULL;
}

static void button_clicked_scpi_clear(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);

    // Clear SCPI input entry
    GtkEntryBuffer *entry_buffer = gtk_entry_get_buffer(self->entry_scpi);
    gtk_entry_buffer_delete_text(entry_buffer, 0, -1);
    gtk_entry_set_buffer(self->entry_scpi, entry_buffer);
}

static void button_clicked_scpi_send(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);

    // Update send button state
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_scpi_send), false);

    // Start thread which sends the SCPI message
    self->send_worker_thread = g_thread_new("send_worker", send_worker_thread, (gpointer)self);
}

static void entry_scpi_enter_pressed(LxiGuiWindow *self, GtkEntry *entry)
{
    UNUSED(entry);

    // Start thread which sends the SCPI message
    self->send_worker_thread = g_thread_new("send_worker", send_worker_thread, (gpointer)self);

    // Update send button state
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_scpi_send), false);
    gtk_toggle_button_set_active(self->toggle_button_scpi_send, true);
}

static void button_clicked_scpi(LxiGuiWindow *self, GtkButton *button)
{
    // Insert button label at entry cursor position
    const char *button_label = gtk_button_get_label(button);
    int cursor_position = gtk_editable_get_position(GTK_EDITABLE(self->entry_scpi));
    GtkEntryBuffer *entry_buffer = gtk_entry_get_buffer(self->entry_scpi);

    gtk_entry_buffer_insert_text(entry_buffer, cursor_position, button_label, strlen(button_label));
    gtk_entry_set_buffer(self->entry_scpi, entry_buffer);

    cursor_position = cursor_position + strlen(button_label);

    gtk_editable_set_position(GTK_EDITABLE(self->entry_scpi), cursor_position);
}

static bool grab_screenshot(LxiGuiWindow *self)
{
    char *plugin_name = (char *) "";
    char *filename = (char *) "";
    unsigned int timeout = g_settings_get_uint(self->settings, "timeout-screenshot");
    int status;

    // Check for instrument
    if (self->ip == NULL)
    {
        show_error(self, "No instrument selected");
        return 1;
    }

    // Allocate 20 MB for image data
    self->image_buffer = g_malloc(0x100000*20);
    if (self->image_buffer == NULL)
    {
        show_error(self, "Failure allocating memory for image data");
        return 1;
    }

    // Capture screenshot
    status = screenshot((char *)self->ip, plugin_name, filename, timeout, false, self->image_buffer, &(self->image_size), self->image_format, self->image_filename);
    if (status != 0)
    {
        show_error(self, "Failed to grab screenshot");
        g_free(self->image_buffer);
        return 1;
    }

    return 0;
}

static gboolean gui_update_grab_screenshot_finished_thread(gpointer user_data)
{
    LxiGuiWindow *self = user_data;
    GdkPixbufLoader *loader;

    if (self->screenshot_ready)
    {
        // Show screenshot
        //loader = gdk_pixbuf_loader_new ();
        loader = gdk_pixbuf_loader_new_with_type(self->image_format, NULL);
        gdk_pixbuf_loader_write(loader, (const guchar *) self->image_buffer, (gsize)self->image_size, NULL);
        self->pixbuf_screenshot = gdk_pixbuf_loader_get_pixbuf (loader);
        if (self->pixbuf_screenshot == NULL)
        {
            show_error(self, "Failure handling image format");
            self->screenshot_loaded = false;
        }
        else
        {
            self->screenshot_size = gdk_pixbuf_get_width(self->pixbuf_screenshot);
            self->screenshot_loaded = true;
            gtk_widget_set_valign(GTK_WIDGET(self->picture_screenshot), GTK_ALIGN_FILL);
            gtk_widget_set_halign(GTK_WIDGET(self->picture_screenshot), GTK_ALIGN_FILL);
            gtk_picture_set_pixbuf(self->picture_screenshot, self->pixbuf_screenshot);
            gdk_pixbuf_loader_close(loader, NULL);
            g_object_unref(loader);

            // Make screenshot picture zoomable
            //gtk_widget_set_sensitive(GTK_WIDGET(self->viewport_screenshot), true);
        }

        g_free(self->image_buffer);
    }

    // Restore screenshot buttons
    gtk_toggle_button_set_active(self->toggle_button_screenshot_grab, false);
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_screenshot_grab), true);

    // Activate screenshot "Save" button if picture was successfully loaded
    if (self->screenshot_loaded)
        gtk_widget_set_sensitive(GTK_WIDGET(self->button_screenshot_save), true);

    return G_SOURCE_REMOVE;
}

static gpointer screenshot_grab_worker_thread(gpointer data)
{
    LxiGuiWindow *self = data;

    if (grab_screenshot(self) == 0)
    {
        self->screenshot_ready = true;
    }
    else
    {
        self->screenshot_ready = false;
    }

    g_idle_add(gui_update_grab_screenshot_finished_thread, self);

    return NULL;
}

static void button_clicked_screenshot_grab(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);

    if (self->ip == NULL)
    {
        show_error(self, "No instrument selected");
        gtk_toggle_button_set_active(self->toggle_button_screenshot_grab, false);
        return;
    }

    // Disable grab button while grabbing the screenshot
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_screenshot_grab), false);

    // Start worker thread that will perform the grab screenshot work
    self->screenshot_grab_worker_thread = g_thread_new("screenshot_grab_worker", screenshot_grab_worker_thread, (gpointer) self);
}

static void on_screenshot_file_save_response(GtkDialog *dialog,
        int        response)
{
    GError *error = NULL;
    gboolean status = true;

    if (response == GTK_RESPONSE_ACCEPT)
    {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);

        g_autoptr(GFile) file = gtk_file_chooser_get_file (chooser);

        status = gdk_pixbuf_save(self_global->pixbuf_screenshot, g_file_get_path(file), "png", &error, NULL);
        if (status == false)
        {
            g_error ("Error: %s\n", error->message);
        }
    }

    gtk_window_destroy (GTK_WINDOW (dialog));
}

static void button_clicked_screenshot_save (LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);
    GtkWidget *dialog;
    GtkFileChooser *chooser;

    // Show file save as dialog
    dialog = gtk_file_chooser_dialog_new ("Select file",
            GTK_WINDOW (self),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Save", GTK_RESPONSE_ACCEPT,
            NULL);
    chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_current_name (chooser, "Untitled screenshot.png");

    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
    gtk_widget_show (dialog);

    g_signal_connect (dialog, "response",
            G_CALLBACK (on_screenshot_file_save_response),
            NULL);
}

static gboolean gui_update_progress_bar_fraction_thread(gpointer user_data)
{
    LxiGuiWindow *self = user_data;

    static int count;

    gtk_progress_bar_set_fraction(self->progress_bar_benchmark, self->progress_bar_fraction);

    // Animate the runner
    if (count++ % 2)
    {
        gtk_image_set_pixel_size(self->image_benchmark, 155);
        gtk_widget_set_margin_start(GTK_WIDGET(self->image_benchmark), 10);
    }
    else
    {
        gtk_image_set_pixel_size(self->image_benchmark, 160);
        gtk_widget_set_margin_start(GTK_WIDGET(self->image_benchmark), 0);
    }

    return G_SOURCE_REMOVE;
}

static void benchmark_progress_cb(unsigned int count)
{
    double fraction_count;
    double total_count = self_global->benchmark_requests_count;

    // Update progress for every 5% fraction reached
    fraction_count = total_count / 20;
    if ((++count % (unsigned int) fraction_count) == 0)
    {
        self_global->progress_bar_fraction = count / fraction_count / 20;
        g_idle_add(gui_update_progress_bar_fraction_thread, self_global);
    }
}

static gboolean gui_update_benchmark_finished_thread(gpointer user_data)
{
    LxiGuiWindow *self = user_data;

    gtk_label_set_text(self->label_benchmark_result, self->benchmark_result_text);
    g_free(self->benchmark_result_text);

    gtk_toggle_button_set_active(self->toggle_button_benchmark_start, false);
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_benchmark_start), true);

    return G_SOURCE_REMOVE;
}

static gpointer benchmark_worker_function(gpointer data)
{
    double result;
    LxiGuiWindow *self = data;
    unsigned int com_protocol = g_settings_get_uint(self->settings, "com-protocol");
    unsigned int raw_port = g_settings_get_uint(self->settings, "raw-port");

    if (com_protocol == VXI11)
    {
        benchmark(self->ip, 0, 1000, VXI11, self->benchmark_requests_count, false, &result, benchmark_progress_cb);
    }
    if (com_protocol == RAW)
    {
        benchmark(self->ip, raw_port, 1000, RAW, self->benchmark_requests_count, false, &result, benchmark_progress_cb);
    }

    // Show benchmark result
    self->benchmark_result_text = g_strdup_printf("%.1f requests/s", result);
    g_idle_add(gui_update_benchmark_finished_thread, self);

    return NULL;
}

static gboolean gui_update_progress_bar_reset_thread(gpointer user_data)
{
    LxiGuiWindow *self = user_data;

    // Reset
    gtk_progress_bar_set_fraction(self->progress_bar_benchmark, 0);
    gtk_label_set_text(self->label_benchmark_result, "");

    return G_SOURCE_REMOVE;
}

static void button_clicked_benchmark_start(LxiGuiWindow *self, GtkToggleButton *button)
{
    UNUSED(button);

    // Reset
    g_idle_add(gui_update_progress_bar_reset_thread, self);
    self->benchmark_requests_count = gtk_spin_button_get_value(self->spin_button_benchmark_requests);

    if (self->ip == NULL)
    {
        show_error(self, "No instrument selected");
        gtk_toggle_button_set_active(self->toggle_button_benchmark_start, true);
        return;
    }

    gtk_widget_set_sensitive(GTK_WIDGET(button), false);
    self->benchmark_worker_thread = g_thread_new("benchmark_worker", benchmark_worker_function, (gpointer) self);
}

static void button_clicked_add_instrument(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(self);
    UNUSED(button);

    // Not implemented
}

static void on_script_file_open_response (GtkDialog *dialog,
        int        response,
        gpointer   user_data)
{
    LxiGuiWindow *self = user_data;

    if (response == GTK_RESPONSE_ACCEPT)
    {
        // Read file contents into script text view

        GFileInputStream *file_input_stream;
        GInputStream *input_stream;
        GtkSourceBuffer *source_buffer_script;
        gboolean status = true;
        gsize bytes_read = 0, bytes_written = 0;
        GError *error = NULL;

        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));

        file_input_stream = g_file_read(file, NULL, &error);
        if (file_input_stream == NULL)
        {
            g_print ("Could not open file: %s\n", error->message);
            g_error_free(error);
            g_object_unref(file_input_stream);
            return;
        }

        GFileInfo *info = g_file_query_info(file, "*", G_FILE_QUERY_INFO_NONE, NULL, NULL);
        goffset len_buffer = g_file_info_get_size(info);

        gchar *buffer = g_malloc(len_buffer);
        if (buffer == NULL)
        {
            g_print("Failure allocating memory for reading file");
            g_object_unref(file_input_stream);
            return;
        }

        input_stream = g_buffered_input_stream_new (G_INPUT_STREAM (file_input_stream));

        status = g_input_stream_read_all(input_stream, buffer, len_buffer, &bytes_read, NULL, &error);
        if (status != TRUE)
        {
            g_print ("Could not read file input stream: %s\n", error->message);
            g_error_free(error);
            g_object_unref(file_input_stream);
            g_free(buffer);
            g_free(input_stream);
            return;
        }

        // Convert input file buffer to UTF-8
        gchar *utf8_buffer = g_convert (buffer, bytes_read, "UTF-8", "ISO-8859-1", NULL, &bytes_written, &error);
        if (error != NULL)
        {
            g_print ("Couldn't convert to UTF-8\n");
            g_error_free (error);
            g_object_unref(file_input_stream);
            g_free(buffer);
            g_free(input_stream);
            return;
        }

        // Get source buffer of script source view
        source_buffer_script = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->source_view_script)));

        // Clear existing text buffer
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(source_buffer_script), &start, &end);
        gtk_text_buffer_delete(GTK_TEXT_BUFFER(source_buffer_script), &start, &end);

        // Read data into text buffer
        GtkTextIter iter;
        gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(source_buffer_script), &iter);
        gtk_text_buffer_insert(GTK_TEXT_BUFFER(source_buffer_script), &iter, utf8_buffer, bytes_written);

        // Free old script file if any
        if (self->script_file != NULL)
            g_object_unref(self->script_file);

        // Update script file reference
        self->script_file = file;

        // Print status
        if (self->script_file != NULL)
        {
            char *filename = g_file_get_path(self->script_file);
            char *basename = g_path_get_basename(filename);
            g_free(filename);

            char *text = g_strdup_printf ("Opening %s\n", basename);
            text_view_add_buffer(self->text_view_script_status, text);
            g_free(text);
        }

        // Cleanup
        g_free(buffer);
        g_object_unref(file_input_stream);
        g_object_unref(input_stream);
    }

    // TODO: Report errors to GUI

    gtk_window_destroy (GTK_WINDOW (dialog));
}

static void button_clicked_script_new(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);
    GtkSourceBuffer *source_buffer_script;

    // Clear existing script file
    self->script_file = NULL;

    // Get source buffer of script source view
    source_buffer_script = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->source_view_script)));

    // Clear existing text buffer
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(source_buffer_script), &start, &end);
    gtk_text_buffer_delete(GTK_TEXT_BUFFER(source_buffer_script), &start, &end);

    // Print status
    text_view_add_buffer(self->text_view_script_status, "New script\n");
}

static void button_clicked_script_open(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);
    GtkWidget *dialog;
    gpointer data = self;

    // Show file open dialog
    dialog = gtk_file_chooser_dialog_new ("Select file",
            GTK_WINDOW (self),
            GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Open", GTK_RESPONSE_ACCEPT,
            NULL);

    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
    gtk_widget_show (dialog);

    g_signal_connect (dialog, "response",
            G_CALLBACK (on_script_file_open_response),
            data);
}



static void on_script_file_save_response(GtkDialog *dialog,
        int        response,
        gpointer   user_data)
{
    LxiGuiWindow *self = user_data;

    if (response == GTK_RESPONSE_ACCEPT)
    {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
        GFile *file = gtk_file_chooser_get_file (chooser);

        GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->source_view_script));
        save_text_buffer_to_file(file, text_buffer);

        // Free old script file if any
        if (self->script_file != NULL)
            g_object_unref(self->script_file);

        // Update script file reference
        self->script_file = file;

        // Print status
        if (self->script_file != NULL)
        {
            char *filename = g_file_get_path(self->script_file);
            char *basename = g_path_get_basename(filename);
            g_free(filename);

            char *text = g_strdup_printf ("Saving %s\n", basename);
            text_view_add_buffer(self->text_view_script_status, text);
            g_free(text);
        }
    }

    gtk_window_destroy (GTK_WINDOW (dialog));
}

static void button_clicked_script_save(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);

    if (self->script_file != NULL)
    {
        // Save file
        GtkTextBuffer *text_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->source_view_script));
        save_text_buffer_to_file(self->script_file, text_buffer);

        // Print status
        char *filename = g_file_get_path(self->script_file);
        char *basename = g_path_get_basename(filename);
        g_free(filename);

        char *text = g_strdup_printf ("Saving %s\n", basename);
        text_view_add_buffer(self->text_view_script_status, text);
        g_free(text);
    }
    else
    {
        GtkWidget *dialog;
        gpointer data = self;

        // Show file save dialog
        dialog = gtk_file_chooser_dialog_new ("Select file",
                GTK_WINDOW (self),
                GTK_FILE_CHOOSER_ACTION_SAVE,
                "_Cancel", GTK_RESPONSE_CANCEL,
                "_Save", GTK_RESPONSE_ACCEPT,
                NULL);

        gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
        gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
        gtk_widget_show (dialog);

        g_signal_connect (dialog, "response",
                G_CALLBACK (on_script_file_save_response),
                data);
    }
}

static void button_clicked_script_save_as(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);
    GtkWidget *dialog;
    gpointer data = self;

    // Show file save dialog
    dialog = gtk_file_chooser_dialog_new ("Select file",
            GTK_WINDOW (self),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Save", GTK_RESPONSE_ACCEPT,
            NULL);

    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
    gtk_widget_show (dialog);

    g_signal_connect (dialog, "response",
            G_CALLBACK (on_script_file_save_response),
            data);
}

void initialize_script_engine(LxiGuiWindow *self)
{
    // Print lua engine status
    char *text = g_strdup_printf ("%s engine ready\n", LUA_VERSION);
    text_view_add_buffer(self->text_view_script_status, text);
    text_view_add_buffer(self->text_view_script_status, "Loaded lxi-tools extensions\n");
    g_free(text);

    self->lua_stop_requested = false;
}

static void lua_print_error(LxiGuiWindow *self, const char *string)
{
    text_view_add_buffer(self->text_view_script_status, string);
    text_view_add_buffer(self->text_view_script_status, "\n");
}

static void lua_print_string(const char *string)
{
    text_view_add_buffer(self_global->text_view_script_status, string);
    text_view_add_buffer(self_global->text_view_script_status, "\n");
}

static void chart_destroyed_cb (GtkWidget *widget,
        gpointer user_data)
{
    UNUSED(user_data);
    int handle;

    // Mark widget deallocated
    for (handle=0; handle<CHARTS_MAX; handle++)
    {
        if (gui_chart[handle].widget == widget)
        {
            gui_chart[handle].allocated = false;
            break;
        }
    }
}

static void on_chart_save_image_response(GtkDialog *dialog,
        int            response,
        struct chart_t *chart)
{
    GError *error = NULL;
    gboolean status = true;

    if (response == GTK_RESPONSE_ACCEPT)
    {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);

        g_autoptr(GFile) file = gtk_file_chooser_get_file (chooser);

        status = gtk_chart_save_png(GTK_CHART(chart->widget), g_file_get_path(file));
        if (status == false)
        {
            g_error ("Error: %s\n", error->message);
        }
    }

    gtk_window_destroy (GTK_WINDOW (dialog));
}

static void chart_save_image(GSimpleAction *action,
        GVariant      *state,
        gpointer       user_data)
{
    UNUSED(action);
    UNUSED(state);

    GtkWidget *dialog;
    GtkFileChooser *chooser;
    struct chart_t *chart = user_data;

    // Show file save as dialog
    dialog = gtk_file_chooser_dialog_new ("Select file",
            GTK_WINDOW (chart->window),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Save", GTK_RESPONSE_ACCEPT,
            NULL);
    chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_current_name (chooser, "Untitled screenshot.png");

    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
    gtk_widget_show (dialog);

    g_signal_connect (dialog, "response",
            G_CALLBACK (on_chart_save_image_response),
            chart);
}

static void on_chart_save_csv_response(GtkDialog *dialog,
        int            response,
        struct chart_t *chart)
{
    GError *error = NULL;
    gboolean status = true;

    if (response == GTK_RESPONSE_ACCEPT)
    {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);

        g_autoptr(GFile) file = gtk_file_chooser_get_file (chooser);

        status = gtk_chart_save_csv(GTK_CHART(chart->widget), g_file_get_path(file));
        if (status == false)
        {
            g_error ("Error: %s\n", error->message);
        }
    }

    gtk_window_destroy (GTK_WINDOW (dialog));
}

static void chart_save_csv(GSimpleAction *action,
        GVariant      *state,
        gpointer       user_data)
{
    UNUSED(action);
    UNUSED(state);

    GtkWidget *dialog;
    GtkFileChooser *chooser;
    struct chart_t *chart = user_data;

    // Show file save as dialog
    dialog = gtk_file_chooser_dialog_new ("Select file",
            GTK_WINDOW (chart->window),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Save", GTK_RESPONSE_ACCEPT,
            NULL);
    chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_current_name (chooser, "Untitled.csv");

    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);
    gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
    gtk_widget_show (dialog);

    g_signal_connect (dialog, "response",
            G_CALLBACK (on_chart_save_csv_response),
            chart);
}

static GActionEntry win_actions[] =
{
    { "save-image", chart_save_image, NULL, NULL, NULL, {} },
    { "save-csv", chart_save_csv, NULL, NULL, NULL, {} }
};

static void chart_button_clicked_fullscreen(GtkButton *button, gpointer data)
{
    UNUSED(button);
    GtkWindow *window = GTK_WINDOW(data);

    gtk_window_fullscreen(window);
}

static bool chart_key_pressed_cb(GtkEventControllerKey* self,
        guint keyval,
        guint keycode,
        GdkModifierType state,
        gpointer user_data)
{
    UNUSED(self);
    UNUSED(state);
    GtkWindow *window = GTK_WINDOW(user_data);

    // If <ESC> key pressed
    if ((keyval == 0xff1b) && (keycode == 0x9))
    {
        // Exit fullscreen
        if (gtk_window_is_fullscreen(window))
        {
            gtk_window_unfullscreen(window);
        }
    }

    return true;
}

static gboolean gui_chart_save_csv_thread(gpointer user_data)
{
    struct chart_t *chart = user_data;

    gtk_chart_save_csv(GTK_CHART(chart->widget), chart->filename_csv);
    g_free(chart->filename_csv);

    // Signal we are finished saving csv file
    g_mutex_unlock(&self_global->mutex_save_csv);

    return G_SOURCE_REMOVE;
}

// lua: chart_save_csv(handle)
static int lua_gui_chart_save_csv(lua_State* L)
{
    int handle = lua_tointeger(L, 1);
    const char *filename = lua_tostring(L, 2);

    if (gui_chart[handle].allocated == true)
    {
        gui_chart[handle].filename_csv = g_strdup(filename);
        char *text = g_strdup_printf ("Saving %s\n", filename);
        text_view_add_buffer(self_global->text_view_script_status, text);
        g_free(text);
        g_idle_add(gui_chart_save_csv_thread, &gui_chart[handle]);

        // Wait for save csv operation finished
        g_mutex_lock(&self_global->mutex_save_csv);
    }

    return 0;
}

static gboolean gui_chart_save_png_thread(gpointer user_data)
{
    struct chart_t *chart = user_data;

    gtk_chart_save_png(GTK_CHART(chart->widget), chart->filename_png);
    g_free(chart->filename_png);

    // Signal we are finished saving png file
    g_mutex_unlock(&self_global->mutex_save_png);

    return G_SOURCE_REMOVE;
}

// lua: chart_save_png(handle)
static int lua_gui_chart_save_png(lua_State* L)
{
    int handle = lua_tointeger(L, 1);
    const char *filename = lua_tostring(L, 2);

    if (gui_chart[handle].allocated == true)
    {
        gui_chart[handle].filename_png = g_strdup(filename);
        char *text = g_strdup_printf ("Saving %s\n", filename);
        text_view_add_buffer(self_global->text_view_script_status, text);
        g_free(text);
        g_idle_add(gui_chart_save_png_thread, &gui_chart[handle]);

        // Wait for save png operation finished
        g_mutex_lock(&self_global->mutex_save_png);
    }

    return 0;
}

static gboolean gui_chart_close_thread(gpointer user_data)
{
    GtkWindow *window = user_data;

    gtk_window_close(window);

    return G_SOURCE_REMOVE;
}

// lua: chart_close(handle)
static int lua_gui_chart_close(lua_State* L)
{
    int handle = lua_tointeger(L, 1);

    if (gui_chart[handle].allocated == true)
    {
        g_idle_add(gui_chart_close_thread, gui_chart[handle].window);
    }

    return 0;
}

static gboolean gui_chart_plot_thread(gpointer user_data)
{
    struct chart_t *chart = user_data;

    gtk_chart_plot_point(GTK_CHART(chart->widget), chart->x, chart->y);

    return G_SOURCE_REMOVE;
}

// lua: chart_plot(handle, x_value, y_value)
static int lua_gui_chart_plot(lua_State* L)
{
    int handle = lua_tointeger(L, 1);
    double x = lua_tonumber(L, 2);
    double y = lua_tonumber(L, 3);

    gui_chart[handle].x = x;
    gui_chart[handle].y = y;

    if (gui_chart[handle].allocated == true)
    {
        g_idle_add(gui_chart_plot_thread, &gui_chart[handle]);
    }

    return 0;
}

static gboolean gui_chart_set_value_thread(gpointer user_data)
{
    struct chart_t *chart = user_data;

    gtk_chart_set_value(GTK_CHART(chart->widget), chart->value);

    return G_SOURCE_REMOVE;
}

// lua: chart_set_value(handle, value)
static int lua_gui_chart_set_value(lua_State* L)
{
    int handle = lua_tointeger(L, 1);
    double value = lua_tonumber(L, 2);

    gui_chart[handle].value = value;

    if (gui_chart[handle].allocated == true)
    {
        g_idle_add(gui_chart_set_value_thread, &gui_chart[handle]);
    }

    return 0;
}

static gboolean gui_chart_new_thread(gpointer data)
{
    struct chart_t *chart = data;

    GAction *action;
    GActionGroup *actions;

    /* Construct a GtkBuilder instance from UI description */
    GtkBuilder *builder = gtk_builder_new_from_resource("/io/github/lxi-tools/lxi-gui/lxi_gui-chart.ui");

    // Get UI objects
    GtkWindow *window = GTK_WINDOW(gtk_builder_get_object (builder, "window"));
    GObject *button_fullscreen = gtk_builder_get_object (builder, "button_fullscreen");
    GtkWidget *widget = GTK_WIDGET(gtk_builder_get_object (builder, "chart"));

    // Map window actions
    actions = G_ACTION_GROUP (g_simple_action_group_new ());
    g_action_map_add_action_entries (G_ACTION_MAP(actions), win_actions, G_N_ELEMENTS(win_actions), chart);
    gtk_widget_insert_action_group (GTK_WIDGET (window), "chart", G_ACTION_GROUP (actions));

    // Disable "Save CSV" if chart features no CSV data
    action = g_action_map_lookup_action (G_ACTION_MAP (actions), "save-csv");
    if (chart->no_csv)
    {
        g_simple_action_set_enabled (G_SIMPLE_ACTION (action), false);
    }

    // Prepare window
    chart->window = window;
    gtk_window_set_decorated(window, true);
    gtk_window_set_modal(window, false);
    gtk_window_set_transient_for(window, GTK_WINDOW(self_global));
    gtk_window_set_resizable(window, true);

    switch (chart->type)
    {
        case GTK_CHART_TYPE_LINE:
            gtk_window_set_title(window, "Line Chart");
            gtk_window_set_default_size(window, chart->width, chart->width/2);
            break;

        case GTK_CHART_TYPE_SCATTER:
            gtk_window_set_title(window, "Scatter Chart");
            gtk_window_set_default_size(window, chart->width, chart->width/2);
            break;

        case GTK_CHART_TYPE_NUMBER:
            gtk_window_set_title(window, "Number Chart");
            gtk_window_set_default_size(window, chart->width, chart->width/2);
            break;

        case GTK_CHART_TYPE_GAUGE_LINEAR:
            gtk_window_set_title(window, "Linear Gauge");
            gtk_window_set_default_size(window, chart->width, chart->width*2);
            break;

        case GTK_CHART_TYPE_GAUGE_ANGULAR:
            gtk_window_set_title(window, "Angular Gauge");
            gtk_window_set_default_size(window, chart->width, chart->width);
            break;

        default: // Do nothing
            break;
    }

    // Prepare chart widget
    chart->widget = widget;
    gtk_chart_set_type(GTK_CHART(widget), chart->type);
    gtk_chart_set_title(GTK_CHART(widget), chart->title);
    g_free(chart->title);
    gtk_chart_set_width(GTK_CHART(widget), chart->width);

    switch (chart->type)
    {
        case GTK_CHART_TYPE_LINE:
        case GTK_CHART_TYPE_SCATTER:
            gtk_chart_set_x_label(GTK_CHART(widget), chart->x_label);
            g_free(chart->x_label);
            gtk_chart_set_y_label(GTK_CHART(widget), chart->y_label);
            g_free(chart->y_label);
            gtk_chart_set_x_max(GTK_CHART(widget), chart->x_max);
            gtk_chart_set_y_max(GTK_CHART(widget), chart->y_max);
            break;

        case GTK_CHART_TYPE_NUMBER:
            gtk_chart_set_label(GTK_CHART(widget), chart->label);
            g_free(chart->label);
            break;

        case GTK_CHART_TYPE_GAUGE_LINEAR:
        case GTK_CHART_TYPE_GAUGE_ANGULAR:
            gtk_chart_set_label(GTK_CHART(widget), chart->label);
            g_free(chart->label);
            gtk_chart_set_value_min(GTK_CHART(widget), chart->value_min);
            gtk_chart_set_value_max(GTK_CHART(widget), chart->value_max);
            break;

        default: // Do nothing
            break;
    }

    // Install event controller to listen for key presses
    GtkEventController *controller = gtk_event_controller_key_new();
    gtk_widget_add_controller(GTK_WIDGET(window), controller);

    // Associate window with application for signals etc. to propagate
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self_global));
    gtk_application_add_window(app, window);

    // Connect signals
    g_signal_connect (button_fullscreen, "clicked", G_CALLBACK (chart_button_clicked_fullscreen), window);
    g_signal_connect (controller, "key-pressed", G_CALLBACK (chart_key_pressed_cb), window);
    g_signal_connect (widget, "destroy", G_CALLBACK (chart_destroyed_cb), NULL);

    // Show window
    gtk_window_present(window);

    // Cleanup
    g_object_unref(builder);

    // Signal we are finished creating chart
    g_mutex_unlock(&self_global->mutex_gui_chart);

    return G_SOURCE_REMOVE;
}

// lua: handle = chart_new(...)
static int lua_gui_chart_new(lua_State* L)
{
    int handle;

    // Find free chart handle
    for (handle=0; handle<CHARTS_MAX; handle++)
    {
        if (gui_chart[handle].allocated == false)
        {
            gui_chart[handle].allocated = true;
            break;
        }
    }

    struct chart_t *chart = &gui_chart[handle];

    const char *type = lua_tostring(L, 1);
    if (strcmp(type, "line") == 0)
    {
        chart->type = GTK_CHART_TYPE_LINE;
    }
    else if (strcmp(type, "scatter") == 0)
    {
        chart->type = GTK_CHART_TYPE_SCATTER;
    }
    else if (strcmp(type, "number") == 0)
    {
        chart->type = GTK_CHART_TYPE_NUMBER;
    }
    else if (strcmp(type, "linear-gauge") == 0)
    {
        chart->type = GTK_CHART_TYPE_GAUGE_LINEAR;
    }
    else if (strcmp(type, "angular-gauge") == 0)
    {
        chart->type = GTK_CHART_TYPE_GAUGE_ANGULAR;
    }
    else
    {
        chart->type = GTK_CHART_TYPE_UNKNOWN;
    }

    chart->handle = handle;

    // Parse arguments depending on chart type
    switch (chart->type)
    {
        case GTK_CHART_TYPE_LINE:
        case GTK_CHART_TYPE_SCATTER:
            chart->title = g_strdup(lua_tostring(L, 2));
            chart->x_label = g_strdup(lua_tostring(L, 3));
            chart->y_label = g_strdup(lua_tostring(L, 4));
            chart->x_max = lua_tonumber(L, 5);
            chart->y_max = lua_tonumber(L, 6);
            chart->width = lua_tointeger(L, 7);
            chart->autoscale = lua_toboolean(L, 8);
            chart->no_csv = false;
            break;

        case GTK_CHART_TYPE_NUMBER:
            chart->title = g_strdup(lua_tostring(L, 2));
            chart->label = g_strdup(lua_tostring(L, 3));
            chart->width = lua_tointeger(L, 4);
            chart->no_csv = true;
            break;

        case GTK_CHART_TYPE_GAUGE_LINEAR:
        case GTK_CHART_TYPE_GAUGE_ANGULAR:
            chart->title = g_strdup(lua_tostring(L, 2));
            chart->label = g_strdup(lua_tostring(L, 3));
            chart->value_min = lua_tonumber(L, 4);
            chart->value_max = lua_tonumber(L, 5);
            chart->width = lua_tointeger(L, 6);
            chart->no_csv = true;
            break;

        default:
            break;
    }

    // Create new chart window
    g_idle_add(gui_chart_new_thread, chart);

    // Wait for chart ready (sleeps here until unlocked)
    g_mutex_lock(&self_global->mutex_gui_chart);

    // Return chart handle
    lua_pushinteger(L, handle);

    return 1;
}

// lua: print(string)
// Note: Overrides lua builtin print()
static int lua_print(lua_State* L)
{
    int nargs = lua_gettop(L);

    for (int i=1; i <= nargs; i++)
    {
        if (lua_isstring(L, i))
        {
            lua_print_string(lua_tostring(L,i));
        }
        else
        {
            /* non-strings */
        }
    }

    return 0;
}

// lua: ip = selected_ip()
static int lua_gui_ip(lua_State* L)
{
    // Return currently GUI selected IP
    lua_pushstring(L, self_global->ip);

    return 1;
}

// lua: id = selected_id()
static int lua_gui_id(lua_State* L)
{
    // Return currently GUI selected ID
    lua_pushstring(L, self_global->id);

    return 1;
}

// lua: version = version()
static int lua_gui_version(lua_State* L)
{
    // Return GUI version
    lua_pushstring(L, PACKAGE_VERSION);

    return 1;
}

static const struct luaL_Reg gui_lib [] =
{
    {"chart_new", lua_gui_chart_new},
    {"chart_plot", lua_gui_chart_plot},
    {"chart_set_value", lua_gui_chart_set_value},
    {"chart_close", lua_gui_chart_close},
    {"chart_save_csv", lua_gui_chart_save_csv},
    {"chart_save_png", lua_gui_chart_save_png},
    {"selected_ip", lua_gui_ip},
    {"selected_id", lua_gui_id},
    {"version", lua_gui_version},
    {"print", lua_print},
    {NULL, NULL}
};

static void lua_line_hook(lua_State *L, lua_Debug *ar)
{
    if (ar->event == LUA_HOOKLINE)
    {
        if (self_global->lua_stop_requested == true)
        {
            luaL_error(L, "Stopped by user");
        }
    }
}

extern int lua_register_gui(lua_State *L)
{
    // Register gui functions
    lua_getglobal(L, "_G");
    luaL_setfuncs(L, gui_lib, 0);
    lua_pop(L, 1);

    // Install line hook to manage run/stop execution
    lua_sethook(L, &lua_line_hook, LUA_MASKLINE, 0);

    return 0;
}

static void load_log_script(lua_State *L)
{
    gsize size;
    int error;

    GResource *resource = lxi_gui_get_resource();

    GBytes *script = g_resource_lookup_data (resource,
            "/io/github/lxi-tools/lxi-gui/log.lua",
            G_RESOURCE_LOOKUP_FLAGS_NONE,
            NULL);

    gconstpointer script_buffer = g_bytes_get_data (script, &size);

    error = luaL_loadbuffer(L, script_buffer, strlen(script_buffer), "lxi-gui") ||
        lua_pcall(L, 0, 0, 0);
    if (error)
    {
        lua_print_error(self_global, lua_tostring(L, -1));
        lua_pop(L, 1);  /* pop error message from the stack */
    }
}

static gpointer script_run_worker_function(gpointer data)
{
    LxiGuiWindow *self = data;
    GtkTextBuffer *buffer_script = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->source_view_script));
    GtkTextIter start, end;
    gchar *code_buffer;
    int error;
    char *chunkname = NULL;
    char *filename;

    // Reset lua control state
    self->lua_stop_requested = false;

    // Initialize new Lua session
    lua_State *L = luaL_newstate();

    // Open all standard Lua libraries
    luaL_openlibs(L);

    // Bind GUI functions
    lua_register_gui(L);

    // Bind lxi functions
    lua_register_lxi(L);

    // Load data logger script
    load_log_script(L);

    // Hardcode locale so script handles number conversion correct etc.
    setlocale(LC_ALL, "C.UTF-8");

    // Get buffer of script text view
    gtk_text_buffer_get_bounds(buffer_script, &start, &end);
    code_buffer = gtk_text_buffer_get_text(buffer_script, &start, &end, true);

    // Use filename as chunk name if working with a file
    if (self->script_file != NULL)
    {
        filename = g_file_get_path(self->script_file);
        chunkname = g_path_get_basename(filename);
        g_free(filename);
    }
    else
    {
        chunkname = strdup("buffer");
    }

    // Let lua load buffer and do error checking before running
    error = luaL_loadbuffer(L, code_buffer, strlen(code_buffer), chunkname) ||
        lua_pcall(L, 0, 0, 0);
    if (error)
    {
        lua_print_error(self, lua_tostring(L, -1));
        lua_pop(L, 1);  /* pop error message from the stack */
    }

    // Cleanup
    g_free(chunkname);
    lua_close(L);

    // Restore script run button
    g_idle_add(gui_update_script_run_worker_function_finished_thread, self);

    return NULL;
}

static void toggle_button_clicked_script_run(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(button);

    // Only allow to run once until execution is done
    gtk_widget_set_sensitive(GTK_WIDGET(self->toggle_button_script_run), false);

    text_view_clear_buffer(self->text_view_script_status);

    // Start thread which starts interpreting the Lua script
    self->script_run_worker_thread = g_thread_new("script_worker", script_run_worker_function, (gpointer) self);
}

static void button_clicked_script_stop(LxiGuiWindow *self, GtkButton *button)
{
    UNUSED(self);
    UNUSED(button);

    // Signal lua script engine to stop execution
    self->lua_stop_requested = true;
}

static void info_bar_clicked(LxiGuiWindow *self, GtkInfoBar *infobar)
{
    UNUSED(self);
    UNUSED(infobar);

    // TODO: Fix and use callback parameters
    gtk_widget_hide(GTK_WIDGET(self_global->info_bar));
}

static void lxi_gui_window_dispose(GObject *object)
{
    LxiGuiWindow *window = (LxiGuiWindow *)object;

    g_object_unref (window->settings);

    // Remove list view port as parent to list popover menu
    gtk_widget_unparent(GTK_WIDGET(window->list_widget_popover_menu));

    G_OBJECT_CLASS (lxi_gui_window_parent_class)->dispose (object);
}

static void lxi_gui_window_action_copy_screenshot_cb(GtkWidget  *widget,
        const char *action_name,
        GVariant   *param)
{
    UNUSED(action_name);
    UNUSED(param);

    LxiGuiWindow *self = LXI_GUI_WINDOW (widget);
    gdk_clipboard_set_texture(self->clipboard, gdk_texture_new_for_pixbuf(self->pixbuf_screenshot));
}

static void lxi_gui_window_action_search_cb(GtkWidget  *widget,
        const char *action_name,
        GVariant   *param)
{
    UNUSED(action_name);
    UNUSED(param);
    LxiGuiWindow *self = LXI_GUI_WINDOW (widget);

    g_assert (LXI_GUI_IS_WINDOW (self));

    // Disable shortcut action temporarily
    gtk_widget_action_set_enabled (GTK_WIDGET (self), "action.search", false);

    button_clicked_search(self, NULL);
}

static void lxi_gui_window_action_toggle_flap_cb(GtkWidget  *widget,
        const char *action_name,
        GVariant   *param)
{
    UNUSED(action_name);
    UNUSED(param);
    LxiGuiWindow *self = LXI_GUI_WINDOW (widget);

    g_assert (LXI_GUI_IS_WINDOW (self));

    bool flap_state = adw_flap_get_reveal_flap(self->flap);

    // Toggle flap
    adw_flap_set_reveal_flap(self->flap, !flap_state);
}

static void lxi_gui_window_class_init(LxiGuiWindowClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS (class);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

    object_class->dispose = lxi_gui_window_dispose;

    // Bind widgets
    gtk_widget_class_set_template_from_resource (widget_class, "/io/github/lxi-tools/lxi-gui/lxi_gui-window.ui");
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, list_instruments);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, list_viewport);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, entry_scpi);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, text_view_scpi);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, toggle_button_scpi_send);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, picture_screenshot);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, toggle_button_screenshot_grab);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, button_screenshot_save);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, progress_bar_benchmark);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, toggle_button_benchmark_start);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, spin_button_benchmark_requests);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, label_benchmark_result);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, image_benchmark);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, toggle_button_search);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, source_view_script);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, text_view_script_status);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, info_bar);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, label_info_bar);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, viewport_screenshot);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, toggle_button_script_run);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, flap);
    gtk_widget_class_bind_template_child (widget_class, LxiGuiWindow, status_page_instruments);

    // Bind signal callbacks
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_search);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_add_instrument);
    gtk_widget_class_bind_template_callback (widget_class, entry_scpi_enter_pressed);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_scpi);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_scpi_clear);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_scpi_send);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_screenshot_grab);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_screenshot_save);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_benchmark_start);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_script_new);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_script_open);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_script_save);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_script_save_as);
    gtk_widget_class_bind_template_callback (widget_class, toggle_button_clicked_script_run);
    gtk_widget_class_bind_template_callback (widget_class, button_clicked_script_stop);
    gtk_widget_class_bind_template_callback (widget_class, info_bar_clicked);

    /* These are the actions that we are using in the menu */
    gtk_widget_class_install_action (widget_class, "action.copy_ip", NULL, action_cb);
    gtk_widget_class_install_action (widget_class, "action.copy_id", NULL, action_cb);
    gtk_widget_class_install_action (widget_class, "action.open_browser", NULL, action_cb);
    gtk_widget_class_install_action (widget_class, "action.search", NULL, lxi_gui_window_action_search_cb);
    gtk_widget_class_install_action (widget_class, "action.toggle_flap", NULL, lxi_gui_window_action_toggle_flap_cb);
    gtk_widget_class_install_action (widget_class, "scpi_clear_all", NULL, scpi_action_cb);
    gtk_widget_class_install_action (widget_class, "scpi_save_as", NULL, scpi_action_cb);
    gtk_widget_class_install_action (widget_class, "action.copy_screenshot", NULL, lxi_gui_window_action_copy_screenshot_cb);

    /* Create shortcuts */
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_s, GDK_CONTROL_MASK, "action.search", NULL);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_h, GDK_CONTROL_MASK, "action.toggle_flap", NULL);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_c, GDK_CONTROL_MASK, "action.copy_screenshot", NULL);

    // Initialize LXI library
    lxi_init();

    // Set up search information callbacks
    info.broadcast = &vxi11_broadcast;
    info.device = &vxi11_device;
    info.service = &mdns_service; // For mDNS
}

static gboolean scroll_screenshot(
        GtkEventControllerScroll* controller,
        gdouble dx,
        gdouble dy,
        gpointer data
        )
{
    UNUSED(controller);
    UNUSED(dx);

    LxiGuiWindow *self = data;
    int width;

    width = gtk_widget_get_width(GTK_WIDGET(self->picture_screenshot));

    if (self->screenshot_loaded)
    {
        gtk_widget_set_size_request(GTK_WIDGET(self->picture_screenshot), width, -1);
        gtk_widget_set_valign(GTK_WIDGET(self->picture_screenshot), GTK_ALIGN_CENTER);
        gtk_widget_set_halign(GTK_WIDGET(self->picture_screenshot), GTK_ALIGN_CENTER);
        self->screenshot_loaded = false;
    }

    if (dy > 0)
    {
        // Scroll down -> zoom in
        width = width * 0.9;
    } else
    {
        // Scroll up -> zoom out
        width = width * 1.1;
    }

    gtk_widget_set_size_request(GTK_WIDGET(self->picture_screenshot), width, -1);

    return TRUE;
}

static void lxi_gui_window_init(LxiGuiWindow *self)
{
    GtkGesture *list_widget_gesture = gtk_gesture_click_new();

    gtk_widget_init_template (GTK_WIDGET (self));

    self_global = self;

    // Required for GtkSourceView to be recognized by builder
    gtk_source_view_get_type();

    // Required for GtkChart to be recognized by builder
    gtk_chart_get_type();

    // Load settings
    self->settings = g_settings_new ("io.github.lxi-tools.lxi-gui");

    // Set up clipboard
    GdkDisplay* gdk_display = gdk_display_get_default();
    self->clipboard = gdk_display_get_clipboard(gdk_display);

    // Manage dark theme setting
    bool prefer_dark_theme = g_settings_get_boolean(self->settings, "prefer-dark-theme");
    if (prefer_dark_theme)
    {
        AdwStyleManager *adw_style_manager = adw_style_manager_get_default();
        adw_style_manager_set_color_scheme(adw_style_manager, ADW_COLOR_SCHEME_PREFER_DARK);
    }

    // Load and apply CSS
    GtkCssProvider *provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_resource (provider, "/io/github/lxi-tools/lxi-gui/lxi_gui.css");
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
            GTK_STYLE_PROVIDER (provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Load instrument popover menu from model
    GtkBuilder *builder = gtk_builder_new_from_resource ("/io/github/lxi-tools/lxi-gui/lxi_gui-window_list_widget_menu_model.ui");
    self->list_widget_menu_model = G_MENU_MODEL (gtk_builder_get_object (builder, "list-widget-menu-model"));

    // Load popover menu from menu model
    self->list_widget_popover_menu = gtk_popover_menu_new_from_model(self->list_widget_menu_model);
    gtk_popover_set_has_arrow(GTK_POPOVER(self->list_widget_popover_menu), false);

    // Add list view port as parent to list popover menu
    gtk_widget_set_parent (GTK_WIDGET(self->list_widget_popover_menu), GTK_WIDGET(self->list_viewport));

    // Add event controller to handle any click gesture on list item widget
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (list_widget_gesture), 0);
    g_signal_connect (list_widget_gesture, "pressed", G_CALLBACK (pressed_cb), self);
    gtk_widget_add_controller (GTK_WIDGET(self->list_viewport), GTK_EVENT_CONTROLLER (list_widget_gesture));

    // Add event controller to capture scroll events on the surface of screenshot viewport
    GtkEventController *event_controller_screenshot;
    event_controller_screenshot = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    gtk_widget_add_controller(GTK_WIDGET(self->viewport_screenshot), event_controller_screenshot);
    g_signal_connect(event_controller_screenshot, "scroll", G_CALLBACK (scroll_screenshot), self);

    // Set up SCPI command entry
    gtk_editable_set_enable_undo (GTK_EDITABLE (self->entry_scpi), TRUE);

    g_object_unref (builder);
    g_object_unref (provider);

    self->ip = NULL;
    self->id = NULL;

    // Register LXI screenshot plugins
    screenshot_register_plugins();

    // Set greeting image on screenshot page and make it not zoomable
    gtk_widget_set_size_request(GTK_WIDGET(self->picture_screenshot), 200, 200);
    gtk_picture_set_resource(self->picture_screenshot, "/io/github/lxi-tools/lxi-gui/images/photo-camera.png");
    gtk_widget_set_sensitive(GTK_WIDGET(self->viewport_screenshot), false);

    // Set greeting image on benchmark page
    gtk_image_set_pixel_size(self->image_benchmark, 160);
    gtk_image_set_from_resource(self->image_benchmark, "/io/github/lxi-tools/lxi-gui/images/runner.png");

    // Grab focus to SCPI input entry
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_scpi));

    // Disable screenshot "Save" button until image is present
    gtk_widget_set_sensitive(GTK_WIDGET(self->button_screenshot_save), false);

    // Initialize script file
    self->script_file = NULL;

    // Set language of source buffer to "lua-lxi-gui"
    GtkSourceBuffer *source_buffer_script =
        GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->source_view_script)));
    GtkSourceLanguageManager *language_manager = gtk_source_language_manager_get_default();
    gtk_source_language_manager_append_search_path(language_manager,
            "resource:///io/github/lxi-tools/lxi-gui/language-specs");
    GtkSourceLanguage *language = gtk_source_language_manager_get_language(language_manager, "lua-lxi-gui");
    gtk_source_buffer_set_language(source_buffer_script, language);

    // Enable line numbers
    gtk_source_view_set_show_line_numbers(self->source_view_script, true);

    // Enable syntax highlighting according to language
    gtk_source_buffer_set_highlight_syntax(source_buffer_script, true);

    // Highlight current line
    gtk_source_view_set_highlight_current_line(self->source_view_script, true);

    // Set script view theme to "classic"
    GtkSourceStyleSchemeManager* style_manager = gtk_source_style_scheme_manager_new();
    GtkSourceStyleScheme *style = gtk_source_style_scheme_manager_get_scheme(style_manager, "classic-dark");
    gtk_source_buffer_set_style_scheme(source_buffer_script, style);

    // Initialize mutexes
    g_mutex_lock(&self->mutex_gui_chart);
    g_mutex_lock(&self->mutex_save_png);
    g_mutex_lock(&self->mutex_save_csv);

    // Initialize lua script engine
    initialize_script_engine(self);

    // Mark instrument list unpopulated
    self->no_instruments = true;

    // Add extra menu model for SCPI text view (right click menu)
    GMenu *menu = g_menu_new ();
    g_menu_append (menu, "Clear all", "scpi_clear_all");
    g_menu_append (menu, "Save as..", "scpi_save_as");
    gtk_text_view_set_extra_menu (self->text_view_scpi, G_MENU_MODEL(menu));

#if DEVEL_MODE
    gtk_widget_add_css_class(GTK_WIDGET(self), "devel");
#endif

}
