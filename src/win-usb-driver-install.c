/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2011 Red Hat, Inc.

   Red Hat Authors:
   Uri Lublin <uril@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Some notes:
 * Each installer (instance) opens a named-pipe to talk with win-usb-clerk.
 * Each installer (instance) requests driver installation for a single device.
 */

#include "config.h"

#include <windows.h>
#include <gio/gio.h>
#include <gio/gwin32inputstream.h>
#include <gio/gwin32outputstream.h>
#include "spice-util.h"
#include "win-usb-clerk.h"
#include "win-usb-driver-install.h"
#include "usb-device-manager-priv.h"

/* ------------------------------------------------------------------ */
/* gobject glue                                                       */

#define SPICE_WIN_USB_DRIVER_GET_PRIVATE(obj)     \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), SPICE_TYPE_WIN_USB_DRIVER, SpiceWinUsbDriverPrivate))

struct _SpiceWinUsbDriverPrivate {
    USBClerkReply         reply;
    GTask                 *task;
    HANDLE                handle;
    SpiceUsbDevice        *device;
};


static void spice_win_usb_driver_initable_iface_init(GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE(SpiceWinUsbDriver, spice_win_usb_driver, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, spice_win_usb_driver_initable_iface_init));

static void spice_win_usb_driver_init(SpiceWinUsbDriver *self)
{
    self->priv = SPICE_WIN_USB_DRIVER_GET_PRIVATE(self);
}

static gboolean spice_win_usb_driver_initable_init(GInitable     *initable,
                                                   GCancellable  *cancellable,
                                                   GError        **err)
{
    SpiceWinUsbDriver *self = SPICE_WIN_USB_DRIVER(initable);
    SpiceWinUsbDriverPrivate *priv = self->priv;

    SPICE_DEBUG("win-usb-driver-install: connecting to usbclerk named pipe");
    priv->handle = CreateFile(USB_CLERK_PIPE_NAME,
                              GENERIC_READ | GENERIC_WRITE,
                              0, NULL,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                              NULL);
    if (priv->handle == INVALID_HANDLE_VALUE) {
        DWORD errval  = GetLastError();
        gchar *errstr = g_win32_error_message(errval);
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_USB_SERVICE,
                    "Failed to create service named pipe (%ld) %s", errval, errstr);
        g_free(errstr);
        return FALSE;
    }

    return TRUE;
}

static void spice_win_usb_driver_finalize(GObject *gobject)
{
    SpiceWinUsbDriver *self = SPICE_WIN_USB_DRIVER(gobject);
    SpiceWinUsbDriverPrivate *priv = self->priv;

    if (priv->handle)
        CloseHandle(priv->handle);

    g_clear_object(&priv->task);

    if (G_OBJECT_CLASS(spice_win_usb_driver_parent_class)->finalize)
        G_OBJECT_CLASS(spice_win_usb_driver_parent_class)->finalize(gobject);
}

static void spice_win_usb_driver_class_init(SpiceWinUsbDriverClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    gobject_class->finalize     = spice_win_usb_driver_finalize;

    g_type_class_add_private(klass, sizeof(SpiceWinUsbDriverPrivate));
}

static void spice_win_usb_driver_initable_iface_init(GInitableIface *iface)
{
    iface->init = spice_win_usb_driver_initable_init;
}

/* ------------------------------------------------------------------ */
/* callbacks                                                          */

