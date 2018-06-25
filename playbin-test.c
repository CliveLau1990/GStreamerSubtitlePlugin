// Build command: gcc playbin-test.c dictionary.c iniparser.c -o playbin-test `pkg-config --cflags --libs gstreamer-video-1.0 gtk+-3.0 gstreamer-1.0`

#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <gdk/gdk.h>
#if defined (GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#elif defined (GDK_WINDOWING_WIN32)
#include <gdk/gdkwin32.h>
#elif defined (GDK_WINDOWING_QUARTZ)
#include <gdk/gdkquartz.h>
#endif

#include "iniparser.h"

#define CONFIG_INI "config.ini"

/* Copied from gst-plugins-base/gst/playback/gstplay-enum.h */
typedef enum
{
  GST_PLAY_FLAG_VIDEO = (1 << 0),
  GST_PLAY_FLAG_AUDIO = (1 << 1),
  GST_PLAY_FLAG_TEXT = (1 << 2),
  GST_PLAY_FLAG_VIS = (1 << 3),
  GST_PLAY_FLAG_SOFT_VOLUME = (1 << 4),
  GST_PLAY_FLAG_NATIVE_AUDIO = (1 << 5),
  GST_PLAY_FLAG_NATIVE_VIDEO = (1 << 6),
  GST_PLAY_FLAG_DOWNLOAD = (1 << 7),
  GST_PLAY_FLAG_BUFFERING = (1 << 8),
  GST_PLAY_FLAG_DEINTERLACE = (1 << 9),
  GST_PLAY_FLAG_SOFT_COLORBALANCE = (1 << 10)
} GstPlayFlags;

/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData {
  GstElement *playbin;            /* Our one and only pipeline */

  GtkWidget *slider;              /* Slider widget to keep track of current position */
  GtkWidget *streams_list;        /* Text widget to display info about the streams */
  gulong slider_update_signal_id; /* Signal ID for the slider update signal */

  GstState state;                 /* Current state of the pipeline */
  gint64 duration;                /* Duration of the clip, in nanoseconds */

  dictionary *ini;

  gboolean subtitle_silent;
  gint subtitle_offset;
} CustomData;

static void analyze_streams (CustomData *data);

int iniparser_save(dictionary * d, const char *inipath)
{
    int ret = 0;
    FILE *fp = NULL;

    if (inipath == NULL || d == NULL) {
        ret = -1;
        printf("saveConfig error:%d from (filepath == NULL || head == NULL)\n",ret);
        return ret;
    }

    fp = fopen(inipath,"w");
    if (fp == NULL) {
        ret = -2;
        printf("saveConfig:open file error:%d from %s\n",ret,inipath);
        return ret;
    }

    iniparser_dump_ini(d,fp);

    fclose(fp);

    return 0;
}

/* This function is called when xxx */
static void
update_flag (GstElement * pipeline, GstPlayFlags flag, gboolean state)
{
  gint flags;

  g_print ("%ssetting flag 0x%08x\n", (state ? "" : "un"), flag);

  g_object_get (pipeline, "flags", &flags, NULL);
  if (state)
    flags |= flag;
  else
    flags &= ~(flag);
  g_object_set (pipeline, "flags", flags, NULL);
}

/* This function is called when the GUI toolkit creates the physical window that will hold the video.
 * At this point we can retrieve its handler (which has a different meaning depending on the windowing system)
 * and pass it to GStreamer through the XOverlay interface. */
static void realize_cb (GtkWidget *widget, CustomData *data) {
  g_print("%s called\n", __func__);

  GdkWindow *window = gtk_widget_get_window (widget);
  guintptr window_handle;

  if (!gdk_window_ensure_native (window))
    g_error ("Couldn't create native window needed for GstXOverlay!");

  /* Retrieve window handler from GDK */
#if defined (GDK_WINDOWING_WIN32)
  window_handle = (guintptr)GDK_WINDOW_HWND (window);
#elif defined (GDK_WINDOWING_QUARTZ)
  window_handle = gdk_quartz_window_get_nsview (window);
#elif defined (GDK_WINDOWING_X11)
  window_handle = GDK_WINDOW_XID (window);
#endif
  /* Pass it to playbin, which implements XOverlay and will forward it to the video sink */
  gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (data->playbin), window_handle);
}

