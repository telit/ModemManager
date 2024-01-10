/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2019 Daniele Palmas <dnlplm@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-log-object.h"
#include "mm-modem-helpers.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-modem-mbim-telit.h"
#include "mm-modem-helpers-telit.h"
#include "mm-shared-telit.h"

static void iface_modem_init      (MMIfaceModem     *iface);
static void iface_modem_3gpp_init (MMIfaceModem3gpp *iface);
static void shared_telit_init     (MMSharedTelit    *iface);

static MMIfaceModem     *iface_modem_parent;
static MMIfaceModem3gpp *iface_modem_3gpp_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemMbimTelit, mm_broadband_modem_mbim_telit, MM_TYPE_BROADBAND_MODEM_MBIM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_TELIT, shared_telit_init))

struct _MMBroadbandModemMbimTelitPrivate {
    GRegex *srvlostena_regex;
    GRegex *at_5grrcind_regex;
};

/*****************************************************************************/
/* Load supported modes (Modem interface) */

static GArray *
load_supported_modes_finish (MMIfaceModem *self,
                             GAsyncResult *res,
                             GError **error)
{
    return (GArray *) g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_supported_modes_ready (MMIfaceModem *self,
                            GAsyncResult *res,
                            GTask *task)
{
    MMModemModeCombination modes_combination;
    MMModemMode modes_mask = MM_MODEM_MODE_NONE;
    const gchar   *response;
    GArray        *modes;
    GArray        *all;
    GArray        *combinations;
    GArray        *filtered;
    GError        *error = NULL;
    MMSharedTelit *shared = MM_SHARED_TELIT (self);
    guint          i;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, &error);
    if (error) {
        g_prefix_error (&error, "generic query of supported 3GPP networks with WS46=? failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    modes = mm_3gpp_parse_ws46_test_response (response, self, &error);
    if (!modes) {
        g_prefix_error (&error, "parsing WS46=? response failed: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    for (i = 0; i < modes->len; i++) {
        MMModemMode       mode;
        g_autofree gchar *str = NULL;

        mode = g_array_index (modes, MMModemMode, i);

        modes_mask |= mode;

        str = mm_modem_mode_build_string_from_mask (mode);
        mm_obj_dbg (self, "device allows (3GPP) mode combination: %s", str);
    }

    g_array_unref (modes);

    /* Build a mask with all supported modes */
    all = g_array_sized_new (FALSE, FALSE, sizeof (MMModemModeCombination), 1);
    modes_combination.allowed = modes_mask;
    modes_combination.preferred = MM_MODEM_MODE_NONE;
    g_array_append_val (all, modes_combination);

    /* Filter out those unsupported modes */
    combinations = mm_telit_build_modes_list();
    filtered = mm_filter_supported_modes (all, combinations, self);
    g_array_unref (all);
    g_array_unref (combinations);

    mm_shared_telit_store_supported_modes (shared, filtered);
    g_task_return_pointer (task, filtered, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

static void
load_supported_modes (MMIfaceModem *self,
                      GAsyncReadyCallback callback,
                      gpointer user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+WS46=?",
                              3,
                              TRUE, /* allow caching, it's a test command */
                              (GAsyncReadyCallback) load_supported_modes_ready,
                              task);
}

/*****************************************************************************/
/* Load revision (Modem interface) */

static gchar *
load_revision_finish (MMIfaceModem *self,
                      GAsyncResult *res,
                      GError      **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_revision_ready_shared (MMIfaceModem *self,
                            GAsyncResult *res,
                            GTask        *task)
{
    GError *error = NULL;
    gchar  *revision = NULL;

    revision = mm_shared_telit_modem_load_revision_finish (self, res, &error);
    if (!revision) {
        /* give up */
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    mm_shared_telit_store_revision (MM_SHARED_TELIT (self), revision);
    g_task_return_pointer (task, revision, g_free);
    g_object_unref (task);
}

static void
parent_load_revision_ready (MMIfaceModem *self,
                            GAsyncResult *res,
                            GTask        *task)
{
    gchar *revision = NULL;

    revision = iface_modem_parent->load_revision_finish (self, res, NULL);
    if (!revision || !strlen (revision)) {
        /* Some firmware versions do not properly populate the revision in the
         * MBIM response, so try using the AT ports */
        g_free (revision);
        mm_shared_telit_modem_load_revision (
            self,
            (GAsyncReadyCallback)load_revision_ready_shared,
            task);
        return;
    }
    mm_shared_telit_store_revision (MM_SHARED_TELIT (self), revision);
    g_task_return_pointer (task, revision, g_free);
    g_object_unref (task);
}

static void
load_revision (MMIfaceModem        *self,
               GAsyncReadyCallback  callback,
               gpointer             user_data)
{
    /* Run parent's loading */
    /* Telit's custom revision loading (in telit/mm-shared) is AT-only and the
     * MBIM modem might not have an AT port available, so we call the parent's
     * load_revision and store the revision taken from the firmware info capabilities. */
    iface_modem_parent->load_revision (
        MM_IFACE_MODEM (self),
        (GAsyncReadyCallback)parent_load_revision_ready,
        g_task_new (self, NULL, callback, user_data));
}

/*****************************************************************************/
/* Enable unsolicited events (3GPP interface) */

static gboolean
modem_3gpp_enable_disable_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                                                     GAsyncResult      *res,
                                                     GError           **error)
{
    GError *err = NULL;

    mm_base_modem_at_sequence_finish (MM_BASE_MODEM (self), res, NULL, &err);

    return !err;
}

static void
modem_3gpp_enable_disable_unsolicited_events_ready (MMBroadbandModemMbimTelit *self,
                                                    GAsyncResult              *res,
                                                    GTask                     *task)
{
    if (!modem_3gpp_enable_disable_unsolicited_events_finish (MM_IFACE_MODEM_3GPP(self), res, NULL))
        mm_obj_warn (self, "error during modem 3gpp Telit unsolicited enablement");

    /* Error is not blocking */
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static const MMBaseModemAtCommand enable_unsolicited_events[] = {
    { "#SRVLOSTENA=1", 3, FALSE, mm_base_modem_response_processor_no_result_continue },
    { "#5GRRCIND=1",   3, FALSE, mm_base_modem_response_processor_no_result_continue },
    { NULL }
};

static const MMBaseModemAtCommand disable_unsolicited_events[] = {
    { "#SRVLOSTENA=0", 3, FALSE, mm_base_modem_response_processor_no_result_continue },
    { "#5GRRCIND=0",   3, FALSE, mm_base_modem_response_processor_no_result_continue },
    { NULL }
};

static void
modem_3gpp_enable_disable_unsolicited_events (MMBroadbandModemMbimTelit *self,
                                              gboolean                   enable,
                                              GAsyncReadyCallback        callback,
                                              GTask                     *task)
{
    MMPortSerialAt *port;

    port = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));

    if (port) {
        const MMBaseModemAtCommand *cmds = NULL;

        if (enable)
            cmds = enable_unsolicited_events;
        else
            cmds = disable_unsolicited_events;

        mm_base_modem_at_sequence (MM_BASE_MODEM (self),
                                   cmds,
                                   NULL,
                                   NULL,
                                   callback,
                                   task);

        return;
    } else {
        g_task_return_new_error (task,
                                 MM_CORE_ERROR,
                                 MM_CORE_ERROR_FAILED,
                                 "unable to find the primary AT port");
        g_object_unref (task);
    }
}

static void
parent_3gpp_modem_enable_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                                   GAsyncResult     *res,
                                                   GTask            *task)
{
    g_autoptr(GError) error = NULL;

    if (!iface_modem_3gpp_parent->enable_unsolicited_events_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
    } else
        modem_3gpp_enable_disable_unsolicited_events (
            MM_BROADBAND_MODEM_MBIM_TELIT (self),
            TRUE,
            (GAsyncReadyCallback) modem_3gpp_enable_disable_unsolicited_events_ready,
            task);
}

static void
modem_3gpp_enable_unsolicited_events (MMIfaceModem3gpp    *self,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* chain up parent's enable first */
    iface_modem_3gpp_parent->enable_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_3gpp_modem_enable_unsolicited_events_ready,
        task);
}

static void
parent_modem_3gpp_disable_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                                    GAsyncResult     *res,
                                                    GTask            *task)
{
    g_autoptr(GError)  error = NULL;

    if (!iface_modem_3gpp_parent->disable_unsolicited_events_finish (self, res, &error))
        mm_obj_warn (self, "couldn't disable parent modem 3gpp unsolicited events: %s", error->message);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_3gpp_disable_unsolicited_events_ready (MMBroadbandModemMbimTelit *self,
                                             GAsyncResult              *res,
                                             GTask                     *task)
{
    if (!modem_3gpp_enable_disable_unsolicited_events_finish (MM_IFACE_MODEM_3GPP(self), res, NULL))
        mm_obj_warn (self, "couldn't disable modem 3gpp Telit unsolicited events");

    /* Chain up parent's disable */
    iface_modem_3gpp_parent->disable_unsolicited_events (
        MM_IFACE_MODEM_3GPP (self),
        (GAsyncReadyCallback)parent_modem_3gpp_disable_unsolicited_events_ready,
        task);
}

static void
modem_3gpp_disable_unsolicited_events (MMIfaceModem3gpp    *self,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* our own disabling first */
    modem_3gpp_enable_disable_unsolicited_events (MM_BROADBAND_MODEM_MBIM_TELIT (self),
        FALSE,
        (GAsyncReadyCallback) modem_3gpp_disable_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Setup/Cleanup unsolicited events (3GPP interface) */

static void
bearer_list_disconnect_foreach (MMBaseBearer *bearer)
{
    mm_base_bearer_disconnect_force (bearer);
}

static void
srvlostena_received (MMPortSerialAt            *port,
                     GMatchInfo                *match_info,
                     MMBroadbandModemMbimTelit *self)
{
    guint srvc_state;

    if (!mm_get_uint_from_match_info (match_info, 1, &srvc_state)) {
        mm_obj_warn (self, "couldn't parse service status from #SRVLOSTENA line");
        return;
    }

    if (srvc_state == 0) {
        MMBearerList *list = NULL;

        mm_obj_dbg (self, "service lost happened");
        /* If empty bearer list, nothing else to do */
        g_object_get (self,
                      MM_IFACE_MODEM_BEARER_LIST, &list,
                      NULL);
        if (!list)
            return;

        mm_bearer_list_foreach (list,
                                (MMBearerListForeachFunc)bearer_list_disconnect_foreach,
                                NULL);
        g_object_unref (list);
    } else if (srvc_state == 1)
        mm_obj_dbg (self, "service PLMN acquired");
}

static void
at_5grrcind_received (MMPortSerialAt            *port,
                      GMatchInfo                *match_info,
                      MMBroadbandModemMbimTelit *self)
{
    guint srvc_state;

    if (!mm_get_uint_from_match_info (match_info, 1, &srvc_state)) {
        mm_obj_warn (self, "couldn't parse service status from #5GRRCIND line");
        return;
    }

    if (srvc_state == 0) {
        MMBearerList *list = NULL;

        mm_obj_dbg (self, "NR5G RRC idle status");
        /* If empty bearer list, nothing else to do */
        g_object_get (self,
                      MM_IFACE_MODEM_BEARER_LIST, &list,
                      NULL);
        if (!list)
            return;

        mm_bearer_list_foreach (list,
                                (MMBearerListForeachFunc)bearer_list_disconnect_foreach,
                                NULL);
        g_object_unref (list);
    } else if (srvc_state == 1)
        mm_obj_dbg (self, "NR5G RRC connected");
}

static void
set_unsolicited_events_handlers (MMBroadbandModemMbimTelit *self,
                                 gboolean                   enable)
{
    MMPortSerialAt *port;

    port = mm_base_modem_peek_port_primary (MM_BASE_MODEM (self));

    if (port) {
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->srvlostena_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)srvlostena_received : NULL,
            enable ? self : NULL,
            NULL);
        mm_port_serial_at_add_unsolicited_msg_handler (
            port,
            self->priv->at_5grrcind_regex,
            enable ? (MMPortSerialAtUnsolicitedMsgFn)at_5grrcind_received : NULL,
            enable ? self : NULL,
            NULL);
    } else
        mm_obj_dbg (self, "unable to find the primary AT port");
}

static gboolean
modem_3gpp_setup_cleanup_unsolicited_events_finish (MMIfaceModem3gpp  *self,
                                                    GAsyncResult      *res,
                                                    GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_cleanup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                         GAsyncResult     *res,
                                         GTask            *task)
{
    g_autoptr(GError) error = NULL;

    if (!iface_modem_3gpp_parent->cleanup_unsolicited_events_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_3gpp_cleanup_unsolicited_events (MMIfaceModem3gpp    *self,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* our own cleanup first */
    set_unsolicited_events_handlers (MM_BROADBAND_MODEM_MBIM_TELIT (self), FALSE);

    /* Chain up parent's setup */
    iface_modem_3gpp_parent->cleanup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_cleanup_unsolicited_events_ready,
        task);
}

static void
parent_setup_unsolicited_events_ready (MMIfaceModem3gpp *self,
                                       GAsyncResult     *res,
                                       GTask            *task)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->setup_unsolicited_events_finish (self, res, &error))
        g_task_return_error (task, error);
    else {
        set_unsolicited_events_handlers (MM_BROADBAND_MODEM_MBIM_TELIT (self), TRUE);
        g_task_return_boolean (task, TRUE);
    }
    g_object_unref (task);
}

static void
modem_3gpp_setup_unsolicited_events (MMIfaceModem3gpp    *self,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Chain up parent's setup */
    iface_modem_3gpp_parent->setup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_setup_unsolicited_events_ready,
        task);
}

