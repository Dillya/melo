/*
 * melo_tags.c: Media tags handler
 *
 * Copyright (C) 2016 Alexandre Dilly <dillya@sparod.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <string.h>

#include <gst/tag/tag.h>

#include "melo_tags.h"

MeloTags *
melo_tags_new (void)
{
  MeloTags *tags;

  /* Allocate new tags */
  tags = g_slice_new0 (MeloTags);
  if (!tags)
    return NULL;

  /* Set reference counter to 1 */
  tags->ref_count = 1;

  return tags;
}

MeloTags *
melo_tags_ref (MeloTags *tags)
{
  tags->ref_count++;
  return tags;
}

MeloTags *
melo_tags_new_from_gst_tag_list (GstTagList *tlist, MeloTagsFields fields)
{
  MeloTags *tags;

  /* Create new tags */
  tags = melo_tags_new ();
  if (!tags)
    return NULL;

  /* No fields to read */
  if (fields == MELO_TAGS_FIELDS_NONE)
    return tags;

  /* Fill MeloTags from GstTagList */
  if (fields & MELO_TAGS_FIELDS_TITLE)
    gst_tag_list_get_string (tlist, GST_TAG_TITLE, &tags->title);
  if (fields & MELO_TAGS_FIELDS_ARTIST)
    gst_tag_list_get_string (tlist, GST_TAG_ARTIST, &tags->artist);
  if (fields & MELO_TAGS_FIELDS_ALBUM)
    gst_tag_list_get_string (tlist, GST_TAG_ALBUM, &tags->album);
  if (fields & MELO_TAGS_FIELDS_GENRE)
    gst_tag_list_get_string (tlist, GST_TAG_GENRE, &tags->genre);
  if (fields & MELO_TAGS_FIELDS_TRACK)
    gst_tag_list_get_uint (tlist, GST_TAG_TRACK_NUMBER, &tags->track);
  if (fields & MELO_TAGS_FIELDS_TRACKS)
    gst_tag_list_get_uint (tlist, GST_TAG_TRACK_COUNT, &tags->tracks);

  /* Get date */
  if (fields & MELO_TAGS_FIELDS_DATE) {
    GDate *date;

    /* Get only year from GDate */
    if (gst_tag_list_get_date (tlist, GST_TAG_DATE, &date)) {
      tags->date = g_date_get_year (date);
      g_date_free (date);
    }
  }

  /* Get album / single cover */
  if (fields & MELO_TAGS_FIELDS_COVER) {
    GstBuffer *buffer = NULL;
    gint count, i;

    /* Find the best image (front cover if possible) */
    count = gst_tag_list_get_tag_size(tlist, GST_TAG_IMAGE);
    for (i = 0; i < count; i++) {
      GstTagImageType type = GST_TAG_IMAGE_TYPE_NONE;
      const GstStructure *info;
      GstSample *sample;

      /* Get next image */
      if (!gst_tag_list_get_sample_index (tlist, GST_TAG_IMAGE, i, &sample))
        continue;

      /* Get infos about image */
      info = gst_sample_get_info (sample);
      if (!info) {
        gst_sample_unref (sample);
        continue;
      }

      /* Get image type */
      gst_structure_get_enum (info, "image-type", GST_TYPE_TAG_IMAGE_TYPE,
                              &type);
      /* Select only front cover or first undefined image */
      if (type == GST_TAG_IMAGE_TYPE_FRONT_COVER ||
          (type == GST_TAG_IMAGE_TYPE_UNDEFINED && buffer == NULL)) {
        if (buffer)
          gst_buffer_unref (buffer);
        buffer = gst_buffer_ref (gst_sample_get_buffer (sample));
      }
      gst_sample_unref (sample);
    }

    /* Get preview image if no image found */
    if (!buffer) {
      GstSample *sample;

      /* Get preview */
      if (gst_tag_list_get_sample (tlist, GST_TAG_PREVIEW_IMAGE, &sample)) {
        buffer = gst_buffer_ref (gst_sample_get_buffer (sample));
        gst_sample_unref (sample);
      }
    }

    /* Copy found image */
    if (buffer) {
        gpointer data;
        gsize size, dsize;

        /* Extract data from buffer */
        size = gst_buffer_get_size (buffer);
        gst_buffer_extract_dup (buffer, 0, size, &data, &dsize);

        /* Create a new GBytes with data */
        tags->cover = g_bytes_new_take (data, dsize);
        gst_buffer_unref (buffer);
    }
  }

  return tags;
}

