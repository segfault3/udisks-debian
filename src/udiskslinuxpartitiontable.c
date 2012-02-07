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
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>

#include <glib/gstdio.h>

#include "udiskslogging.h"
#include "udiskslinuxpartitiontable.h"
#include "udiskslinuxblockobject.h"
#include "udisksdaemon.h"
#include "udiskscleanup.h"
#include "udisksdaemonutil.h"

/**
 * SECTION:udiskslinuxpartitiontable
 * @title: UDisksLinuxPartitionTable
 * @short_description: Linux implementation of #UDisksPartitionTable
 *
 * This type provides an implementation of the #UDisksPartitionTable
 * interface on Linux.
 */

typedef struct _UDisksLinuxPartitionTableClass   UDisksLinuxPartitionTableClass;

/**
 * UDisksLinuxPartitionTable:
 *
 * The #UDisksLinuxPartitionTable structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxPartitionTable
{
  UDisksPartitionTableSkeleton parent_instance;
};

struct _UDisksLinuxPartitionTableClass
{
  UDisksPartitionTableSkeletonClass parent_class;
};

static void partition_table_iface_init (UDisksPartitionTableIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxPartitionTable, udisks_linux_partition_table, UDISKS_TYPE_PARTITION_TABLE_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_PARTITION_TABLE, partition_table_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_partition_table_init (UDisksLinuxPartitionTable *partition_table)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (partition_table),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_partition_table_class_init (UDisksLinuxPartitionTableClass *klass)
{
}

/**
 * udisks_linux_partition_table_new:
 *
 * Creates a new #UDisksLinuxPartitionTable instance.
 *
 * Returns: A new #UDisksLinuxPartitionTable. Free with g_object_unref().
 */
UDisksPartitionTable *
udisks_linux_partition_table_new (void)
{
  return UDISKS_PARTITION_TABLE (g_object_new (UDISKS_TYPE_LINUX_PARTITION_TABLE,
                                               NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_partition_table_update:
 * @table: A #UDisksLinuxPartitionTable.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Updates the interface.
 */
void
udisks_linux_partition_table_update (UDisksLinuxPartitionTable  *table,
                                     UDisksLinuxBlockObject     *object)
{
  const gchar *type = NULL;
  GUdevDevice *device = NULL;;

  device = udisks_linux_block_object_get_device (object);
  if (device != NULL)
    type = g_udev_device_get_property (device, "ID_PART_TABLE_TYPE");

  udisks_partition_table_set_type_ (UDISKS_PARTITION_TABLE (table), type);

  g_clear_object (&device);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
ranges_overlap (guint64 a_offset, guint64 a_size,
                guint64 b_offset, guint64 b_size)
{
  guint64 a1 = a_offset, a2 = a_offset + a_size;
  guint64 b1 = b_offset, b2 = b_offset + b_size;
  gboolean ret = FALSE;

  /* There are only two cases when these intervals can overlap
   *
   * 1.  [a1-------a2]
   *               [b1------b2]
   *
   * 2.            [a1-------a2]
   *     [b1------b2]
   */

  if (a1 <= b1)
    {
      /* case 1 */
      if (a2 > b1)
        {
          ret = TRUE;
          goto out;
        }
    }
  else
    {
      /* case 2 */
      if (b2 > a1)
        {
          ret = TRUE;
          goto out;
        }
    }

 out:
  return ret;
}

static gboolean
have_partition_in_range (UDisksPartitionTable     *table,
                         guint64                   start,
                         guint64                   end,
                         gboolean                  ignore_container)
{
  gboolean ret = FALSE;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  GDBusObjectManager *object_manager = NULL;
  const gchar *table_object_path;
  GList *objects = NULL, *l;

  object = g_object_ref (UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (table))));
  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  object_manager = G_DBUS_OBJECT_MANAGER (udisks_daemon_get_object_manager (daemon));

  table_object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));

  objects = g_dbus_object_manager_get_objects (object_manager);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *i_object = UDISKS_OBJECT (l->data);
      UDisksPartition *i_partition = NULL;

      i_partition = udisks_object_get_partition (i_object);

      if (i_partition == NULL)
        goto cont;

      if (g_strcmp0 (udisks_partition_get_table (i_partition), table_object_path) != 0)
        goto cont;

      if (ignore_container && udisks_partition_get_is_container (i_partition))
        goto cont;

      if (!ranges_overlap (start, end - start,
                           udisks_partition_get_offset (i_partition), udisks_partition_get_size (i_partition)))
        goto cont;

      ret = TRUE;
      g_clear_object (&i_partition);
      goto out;

    cont:
      g_clear_object (&i_partition);
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  g_clear_object (&object);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  UDisksObject *partition_table_object;
  guint64       pos_to_wait_for;
  gboolean      ignore_container;
} WaitForPartitionData;

