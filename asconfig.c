/* asconfig.c
 * Configure alsa .asoundrc file for playback
 * Compile with:
 * gcc -Wall -g asconfig.c `pkg-config --libs --cflags gtk+-3.0` -lasound -o asconfig
 */

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>

/* Config */

/* Set the default parameters for dmix and forced parameters
 * Forced parameters are used for cards which only support one
 * sample rate. These are tested on the available cards; if the
 * defaults below are not valid for a card, the nearest sample
 * rate is choosen along with the first supported format and the
 * minimum supported number of channels returned by the hardware.
 */
#define ASCONFIG_DEFAULT_RATE 48000
#define ASCONFIG_DEFAULT_FORMAT_NAME "S16_LE"
#define ASCONFIG_DEFAULT_FORMAT SND_PCM_FORMAT_S16_LE
#define ASCONFIG_DEFAULT_CHANNELS 2
/* Set the default resampler and interface from the arrays below
 * This sets the default selected item in the dropdowns
 */
#define ASCONFIG_DEFAULT_RESAMPLER 1
#define ASCONFIG_DEFAULT_PLAYBACK_INTERFACE 1
#define ASCONFIG_DEFAULT_CAPTURE_INTERFACE 1

/* Set the command to use for the streaming output
 * ASCONFIG_STREAM_INPUT_FORMAT:    output format of alsa file plugin. Can be "raw" or "wav".
 * ASCONFIG_STREAM_COMMAND:         filename or pipe followed by streaming command. Examples below.
 *
 * EXAMPLES using icecast and ezstream with raw alsa output
 * #define ASCONFIG_STREAM_INPUT_FORMAT "raw"
 * #define ASCONFIG_STREAM_COMMAND "| oggenc -Q -q6 -r -B %b -C %c -R %r - | /usr/local/bin/ezstream -c /path/to/config"
 * #define ASCONFIG_STREAM_COMMAND "| lame -r --bitwidth %b -s %r -m j -q6 --cbr -b 192 - - | /usr/local/bin/ezstream -c /path/to/config"
 *
 * EXAMPLES using ffmpeg with wav alsa output
 * #define ASCONFIG_STREAM_INPUT_FORMAT "wav"
 * High quality flac stream:
 * #define ASCONFIG_STREAM_COMMAND "| ffmpeg -hide_banner -nostats -guess_layout_max 0 -loglevel error -re -i - -c:a flac -f ogg -content_type 'application/ogg' icecast://source:PASSWORD@icecast_server:8000/test.ogg"
 * WARNING: use -re if the stream is not default device to play in realtime
 *          REMOVE -re if using as default device, otherwise pipe will block and audio will stutter
 */
#define ASCONFIG_STREAM_INPUT_FORMAT "raw"
#define ASCONFIG_STREAM_COMMAND "| lame -r --bitwidth %b -s %r -m j -q6 --cbr -b 192 - - | /usr/local/bin/ezstream -c /path/to/config"
/* End of config */

typedef struct {
   guint card;
   gchar *ID;
   gchar *name;
} ASCONFIG_CARD;

typedef struct {
   GtkWidget *playbackInterface;
   GtkWidget *captureInterface;
   GtkWidget *resampler;
   GtkWidget *streamSwitch;
   GtkWidget *streamDefault;
} ASCONFIG_CONTROLS;

typedef struct {
   GtkWidget *playbackTreeview;
   GtkWidget *captureTreeview;
} ASCONFIG_DEVICE_VIEW;

enum {
   COLUMN_IN_USE,
   COLUMN_CARD,
   COLUMN_CARD_ID,
   COLUMN_CARD_NAME,
   COLUMN_DEVICE,
   COLUMN_DEVICE_ID,
   COLUMN_DEVICE_NAME,
   COLUMN_DEVICE_MIN_CHANNELS,
   COLUMN_DEVICE_MAX_CHANNELS,
   COLUMN_DEVICE_MIN_RATE,
   COLUMN_DEVICE_MAX_RATE,
   COLUMN_DEVICE_FORMAT,
   COLUMN_DEVICE_ALSA_HW,
   COLUMN_DEFAULT_RATE,
   COLUMN_DEFAULT_FORMAT,
   COLUMN_DEFAULT_CHANNELS,
   NUM_COLUMNS
};

static GtkWidget *window = NULL;

static snd_ctl_t *handle= NULL;
static snd_pcm_t *pcm= NULL;
static snd_ctl_card_info_t *info;
static snd_pcm_info_t *pcminfo;
static snd_pcm_hw_params_t *pars;
static snd_pcm_format_mask_t *fmask;
static ASCONFIG_CONTROLS asconfigControls;
static const gchar *playbackInterfaceTypes[] = { "hw", "plug", "dmix", NULL };
static const gchar *captureInterfaceTypes[] = { "hw", "plug", "dsnoop", NULL };
static const gchar *resamplers[] = { "speexrate", "speexrate_medium", "speexrate_best", NULL };

static int show_actionbox(const gchar *msg, const gchar *title);
static void show_msgbox(const gchar *msg, const gchar *title, gint type);

