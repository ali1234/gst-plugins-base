/* Copyright (C) 2006 Tim-Philipp Müller <tim centricular net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "gstalsa.h"

#include <gst/audio/multichannel.h>

static GstCaps *
gst_alsa_detect_rates (GstObject * obj, snd_pcm_hw_params_t * hw_params,
    GstCaps * in_caps)
{
  GstCaps *caps;
  guint min, max;
  gint err, dir, min_rate, max_rate, i;

  GST_LOG_OBJECT (obj, "probing sample rates ...");

  if ((err = snd_pcm_hw_params_get_rate_min (hw_params, &min, &dir)) < 0)
    goto min_rate_err;

  if ((err = snd_pcm_hw_params_get_rate_max (hw_params, &max, &dir)) < 0)
    goto max_rate_err;

  min_rate = min;
  max_rate = max;

  if (min_rate < 4000)
    min_rate = 4000;            /* random 'sensible minimum' */

  if (max_rate <= 0)
    max_rate = G_MAXINT;        /* or maybe just use 192400 or so? */
  else if (max_rate > 0 && max_rate < 4000)
    max_rate = MAX (4000, min_rate);

  GST_DEBUG_OBJECT (obj, "Min. rate = %u (%d)", min_rate, min);
  GST_DEBUG_OBJECT (obj, "Max. rate = %u (%d)", max_rate, max);

  caps = gst_caps_make_writable (in_caps);

  for (i = 0; i < gst_caps_get_size (caps); ++i) {
    GstStructure *s;

    s = gst_caps_get_structure (caps, i);
    if (min_rate == max_rate) {
      gst_structure_set (s, "rate", G_TYPE_INT, min_rate, NULL);
    } else {
      gst_structure_set (s, "rate", GST_TYPE_INT_RANGE,
          min_rate, max_rate, NULL);
    }
  }

  return caps;

min_rate_err:
  {
    GST_ERROR_OBJECT (obj, "failed to query minimum sample rate: %s",
        snd_strerror (err));
    gst_caps_unref (in_caps);
    return NULL;
  }
max_rate_err:
  {
    GST_ERROR_OBJECT (obj, "failed to query maximum sample rate: %s",
        snd_strerror (err));
    gst_caps_unref (in_caps);
    return NULL;
  }
}

static const struct
{
  const int sformat;
  const int uformat;
} pcmformats[4] = {
  {
  SND_PCM_FORMAT_S8, SND_PCM_FORMAT_U8}, {
  SND_PCM_FORMAT_S16, SND_PCM_FORMAT_U16}, {
  SND_PCM_FORMAT_UNKNOWN, SND_PCM_FORMAT_UNKNOWN}, {
  SND_PCM_FORMAT_S32, SND_PCM_FORMAT_U32}
};

static GstCaps *
gst_alsa_detect_formats (GstObject * obj, snd_pcm_hw_params_t * hw_params,
    GstCaps * in_caps)
{
  snd_pcm_format_mask_t *mask;
  GstStructure *s;
  GstCaps *caps;
  gint i;

  snd_pcm_format_mask_alloca (&mask);
  snd_pcm_hw_params_get_format_mask (hw_params, mask);

  caps = gst_caps_new_empty ();

  for (i = 0; i < gst_caps_get_size (in_caps); ++i) {
    GstStructure *scopy;
    gint w, width = 0;

    s = gst_caps_get_structure (in_caps, i);
    if (!gst_structure_has_name (s, "audio/x-raw-int")) {
      GST_WARNING_OBJECT (obj, "skipping non-int format");
      continue;
    }
    gst_structure_get_int (s, "width", &width);
    g_assert (width != 0 && (width % 8) == 0);
    w = (width / 8) - 1;
    if (snd_pcm_format_mask_test (mask, pcmformats[w].sformat) &&
        snd_pcm_format_mask_test (mask, pcmformats[w].uformat)) {
      /* template contains { true, false } or just one, leave it as it is */
      scopy = gst_structure_copy (s);
    } else if (snd_pcm_format_mask_test (mask, pcmformats[w].sformat)) {
      scopy = gst_structure_copy (s);
      gst_structure_set (scopy, "signed", G_TYPE_BOOLEAN, TRUE, NULL);
    } else if (snd_pcm_format_mask_test (mask, pcmformats[w].uformat)) {
      scopy = gst_structure_copy (s);
      gst_structure_set (scopy, "signed", G_TYPE_BOOLEAN, FALSE, NULL);
    } else {
      scopy = NULL;
    }
    if (scopy) {
      if (width > 8) {
        /* TODO: proper endianness detection, for now it's CPU endianness only */
        gst_structure_set (scopy, "endianness", G_TYPE_INT, G_BYTE_ORDER, NULL);
      }
      gst_caps_append_structure (caps, scopy);
    }
  }

  gst_caps_unref (in_caps);
  return caps;
}