/* This function is called when the "subtitle uri menu item" is clicked */
static void subtitle_uri_cb (GtkWidget *widget, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_PAUSED);

  GtkWidget *dialog = gtk_file_chooser_dialog_new ("Select Subtitle File",
                                        NULL,
                                        GTK_FILE_CHOOSER_ACTION_OPEN,
                                        _("Cancel"),
                                        GTK_RESPONSE_CANCEL,
                                        _("Open"),
                                        GTK_RESPONSE_ACCEPT,
                                        NULL);

  //后缀为.srt的文件
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name (filter, _("Subtitle files"));
  gtk_file_filter_add_pattern (filter, "*.[Ss][Rr][Tt]");
  gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
    gst_element_set_state (data->playbin, GST_STATE_READY);

    /* Set the subtitle URI to play and some font description */
    gchar *uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER (dialog));
    g_object_set (data->playbin, "suburi", uri, NULL);

    g_free(uri);
  }

  gtk_widget_destroy (dialog);

  gst_element_set_state (data->playbin, GST_STATE_PLAYING);
}

/* This function is called when the "subtitle font menu item" is clicked */
static void subtitle_font_cb (GtkWidget *widget, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_PAUSED);

  GtkWidget *dialog = gtk_font_chooser_dialog_new("Select Subtitle Font)", NULL);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
    /* Set the subtitle font description */
    gchar *font_name = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(dialog));
    g_object_set (data->playbin, "subtitle-font-desc", font_name, NULL);

    g_free(font_name);
  }

  gtk_widget_destroy(dialog);

  gst_element_set_state (data->playbin, GST_STATE_PLAYING);

  analyze_streams(data);
}

/* This function is called when the "subtitle silent menu item" is clicked */
static void subtitle_silent_cb (GtkWidget *widget, CustomData *data) {

  data->subtitle_silent = !data->subtitle_silent;

  iniparser_set (data->ini, "Subtitles:silent", data->subtitle_silent ? "TRUE" : "FALSE");
  iniparser_save (data->ini, CONFIG_INI);

  g_print("%s called(silent:%s)\n", __func__, data->subtitle_silent ? "True" : "False");

  update_flag(data->playbin, GST_PLAY_FLAG_TEXT, !data->subtitle_silent);

  analyze_streams(data);
}

/* This function is called when the "subtitle step quickly menu item" is clicked */
static void subtitle_offset_forward_cb (GtkWidget *widget, CustomData *data) {

  char offset_value[16];

  data->subtitle_offset += 100;

  sprintf(offset_value, "%d", data->subtitle_offset);
  iniparser_set (data->ini, "Subtitles:offset", offset_value);
  iniparser_save (data->ini, CONFIG_INI);

  g_print("%s called(offset:%d)\n", __func__, data->subtitle_offset);

  g_object_set (data->playbin, "subtitle-offset", data->subtitle_offset, NULL);

  analyze_streams(data);
}

/* This function is called when the "subtitle step slowly menu item" is clicked */
static void subtitle_offset_backward_cb (GtkWidget *widget, CustomData *data) {

  char offset_value[16];

  data->subtitle_offset -= 100;

  sprintf(offset_value, "%d", data->subtitle_offset);
  iniparser_set (data->ini, "Subtitles:offset", offset_value);
  iniparser_save (data->ini, CONFIG_INI);

  g_print("%s called(offset:%d)\n", __func__, data->subtitle_offset);

  g_object_set (data->playbin, "subtitle-offset", data->subtitle_offset, NULL);

  analyze_streams(data);
}