static gchar **getSampleFormats(const snd_pcm_format_mask_t *fmask) {
   guint fmt, i=0;
   gchar **sample_formats;
   
   sample_formats=malloc((SND_PCM_FORMAT_LAST+1)*sizeof(gchar*));
   for (fmt=0; fmt <= SND_PCM_FORMAT_LAST; fmt++) {
      if (snd_pcm_format_mask_test(fmask,(snd_pcm_format_t)fmt)) {
         sample_formats[i]=g_strdup(snd_pcm_format_name((snd_pcm_format_t)fmt));
         i++;
      }
   }
   sample_formats[i]=NULL;
   return sample_formats;
}

static void free_sample_formats(gchar **sample_formats) {
   guint i;
   
   for (i=0; sample_formats[i]!=NULL; i++)
      free(sample_formats[i]);
   
   free(sample_formats);
}
/* Stream is SND_PCM_STREAM_PLAYBACK or SND_PCM_STREAM_CAPTURE */
static void scancards(snd_pcm_stream_t stream, GtkListStore *store)
{
   gchar hwdev[64];
   gchar defaultFormat[64];
   guint min_sr, max_sr, min_ch, max_ch;
   guint defaultRate, defaultChannels;
   gint card, err, dev, direction;
   ASCONFIG_CARD cardInfo;
   GtkTreeIter iter;
   gchar **sample_formats;
   gchar *sampleFormatsCSV;
   gchar playback[16]="Playback";
   gchar capture[16]="Capture";
   gchar *streamType;

   if (stream==SND_PCM_STREAM_PLAYBACK)
      streamType=playback;
   else
      streamType=capture;

   card=-1; /* Return first available card */

   while (snd_card_next(&card)==0 && card>=0) {
      snprintf(hwdev, 64, "hw:%d", card);
      err=snd_ctl_open(&handle, hwdev, 0);
      if (err!=0) {
         g_warning("%s: Error opening card %s: %s", streamType, hwdev, strerror(-err));
         continue;
      }
      err=snd_ctl_card_info(handle, info);
      if (err!=0) {
         g_warning("%s: Error opening card %s: %s", streamType, hwdev, strerror(-err));
         snd_ctl_close(handle);
         continue;
      }
      cardInfo.card=card;
      cardInfo.ID=g_strdup(snd_ctl_card_info_get_id(info));
      cardInfo.name=g_strdup(snd_ctl_card_info_get_name(info));
      
      dev=-1;  /* Return first available device */

      while (snd_ctl_pcm_next_device(handle, &dev)==0 && dev>=0) {
         snprintf(hwdev, 64, "hw:%d,%d", card, dev);
         snd_pcm_info_set_device(pcminfo, dev);
         snd_pcm_info_set_subdevice(pcminfo, 0);
         snd_pcm_info_set_stream(pcminfo, stream);
         err=snd_ctl_pcm_info(handle, pcminfo);
         if (err!=0) {
            g_warning("%s: Error opening device %s: %s", streamType, hwdev, strerror(-err));
            continue;
         }

         gtk_list_store_insert_with_values (store, &iter, -1,
                              COLUMN_CARD, cardInfo.card,
                              COLUMN_CARD_ID, cardInfo.ID,
                              COLUMN_CARD_NAME, cardInfo.name,
                              COLUMN_DEVICE, dev,
                              COLUMN_DEVICE_ID, snd_pcm_info_get_id(pcminfo),
                              COLUMN_DEVICE_NAME, snd_pcm_info_get_name(pcminfo),
                              COLUMN_DEVICE_ALSA_HW, hwdev,
                              -1);
                              
         err=snd_pcm_open(&pcm, hwdev, stream, SND_PCM_NONBLOCK);
         if (err!=0) {
            if (err==-EBUSY)
               gtk_list_store_set(store, &iter, COLUMN_IN_USE, "*", -1);
            else {
               g_warning("%s: Error opening pcm device %s: %s", streamType, hwdev, strerror(-err));
               gtk_list_store_set(store, &iter, COLUMN_IN_USE, "E", -1);
            }
            continue;
         }
         
         err= snd_pcm_hw_params_any(pcm, pars);
         if (err==0) {
            snd_pcm_hw_params_get_channels_min(pars, &min_ch);
            snd_pcm_hw_params_get_channels_max(pars, &max_ch);
            snd_pcm_hw_params_get_rate_min(pars, &min_sr, NULL);
            snd_pcm_hw_params_get_rate_max(pars, &max_sr, NULL);

            snd_pcm_hw_params_get_format_mask(pars, fmask);
            sample_formats=getSampleFormats(fmask);
            sampleFormatsCSV=g_strjoinv(", ", sample_formats);

            defaultRate=ASCONFIG_DEFAULT_RATE;
            err=snd_pcm_hw_params_set_rate_near(pcm, pars, &defaultRate, &direction);
            if (err!=0)
               defaultRate=min_sr;
         
            err=snd_pcm_hw_params_set_format(pcm, pars, ASCONFIG_DEFAULT_FORMAT);
            if (err==0)
               snprintf(defaultFormat, 64, "%s", ASCONFIG_DEFAULT_FORMAT_NAME);
            else
               snprintf(defaultFormat, 64, "%s", sample_formats[0]); /* Fall back to first supported format */
            
            err=snd_pcm_hw_params_set_channels(pcm, pars, ASCONFIG_DEFAULT_CHANNELS);
            if (err==0)
               defaultChannels=ASCONFIG_DEFAULT_CHANNELS;
            else
               defaultChannels=min_ch; /* Fall back to minimum channels */

            gtk_list_store_set(store, &iter,
                                 COLUMN_IN_USE, NULL,
                                 COLUMN_DEVICE_MIN_CHANNELS, min_ch,
                                 COLUMN_DEVICE_MAX_CHANNELS, max_ch,
                                 COLUMN_DEVICE_MIN_RATE, min_sr,
                                 COLUMN_DEVICE_MAX_RATE, max_sr,
                                 COLUMN_DEVICE_FORMAT, sampleFormatsCSV,
                                 COLUMN_DEFAULT_RATE, defaultRate,
                                 COLUMN_DEFAULT_FORMAT, defaultFormat,
                                 COLUMN_DEFAULT_CHANNELS, defaultChannels,
                                 -1);
            free_sample_formats(sample_formats);
            g_free(sampleFormatsCSV);
         }
         else {
            g_warning("%s: Error obtaining device %s parameters", streamType, hwdev);
            gtk_list_store_set(store, &iter, COLUMN_IN_USE, "E", -1);
         }
         snd_pcm_close(pcm);
         pcm=NULL;
      }
      snd_ctl_close(handle);
      g_free(cardInfo.ID);
      g_free(cardInfo.name);
  }
}