void win_usb_driver_handle_reply_cb(GObject *gobject,
                                    GAsyncResult *read_res,
                                    gpointer user_data)
{
    SpiceWinUsbDriver *self;
    SpiceWinUsbDriverPrivate *priv;

    GInputStream *istream;
    GError *err = NULL;
    gssize bytes;

    g_return_if_fail(SPICE_IS_WIN_USB_DRIVER(user_data));
    self = SPICE_WIN_USB_DRIVER(user_data);
    priv = self->priv;
    istream = G_INPUT_STREAM(gobject);

    bytes = g_input_stream_read_finish(istream, read_res, &err);

    SPICE_DEBUG("Finished reading reply-msg from usbclerk: bytes=%ld "
                "err_exist?=%d", (long)bytes, err!=NULL);

    g_warn_if_fail(g_input_stream_close(istream, NULL, NULL));
    g_clear_object(&istream);

    if (err) {
        g_warning("failed to read reply from usbclerk (%s)", err->message);
        g_task_return_error(priv->task, err);
        goto failed_reply;
    }

    if (bytes == 0) {
        g_warning("unexpected EOF from usbclerk");
        g_task_return_new_error(priv->task,
                                SPICE_WIN_USB_DRIVER_ERROR,
                                SPICE_WIN_USB_DRIVER_ERROR_FAILED,
                                "unexpected EOF from usbclerk");
        goto failed_reply;
    }

    if (bytes != sizeof(priv->reply)) {
        g_warning("usbclerk size mismatch: read %"G_GSSIZE_FORMAT" bytes,expected "
                  "%"G_GSSIZE_FORMAT" (header %"G_GSSIZE_FORMAT", size in header %d)",
                  bytes, sizeof(priv->reply), sizeof(priv->reply.hdr), priv->reply.hdr.size);
        /* For now just warn, do not fail */
    }

    if (priv->reply.hdr.magic != USB_CLERK_MAGIC) {
        g_warning("usbclerk magic mismatch: mine=0x%04x  server=0x%04x",
                  USB_CLERK_MAGIC, priv->reply.hdr.magic);
        g_task_return_new_error(priv->task,
                                SPICE_WIN_USB_DRIVER_ERROR,
                                SPICE_WIN_USB_DRIVER_ERROR_MESSAGE,
                                "usbclerk magic mismatch");
        goto failed_reply;
    }

    if (priv->reply.hdr.version != USB_CLERK_VERSION) {
        g_warning("usbclerk version mismatch: mine=0x%04x  server=0x%04x",
                  USB_CLERK_VERSION, priv->reply.hdr.version);
        g_task_return_new_error(priv->task,
                                SPICE_WIN_USB_DRIVER_ERROR,
                                SPICE_WIN_USB_DRIVER_ERROR_MESSAGE,
                                "usbclerk version mismatch");
    }

    if (priv->reply.hdr.type != USB_CLERK_REPLY) {
        g_warning("usbclerk message with unexpected type %d",
                  priv->reply.hdr.type);
        g_task_return_new_error(priv->task,
                                SPICE_WIN_USB_DRIVER_ERROR,
                                SPICE_WIN_USB_DRIVER_ERROR_MESSAGE,
                                "usbclerk message with unexpected type");
        goto failed_reply;
    }

    if (priv->reply.hdr.size != bytes) {
        g_warning("usbclerk message size mismatch: read %"G_GSSIZE_FORMAT" bytes  hdr.size=%d",
                  bytes, priv->reply.hdr.size);
        g_task_return_new_error(priv->task,
                                SPICE_WIN_USB_DRIVER_ERROR,
                                SPICE_WIN_USB_DRIVER_ERROR_MESSAGE,
                                "usbclerk message with unexpected size");
        goto failed_reply;
    }

    if (priv->reply.status == 0) {
        g_task_return_new_error(priv->task,
                                SPICE_WIN_USB_DRIVER_ERROR,
                                SPICE_WIN_USB_DRIVER_ERROR_MESSAGE,
                                "usbclerk error reply");
        goto failed_reply;
    }

    g_task_return_boolean (priv->task, TRUE);

 failed_reply:
    g_clear_object(&priv->task);
}

/* ------------------------------------------------------------------ */
/* helper functions                                                   */

static
gboolean spice_win_usb_driver_send_request(SpiceWinUsbDriver *self, guint16 op,
                                           guint16 vid, guint16 pid, GError **err)
{
    USBClerkDriverOp req;
    GOutputStream *ostream;
    SpiceWinUsbDriverPrivate *priv;
    gsize bytes;
    gboolean ret;

    SPICE_DEBUG("sending a request to usbclerk service (op=%d vid=0x%04x pid=0x%04x",
                op, vid, pid);

    g_return_val_if_fail(SPICE_IS_WIN_USB_DRIVER(self), FALSE);
    priv = self->priv;

    memset(&req, 0, sizeof(req));
    req.hdr.magic   = USB_CLERK_MAGIC;
    req.hdr.version = USB_CLERK_VERSION;
    req.hdr.type    = op;
    req.hdr.size    = sizeof(req);
    req.vid = vid;
    req.pid = pid;

    ostream = g_win32_output_stream_new(priv->handle, FALSE);

    ret = g_output_stream_write_all(ostream, &req, sizeof(req), &bytes, NULL, err);
    g_warn_if_fail(g_output_stream_close(ostream, NULL, NULL));
    g_object_unref(ostream);
    SPICE_DEBUG("write_all request returned %d written bytes %"G_GSIZE_FORMAT
                " expecting %"G_GSIZE_FORMAT,
                ret, bytes, sizeof(req));
    return ret;
}