/* This function is called when the "subtitle step reset menu item" is clicked */
static void subtitle_offset_reset_cb (GtkWidget *widget, CustomData *data) {

  char offset_value[16];

  data->subtitle_offset = 0;

  sprintf(offset_value, "%d", data->subtitle_offset);
  iniparser_set (data->ini, "Subtitles:offset", offset_value);
  iniparser_save (data->ini, CONFIG_INI);

  g_print("%s called(offset:%d)\n", __func__, data->subtitle_offset);

  g_object_set (data->playbin, "subtitle-offset", data->subtitle_offset, NULL);

  analyze_streams(data);
}

/* This function is called when the PLAY button is clicked */
static void play_cb (GtkButton *button, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_PLAYING);
}

/* This function is called when the PAUSE button is clicked */
static void pause_cb (GtkButton *button, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_PAUSED);
}

/* This function is called when the STOP button is clicked */
static void stop_cb (GtkButton *button, CustomData *data) {
  gst_element_set_state (data->playbin, GST_STATE_READY);
}

/* This function is called when the main window is closed */
static void delete_event_cb (GtkWidget *widget, GdkEvent *event, CustomData *data) {
  stop_cb (NULL, data);
  gtk_main_quit ();
}

/* This function is called everytime the video window needs to be redrawn (due to damage/exposure,
 * rescaling, etc). GStreamer takes care of this in the PAUSED and PLAYING states, otherwise,
 * we simply draw a black rectangle to avoid garbage showing up. */
static gboolean draw_cb (GtkWidget *widget, cairo_t *cr, CustomData *data) {
  if (data->state < GST_STATE_PAUSED) {
    GtkAllocation allocation;

    /* Cairo is a 2D graphics library which we use here to clean the video window.
     * It is used by GStreamer for other reasons, so it will always be available to us. */
    gtk_widget_get_allocation (widget, &allocation);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_rectangle (cr, 0, 0, allocation.width, allocation.height);
    cairo_fill (cr);
  }

  return FALSE;
}

/* This function is called when the slider changes its position. We perform a seek to the
 * new position here. */
static void slider_cb (GtkRange *range, CustomData *data) {
  g_print("%s called\n", __func__);

  gdouble value = gtk_range_get_value (GTK_RANGE (data->slider));
  gst_element_seek_simple (data->playbin, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
      (gint64)(value * GST_SECOND));
}

