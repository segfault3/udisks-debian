/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include <glib/gi18n.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include "udiskslogging.h"
#include "udisksdaemontypes.h"
#include "udisksdaemon.h"

/* ---------------------------------------------------------------------------------------------------- */

static GMainLoop *loop = NULL;
static gboolean opt_replace = FALSE;
static gboolean opt_no_debug = FALSE;
static gboolean opt_no_sigint = FALSE;
static GOptionEntry opt_entries[] =
{
  {"replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace existing daemon", NULL},
  {"no-debug", 'n', 0, G_OPTION_ARG_NONE, &opt_no_debug, "Don't print debug information on stdout/stderr", NULL},
  {"no-sigint", 's', 0, G_OPTION_ARG_NONE, &opt_no_sigint, "Do not handle SIGINT for controlled shutdown", NULL},
  {NULL }
};

static UDisksDaemon *the_daemon = NULL;

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  the_daemon = udisks_daemon_new (connection);
  udisks_debug ("Connected to the system bus");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  if (the_daemon == NULL)
    {
      udisks_error ("Failed to connect to the system message bus");
    }
  else
    {
      udisks_info ("Lost (or failed to acquire) the name %s on the system message bus", name);
    }
  g_main_loop_quit (loop);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  udisks_notice ("Acquired the name %s on the system message bus", name);
}

static gboolean
on_sigint (gpointer user_data)
{
  udisks_info ("Caught SIGINT. Initiating shutdown");
  g_main_loop_quit (loop);
  return FALSE;
}

int
main (int    argc,
      char **argv)
{
  GError *error;
  GOptionContext *opt_context;
  gint ret;
  guint name_owner_id;
  guint sigint_id;

  ret = 1;
  loop = NULL;
  opt_context = NULL;
  name_owner_id = 0;
  sigint_id = 0;

  g_type_init ();

  /* avoid gvfs (http://bugzilla.gnome.org/show_bug.cgi?id=526454) */
  if (!g_setenv ("GIO_USE_VFS", "local", TRUE))
    {
      g_printerr ("Error setting GIO_USE_GVFS\n");
      goto out;
    }

  opt_context = g_option_context_new ("udisks storage daemon");
  g_option_context_add_main_entries (opt_context, opt_entries, NULL);
  error = NULL;
  if (!g_option_context_parse (opt_context, &argc, &argv, &error))
    {
      g_printerr ("Error parsing options: %s\n", error->message);
      g_error_free (error);
      goto out;
    }

  /* TODO: this hammer is too big - it would be a lot better to configure the
   *       logging routines and avoid printf(3) overhead and so on
   */
  if (opt_no_debug)
    {
      gint dev_null_fd;
      dev_null_fd = open ("/dev/null", O_RDWR);
      if (dev_null_fd >= 0)
        {
          dup2 (dev_null_fd, STDIN_FILENO);
          dup2 (dev_null_fd, STDOUT_FILENO);
          dup2 (dev_null_fd, STDERR_FILENO);
          close (dev_null_fd);
        }
      else
        {
          g_warning ("Error opening /dev/null: %m");
        }
    }

  if (g_getenv ("PATH") == NULL)
    g_setenv ("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", TRUE);

  udisks_notice ("udisks daemon version %s starting", PACKAGE_VERSION);

  loop = g_main_loop_new (NULL, FALSE);

  sigint_id = 0;
  if (!opt_no_sigint)
    {
      sigint_id = g_unix_signal_add_full (G_PRIORITY_DEFAULT,
                                          SIGINT,
                                          on_sigint,
                                          NULL,  /* user_data */
                                          NULL); /* GDestroyNotify */
    }

  name_owner_id = g_bus_own_name (G_BUS_TYPE_SYSTEM,
                                  "org.freedesktop.UDisks2",
                                  G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                                    (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);


  udisks_debug ("Entering main event loop");

  g_main_loop_run (loop);

  ret = 0;

 out:
  if (sigint_id > 0)
    g_source_remove (sigint_id);
  if (the_daemon != NULL)
    g_object_unref (the_daemon);
  if (name_owner_id != 0)
    g_bus_unown_name (name_owner_id);
  if (loop != NULL)
    g_main_loop_unref (loop);
  if (opt_context != NULL)
    g_option_context_free (opt_context);

  udisks_notice ("udisks daemon version %s exiting", PACKAGE_VERSION);

  return ret;
}