/* we don't have channel mappings for more than this many channels */
#define GST_ALSA_MAX_CHANNELS 8

static GstStructure *
get_channel_free_structure (const GstStructure * in_structure)
{
  GstStructure *s = gst_structure_copy (in_structure);

  gst_structure_remove_field (s, "channels");
  return s;
}

static void
caps_add_channel_configuration (GstCaps * caps,
    const GstStructure * in_structure, gint min_chans, gint max_chans)
{
  GstAudioChannelPosition pos[8] = {
    GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
    GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
    GST_AUDIO_CHANNEL_POSITION_REAR_LEFT,
    GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
    GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER,
    GST_AUDIO_CHANNEL_POSITION_LFE,
    GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT,
    GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT
  };
  GstStructure *s = NULL;
  gint c;

  if (min_chans == max_chans) {
    s = get_channel_free_structure (in_structure);
    gst_structure_set (s, "channels", G_TYPE_INT, max_chans, NULL);
    gst_caps_append_structure (caps, s);
    return;
  }

  g_assert (min_chans >= 1);

  /* mono and stereo don't need channel configurations */
  if (min_chans == 2) {
    s = get_channel_free_structure (in_structure);
    gst_structure_set (s, "channels", G_TYPE_INT, 2, NULL);
    gst_caps_append_structure (caps, s);
  } else if (min_chans == 1 && max_chans >= 2) {
    s = get_channel_free_structure (in_structure);
    gst_structure_set (s, "channels", GST_TYPE_INT_RANGE, 1, 2, NULL);
    gst_caps_append_structure (caps, s);
  }

  /* don't know whether to use 2.1 or 3.0 here - but I suspect
   * alsa might work around that/fix it somehow. Can we tell alsa
   * what our channel layout is like? */
  if (max_chans >= 3) {
    GstAudioChannelPosition pos_21[3] = {
      GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT,
      GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
      GST_AUDIO_CHANNEL_POSITION_LFE
    };

    s = get_channel_free_structure (in_structure);
    gst_structure_set (s, "channels", G_TYPE_INT, 3, NULL);
    gst_audio_set_channel_positions (s, pos_21);
    gst_caps_append_structure (caps, s);
  }

  /* everything else (4, 6, 8 channels) needs a channel layout */
  for (c = 4; c <= 8; c += 2) {
    if (max_chans >= c) {
      s = get_channel_free_structure (in_structure);
      gst_structure_set (s, "channels", G_TYPE_INT, c, NULL);
      gst_audio_set_channel_positions (s, pos);
      gst_caps_append_structure (caps, s);
    }
  }
}