/*****************************************************************************/

MMBroadbandModemMbimTelit *
mm_broadband_modem_mbim_telit_new (const gchar  *device,
                                   const gchar  *physdev,
                                   const gchar **drivers,
                                   const gchar  *plugin,
                                   guint16       vendor_id,
                                   guint16       product_id,
                                   guint16       subsystem_vendor_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_MBIM_TELIT,
                         MM_BASE_MODEM_DEVICE,              device,
                         MM_BASE_MODEM_PHYSDEV,             physdev,
                         MM_BASE_MODEM_DRIVERS,             drivers,
                         MM_BASE_MODEM_PLUGIN,              plugin,
                         MM_BASE_MODEM_VENDOR_ID,           vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID,          product_id,
                         MM_BASE_MODEM_SUBSYSTEM_VENDOR_ID, subsystem_vendor_id,
                         /* MBIM bearer supports NET only */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_SUPPORTED, TRUE,
                         NULL);
}

static void
mm_broadband_modem_mbim_telit_init (MMBroadbandModemMbimTelit *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_BROADBAND_MODEM_MBIM_TELIT, MMBroadbandModemMbimTelitPrivate);
    self->priv->srvlostena_regex = g_regex_new ("#SRVLOSTENA: 1,\\s*0*([0-1])", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
    self->priv->at_5grrcind_regex = g_regex_new ("#5GRRCIND: 1,([0-1])", G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}

static void
finalize (GObject *object)
{
    MMBroadbandModemMbimTelit *self = MM_BROADBAND_MODEM_MBIM_TELIT (object);

    g_regex_unref (self->priv->srvlostena_regex);
    g_regex_unref (self->priv->at_5grrcind_regex);
    G_OBJECT_CLASS (mm_broadband_modem_mbim_telit_parent_class)->finalize (object);
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);

    iface->enable_unsolicited_events = modem_3gpp_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = modem_3gpp_enable_disable_unsolicited_events_finish;
    iface->disable_unsolicited_events = modem_3gpp_disable_unsolicited_events;
    iface->disable_unsolicited_events_finish = modem_3gpp_enable_disable_unsolicited_events_finish;

    iface->setup_unsolicited_events = modem_3gpp_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_3gpp_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_3gpp_setup_cleanup_unsolicited_events_finish;
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);

    iface->set_current_bands = mm_shared_telit_modem_set_current_bands;
    iface->set_current_bands_finish = mm_shared_telit_modem_set_current_bands_finish;
    iface->load_current_bands = mm_shared_telit_modem_load_current_bands;
    iface->load_current_bands_finish = mm_shared_telit_modem_load_current_bands_finish;
    iface->load_supported_bands = mm_shared_telit_modem_load_supported_bands;
    iface->load_supported_bands_finish = mm_shared_telit_modem_load_supported_bands_finish;
    iface->load_supported_modes = load_supported_modes;
    iface->load_supported_modes_finish = load_supported_modes_finish;
    iface->load_current_modes = mm_shared_telit_load_current_modes;
    iface->load_current_modes_finish = mm_shared_telit_load_current_modes_finish;
    iface->set_current_modes = mm_shared_telit_set_current_modes;
    iface->set_current_modes_finish = mm_shared_telit_set_current_modes_finish;
    iface->load_revision_finish = load_revision_finish;
    iface->load_revision = load_revision;
}

static MMIfaceModem *
peek_parent_modem_interface (MMSharedTelit *self)
{
    return iface_modem_parent;
}

static void
shared_telit_init (MMSharedTelit *iface)
{
    iface->peek_parent_modem_interface = peek_parent_modem_interface;
}

static void
mm_broadband_modem_mbim_telit_class_init (MMBroadbandModemMbimTelitClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemMbimTelitPrivate));
    object_class->finalize = finalize;
}