// TODO: channels and bindings?
static void add_dsnoop(FILE *asoundrcFD, gchar *pcmName, gchar *slavePCM, gchar *defaultFormat, guint defaultChannels, guint defaultRate) {
   fprintf(asoundrcFD, "# Allow capture by multiple applications.\n"
                       "pcm.!%s {\n"
                       "   type dsnoop\n"
                       "   ipc_key 17022021\n"
                       "   ipc_key_add_uid yes\n"
                       "   slave {\n"
                       "      pcm \"%s\"\n"
                       "      period_size 1024\n"
                       "      buffer_size 4096\n"
                       "      format %s\n"
                       "      rate %u\n"
                       "      channels %u\n"
                       "      periods 0\n"
                       "      period_time 0\n"
                       "   }\n"
                       "   bindings {\n"
                       "      0 0\n"
                       "      1 1\n"
                       "   }\n"
                       "}\n", pcmName, slavePCM, defaultFormat, defaultRate, defaultChannels);
}

static void add_dmixStream(FILE *asoundrcFD, gchar *pcmName, gchar *dmixPCM, gchar *streamPCM) {
   fprintf(asoundrcFD, "# NOTE: dmix can only output to a hardware device.\n"
                       "# To use the stream pcm, the program whose output \n"
                       "# is to be streamed must be told to use the %s pcm\n"
                       "# e.g.\n" \
                       "#    mplayer -ao alsa:device=%s\n"
                       "#    chromium --alsa-output-device='%s'\n"
                       "#    AUDIODEV=%s ffplay\n", streamPCM, streamPCM, streamPCM, streamPCM);

   fprintf(asoundrcFD, "# Local volume control for stream input to dmix.\n"
                       "pcm.!%s {\n"
                       "   type softvol\n"
                       "   slave {\n"
                       "      pcm %s\n"
                       "   }\n"
                       "   control {\n"
                       "      name Stream\n"
                       "      card 0\n"
                       "   }\n"
                       "}\n", pcmName, dmixPCM);
}

static void add_streamOut(FILE *asoundrcFD, gchar *pcmName, const gchar *streamFormat, char *streamSlavePCM, const gchar *streamCommand) {
   fprintf(asoundrcFD, "# Stream output.\n"
                       "pcm.!%s {\n"
                       "   type file\n"
                       "   format \"%s\"\n"
                       "   slave {\n"
                       "      pcm %s\n"
                       "   }\n"
                       "   file \"%s\"\n"
                       "}\n", pcmName, streamFormat, streamSlavePCM, streamCommand);
}

static void add_plug(FILE *asoundrcFD, gchar *pcmName, gchar *slavePCM) {
   fprintf(asoundrcFD, "# Convert formats (bit depth) and sample rates.\n"
                       "pcm.!%s {\n"
                       "   type plug\n"
                       "   slave {\n"
                       "      pcm %s\n"
                       "   }\n"
                       "}\n", pcmName, slavePCM);
}

static void add_dmix(FILE *asoundrcFD, gchar *pcmName, gchar *slavePCM, gchar *defaultFormat, guint defaultChannels, guint defaultRate) {
   fprintf(asoundrcFD, "# Mix streams from several sources.\n"
                       "pcm.!%s {\n"
                       "   type dmix\n"
                       "   ipc_key 16022021\n"
                       "   ipc_key_add_uid yes\n"
                       "   slave {\n"
                       "      pcm %s\n"
                       "      format %s\n"
                       "      channels %u\n"
                       "      rate %u\n"
                       "   }\n"
                       "}\n", pcmName, slavePCM, defaultFormat, defaultChannels, defaultRate);
}