MeloTagsFields
melo_tags_get_fields_from_json_array (JsonArray *array)
{
  MeloTagsFields fields = MELO_TAGS_FIELDS_NONE;
  const gchar *field;
  guint count, i;

  /* Parse array */
  count = json_array_get_length (array);
  for (i = 0; i < count; i++) {
    field = json_array_get_string_element (array, i);
    if (!field)
      break;
    if (!g_strcmp0 (field, "none")) {
      fields = MELO_TAGS_FIELDS_NONE;
      break;
    } else if (!g_strcmp0 (field, "full")) {
      fields = MELO_TAGS_FIELDS_FULL;
      break;
    } else if (!g_strcmp0 (field, "title"))
      fields |= MELO_TAGS_FIELDS_TITLE;
    else if (!g_strcmp0 (field, "artist"))
      fields |= MELO_TAGS_FIELDS_ARTIST;
    else if (!g_strcmp0 (field, "album"))
      fields |= MELO_TAGS_FIELDS_ALBUM;
    else if (!g_strcmp0 (field, "genre"))
      fields |= MELO_TAGS_FIELDS_GENRE;
    else if (!g_strcmp0 (field, "date"))
      fields |= MELO_TAGS_FIELDS_DATE;
    else if (!g_strcmp0 (field, "track"))
      fields |= MELO_TAGS_FIELDS_TRACK;
    else if (!g_strcmp0 (field, "tracks"))
      fields |= MELO_TAGS_FIELDS_TRACKS;
    else if (!g_strcmp0 (field, "cover"))
      fields |= MELO_TAGS_FIELDS_COVER;
  }

  return fields;
}

void
melo_tags_add_to_json_object (MeloTags *tags, JsonObject *obj,
                              MeloTagsFields fields)
{
  /* Nothing to do */
  if (!tags || fields == MELO_TAGS_FIELDS_NONE)
    return;

  /* Fill object */
  if (fields & MELO_TAGS_FIELDS_TITLE)
    json_object_set_string_member (obj, "title", tags->title);
  if (fields & MELO_TAGS_FIELDS_ARTIST)
    json_object_set_string_member (obj, "artist", tags->artist);
  if (fields & MELO_TAGS_FIELDS_ALBUM)
    json_object_set_string_member (obj, "album", tags->album);
  if (fields & MELO_TAGS_FIELDS_GENRE)
    json_object_set_string_member (obj, "genre", tags->genre);
  if (fields & MELO_TAGS_FIELDS_DATE)
    json_object_set_int_member (obj, "date", tags->date);
  if (fields & MELO_TAGS_FIELDS_TRACK)
    json_object_set_int_member (obj, "track", tags->track);
  if (fields & MELO_TAGS_FIELDS_TRACKS)
    json_object_set_int_member (obj, "tracks", tags->tracks);

  /* Convert image to base64 */
  if (fields & MELO_TAGS_FIELDS_COVER && tags->cover) {
    const guchar *data;
    gsize size;
    gchar *cover;

    /* Get data and encode */
    data = g_bytes_get_data (tags->cover, &size);
    cover = g_base64_encode (data, size);

    /* Add to object */
    json_object_set_string_member (obj, "cover", cover);
    g_free (cover);
  }
}

JsonObject *
melo_tags_to_json_object (MeloTags *tags, MeloTagsFields fields)
{
  JsonObject *obj;

  /* Create a new JSON object */
  obj = json_object_new ();
  if (!obj)
    return NULL;

  /* Fill object */
  melo_tags_add_to_json_object (tags, obj, fields);

  return obj;
}

void
melo_tags_unref (MeloTags *tags)
{
  tags->ref_count--;
  if (tags->ref_count)
    return;

  /* Free tags */
  g_free (tags->title);
  g_free (tags->artist);
  g_free (tags->album);
  g_free (tags->genre);
  g_bytes_unref (tags->cover);
  g_slice_free (MeloTags, tags);
}