static
void spice_win_usb_driver_read_reply_async(SpiceWinUsbDriver *self)
{
    SpiceWinUsbDriverPrivate *priv;
    GInputStream  *istream;

    g_return_if_fail(SPICE_IS_WIN_USB_DRIVER(self));
    priv = self->priv;

    SPICE_DEBUG("waiting for a reply from usbclerk");

    istream = g_win32_input_stream_new(priv->handle, FALSE);

    g_input_stream_read_async(istream, &priv->reply, sizeof(priv->reply),
                              G_PRIORITY_DEFAULT,
                              g_task_get_cancellable(priv->task),
                              win_usb_driver_handle_reply_cb, self);
}


/* ------------------------------------------------------------------ */
/* private api                                                        */


G_GNUC_INTERNAL
SpiceWinUsbDriver *spice_win_usb_driver_new(GError **err)
{
    GObject *self;

    g_return_val_if_fail(err == NULL || *err == NULL, FALSE);

    self = g_initable_new(SPICE_TYPE_WIN_USB_DRIVER, NULL, err, NULL);

    return SPICE_WIN_USB_DRIVER(self);
}

static
void spice_win_usb_driver_op(SpiceWinUsbDriver *self,
                             SpiceUsbDevice *device,
                             guint16 op_type,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
    guint16 vid, pid;
    GError *err = NULL;
    GTask *task;
    SpiceWinUsbDriverPrivate *priv;

    g_return_if_fail(SPICE_IS_WIN_USB_DRIVER(self));
    g_return_if_fail(device != NULL);

    priv = self->priv;

    task = g_task_new(self, cancellable, callback, user_data);

    if (priv->task) { /* allow one install/uninstall request at a time */
        g_warning("Another request exists -- try later");
        g_task_return_new_error(task,
                  SPICE_WIN_USB_DRIVER_ERROR, SPICE_WIN_USB_DRIVER_ERROR_FAILED,
                  "Another request exists -- try later");
        goto failed_request;
    }


    vid = spice_usb_device_get_vid(device);
    pid = spice_usb_device_get_pid(device);

    if (!spice_win_usb_driver_send_request(self, op_type,
                                           vid, pid, &err)) {
        g_warning("failed to send a request to usbclerk %s", err->message);
        g_task_return_error(task, err);
        goto failed_request;
    }

    /* set up for async read */
    priv->task = task;
    priv->device = device;

    spice_win_usb_driver_read_reply_async(self);

    return;

 failed_request:
    g_clear_object(&task);
}

/**
 * Returns: currently returns 0 (failure) and 1 (success)
 * possibly later we'll add error-codes
 */
static gboolean
spice_win_usb_driver_op_finish(SpiceWinUsbDriver *self,
                               GAsyncResult *res, GError **err)
{
    GTask *task = G_TASK(res);

    g_return_val_if_fail(SPICE_IS_WIN_USB_DRIVER(self), 0);
    g_return_val_if_fail(g_task_is_valid(task, self), FALSE);

    return g_task_propagate_boolean(task, err);
}

/**
 * spice_win_usb_driver_install_async:
 * Start libusb driver installation for @device
 *
 * A new NamedPipe is created for each request.
 *
 * Returns: TRUE if a request was sent to usbclerk
 *          FALSE upon failure to send a request.
 */
G_GNUC_INTERNAL
void spice_win_usb_driver_install_async(SpiceWinUsbDriver *self,
                                        SpiceUsbDevice *device,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data)
{
    SPICE_DEBUG("Win usb driver installation started");

    spice_win_usb_driver_op(self, device, USB_CLERK_DRIVER_SESSION_INSTALL,
                            cancellable, callback, user_data);
}

G_GNUC_INTERNAL
void spice_win_usb_driver_uninstall_async(SpiceWinUsbDriver *self,
                                          SpiceUsbDevice *device,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data)
{
    SPICE_DEBUG("Win usb driver uninstall operation started");

    spice_win_usb_driver_op(self, device, USB_CLERK_DRIVER_REMOVE, cancellable,
                            callback, user_data);
}

G_GNUC_INTERNAL
gboolean spice_win_usb_driver_install_finish(SpiceWinUsbDriver *self,
                                          GAsyncResult *res, GError **err)
{
    return spice_win_usb_driver_op_finish(self, res, err);
}

G_GNUC_INTERNAL
gboolean spice_win_usb_driver_uninstall_finish(SpiceWinUsbDriver *self,
                                           GAsyncResult *res, GError **err)
{
    return spice_win_usb_driver_op_finish(self, res, err);
}

G_GNUC_INTERNAL
SpiceUsbDevice *spice_win_usb_driver_get_device(SpiceWinUsbDriver *self)
{
    g_return_val_if_fail(SPICE_IS_WIN_USB_DRIVER(self), 0);

    return self->priv->device;
}

GQuark spice_win_usb_driver_error_quark(void)
{
    return g_quark_from_static_string("spice-win-usb-driver-error-quark");
}