static void add_default(FILE *asoundrcFD, gchar *playbackPCM, gchar *capturePCM) {
   if (capturePCM==NULL)
      fprintf(asoundrcFD, "pcm.!default pcm.%s\n", playbackPCM);
   else {
      fprintf(asoundrcFD, "pcm.!default {\n"
                          "   type asym\n"
                          "   playback.pcm \"%s\"\n"
                          "   capture.pcm \"%s\"\n"
                          "}\n", playbackPCM, capturePCM);
   }
}

static void print_asoundrc(ASCONFIG_DEVICE_VIEW *deviceTreeview) {
   gint resampler, playbackInterfaceType=-1, captureInterfaceType=-1;
   gchar *defaultFormat=NULL, *captureFormat=NULL;
   guint card, dev;
   guint defaultRate, defaultChannels, min_ch, max_ch, min_sr, max_sr;
   guint captureCard, captureDev, captureRate, captureChannels;
   gchar slavePCM[16];
   gchar defaultPlaybackPCM[16], *defaultCapturePCM=NULL; /* Selected pcm devices for defaults */
   gchar *asoundrc;
   gint response_id=GTK_RESPONSE_NO;
   FILE *asoundrcFD;
   gboolean streamSwitchState, streamDefault;
   GtkTreeIter iter;
   GtkTreeModel *playbackModel, *captureModel;
   GtkTreeSelection *playbackSelection, *captureSelection;
   gchar *in_use;

   //playbackModel=gtk_tree_view_get_model(GTK_TREE_VIEW(deviceTreeview->playbackTreeview));
   playbackSelection=gtk_tree_view_get_selection(GTK_TREE_VIEW(deviceTreeview->playbackTreeview));

   if ( ! gtk_tree_selection_get_selected(playbackSelection, &playbackModel, &iter)) {
      show_msgbox("No selected playback device: please select a playback device from the list: not writing asoundrc!", "asconfig", GTK_MESSAGE_INFO);
      return;
   }
   gtk_tree_model_get(playbackModel, &iter, COLUMN_IN_USE, &in_use, -1);
   if (in_use!=NULL) {
      show_msgbox("The selected playback device is currently in use (blocked): not writing asoundrc!", "asconfig", GTK_MESSAGE_ERROR);
      g_free(in_use);
      return;
   }

   asoundrc=g_build_filename(g_get_home_dir(), ".asoundrc", NULL);
   if (g_file_test(asoundrc, G_FILE_TEST_EXISTS)) {
      response_id=show_actionbox("User alsa config file <i>.asoundrc</i> exists. <b>Overwrite?</b>", "Overwrite");
      if (response_id==GTK_RESPONSE_NO)
         return;
   }

   asoundrcFD=fopen(asoundrc, "w");
   if (asoundrcFD==NULL) {
      show_msgbox("Error opening .asoundrc for writing", "asconfig", GTK_MESSAGE_ERROR);
      return;
   }
   fprintf(asoundrcFD, "# User asoundrc file written by asconfig\n");

   gtk_tree_model_get(playbackModel, &iter,
               COLUMN_CARD, &card,
               COLUMN_DEVICE, &dev,
               COLUMN_DEVICE_MIN_CHANNELS, &min_ch,
               COLUMN_DEVICE_MAX_CHANNELS, &max_ch,
               COLUMN_DEVICE_MIN_RATE, &min_sr,
               COLUMN_DEVICE_MAX_RATE, &max_sr,
               COLUMN_DEFAULT_RATE, &defaultRate,
               COLUMN_DEFAULT_FORMAT, &defaultFormat,
               COLUMN_DEFAULT_CHANNELS, &defaultChannels,
               -1);

   /* If these are undefined for some reason fall back to hard coded defaults */
   if (defaultRate==0) defaultRate=ASCONFIG_DEFAULT_RATE;
   if (defaultFormat==NULL) defaultFormat=g_strdup(ASCONFIG_DEFAULT_FORMAT_NAME);
   if (defaultChannels==0) defaultChannels=ASCONFIG_DEFAULT_CHANNELS;

   resampler=gtk_combo_box_get_active(GTK_COMBO_BOX(asconfigControls.resampler));
   playbackInterfaceType=gtk_combo_box_get_active(GTK_COMBO_BOX(asconfigControls.playbackInterface));
   streamSwitchState=gtk_switch_get_active(GTK_SWITCH(asconfigControls.streamSwitch));
   streamDefault=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(asconfigControls.streamDefault));

   //captureModel=gtk_tree_view_get_model(GTK_TREE_VIEW(deviceTreeview->captureTreeview));
   captureSelection=gtk_tree_view_get_selection(GTK_TREE_VIEW(deviceTreeview->captureTreeview));
   if (gtk_tree_selection_get_selected(captureSelection, &captureModel, &iter)==TRUE) {
      gtk_tree_model_get(captureModel, &iter,
            COLUMN_CARD, &captureCard,
            COLUMN_DEVICE, &captureDev,
            COLUMN_DEFAULT_RATE, &captureRate,
            COLUMN_DEFAULT_FORMAT, &captureFormat,
            COLUMN_DEFAULT_CHANNELS, &captureChannels,
            -1);
      if (captureRate==0) captureRate=ASCONFIG_DEFAULT_RATE;
      if (captureFormat==NULL) captureFormat=g_strdup(ASCONFIG_DEFAULT_FORMAT_NAME);
      if (captureChannels==0) captureChannels=ASCONFIG_DEFAULT_CHANNELS;

      defaultCapturePCM=g_strdup("capture");
      captureInterfaceType=gtk_combo_box_get_active(GTK_COMBO_BOX(asconfigControls.captureInterface));
      fprintf(asoundrcFD, "# Selected capture device\n"
                          "pcm.!%s {\n"
                          "   type hw\n"
                          "   card %u\n"
                          "   device %u\n"
                          "}\n", defaultCapturePCM, captureCard, captureDev);
   }  /* If nothing selected, captureInterfaceType=-1 and defaultCapturePCM=NULL */

   switch (captureInterfaceType) {
      case 0:  /* hw */
         fprintf(asoundrcFD,"# Direct hardware access selected - no software conversions.\n"
                            "# Only one application can use the capture device at a time.\n"
                            "# Capture sample rates / formats / channels *MUST* match\n"
                            "# the cards native ranges, otherwise capturing will fail.\n");
      break;
      case 1:  /* plug */
         fprintf(asoundrcFD, "# Access hardware via plug: The capture format (bit depth)\n"
                             "# may be changed and / or resampling may take place in order\n"
                             "# to match the hardware requirements. Only one application \n"
                             "# can use the capture device at a time.\n");

         add_plug(asoundrcFD, "matchCapture", defaultCapturePCM);
         g_free(defaultCapturePCM); defaultCapturePCM=g_strdup("matchCapture");
      break;
      case 2:  /* dsnoop */
         fprintf(asoundrcFD, "# Allow multiple applications to capture at once. Output\n"
                             "# streams may be converted to a common format (bit depth)\n"
                             "# and sample rate using plug (dsnoop doesn't do conversions).\n");

         add_plug(asoundrcFD, "matchCapture", "snoopCapture");
         add_dsnoop(asoundrcFD, "snoopCapture", defaultCapturePCM, captureFormat, captureChannels, captureRate);
         g_free(defaultCapturePCM); defaultCapturePCM=g_strdup("matchCapture");
      break;
      default:
         /* Do nothing: no device selected or unknown interface type: Note captureInterfaceType=-1 also if no interface type selected */
      break;
   } 

   /* Common setup */
   strcpy(defaultPlaybackPCM, "playback");
   fprintf(asoundrcFD, "# Selected playback device\n"
                       "pcm.!%s {\n"
                       "   type hw\n"
                       "   card %u\n"
                       "   device %u\n"
                       "}\n", defaultPlaybackPCM, card, dev);

   if (min_sr>0 && min_sr==max_sr) {
      fprintf(asoundrcFD, "# Force parameters for playback on single rate cards\n"
                          "# Required for some cards, e.g bytcrrt5640\n"
                          "pcm.+%s {\n"
                          "   format %s\n"
                          "   channels %u\n"
                          "   rate %u\n"
                          "}\n", defaultPlaybackPCM, defaultFormat, defaultChannels, defaultRate);
   }

   fprintf(asoundrcFD, "# Default rate converter for plug and dmix\n"
                       "# Make sure package alsa-plugins is installed to use\n"
                       "# higher quality speexrate_medium resampling.\n"
                       "defaults.pcm.rate_converter \"%s\"\n", resamplers[resampler]);

   fprintf(asoundrcFD, "# Selected card mixer controls\n"
                       "ctl.!default {\n"
                       "   type hw\n"
                       "   card %u\n"
                       "}\n", card);
   /* End of common setup */

   switch (playbackInterfaceType) {
      case 0:  /* hw */
         fprintf(asoundrcFD,"# Direct hardware access selected - no software conversions.\n"
                            "# Only one application can use the playback device at a time.\n"
                            "# Playback sample rates / formats / channels *MUST* match\n"
                            "# the cards native ranges, otherwise playback will fail.\n");
         if (streamSwitchState==TRUE) {
            if (streamDefault==TRUE) {
               strcpy(slavePCM, defaultPlaybackPCM);
               strcpy(defaultPlaybackPCM, "stream");
            }
            else
               strcpy(slavePCM, "null");
            add_streamOut(asoundrcFD, "stream", ASCONFIG_STREAM_INPUT_FORMAT, slavePCM, ASCONFIG_STREAM_COMMAND);
         }
         add_default(asoundrcFD, defaultPlaybackPCM, defaultCapturePCM);
      break;
      case 1:  /* plug */
         fprintf(asoundrcFD, "# Access hardware via plug: The playback format (bit depth)\n"
                             "# may be changed and / or resampling may take place in order\n"
                             "# to match the hardware requirements. Only one application \n"
                             "# can use the playback device at a time.\n");
         if (streamSwitchState==TRUE) {
            if (streamDefault==TRUE) {
               strcpy(slavePCM, defaultPlaybackPCM);
               strcpy(defaultPlaybackPCM, "stream");
            }
            else
               strcpy(slavePCM, "null");
            add_streamOut(asoundrcFD, "stream", ASCONFIG_STREAM_INPUT_FORMAT, slavePCM, ASCONFIG_STREAM_COMMAND);
         }
         add_plug(asoundrcFD, "match", defaultPlaybackPCM);
         add_default(asoundrcFD, "match", defaultCapturePCM);
      break;
      case 2:  /* dmix */
         fprintf(asoundrcFD, "# Allow playback from multiple applications at once. Input\n"
                             "# streams may be converted to a common format (bit depth)\n"
                             "# and sample rate using plug (dmix doesn't do conversions).\n");
         if (streamSwitchState==TRUE) {
            add_dmixStream(asoundrcFD, "streamvol", "mix", "stream");
            add_streamOut(asoundrcFD, "stream", ASCONFIG_STREAM_INPUT_FORMAT, "streamvol", ASCONFIG_STREAM_COMMAND);
         }
         add_plug(asoundrcFD, "match", "mix");
         add_dmix(asoundrcFD, "mix", defaultPlaybackPCM, defaultFormat, defaultChannels, defaultRate);
         add_default(asoundrcFD, "match", defaultCapturePCM);
      break;
      default:
         g_warning("print_asoundrc(): Unknown interface type");
         add_default(asoundrcFD, defaultPlaybackPCM, defaultCapturePCM);
      break;
   }  
   fclose(asoundrcFD);

   g_free(defaultCapturePCM);
   g_free(defaultFormat);
   g_free(captureFormat);
   g_free(asoundrc);
}