/* This creates all the GTK+ widgets that compose our application, and registers the callbacks */
static void create_ui (CustomData *data) {
  GtkWidget *main_window;  /* The uppermost window, containing all other windows */
  GtkWidget *video_window; /* The drawing area where the video will be shown */
  GtkWidget *main_box;     /* VBox to hold main_hbox and the controls */
  GtkWidget *main_hbox;    /* HBox to hold the video_window and the stream info text widget */
  GtkWidget *controls;     /* HBox to hold the buttons and the slider */
  GtkWidget *play_button, *pause_button, *stop_button; /* Buttons */
  GtkWidget *menubar;
#if 0
  GtkWidget *subtitle_menu_item, \
            *subtitle_dropdown_menu, \
            *subtitle_uri_item, \
            *subtitle_font_item, \
            *subtitle_color_item, \
            *subtitle_outline_color_item, \
            *subtitle_position_item, \
            *subtitle_silent_item, \
            *subtitle_offset_forward_item, \
            *subtitle_offset_backward_item, \
            *subtitle_offset_reset_item;
#else
  GtkWidget *subtitle_menu_item, \
            *subtitle_dropdown_menu, \
            *subtitle_uri_item, \
            *subtitle_font_item, \
            *subtitle_color_item, \
            *subtitle_outline_color_item, \
            *subtitle_position_item, \
            *subtitle_silent_item;
  GtkWidget *subtitle_offset_forward_button, \
            *subtitle_offset_backward_button, \
            *subtitle_offset_reset_button;
#endif

  main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (delete_event_cb), data);

  video_window = gtk_drawing_area_new ();
  gtk_widget_set_double_buffered (video_window, FALSE);
  g_signal_connect (video_window, "realize", G_CALLBACK (realize_cb), data);
  g_signal_connect (video_window, "draw", G_CALLBACK (draw_cb), data);

  menubar = gtk_menu_bar_new ();
  // top-layer menu item
  subtitle_menu_item = gtk_menu_item_new_with_label ("Subtitles");
  gtk_menu_shell_append (GTK_MENU_SHELL (menubar), subtitle_menu_item);
  // drop-down menu
  subtitle_dropdown_menu = gtk_menu_new ();
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (subtitle_menu_item), subtitle_dropdown_menu);
  // sub-menu item -- Subtitle File
  subtitle_uri_item = gtk_menu_item_new_with_label ("Open...");
  gtk_menu_shell_append (GTK_MENU_SHELL (subtitle_dropdown_menu), subtitle_uri_item);
  g_signal_connect (G_OBJECT (subtitle_uri_item), "activate", G_CALLBACK (subtitle_uri_cb), data);
  // sub-menu item -- Subtitle Font
  subtitle_font_item = gtk_menu_item_new_with_label ("Font...");
  gtk_menu_shell_append (GTK_MENU_SHELL (subtitle_dropdown_menu), subtitle_font_item);
  g_signal_connect (G_OBJECT (subtitle_font_item), "activate", G_CALLBACK (subtitle_font_cb), data);
  // sub-menu item -- Subtitle Silent
  subtitle_silent_item = gtk_menu_item_new_with_label ("Show / Dismiss");
  gtk_menu_shell_append (GTK_MENU_SHELL (subtitle_dropdown_menu), subtitle_silent_item);
  g_signal_connect (G_OBJECT (subtitle_silent_item), "activate", G_CALLBACK (subtitle_silent_cb), data);

#if 0
  // sub-menu item -- Subtitle Offset Forward
  subtitle_offset_forward_item = gtk_menu_item_new_with_label ("Offset Forward");
  gtk_menu_shell_append (GTK_MENU_SHELL (subtitle_dropdown_menu), subtitle_offset_forward_item);
  g_signal_connect (G_OBJECT (subtitle_offset_forward_item), "activate", G_CALLBACK (subtitle_offset_forward_cb), data);
  // sub-menu item -- Subtitle Offset Backward
  subtitle_offset_backward_item = gtk_menu_item_new_with_label ("Offset Backward");
  gtk_menu_shell_append (GTK_MENU_SHELL (subtitle_dropdown_menu), subtitle_offset_backward_item);
  g_signal_connect (G_OBJECT (subtitle_offset_backward_item), "activate", G_CALLBACK (subtitle_offset_backward_cb), data);
  // sub-menu item -- Subtitle Step Reset
  subtitle_offset_reset_item = gtk_menu_item_new_with_label ("Offset Reset");
  gtk_menu_shell_append (GTK_MENU_SHELL (subtitle_dropdown_menu), subtitle_offset_reset_item);
  g_signal_connect (G_OBJECT (subtitle_offset_reset_item), "activate", G_CALLBACK (subtitle_offset_reset_cb), data);
#else
  subtitle_offset_forward_button = gtk_button_new_with_label ("forward");
  g_signal_connect (G_OBJECT (subtitle_offset_forward_button), "clicked", G_CALLBACK (subtitle_offset_forward_cb), data);

  subtitle_offset_backward_button = gtk_button_new_with_label ("backward");
  g_signal_connect (G_OBJECT (subtitle_offset_backward_button), "clicked", G_CALLBACK (subtitle_offset_backward_cb), data);

  subtitle_offset_reset_button = gtk_button_new_with_label ("reset");
  g_signal_connect (G_OBJECT (subtitle_offset_reset_button), "clicked", G_CALLBACK (subtitle_offset_reset_cb), data);
