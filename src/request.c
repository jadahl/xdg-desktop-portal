/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "request.h"
#include "xdp-utils.h"

#include <string.h>

static void request_skeleton_iface_init (XdpRequestIface *iface);

G_DEFINE_TYPE_WITH_CODE (Request, request, XDP_TYPE_REQUEST_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_REQUEST, request_skeleton_iface_init))

static void
request_on_signal_response (XdpRequest *object,
                            guint arg_response,
                            GVariant *arg_results)
{
  Request *request = (Request *)object;
  XdpRequestSkeleton *skeleton = XDP_REQUEST_SKELETON (object);
  GList      *connections, *l;
  GVariant   *signal_variant;

  connections = g_dbus_interface_skeleton_get_connections (G_DBUS_INTERFACE_SKELETON (skeleton));

  signal_variant = g_variant_ref_sink (g_variant_new ("(u@a{sv})",
                                                      arg_response,
                                                      arg_results));
  for (l = connections; l != NULL; l = l->next)
    {
      GDBusConnection *connection = l->data;
      g_dbus_connection_emit_signal (connection,
                                     request->sender,
                                     g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (skeleton)),
                                     "org.freedesktop.portal.Request",
                                     "Response",
                                     signal_variant,
                                     NULL);
    }
  g_variant_unref (signal_variant);
  g_list_free_full (connections, g_object_unref);
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation)
{
  Request *request = (Request *)object;
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (request->exported)
    {
      if (request->impl_request &&
          !xdp_impl_request_call_close_sync (request->impl_request, NULL, &error))
        {
          if (invocation)
            g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }

      request_unexport (request);
    }

  if (invocation)
    xdp_request_complete_close (XDP_REQUEST (request), invocation);

  return TRUE;
}

static void
request_skeleton_iface_init (XdpRequestIface *iface)
{
  iface->handle_close = handle_close;
  iface->response = request_on_signal_response;
}

G_LOCK_DEFINE (requests);
static GHashTable *requests;

static void
request_init (Request *request)
{
  g_mutex_init (&request->mutex);
}

static void
request_finalize (GObject *object)
{
  Request *request = (Request *)object;

  G_LOCK (requests);
  g_hash_table_remove (requests, request->id);
  G_UNLOCK (requests);

  g_clear_object (&request->impl_request);

  g_free (request->app_id);
  g_free (request->sender);
  g_free (request->id);
  g_mutex_clear (&request->mutex);
  if (request->app_info)
    g_key_file_unref (request->app_info);

  G_OBJECT_CLASS (request_parent_class)->finalize (object);
}

static void
request_class_init (RequestClass *klass)
{
  GObjectClass *gobject_class;

  requests = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    NULL, NULL);

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize  = request_finalize;
}

static gboolean
request_authorize_callback (GDBusInterfaceSkeleton *interface,
                            GDBusMethodInvocation  *invocation,
                            gpointer                user_data)
{
  const gchar *request_sender = user_data;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);

  if (strcmp (sender, request_sender) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Portal operation not allowed: Unmatched caller");
      return FALSE;
    }

  return TRUE;
}

void
request_init_invocation (GDBusMethodInvocation  *invocation, const char *app_id)
{
  Request *request;
  guint32 r;
  char *id = NULL;

  request = g_object_new (request_get_type (), NULL);
  request->app_id = g_strdup (app_id);
  request->sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  request->app_info = xdp_invocation_lookup_cached_app_info (invocation);

  G_LOCK (requests);

  do
    {
      r = g_random_int ();
      g_free (id);
      id = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%u", r);
    }
  while (g_hash_table_lookup (requests, id) != NULL);

  request->id = id;
  g_hash_table_insert (requests, id, request);

  G_UNLOCK (requests);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (request),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_signal_connect (request, "g-authorize-method",
                    G_CALLBACK (request_authorize_callback),
                    request->sender);


  g_object_set_data_full (G_OBJECT (invocation), "request", request, g_object_unref);
}

Request *
request_from_invocation (GDBusMethodInvocation *invocation)
{
  return g_object_get_data (G_OBJECT (invocation), "request");
}

void
request_export (Request *request,
                GDBusConnection *connection)
{
  g_autoptr(GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (request),
                                         connection,
                                         request->id,
                                         &error))
    {
      g_warning ("Error exporting request: %s", error->message);
      g_clear_error (&error);
    }

  g_object_ref (request);
  request->exported = TRUE;
}

void
request_unexport (Request *request)
{
  request->exported = FALSE;
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (request));
  g_object_unref (request);
}

void
request_set_impl_request (Request *request,
                          XdpImplRequest *impl_request)
{
  g_set_object (&request->impl_request, impl_request);
}

void
close_requests_in_thread_func (GTask        *task,
                               gpointer      source_object,
                               gpointer      task_data,
                               GCancellable *cancellable)
{
  const char *sender = (const char *)task_data;
  GSList *list = NULL;
  GSList *l;
  GHashTableIter iter;
  Request *request;

  G_LOCK (requests);
  if (requests)
    {
      g_hash_table_iter_init (&iter, requests);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&request))
        {
          if (strcmp (sender, request->sender) == 0)
            list = g_slist_prepend (list, g_object_ref (request));
        }
    }
  G_UNLOCK (requests);

  for (l = list; l; l = l->next)
    {
      Request *request = l->data;

      REQUEST_AUTOLOCK (request);

      if (request->exported)
        {
          if (request->impl_request)
            xdp_impl_request_call_close_sync (request->impl_request, NULL, NULL);

          request_unexport (request);
        }
    }

  g_slist_free_full (list, g_object_unref);
}

void
close_requests_for_sender (const char *sender)
{
  GTask *task;

  task = g_task_new (NULL, NULL, NULL, NULL);
  g_task_set_task_data (task, g_strdup (sender), g_free);
  g_task_run_in_thread (task, close_requests_in_thread_func);
  g_object_unref (task);
}