static int show_actionbox(const gchar *msg, const gchar *title) {
   GtkWidget *dialog;
   GtkWidget *content_area;
   GtkWidget *title_label;
   GtkWidget *dialog_label;
   gint response_id=GTK_RESPONSE_CANCEL;
   gchar *title_markup=NULL;

   dialog=gtk_dialog_new_with_buttons(title, GTK_WINDOW(window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                       "_No", GTK_RESPONSE_NO,
                                       "_Yes", GTK_RESPONSE_YES,
                                       NULL);
   gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);
   dialog_label=gtk_label_new(NULL);
   title_label=gtk_label_new(NULL);
   title_markup=g_strdup_printf("%s%s%s","\n<b><span size=\"large\">",title,":</span></b>\n");
   gtk_label_set_markup(GTK_LABEL(title_label),title_markup);
   g_free(title_markup);
   gtk_label_set_markup(GTK_LABEL(dialog_label),msg);
   content_area=gtk_dialog_get_content_area(GTK_DIALOG(dialog));
   gtk_container_add(GTK_CONTAINER(content_area), title_label);
   gtk_container_add(GTK_CONTAINER(content_area), dialog_label);
   gtk_widget_show_all(dialog);
   response_id=gtk_dialog_run(GTK_DIALOG(dialog));
   gtk_widget_destroy(dialog);
   return response_id;
}