static GstCaps *
gst_alsa_detect_channels (GstObject * obj, snd_pcm_hw_params_t * hw_params,
    GstCaps * in_caps)
{
  GstCaps *caps;
  guint min, max;
  gint min_chans, max_chans;
  gint err, i;

  GST_LOG_OBJECT (obj, "probing channels ...");

  if ((err = snd_pcm_hw_params_get_channels_min (hw_params, &min)) < 0)
    goto min_chan_error;

  if ((err = snd_pcm_hw_params_get_channels_max (hw_params, &max)) < 0)
    goto max_chan_error;

  /* note: the above functions may return (guint) -1 */
  min_chans = min;
  max_chans = max;

  if (min_chans < 0) {
    min_chans = 1;
    max_chans = GST_ALSA_MAX_CHANNELS;
  } else if (max_chans < 0) {
    max_chans = GST_ALSA_MAX_CHANNELS;
  }

  if (min_chans > max_chans) {
    gint temp;

    GST_WARNING_OBJECT (obj, "minimum channels > maximum channels (%d > %d), "
        "please fix your soundcard drivers", min, max);
    temp = min_chans;
    min_chans = max_chans;
    max_chans = temp;
  }

  min_chans = MAX (min_chans, 1);
  max_chans = MIN (GST_ALSA_MAX_CHANNELS, max_chans);

  GST_DEBUG_OBJECT (obj, "Min. channels = %d (%d)", min_chans, min);
  GST_DEBUG_OBJECT (obj, "Max. channels = %d (%d)", max_chans, max);

  caps = gst_caps_new_empty ();

  for (i = 0; i < gst_caps_get_size (in_caps); ++i) {
    GstStructure *s;
    GType field_type;
    gint c_min = min_chans;
    gint c_max = max_chans;

    s = gst_caps_get_structure (in_caps, i);
    /* the template caps might limit the number of channels (like alsasrc),
     * in which case we don't want to return a superset, so hack around this
     * for the two common cases where the channels are either a fixed number
     * or a min/max range). Example: alsasrc template has channels = [1,2] and 
     * the detection will claim to support 8 channels for device 'plughw:0' */
    field_type = gst_structure_get_field_type (s, "channels");
    if (field_type == G_TYPE_INT) {
      gst_structure_get_int (s, "channels", &c_min);
      gst_structure_get_int (s, "channels", &c_max);
    } else if (field_type == GST_TYPE_INT_RANGE) {
      const GValue *val;

      val = gst_structure_get_value (s, "channels");
      c_min = CLAMP (gst_value_get_int_range_min (val), min_chans, max_chans);
      c_max = CLAMP (gst_value_get_int_range_max (val), min_chans, max_chans);
    } else {
      c_min = min_chans;
      c_max = max_chans;
    }

    caps_add_channel_configuration (caps, s, c_min, c_max);
  }

  gst_caps_unref (in_caps);

  return caps;

min_chan_error:
  {
    GST_ERROR_OBJECT (obj, "failed to query minimum channel count: %s",
        snd_strerror (err));
    return NULL;
  }
max_chan_error:
  {
    GST_ERROR_OBJECT (obj, "failed to query maximum channel count: %s",
        snd_strerror (err));
    return NULL;
  }
}

/*
 * gst_alsa_probe_supported_formats:
 *
 * Takes the template caps and returns the subset which is actually
 * supported by this device.
 *
 */

GstCaps *
gst_alsa_probe_supported_formats (GstObject * obj, snd_pcm_t * handle,
    const GstCaps * template_caps)
{
  snd_pcm_hw_params_t *hw_params;
  GstCaps *caps;
  gint err;

  snd_pcm_hw_params_alloca (&hw_params);
  if ((err = snd_pcm_hw_params_any (handle, hw_params)) < 0)
    goto error;

  caps = gst_caps_copy (template_caps);

  if (!(caps = gst_alsa_detect_formats (obj, hw_params, caps)))
    goto subroutine_error;

  if (!(caps = gst_alsa_detect_rates (obj, hw_params, caps)))
    goto subroutine_error;

  if (!(caps = gst_alsa_detect_channels (obj, hw_params, caps)))
    goto subroutine_error;

  return caps;

error:
  {
    GST_ERROR_OBJECT (obj, "failed to query formats: %s", snd_strerror (err));
    return NULL;
  }

subroutine_error:
  {
    GST_ERROR_OBJECT (obj, "failed to query formats");
    return NULL;
  }
}