#endif

  play_button = gtk_button_new_from_icon_name ("media-playback-start", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (play_button), "clicked", G_CALLBACK (play_cb), data);

  pause_button = gtk_button_new_from_icon_name ("media-playback-pause", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (pause_button), "clicked", G_CALLBACK (pause_cb), data);

  stop_button = gtk_button_new_from_icon_name ("media-playback-stop", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (stop_button), "clicked", G_CALLBACK (stop_cb), data);

  data->slider = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_scale_set_draw_value (GTK_SCALE (data->slider), 0);
  data->slider_update_signal_id = g_signal_connect (G_OBJECT (data->slider), "value-changed", G_CALLBACK (slider_cb), data);

  data->streams_list = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (data->streams_list), FALSE);

  controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (controls), play_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), pause_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), stop_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), data->slider, TRUE, TRUE, 2);
  gtk_box_pack_start (GTK_BOX (controls), subtitle_offset_forward_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), subtitle_offset_backward_button, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (controls), subtitle_offset_reset_button, FALSE, FALSE, 2);

  main_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), menubar, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), video_window, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (main_hbox), data->streams_list, FALSE, FALSE, 2);

  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start (GTK_BOX (main_box), main_hbox, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (main_box), controls, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (main_window), main_box);
  gtk_window_set_default_size (GTK_WINDOW (main_window), 1280, 720);

  gtk_widget_show_all (main_window);
}

/* This function is called periodically to refresh the GUI */
static gboolean refresh_ui (CustomData *data) {
  gint64 current = -1;

  /* We do not want to update anything unless we are in the PAUSED or PLAYING states */
  if (data->state < GST_STATE_PAUSED)
    return TRUE;

  /* If we didn't know it yet, query the stream duration */
  if (!GST_CLOCK_TIME_IS_VALID (data->duration)) {
    if (!gst_element_query_duration (data->playbin, GST_FORMAT_TIME, &data->duration)) {
      g_printerr ("Could not query current duration.\n");
    } else {
      /* Set the range of the slider to the clip duration, in SECONDS */
      gtk_range_set_range (GTK_RANGE (data->slider), 0, (gdouble)data->duration / GST_SECOND);
    }
  }

  if (gst_element_query_position (data->playbin, GST_FORMAT_TIME, &current)) {
    /* Block the "value-changed" signal, so the slider_cb function is not called
     * (which would trigger a seek the user has not requested) */
    g_signal_handler_block (data->slider, data->slider_update_signal_id);
    /* Set the position of the slider to the current pipeline positoin, in SECONDS */
    gtk_range_set_value (GTK_RANGE (data->slider), (gdouble)current / GST_SECOND);
    /* Re-enable the signal */
    g_signal_handler_unblock (data->slider, data->slider_update_signal_id);
  }
  return TRUE;
}

/* This function is called when new metadata is discovered in the stream */
static void tags_cb (GstElement *playbin, gint stream, CustomData *data) {
  /* We are possibly in a GStreamer working thread, so we notify the main
   * thread of this event through a message in the bus */
  gst_element_post_message (playbin,
    gst_message_new_application (GST_OBJECT (playbin),
      gst_structure_new_empty ("tags-changed")));
}

/* This function is called when an error message is posted on the bus */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;

  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);

  /* Set the pipeline to READY (which stops playback) */
  gst_element_set_state (data->playbin, GST_STATE_READY);
}

/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) */
static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  g_print ("End-Of-Stream reached.\n");
  gst_element_set_state (data->playbin, GST_STATE_READY);
}

/* This function is called when the pipeline changes states. We use it to
 * keep track of the current state. */
static void state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstState old_state, new_state, pending_state;
  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->playbin)) {
    data->state = new_state;
    g_print ("State set to %s\n", gst_element_state_get_name (new_state));
    if (old_state == GST_STATE_READY && new_state == GST_STATE_PAUSED) {
      /* For extra responsiveness, we refresh the GUI as soon as we reach the PAUSED state */
      refresh_ui (data);

      update_flag(data->playbin, GST_PLAY_FLAG_TEXT, !data->subtitle_silent);
      g_object_set (data->playbin, "subtitle-offset", data->subtitle_offset, NULL);
    }
  }
}