static void show_msgbox(const gchar *msg, const gchar *title, gint type) {
   GtkWidget *dialog;
   gchar *utf8_string=g_locale_to_utf8(msg, -1, NULL, NULL, NULL);

   if (utf8_string==NULL)
      g_warning("Can't convert message text to UTF8!");
   else
      dialog=gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_DESTROY_WITH_PARENT, type,GTK_BUTTONS_OK, "%s", utf8_string);

   gtk_window_set_title(GTK_WINDOW(dialog), title);
   g_signal_connect_swapped (dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
   gtk_widget_show_all(dialog);
   gtk_dialog_run(GTK_DIALOG(dialog)); /* Show a modal dialog */
   g_free(utf8_string);
}

static void refresh_clicked(GtkToolItem *item,  ASCONFIG_DEVICE_VIEW *deviceTreeview) {
   GtkTreeModel *model=gtk_tree_view_get_model (GTK_TREE_VIEW(deviceTreeview->playbackTreeview));
   gtk_list_store_clear(GTK_LIST_STORE(model));
   scancards(SND_PCM_STREAM_PLAYBACK, GTK_LIST_STORE(model));

   model=gtk_tree_view_get_model (GTK_TREE_VIEW(deviceTreeview->captureTreeview));
   gtk_list_store_clear(GTK_LIST_STORE(model));
   scancards(SND_PCM_STREAM_CAPTURE, GTK_LIST_STORE(model));
}

static void save_clicked(GtkToolItem *item, ASCONFIG_DEVICE_VIEW *deviceTreeview) {
   print_asoundrc(deviceTreeview);
}

