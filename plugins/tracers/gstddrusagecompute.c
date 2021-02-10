/*
 * Copyright 2021 NXP
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/gst.h>
#include <glib/gstdio.h>
#include "gstddrusagecompute.h"

#include <stdint.h>
#include <unistd.h>
#include <string.h>

#define SOC_NAME_MAX_LENGTH 8
static gchar soc_name[SOC_NAME_MAX_LENGTH];

#define MEAS_INTERVAL 1.0

enum soc_id
{
  IMX8MP,
};

struct perf_ddr_type
{
  const char *name;
  const char *rd_metric_name;
  float rd_metric_value;
  const char *wr_metric_name;
  float wr_metric_value;
};

static struct perf_ddr_type perf_ddr_imx8mp[] = {
  {"all", "imx8mp-ddr0-all-r", 0.0f, "imx8mp-ddr0-all-w", 0.0f},
  {"npu", "imx8mp-ddr0-npu-r", 0.0f, "imx8mp-ddr0-npu-w", 0.0f},
  {"gpu3d", "imx8mp-ddr0-3d-r", 0.0f, "imx8mp-ddr0-3d-w", 0.0f},
  {"gpu2d", "imx8mp-ddr0-2d-r", 0.0f, "imx8mp-ddr0-2d-w", 0.0f},
  {"a53", "imx8mp-ddr0-a53-r", 0.0f, "imx8mp-ddr0-a53-w", 0.0f},
  {"isi1", "imx8mp-ddr0-isi1-r", 0.0f, "imx8mp-ddr0-isi1-w", 0.0f},
  {0}
};

static struct perf_ddr_type *perf_ddr_socs[] = {
  [IMX8MP] = perf_ddr_imx8mp,
};

static struct perf_ddr_type *perf_ddr_soc = NULL;

static GPid perfPID = 0;

static void
gtop_set_perf_ddr_soc (void)
{
  FILE *file;
  char buf[1024];
  if (strlen (soc_name))
    return;

  file = fopen ("/sys/devices/soc0/soc_id", "r");
  if (file == NULL)
    return;
  if ((fgets (buf, 1024, file)) != NULL) {
    if (!strncmp (buf, "i.MX8MP", SOC_NAME_MAX_LENGTH - 1)) {
      perf_ddr_soc = perf_ddr_socs[IMX8MP];
    }
    strncpy (soc_name, buf, SOC_NAME_MAX_LENGTH);
  }

}

static gboolean
cb_err_watch (GIOChannel * channel, GIOCondition cond, GstDDRUsage * usage)
{
  gchar *string;
  gsize size;
  const gchar *delim = ";";
  gchar **items = NULL;
  struct perf_ddr_type *evt;
  gint cnt, idx;
  gfloat fvalue;

  if (cond == G_IO_HUP) {
    g_io_channel_unref (channel);
    return (FALSE);
  }

  g_io_channel_read_line (channel, &string, &size, NULL, NULL);
  g_strstrip (string);
  items = g_strsplit (string, delim, -1);
  if (g_strv_length (items) != 7)
    goto out;
  cnt = usage->meas_count++ % usage->meas_num;
  idx = cnt / 2;
  evt = &perf_ddr_soc[idx];
  fvalue = (float) atof (items[5]);
  fvalue /= (1024 * 1024);
  fvalue /= MEAS_INTERVAL;

  if (cnt % 2) {
    /* Write Value */
    evt->wr_metric_value = fvalue;
  } else {
    /* Read Value */
    evt->rd_metric_value = fvalue;
  }

out:
  g_free (string);
  g_strfreev (items);

  return (TRUE);
}

static GPid
gst_perf_spawn (GstDDRUsage * usage, gchar * script_path)
{
  GPid pid;
  gint err;
  GIOChannel *err_ch;
  gboolean ret;
  GError *error = NULL;

  const gchar *argv[] = {
    "/usr/bin/env",
    "bash",
    script_path,
    NULL
  };

  ret = g_spawn_async_with_pipes (NULL, (gchar **) argv, NULL,
      G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, &pid, NULL, NULL, &err, &error);

  if (!ret) {
    g_error ("SPAWN FAILED: %s", error->message);
    return -1;
  }

  /* Create channels that will be used to read data from pipes. */
  err_ch = g_io_channel_unix_new (err);

  /* Add watches to channels */
  g_io_add_watch (err_ch, G_IO_IN | G_IO_HUP, (GIOFunc) cb_err_watch, usage);

  return pid;
}

gint
gst_ddr_usage_get_nmeas (void)
{
  struct perf_ddr_type *evt;
  gint i = 0;
  gtop_set_perf_ddr_soc ();

  evt = &perf_ddr_soc[0];

  for (i = 0; (evt->name != NULL); i += 2) {
    evt++;
  }

  return i;
}

#define PERF_CMDLINE \
    "\t/usr/bin/perf stat -a -x \";\" -M %s,%s sleep %f &\n\twait $!\n"

void
gst_ddr_usage_init (GstDDRUsage * usage)
{
  struct perf_ddr_type *evt;
  const gchar *metric_pnames[DDR_MEAS_MAX + 1];
  gint i = 0;
  GString *script_str;
  const gchar *tmpdir;
  gchar *tmpscript;

  g_return_if_fail (usage);

  if (perfPID > 0)
    return;                     /* TODO: Reset perf ? */

  memset (usage, 0, sizeof (GstDDRUsage));

  gtop_set_perf_ddr_soc ();

  evt = &perf_ddr_soc[0];
  g_return_if_fail (evt);

  memset (metric_pnames, 0, sizeof (metric_pnames));

  script_str = g_string_sized_new (1024);

  g_string_append (script_str, "#! /usr/bin/env bash\n\nwhile true;\ndo\n");

  for (i = 0; (evt->name != NULL); i += 2) {
    char *meas_name;
    g_string_append_printf (script_str, PERF_CMDLINE, evt->rd_metric_name,
        evt->wr_metric_name, MEAS_INTERVAL);
    /* Read */
    meas_name = usage->event_name[i];
    usage->names[i] = meas_name;
    snprintf (meas_name, DDR_STRING_MAX, "%s-rd", evt->name);
    metric_pnames[i] = evt->rd_metric_name;

    /* Write */
    meas_name = usage->event_name[i + 1];
    usage->names[i + 1] = meas_name;
    snprintf (meas_name, DDR_STRING_MAX, "%s-wr", evt->name);
    metric_pnames[i + 1] = evt->wr_metric_name;


    usage->meas_num += 2;
    evt++;
  }

  g_string_append (script_str, "done\n");

  tmpdir = g_get_tmp_dir ();
  tmpscript = g_build_path ("/", tmpdir, "nnshark-perf-ddr.sh", (gchar *) NULL);

  g_file_set_contents (tmpscript, script_str->str, -1, NULL);

  perfPID = gst_perf_spawn (usage, tmpscript);

  g_free (tmpscript);
  g_string_free (script_str, TRUE);
}

void
gst_ddr_usage_finalize (void)
{
}

void
gst_ddr_usage_compute (GstDDRUsage * usage)
{
  gint i;
  struct perf_ddr_type *evt;

  g_return_if_fail (usage);

  gtop_set_perf_ddr_soc ();
  evt = &perf_ddr_soc[0];

  for (i = 0; (evt->name != NULL); i += 2) {
    /* Read */
    usage->load[i] = evt->rd_metric_value;

    /* Write */
    usage->load[i + 1] = evt->wr_metric_value;

    evt++;
  }
}