/* Extract metadata from all the streams and write it to the text widget in the GUI */
static void analyze_streams (CustomData *data) {
  gint i;
  GstTagList *tags;
  gchar *str, *total_str;
  guint rate;
  gint n_video, n_audio, n_text;
  GtkTextBuffer *text;
  gchar *subtitle_uri;
  gchar *subtitle_font_desc;

  /* Clean current contents of the widget */
  text = gtk_text_view_get_buffer (GTK_TEXT_VIEW (data->streams_list));
  gtk_text_buffer_set_text (text, "", -1);

  /* Read some properties */
  g_object_get (data->playbin, "n-video", &n_video, NULL);
  g_object_get (data->playbin, "n-audio", &n_audio, NULL);
  g_object_get (data->playbin, "n-text", &n_text, NULL);

  for (i = 0; i < n_video; i++) {
    tags = NULL;
    /* Retrieve the stream's video tags */
    g_signal_emit_by_name (data->playbin, "get-video-tags", i, &tags);
    if (tags) {
      total_str = g_strdup_printf ("video stream %d:\n", i);
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      gst_tag_list_get_string (tags, GST_TAG_VIDEO_CODEC, &str);
      total_str = g_strdup_printf ("  codec: %s\n", str ? str : "unknown");
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      g_free (str);
      gst_tag_list_free (tags);
    }
  }

  for (i = 0; i < n_audio; i++) {
    tags = NULL;
    /* Retrieve the stream's audio tags */
    g_signal_emit_by_name (data->playbin, "get-audio-tags", i, &tags);
    if (tags) {
      total_str = g_strdup_printf ("\naudio stream %d:\n", i);
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      if (gst_tag_list_get_string (tags, GST_TAG_AUDIO_CODEC, &str)) {
        total_str = g_strdup_printf ("  codec: %s\n", str);
        gtk_text_buffer_insert_at_cursor (text, total_str, -1);
        g_free (total_str);
        g_free (str);
      }
      if (gst_tag_list_get_string (tags, GST_TAG_LANGUAGE_CODE, &str)) {
        total_str = g_strdup_printf ("  language: %s\n", str);
        gtk_text_buffer_insert_at_cursor (text, total_str, -1);
        g_free (total_str);
        g_free (str);
      }
      if (gst_tag_list_get_uint (tags, GST_TAG_BITRATE, &rate)) {
        total_str = g_strdup_printf ("  bitrate: %d\n", rate);
        gtk_text_buffer_insert_at_cursor (text, total_str, -1);
        g_free (total_str);
      }
      gst_tag_list_free (tags);
    }
  }

  for (i = 0; i < n_text; i++) {
    tags = NULL;
    /* Retrieve the stream's subtitle tags */
    g_signal_emit_by_name (data->playbin, "get-text-tags", i, &tags);
    if (tags) {
      total_str = g_strdup_printf ("\nsubtitle stream %d:\n", i);
      gtk_text_buffer_insert_at_cursor (text, total_str, -1);
      g_free (total_str);
      if (gst_tag_list_get_string (tags, GST_TAG_LANGUAGE_CODE, &str)) {
        total_str = g_strdup_printf ("  language: %s\n", str);
        gtk_text_buffer_insert_at_cursor (text, total_str, -1);
        g_free (total_str);
        g_free (str);
      }
      gst_tag_list_free (tags);
    }
  }

  /* Read some subtitle properties */
  g_object_get (data->playbin, "current-suburi", &subtitle_uri, NULL);
  g_object_get (data->playbin, "subtitle-font-desc", &subtitle_font_desc, NULL);

  total_str = g_strdup_printf ("\nsubtitle:\n"
                              "  uri:%s\n"
                              "  font:%s\n"
                              "  silent:%s\n"
                              "  offset:%dms\n",
                              (subtitle_uri == NULL) ? "Not Loaded" : "Loaded",
                              (subtitle_font_desc == NULL) ? "Sans 12" : subtitle_font_desc,
                              data->subtitle_silent ? "True" : "False",
                              data->subtitle_offset);
  gtk_text_buffer_insert_at_cursor (text, total_str, -1);
  g_free (total_str);
}