static void add_columns(GtkTreeView *treeview) {
   GtkCellRenderer *renderer;
   GtkTreeViewColumn *column;
   guint i;
   const gchar *columnHeadings[]={ "","Card number","Card ID","Card name","Device number","Device ID","Device name","Min. channels","Max. channels","Min. Rate","Max. rate","Sample formats","Alsa HW path" };
   //  GtkTreeModel *model = gtk_tree_view_get_model (treeview);

   for (i=0; i<NUM_COLUMNS-3; i++) { /* Last 3 columns are hidden */
      renderer=gtk_cell_renderer_text_new();
      column=gtk_tree_view_column_new_with_attributes(columnHeadings[i], renderer, "text", i, NULL);
      gtk_tree_view_column_set_sort_column_id(column, i);
      gtk_tree_view_append_column(treeview, column);
   }
}

static GtkWidget *addCombo(const gchar **entries, const gchar *heading, GtkWidget *gbox, gint left, gint top) {
   gint i=0;
   GtkWidget *comboBox=NULL;
   GtkWidget *label;

   label=gtk_label_new(heading);
   gtk_grid_attach (GTK_GRID (gbox), label, left, top, 1, 1);

   comboBox=gtk_combo_box_text_new();
   if (comboBox==NULL) return NULL;
   
   gtk_grid_attach (GTK_GRID (gbox), comboBox, left+1, top, 1, 1);
   if (entries != NULL) {
      while (entries[i]!=NULL) {
         gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(comboBox), entries[i], entries[i]);
         i++;
      }
   }
   return comboBox;
}

static GtkWidget *addSwitch(const gchar *heading, GtkWidget *gbox, gint left, gint top) {
   GtkWidget *switchControl=NULL;
   GtkWidget *label;

   label=gtk_label_new(heading);
   gtk_grid_attach (GTK_GRID (gbox), label, left, top, 1, 1);

   switchControl=gtk_switch_new();
   if (switchControl==NULL) return NULL;
   
   gtk_grid_attach (GTK_GRID (gbox), switchControl, left+1, top, 1, 1);

   return switchControl;
}

static GtkWidget *addCheck(const gchar *heading, GtkWidget *gbox, gint left, gint top) {
   GtkWidget *checkControl=NULL;
   GtkWidget *label;

   label=gtk_label_new(heading);
   gtk_grid_attach (GTK_GRID (gbox), label, left, top, 1, 1);

   checkControl=gtk_check_button_new();
   if (checkControl==NULL) return NULL;
   
   gtk_grid_attach (GTK_GRID (gbox), checkControl, left+1, top, 1, 1);

   return checkControl;
}

static void addToolbar(GtkWidget *gbox, ASCONFIG_DEVICE_VIEW *deviceTreeview) {
   GtkWidget *tool_bar;
   GtkToolItem *toolButton;
   GtkWidget *buttonImage=NULL;
   GtkIconTheme *icon_theme;
   GdkPixbuf *pixbuf;

   tool_bar=gtk_toolbar_new();
   gtk_toolbar_set_style(GTK_TOOLBAR(tool_bar), GTK_TOOLBAR_ICONS);
   gtk_box_pack_start(GTK_BOX(gbox), tool_bar, FALSE, FALSE, 0);

   icon_theme=gtk_icon_theme_get_default();

   pixbuf=gtk_icon_theme_load_icon(icon_theme, "view-refresh", 24, 0, NULL);
   buttonImage=gtk_image_new_from_pixbuf(pixbuf);
   toolButton=gtk_tool_button_new(buttonImage, "Refresh");
   gtk_toolbar_insert(GTK_TOOLBAR(tool_bar), toolButton, -1);
   g_signal_connect(toolButton, "clicked", G_CALLBACK(refresh_clicked), deviceTreeview);
   g_object_unref(pixbuf);

   pixbuf=gtk_icon_theme_load_icon(icon_theme, "document-save", 24, 0, NULL);
   buttonImage=gtk_image_new_from_pixbuf(pixbuf);
   toolButton=gtk_tool_button_new(buttonImage, "Save");
   gtk_toolbar_insert(GTK_TOOLBAR(tool_bar), toolButton, -1);
   g_signal_connect(toolButton, "clicked", G_CALLBACK(save_clicked), deviceTreeview);
   g_object_unref(pixbuf);

   g_object_unref(icon_theme);
}

static gboolean streamSwitchState(GtkSwitch *widget, gboolean state, gpointer user_data) {
   gint playbackInterfaceType;
   
   if (state==TRUE)
      playbackInterfaceType=gtk_combo_box_get_active(GTK_COMBO_BOX(asconfigControls.playbackInterface));
   else
      playbackInterfaceType=-1; /* Control off: force default */

   switch (playbackInterfaceType) {
      case 0:  /* hw */
         gtk_widget_set_sensitive(GTK_WIDGET(asconfigControls.streamDefault), TRUE);
      break;
      case 1:  /* plug */
         gtk_widget_set_sensitive(GTK_WIDGET(asconfigControls.streamDefault), TRUE);
      break;
      default: /* dmix or off: lock default control; dmix output to hardware only  */
         gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(asconfigControls.streamDefault), FALSE);
         gtk_widget_set_sensitive(GTK_WIDGET(asconfigControls.streamDefault), FALSE);
      break;
   }  
   
   return FALSE;
}

static void playbackInterfaceChanged(GtkComboBox *widget, gpointer user_data) {
    streamSwitchState(NULL, gtk_switch_get_active(GTK_SWITCH(asconfigControls.streamSwitch)), NULL);
}