static gboolean
wait_for_partition (UDisksDaemon *daemon,
                    UDisksObject *object,
                    gpointer      user_data)
{
  WaitForPartitionData *data = user_data;
  gboolean ret = FALSE;
  UDisksPartition *partition = NULL;
  guint64 offset;
  guint64 size;

  partition = udisks_object_get_partition (object);
  if (partition == NULL)
    goto out;

  if (g_strcmp0 (udisks_partition_get_table (partition),
                 g_dbus_object_get_object_path (G_DBUS_OBJECT (data->partition_table_object))) != 0)
    goto out;

  offset = udisks_partition_get_offset (partition);
  size = udisks_partition_get_size (partition);

  if (data->pos_to_wait_for >= offset &&
      data->pos_to_wait_for < offset + size)
    {
      if (udisks_partition_get_is_container (partition) && data->ignore_container)
        goto out;
      ret = TRUE;
    }

 out:
  g_clear_object (&partition);
  return ret;
}

#define MIB_SIZE (1048576L)

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_create_partition (UDisksPartitionTable   *table,
                         GDBusMethodInvocation  *invocation,
                         guint64                 offset,
                         guint64                 size,
                         const gchar            *type,
                         const gchar            *name,
                         GVariant               *options)
{
  const gchar *action_id = NULL;
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  gchar *error_message = NULL;
  gchar *escaped_device = NULL;
  gchar *command_line = NULL;
  WaitForPartitionData *wait_data = NULL;
  UDisksObject *partition_object = NULL;
  UDisksBlock *partition_block = NULL;
  gchar *escaped_partition_device = NULL;
  const gchar *table_type;
  GError *error;

  object = g_object_ref (UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (table))));
  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  block = udisks_object_get_block (object);
  if (block == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Partition table object is not a block device");
      goto out;
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  if (udisks_block_get_hint_system (block))
    action_id = "org.freedesktop.udisks2.modify-device-system";
  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    N_("Authentication is required to create a partition on $(udisks2.device)"),
                                                    invocation))
    goto out;

  escaped_device = g_strescape (udisks_block_get_device (block), NULL);

  table_type = udisks_partition_table_get_type_ (table);
  wait_data = g_new0 (WaitForPartitionData, 1);
  if (g_strcmp0 (table_type, "dos") == 0)
    {
      guint64 start_mib;
      guint64 end_bytes;
      const gchar *part_type;
      char *endp;
      gint type_as_int;
      gboolean is_logical = FALSE;

      if (strlen (name) > 0)
        {
          g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                 "MBR partition table does not support names");
          goto out;
        }

      /* Determine whether we are creating a primary, extended or logical partition */
      type_as_int = strtol (type, &endp, 0);
      if (type[0] != '\0' && *endp == '\0' &&
          (type_as_int == 0x05 || type_as_int == 0x0f || type_as_int == 0x85))
        {
          part_type = "extended";
          if (have_partition_in_range (table, offset, offset + size, FALSE))
            {
              g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                     "Requested range is already occupied by a partition");
              goto out;
            }
        }
      else
        {
          if (have_partition_in_range (table, offset, offset + size, FALSE))
            {
              if (have_partition_in_range (table, offset, offset + size, TRUE))
                {
                  g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                         "Requested range is already occupied by a partition");
                  goto out;
                }
              else
                {
                  is_logical = TRUE;
                  part_type = "logical ext2";
                }
            }
          else
            {
              part_type = "primary ext2";
            }
        }

      /* Ensure we _start_ at MiB granularity since that ensures optimal IO...
       * Also round up size to nearest multiple of 512
       */
      start_mib = offset / MIB_SIZE + 1L;
      end_bytes = start_mib * MIB_SIZE + ((size + 511L) & (~511L));

      /* Now reduce size until we are not
       *
       *  - overlapping neighboring partitions; or
       *  - exceeding the end of the disk
       */
      while (end_bytes > start_mib * MIB_SIZE && (have_partition_in_range (table,
                                                                           start_mib * MIB_SIZE,
                                                                           end_bytes, is_logical) ||
                                                  end_bytes > udisks_block_get_size (block)))
        {
          /* TODO: if end_bytes is sufficiently big this could be *a lot* of loop iterations
           *       and thus a potential DoS attack...
           */
          end_bytes -= 512L;
        }
      wait_data->pos_to_wait_for = (start_mib*MIB_SIZE + end_bytes) / 2L;
      wait_data->ignore_container = is_logical;

      command_line = g_strdup_printf ("parted --align optimal --script \"%s\" "
                                      "\"mkpart %s %" G_GUINT64_FORMAT "MiB %" G_GUINT64_FORMAT "b\"",
                                      escaped_device,
                                      part_type,
                                      start_mib,
                                      end_bytes - 1); /* end_bytes is *INCLUSIVE* (!) */
    }
  else if (g_strcmp0 (table_type, "gpt") == 0)
    {
      guint64 start_mib;
      guint64 end_bytes;
      gchar *escaped_name;
      gchar *escaped_escaped_name;

      /* GPT is easy, no extended/logical crap */
      if (have_partition_in_range (table, offset, offset + size, FALSE))
        {
          g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                 "Requested range is already occupied by a partition");
          goto out;
        }

      /* bah, parted(8) is broken with empty names (it sets the name to 'ext2' in that case)
       * TODO: file bug
       */
      if (strlen (name) == 0)
        {
          name = " ";
        }

      escaped_name = g_strescape (name, NULL);
      escaped_escaped_name = g_strescape (escaped_name, NULL);

      /* Ensure we _start_ at MiB granularity since that ensures optimal IO...
       * Also round up size to nearest multiple of 512
       */
      start_mib = offset / MIB_SIZE + 1L;
      end_bytes = start_mib * MIB_SIZE + ((size + 511L) & (~511L));

      /* Now reduce size until we are not
       *
       *  - overlapping neighboring partitions; or
       *  - exceeding the end of the disk (note: the 33 LBAs is the Secondary GPT)
       */
      while (end_bytes > start_mib * MIB_SIZE && (have_partition_in_range (table,
                                                                           start_mib * MIB_SIZE,
                                                                           end_bytes, FALSE) ||
                                                  (end_bytes > udisks_block_get_size (block) - 33*512)))
        {
          /* TODO: if end_bytes is sufficiently big this could be *a lot* of loop iterations
           *       and thus a potential DoS attack...
           */
          end_bytes -= 512L;
        }
      wait_data->pos_to_wait_for = (start_mib*MIB_SIZE + end_bytes) / 2L;
      command_line = g_strdup_printf ("parted --align optimal --script \"%s\" "
                                      "\"mkpart \\\"%s\\\" ext2 %" G_GUINT64_FORMAT "MiB %" G_GUINT64_FORMAT "b\"",
                                      escaped_device,
                                      escaped_escaped_name,
                                      start_mib,
                                      end_bytes - 1); /* end_bytes is *INCLUSIVE* (!) */
      g_free (escaped_escaped_name);
      g_free (escaped_name);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Don't know how to create partitions this partition table of type `%s'",
                                             table_type);
      goto out;
    }

  if (!udisks_daemon_launch_spawned_job_sync (daemon,
                                              object,
                                              NULL, /* GCancellable */
                                              0,    /* uid_t run_as_uid */
                                              0,    /* uid_t run_as_euid */
                                              NULL, /* gint *out_status */
                                              &error_message,
                                              NULL,  /* input_string */
                                              "%s",
                                              command_line))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error creating partition on %s: %s",
                                             udisks_block_get_device (block),
                                             error_message);
      goto out;
    }
  /* this is sometimes needed because parted(8) does not generate the uevent itself */
  udisks_linux_block_object_trigger_uevent (UDISKS_LINUX_BLOCK_OBJECT (object));

  /* sit and wait for the partition to show up */
  g_warn_if_fail (wait_data->pos_to_wait_for > 0);
  wait_data->partition_table_object = object;
  error = NULL;
  partition_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_partition,
                                                         wait_data,
                                                         NULL,
                                                         30,
                                                         &error);
  if (partition_object == NULL)
    {
      g_prefix_error (&error, "Error waiting for partition to appear: ");
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }
  partition_block = udisks_object_get_block (partition_object);
  if (partition_block == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Partition object is not a block device");
      goto out;
    }
  escaped_partition_device = g_strescape (udisks_block_get_device (partition_block), NULL);

  /* TODO: set partition type */

  /* wipe the newly created partition */
  if (!udisks_daemon_launch_spawned_job_sync (daemon,
                                              partition_object,
                                              NULL, /* GCancellable */
                                              0,    /* uid_t run_as_uid */
                                              0,    /* uid_t run_as_euid */
                                              NULL, /* gint *out_status */
                                              &error_message,
                                              NULL,  /* input_string */
                                              "wipefs -a \"%s\"",
                                              escaped_partition_device))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error wiping newly created partition %s: %s",
                                             udisks_block_get_device (partition_block),
                                             error_message);
      goto out;
    }
  /* this is sometimes needed because parted(8) does not generate the uevent itself */
  udisks_linux_block_object_trigger_uevent (UDISKS_LINUX_BLOCK_OBJECT (partition_object));


  udisks_partition_table_complete_create_partition (table,
                                                    invocation,
                                                    g_dbus_object_get_object_path (G_DBUS_OBJECT (partition_object)));

 out:
  g_free (escaped_partition_device);
  g_free (wait_data);
  g_clear_object (&partition_block);
  g_clear_object (&partition_object);
  g_free (command_line);
  g_free (escaped_device);
  g_free (error_message);
  g_clear_object (&object);
  g_clear_object (&block);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
partition_table_iface_init (UDisksPartitionTableIface *iface)
{
  iface->handle_create_partition = handle_create_partition;
}