/* This function is called when an "application" message is posted on the bus.
 * Here we retrieve the message posted by the tags_cb callback */
static void application_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  if (g_strcmp0 (gst_structure_get_name (gst_message_get_structure (msg)), "tags-changed") == 0) {
    /* If the message is the "tags-changed" (only one we are currently issuing), update
     * the stream info GUI */
    analyze_streams (data);
  }
}

static void
print_usage (int argc, char **argv) {
  g_print ("usage: %s <filename>\n", argv[0]);
}

void create_config_ini_file(void)
{
    FILE *ini ;

    if ((ini=fopen(CONFIG_INI, "w"))==NULL) {
        g_printerr ("Cannot create %s\n", CONFIG_INI);
        return ;
    }

    fprintf(ini,
    "#\n"
    "# This is a config ini file\n"
    "#\n"
    "\n"
    "[Subtitles]\n"
    "font = Sans 12;\n"
    "silent = FALSE;\n"
    "offset = 0;\n"
    "\n");
    fclose(ini);
}

int main(int argc, char *argv[]) {
  CustomData data;
  GstStateChangeReturn ret;
  GstBus *bus;

  if (argc < 2) {
    print_usage (argc, argv);
    return -1;
  }

  if (access (CONFIG_INI, F_OK) != 0) {
    g_print ("%s is not existed. Creating...\n", CONFIG_INI);
    create_config_ini_file();
  }

  /* Initialize GTK */
  gtk_init (&argc, &argv);

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  /* Initialize our data structure */
  memset (&data, 0, sizeof (data));
  data.ini = iniparser_load(CONFIG_INI);
  data.duration = GST_CLOCK_TIME_NONE;
  data.subtitle_silent = iniparser_getboolean (data.ini, "Subtitles:silent", FALSE);
  data.subtitle_offset = iniparser_getint (data.ini, "Subtitles:offset", 0);

  /* Create the elements */
  data.playbin = gst_element_factory_make ("playbin", "playbin");

  if (!data.playbin) {
    g_printerr ("Not all elements could be created.\n");
    return -1;
  }

  /* Set the URI to play */
  g_object_set (data.playbin, "uri", gst_filename_to_uri(argv[1], NULL), NULL);

  /* Connect to interesting signals in playbin */
  g_signal_connect (G_OBJECT (data.playbin), "video-tags-changed", (GCallback) tags_cb, &data);
  g_signal_connect (G_OBJECT (data.playbin), "audio-tags-changed", (GCallback) tags_cb, &data);
  g_signal_connect (G_OBJECT (data.playbin), "text-tags-changed", (GCallback) tags_cb, &data);

  /* Create the GUI */
  create_ui (&data);

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  bus = gst_element_get_bus (data.playbin);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, &data);
  g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback)eos_cb, &data);
  g_signal_connect (G_OBJECT (bus), "message::state-changed", (GCallback)state_changed_cb, &data);
  g_signal_connect (G_OBJECT (bus), "message::application", (GCallback)application_cb, &data);
  gst_object_unref (bus);

  /* Start playing */
  ret = gst_element_set_state (data.playbin, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    gst_object_unref (data.playbin);
    return -1;
  }

  /* Register a function that GLib will call every second */
  g_timeout_add_seconds (1, (GSourceFunc)refresh_ui, &data);

  /* Start the GTK main loop. We will not regain control until gtk_main_quit is called. */
  gtk_main ();

  /* Free resources */
  gst_element_set_state (data.playbin, GST_STATE_NULL);
  gst_object_unref (data.playbin);
  return 0;
}