static GtkWidget *addControls(GtkWidget *windowVBox) {
   GtkWidget *controlGrid;
   int i=0;

   controlGrid=gtk_grid_new();
   gtk_grid_set_row_spacing(GTK_GRID (controlGrid), 4);
   gtk_grid_set_column_spacing(GTK_GRID (controlGrid), 4);
   gtk_container_set_border_width(GTK_CONTAINER (controlGrid), 8);
   gtk_container_add (GTK_CONTAINER(windowVBox), controlGrid);

   asconfigControls.resampler=addCombo(resamplers, "Resampler:", controlGrid, 0, i++);
   asconfigControls.playbackInterface=addCombo(playbackInterfaceTypes, "Playback interface:", controlGrid, 0, i++);
   asconfigControls.captureInterface=addCombo(captureInterfaceTypes, "Capture interface:", controlGrid, 0, i++);
   asconfigControls.streamSwitch=addSwitch("Add stream pcm:", controlGrid, 0, i);
   asconfigControls.streamDefault=addCheck("Stream is default:", controlGrid, 2, i++);
   
   gtk_combo_box_set_active(GTK_COMBO_BOX(asconfigControls.resampler), ASCONFIG_DEFAULT_RESAMPLER);
   gtk_combo_box_set_active(GTK_COMBO_BOX(asconfigControls.playbackInterface), ASCONFIG_DEFAULT_PLAYBACK_INTERFACE);
   gtk_combo_box_set_active(GTK_COMBO_BOX(asconfigControls.captureInterface), ASCONFIG_DEFAULT_CAPTURE_INTERFACE);

   gtk_switch_set_active(GTK_SWITCH(asconfigControls.streamSwitch), FALSE);
   streamSwitchState(NULL, gtk_switch_get_active(GTK_SWITCH(asconfigControls.streamSwitch)), NULL);

   return windowVBox;
}

GtkWidget *addTreeview(GtkWidget *vbox, snd_pcm_stream_t stream) {
   GtkWidget *treeview;
   GtkListStore *store;
   GtkWidget *sw;

   store=gtk_list_store_new(NUM_COLUMNS,
                              G_TYPE_STRING,
                              G_TYPE_UINT,
                              G_TYPE_STRING,
                              G_TYPE_STRING,
                              G_TYPE_UINT,
                              G_TYPE_STRING,
                              G_TYPE_STRING,
                              G_TYPE_UINT,
                              G_TYPE_UINT,
                              G_TYPE_UINT,
                              G_TYPE_UINT,
                              G_TYPE_STRING,
                              G_TYPE_STRING,
                              G_TYPE_UINT,
                              G_TYPE_STRING,
                              G_TYPE_UINT);

   scancards(stream, store);
   treeview=gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
   gtk_tree_view_set_search_column (GTK_TREE_VIEW(treeview), COLUMN_CARD);
   g_object_unref(GTK_TREE_MODEL(store));
   add_columns(GTK_TREE_VIEW(treeview));

   sw=gtk_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_ETCHED_IN);
   gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_box_pack_start(GTK_BOX (vbox), sw, TRUE, TRUE, 0);
   gtk_container_add(GTK_CONTAINER(sw), treeview);
   
   return treeview;
}

int main(int argc, char **argv) {
   GtkWidget *vbox;
   GtkWidget *label;
   ASCONFIG_DEVICE_VIEW deviceTreeview;

   gtk_init(NULL, NULL);
   
   /* create window, etc */
   window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
   gtk_window_set_title(GTK_WINDOW(window), "asconfig");

   gtk_container_set_border_width(GTK_CONTAINER(window), 8);

   snd_ctl_card_info_alloca(&info);
   snd_pcm_info_alloca(&pcminfo);
   snd_pcm_hw_params_alloca(&pars);
   snd_pcm_format_mask_alloca(&fmask);

   vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
   gtk_container_add(GTK_CONTAINER (window), vbox);
   addToolbar(vbox, &deviceTreeview);

   label=gtk_label_new("Select playback device:");
   gtk_box_pack_start(GTK_BOX (vbox), label, FALSE, TRUE, 0);
   deviceTreeview.playbackTreeview=addTreeview(vbox, SND_PCM_STREAM_PLAYBACK);
   label=gtk_label_new("Select capture device:");
   gtk_box_pack_start(GTK_BOX (vbox), label, FALSE, TRUE, 0);
   deviceTreeview.captureTreeview=addTreeview(vbox, SND_PCM_STREAM_CAPTURE);
   
   addControls(vbox);
   g_signal_connect(GTK_COMBO_BOX(asconfigControls.playbackInterface), "changed", G_CALLBACK(playbackInterfaceChanged), NULL);
   g_signal_connect(GTK_SWITCH(asconfigControls.streamSwitch), "state-set", G_CALLBACK(streamSwitchState), NULL);

   g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

   /* finish & show */
   gtk_window_set_default_size (GTK_WINDOW (window), 280, 250);

   gtk_widget_show_all (window);
   gtk_main();

  return 0;
}
