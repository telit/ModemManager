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
 * Copyright (C) 2016-2019 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-log-object.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-voice.h"
#include "mm-iface-modem-3gpp-profile-manager.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-bearer.h"
#include "mm-broadband-modem-ublox.h"
#include "mm-broadband-bearer-ublox.h"
#include "mm-sim-ublox.h"
#include "mm-modem-helpers-ublox.h"
#include "mm-ublox-enums-types.h"

static void iface_modem_init (MMIfaceModem *iface);
static void iface_modem_voice_init (MMIfaceModemVoice *iface);
static void iface_modem_3gpp_init(MMIfaceModem3gpp * iface);
static void iface_modem_3gpp_profile_manager_init (MMIfaceModem3gppProfileManager *iface);


static MMIfaceModemVoice *iface_modem_voice_parent;
static MMIfaceModem3gpp * iface_modem_3gpp_parent;
static MMIfaceModem3gppProfileManager *iface_modem_3gpp_profile_manager_parent;



G_DEFINE_TYPE_EXTENDED (MMBroadbandModemUblox, mm_broadband_modem_ublox, MM_TYPE_BROADBAND_MODEM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP_PROFILE_MANAGER, iface_modem_3gpp_profile_manager_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_VOICE, iface_modem_voice_init))


struct _MMBroadbandModemUbloxPrivate {
    /* USB profile in use */
    MMUbloxUsbProfile profile;
    gboolean          profile_checked;
    /* Networking mode in use */
    MMUbloxNetworkingMode mode;
    gboolean              mode_checked;

    /* Flag to specify whether a power operation is ongoing */
    gboolean power_operation_ongoing;

    /* Mode combination to apply if "any" requested */
    MMModemMode any_allowed;

    /* AT command configuration */
    UbloxSupportConfig support_config;

    /* Voice +UCALLSTAT support */
    GRegex *ucallstat_regex;

    FeatureSupport udtmfd_support;
    GRegex *udtmfd_regex;

    /* Regex to ignore */
    GRegex *pbready_regex;
};

/*****************************************************************************/
/* Per-model configuration loading */

static void
preload_support_config (MMBroadbandModemUblox *self)
{
    const gchar *model;
    GError      *error = NULL;

    /* Make sure we load only once */
    if (self->priv->support_config.loaded)
        return;

    model = mm_iface_modem_get_model (MM_IFACE_MODEM (self));

    if (!mm_ublox_get_support_config (model, &self->priv->support_config, &error)) {
        mm_obj_warn (self, "loading support configuration failed: %s", error->message);
        g_error_free (error);

        /* default to NOT SUPPORTED if unknown model */
        self->priv->support_config.method = SETTINGS_UPDATE_METHOD_UNKNOWN;
        self->priv->support_config.uact = FEATURE_UNSUPPORTED;
        self->priv->support_config.ubandsel = FEATURE_UNSUPPORTED;
    } else
        mm_obj_dbg (self, "support configuration found for '%s'", model);

    switch (self->priv->support_config.method) {
        case SETTINGS_UPDATE_METHOD_CFUN:
            mm_obj_dbg (self, "  band update requires low-power mode");
            break;
        case SETTINGS_UPDATE_METHOD_COPS:
            mm_obj_dbg (self, "  band update requires explicit unregistration");
            break;
        case SETTINGS_UPDATE_METHOD_UNKNOWN:
            /* not an error, this just means we don't need anything special */
            break;
        default:
            g_assert_not_reached ();
    }

    switch (self->priv->support_config.uact) {
        case FEATURE_SUPPORTED:
            mm_obj_dbg (self, "  UACT based band configuration supported");
            break;
        case FEATURE_UNSUPPORTED:
            mm_obj_dbg (self, "  UACT based band configuration unsupported");
            break;
        case FEATURE_SUPPORT_UNKNOWN:
        default:
            g_assert_not_reached();
    }

    switch (self->priv->support_config.ubandsel) {
        case FEATURE_SUPPORTED:
            mm_obj_dbg (self, "  UBANDSEL based band configuration supported");
            break;
        case FEATURE_UNSUPPORTED:
            mm_obj_dbg (self, "  UBANDSEL based band configuration unsupported");
            break;
        case FEATURE_SUPPORT_UNKNOWN:
        default:
            g_assert_not_reached();
    }
}

/*****************************************************************************/

static gboolean
acquire_power_operation (MMBroadbandModemUblox  *self,
                         GError                **error)
{
    if (self->priv->power_operation_ongoing) {
        g_set_error (error, MM_CORE_ERROR, MM_CORE_ERROR_RETRY,
                     "An operation which requires power updates is currently in progress");
        return FALSE;
    }
    self->priv->power_operation_ongoing = TRUE;
    return TRUE;
}

static void
release_power_operation (MMBroadbandModemUblox *self)
{
    g_assert (self->priv->power_operation_ongoing);
    self->priv->power_operation_ongoing = FALSE;
}

/*****************************************************************************/
/* Load supported bands (Modem interface) */

static GArray *
load_supported_bands_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    return (GArray *) g_task_propagate_pointer (G_TASK (res), error);
}

static void
load_supported_bands (MMIfaceModem        *self,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    GTask       *task;
    GError      *error = NULL;
    GArray      *bands = NULL;
    const gchar *model;

    model = mm_iface_modem_get_model (self);
    task  = g_task_new (self, NULL, callback, user_data);

    bands = mm_ublox_get_supported_bands (model, self, &error);
    if (!bands)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bands, (GDestroyNotify) g_array_unref);
    g_object_unref (task);
}

/*****************************************************************************/
/* Load current bands (Modem interface) */

static GArray *
load_current_bands_finish (MMIfaceModem  *self,
                           GAsyncResult  *res,
                           GError       **error)
{
    return (GArray *) g_task_propagate_pointer (G_TASK (res), error);
}

static void
uact_load_current_bands_ready (MMBaseModem  *self,
                               GAsyncResult *res,
                               GTask        *task)
{
    GError      *error = NULL;
    const gchar *response;
    GArray      *out;

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    out = mm_ublox_parse_uact_response (response, &error);
    if (!out) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_task_return_pointer (task, out, (GDestroyNotify)g_array_unref);
    g_object_unref (task);
}

static void
ubandsel_load_current_bands_ready (MMBaseModem  *self,
                                   GAsyncResult *res,
                                   GTask        *task)
{
    GError      *error = NULL;
    const gchar *response;
    const gchar *model;
    GArray      *out;

    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    model = mm_iface_modem_get_model (MM_IFACE_MODEM (self));
    out = mm_ublox_parse_ubandsel_response (response, model, self, &error);
    if (!out) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_task_return_pointer (task, out, (GDestroyNotify)g_array_unref);
    g_object_unref (task);
}

static void
load_current_bands (MMIfaceModem        *_self,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    MMBroadbandModemUblox *self = MM_BROADBAND_MODEM_UBLOX (_self);
    GTask                 *task;

    preload_support_config (self);

    task = g_task_new (self, NULL, callback, user_data);

    if (self->priv->support_config.ubandsel == FEATURE_SUPPORTED) {
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "+UBANDSEL?",
            3,
            FALSE,
            (GAsyncReadyCallback)ubandsel_load_current_bands_ready,
            task);
        return;
    }

    if (self->priv->support_config.uact == FEATURE_SUPPORTED) {
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "+UACT?",
            3,
            FALSE,
            (GAsyncReadyCallback)uact_load_current_bands_ready,
            task);
        return;
    }

    g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                             "loading current bands is unsupported");
    g_object_unref (task);
}

/*****************************************************************************/
/* Set allowed modes/bands (Modem interface) */

typedef enum {
    SET_CURRENT_MODES_BANDS_STEP_FIRST,
    SET_CURRENT_MODES_BANDS_STEP_ACQUIRE,
    SET_CURRENT_MODES_BANDS_STEP_CURRENT_POWER,
    SET_CURRENT_MODES_BANDS_STEP_BEFORE_COMMAND,
    SET_CURRENT_MODES_BANDS_STEP_COMMAND,
    SET_CURRENT_MODES_BANDS_STEP_AFTER_COMMAND,
    SET_CURRENT_MODES_BANDS_STEP_RELEASE,
    SET_CURRENT_MODES_BANDS_STEP_LAST,
} SetCurrentModesBandsStep;

typedef struct {
    SetCurrentModesBandsStep  step;
    gchar                    *command;
    MMModemPowerState         initial_state;
    GError                   *saved_error;
} SetCurrentModesBandsContext;

static void
set_current_modes_bands_context_free (SetCurrentModesBandsContext *ctx)
{
    g_assert (!ctx->saved_error);
    g_free (ctx->command);
    g_slice_free (SetCurrentModesBandsContext, ctx);
}

static void
set_current_modes_bands_context_new (GTask *task,
                                     gchar *command)
{
    SetCurrentModesBandsContext *ctx;

    ctx = g_slice_new0 (SetCurrentModesBandsContext);
    ctx->command = command;
    ctx->initial_state = MM_MODEM_POWER_STATE_UNKNOWN;
    ctx->step = SET_CURRENT_MODES_BANDS_STEP_FIRST;
    g_task_set_task_data (task, ctx, (GDestroyNotify) set_current_modes_bands_context_free);
}

static gboolean
common_set_current_modes_bands_finish (MMIfaceModem  *self,
                                       GAsyncResult  *res,
                                       GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void set_current_modes_bands_step (GTask *task);

static void
set_current_modes_bands_reregister_in_network_ready (MMIfaceModem3gpp *self,
                                                     GAsyncResult     *res,
                                                     GTask            *task)
{
    SetCurrentModesBandsContext *ctx;

    ctx = g_task_get_task_data (task);

    /* propagate the error if none already set */
    mm_iface_modem_3gpp_reregister_in_network_finish (self, res, ctx->saved_error ? NULL : &ctx->saved_error);

    /* Go to next step (release power operation) regardless of the result */
    ctx->step++;
    set_current_modes_bands_step (task);
}

static void
set_current_modes_bands_after_command_ready (MMBaseModem  *self,
                                             GAsyncResult *res,
                                             GTask        *task)
{
    SetCurrentModesBandsContext *ctx;

    ctx = g_task_get_task_data (task);

    /* propagate the error if none already set */
    mm_base_modem_at_command_finish (self, res, ctx->saved_error ? NULL : &ctx->saved_error);

    /* Go to next step (release power operation) regardless of the result */
    ctx->step++;
    set_current_modes_bands_step (task);
}

static void
set_current_modes_bands_command_ready (MMBaseModem  *self,
                                       GAsyncResult *res,
                                       GTask        *task)
{
    SetCurrentModesBandsContext *ctx;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (self, res, &ctx->saved_error))
        ctx->step = SET_CURRENT_MODES_BANDS_STEP_RELEASE;
    else
        ctx->step++;

    set_current_modes_bands_step (task);
}

static void
set_current_modes_bands_before_command_ready (MMBaseModem  *self,
                                              GAsyncResult *res,
                                              GTask        *task)
{
    SetCurrentModesBandsContext *ctx;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (self, res, &ctx->saved_error))
        ctx->step = SET_CURRENT_MODES_BANDS_STEP_RELEASE;
    else
        ctx->step++;

    set_current_modes_bands_step (task);
}

static void
set_current_modes_bands_current_power_ready (MMBaseModem  *_self,
                                             GAsyncResult *res,
                                             GTask        *task)
{
    MMBroadbandModemUblox       *self = MM_BROADBAND_MODEM_UBLOX (_self);
    SetCurrentModesBandsContext *ctx;
    const gchar                 *response;

    ctx = g_task_get_task_data (task);

    g_assert (self->priv->support_config.method == SETTINGS_UPDATE_METHOD_CFUN);

    response = mm_base_modem_at_command_finish (_self, res, &ctx->saved_error);
    if (!response || !mm_ublox_parse_cfun_response (response, &ctx->initial_state, &ctx->saved_error))
        ctx->step = SET_CURRENT_MODES_BANDS_STEP_RELEASE;
    else
        ctx->step++;

    set_current_modes_bands_step (task);
}

static void
set_current_modes_bands_step (GTask *task)
{
    MMBroadbandModemUblox       *self;
    SetCurrentModesBandsContext *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
    case SET_CURRENT_MODES_BANDS_STEP_FIRST:
        ctx->step++;
        /* fall through */

    case SET_CURRENT_MODES_BANDS_STEP_ACQUIRE:
        mm_obj_dbg (self, "acquiring power operation...");
        if (!acquire_power_operation (self, &ctx->saved_error)) {
            ctx->step = SET_CURRENT_MODES_BANDS_STEP_LAST;
            set_current_modes_bands_step (task);
            return;
        }
        ctx->step++;
        /* fall through */

    case SET_CURRENT_MODES_BANDS_STEP_CURRENT_POWER:
        /* If using CFUN, we check whether we're already in low-power mode.
         * And if we are, we just skip triggering low-power mode ourselves.
         */
        if (self->priv->support_config.method == SETTINGS_UPDATE_METHOD_CFUN) {
            mm_obj_dbg (self, "checking current power operation...");
            mm_base_modem_at_command (MM_BASE_MODEM (self),
                                      "+CFUN?",
                                      3,
                                      FALSE,
                                      (GAsyncReadyCallback) set_current_modes_bands_current_power_ready,
                                      task);
            return;
        }
        ctx->step++;
        /* fall through */

    case SET_CURRENT_MODES_BANDS_STEP_BEFORE_COMMAND:
        /* If COPS required around the set command, run it unconditionally */
        if (self->priv->support_config.method == SETTINGS_UPDATE_METHOD_COPS) {
            mm_obj_dbg (self, "deregistering from the network for configuration change...");
            mm_base_modem_at_command (
                    MM_BASE_MODEM (self),
                    "+COPS=2",
                    10,
                    FALSE,
                    (GAsyncReadyCallback) set_current_modes_bands_before_command_ready,
                    task);
                return;
        }
        /* If CFUN required, check initial state before triggering low-power mode ourselves */
        else if (self->priv->support_config.method == SETTINGS_UPDATE_METHOD_CFUN) {
            /* Do nothing if already in low-power mode */
            if (ctx->initial_state != MM_MODEM_POWER_STATE_LOW) {
                mm_obj_dbg (self, "powering down for configuration change...");
                mm_base_modem_at_command (
                    MM_BASE_MODEM (self),
                    "+CFUN=4",
                    3,
                    FALSE,
                    (GAsyncReadyCallback) set_current_modes_bands_before_command_ready,
                    task);
                return;
            }
        }

        ctx->step++;
        /* fall through */

    case SET_CURRENT_MODES_BANDS_STEP_COMMAND:
        mm_obj_dbg (self, "updating configuration...");
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            ctx->command,
            3,
            FALSE,
            (GAsyncReadyCallback) set_current_modes_bands_command_ready,
            task);
        return;

    case SET_CURRENT_MODES_BANDS_STEP_AFTER_COMMAND:
        /* If COPS required around the set command, run it unconditionally */
        if (self->priv->support_config.method == SETTINGS_UPDATE_METHOD_COPS) {
            mm_iface_modem_3gpp_reregister_in_network (MM_IFACE_MODEM_3GPP (self),
                                                       (GAsyncReadyCallback) set_current_modes_bands_reregister_in_network_ready,
                                                       task);
            return;
        }
        /* If CFUN required, see if we need to recover power */
        else if (self->priv->support_config.method == SETTINGS_UPDATE_METHOD_CFUN) {
            /* If we were in low-power mode before the change, do nothing, otherwise,
             * full power mode back */
            if (ctx->initial_state != MM_MODEM_POWER_STATE_LOW) {
                mm_obj_dbg (self, "recovering power state after configuration change...");
                mm_base_modem_at_command (
                    MM_BASE_MODEM (self),
                    "+CFUN=1",
                    3,
                    FALSE,
                    (GAsyncReadyCallback) set_current_modes_bands_after_command_ready,
                    task);
                return;
            }
        }
        ctx->step++;
        /* fall through */

    case SET_CURRENT_MODES_BANDS_STEP_RELEASE:
        mm_obj_dbg (self, "releasing power operation...");
        release_power_operation (self);
        ctx->step++;
        /* fall through */

    case SET_CURRENT_MODES_BANDS_STEP_LAST:
        if (ctx->saved_error) {
            g_task_return_error (task, ctx->saved_error);
            ctx->saved_error = NULL;
        } else
            g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

static void
set_current_modes (MMIfaceModem        *self,
                   MMModemMode          allowed,
                   MMModemMode          preferred,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    GTask  *task;
    gchar  *command;
    GError *error = NULL;

    preload_support_config (MM_BROADBAND_MODEM_UBLOX (self));

    task = g_task_new (self, NULL, callback, user_data);

    /* Handle ANY */
    if (allowed == MM_MODEM_MODE_ANY)
        allowed = MM_BROADBAND_MODEM_UBLOX (self)->priv->any_allowed;

    /* Build command */
    command = mm_ublox_build_urat_set_command (allowed, preferred, &error);
    if (!command) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    set_current_modes_bands_context_new (task, command);
    set_current_modes_bands_step (task);
}

static void
set_current_bands (MMIfaceModem        *_self,
                   GArray              *bands_array,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
    MMBroadbandModemUblox *self  = MM_BROADBAND_MODEM_UBLOX (_self);
    GTask                 *task;
    GError                *error = NULL;
    gchar                 *command = NULL;
    const gchar           *model;

    preload_support_config (self);

    task = g_task_new (self, NULL, callback, user_data);

    model = mm_iface_modem_get_model (_self);

    /* Build command */
    if (self->priv->support_config.uact == FEATURE_SUPPORTED)
        command = mm_ublox_build_uact_set_command (bands_array, &error);
    else if (self->priv->support_config.ubandsel == FEATURE_SUPPORTED)
        command = mm_ublox_build_ubandsel_set_command (bands_array, model, &error);

    if (!command) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    set_current_modes_bands_context_new (task, command);
    set_current_modes_bands_step (task);
}

/*****************************************************************************/
/* Load current modes (Modem interface) */

static gboolean
load_current_modes_finish (MMIfaceModem  *self,
                           GAsyncResult  *res,
                           MMModemMode   *allowed,
                           MMModemMode   *preferred,
                           GError       **error)
{
    const gchar *response;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!response)
        return FALSE;

    return mm_ublox_parse_urat_read_response (response, self, allowed, preferred, error);
}

static void
load_current_modes (MMIfaceModem        *self,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+URAT?",
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Load supported modes (Modem interface) */

static GArray *
load_supported_modes_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    const gchar *response;
    GArray      *combinations;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!response)
        return FALSE;

    if (!(combinations = mm_ublox_parse_urat_test_response (response, self, error)))
        return FALSE;

    if (!(combinations = mm_ublox_filter_supported_modes (mm_iface_modem_get_model (self), combinations, self, error)))
        return FALSE;

    /* Decide and store which combination to apply when ANY requested */
    MM_BROADBAND_MODEM_UBLOX (self)->priv->any_allowed = mm_ublox_get_modem_mode_any (combinations);

    /* If 4G supported, explicitly use +CEREG */
    if (MM_BROADBAND_MODEM_UBLOX (self)->priv->any_allowed & MM_MODEM_MODE_4G)
        g_object_set (self, MM_IFACE_MODEM_3GPP_EPS_NETWORK_SUPPORTED, TRUE, NULL);

    return combinations;
}

static void
load_supported_modes (MMIfaceModem        *self,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    mm_base_modem_at_command (
        MM_BASE_MODEM (self),
        "+URAT=?",
        3,
        TRUE,
        callback,
        user_data);
}

/*****************************************************************************/
/* Power state loading (Modem interface) */

static MMModemPowerState
load_power_state_finish (MMIfaceModem  *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    MMModemPowerState  state = MM_MODEM_POWER_STATE_UNKNOWN;
    const gchar       *response;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (response)
        mm_ublox_parse_cfun_response (response, &state, error);
    return state;
}

static void
load_power_state (MMIfaceModem        *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+CFUN?",
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Modem power up/down/off (Modem interface) */

static gboolean
common_modem_power_operation_finish (MMIfaceModem  *self,
                                     GAsyncResult  *res,
                                     GError       **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
power_operation_ready (MMBaseModem  *self,
                       GAsyncResult *res,
                       GTask        *task)
{
    GError *error = NULL;

    release_power_operation (MM_BROADBAND_MODEM_UBLOX (self));

    if (!mm_base_modem_at_command_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
common_modem_power_operation (MMBroadbandModemUblox  *self,
                              const gchar            *command,
                              GAsyncReadyCallback     callback,
                              gpointer                user_data)
{
    GTask  *task;
    GError *error = NULL;

    task = g_task_new (self, NULL, callback, user_data);

    /* Fail if there is already an ongoing power management operation */
    if (!acquire_power_operation (self, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Use AT+CFUN=4 for power down, puts device in airplane mode */
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              command,
                              30,
                              FALSE,
                              (GAsyncReadyCallback) power_operation_ready,
                              task);
}

static void
modem_reset (MMIfaceModem        *self,
             GAsyncReadyCallback  callback,
             gpointer             user_data)
{
    common_modem_power_operation (MM_BROADBAND_MODEM_UBLOX (self), "+CFUN=16", callback, user_data);
}

static void
modem_power_off (MMIfaceModem        *self,
                 GAsyncReadyCallback  callback,
                 gpointer             user_data)
{
    common_modem_power_operation (MM_BROADBAND_MODEM_UBLOX (self), "+CPWROFF", callback, user_data);
}

static void
modem_power_down (MMIfaceModem        *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    common_modem_power_operation (MM_BROADBAND_MODEM_UBLOX (self), "+CFUN=4", callback, user_data);
}

static void
modem_power_up (MMIfaceModem        *self,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
    common_modem_power_operation (MM_BROADBAND_MODEM_UBLOX (self), "+CFUN=1", callback, user_data);
}

/*****************************************************************************/
/* Load unlock retries (Modem interface) */

static MMUnlockRetries *
load_unlock_retries_finish (MMIfaceModem *self,
                            GAsyncResult *res,
                            GError **error)
{
    const gchar     *response;
    MMUnlockRetries *retries;
    guint            pin_attempts = 0;
    guint            pin2_attempts = 0;
    guint            puk_attempts = 0;
    guint            puk2_attempts = 0;

    response = mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, error);
    if (!response || !mm_ublox_parse_upincnt_response (response,
                                                       &pin_attempts, &pin2_attempts,
                                                       &puk_attempts, &puk2_attempts,
                                                       error))
        return NULL;

    retries = mm_unlock_retries_new ();
    mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PIN,  pin_attempts);
    mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PUK,  puk_attempts);
    mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PIN2, pin2_attempts);
    mm_unlock_retries_set (retries, MM_MODEM_LOCK_SIM_PUK2, puk2_attempts);

    return retries;
}

static void
load_unlock_retries (MMIfaceModem        *self,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+UPINCNT",
                              3,
                              FALSE,
                              callback,
                              user_data);
}

/*****************************************************************************/
/* Common enable/disable voice unsolicited events */

typedef enum {
    VOICE_UNSOLICITED_EVENTS_STEP_FIRST,
    VOICE_UNSOLICITED_EVENTS_STEP_UCALLSTAT_PRIMARY,
    VOICE_UNSOLICITED_EVENTS_STEP_UCALLSTAT_SECONDARY,
    VOICE_UNSOLICITED_EVENTS_STEP_UDTMFD_PRIMARY,
    VOICE_UNSOLICITED_EVENTS_STEP_UDTMFD_SECONDARY,
    VOICE_UNSOLICITED_EVENTS_STEP_LAST,
} VoiceUnsolicitedEventsStep;

typedef struct {
    gboolean                    enable;
    VoiceUnsolicitedEventsStep  step;
    MMPortSerialAt             *primary;
    MMPortSerialAt             *secondary;
    gchar                      *ucallstat_command;
    gchar                      *udtmfd_command;
} VoiceUnsolicitedEventsContext;

static void
voice_unsolicited_events_context_free (VoiceUnsolicitedEventsContext *ctx)
{
    g_clear_object (&ctx->secondary);
    g_clear_object (&ctx->primary);
    g_free (ctx->ucallstat_command);
    g_free (ctx->udtmfd_command);
    g_slice_free (VoiceUnsolicitedEventsContext, ctx);
}

static gboolean
common_voice_enable_disable_unsolicited_events_finish (MMBroadbandModemUblox  *self,
                                                       GAsyncResult           *res,
                                                       GError                **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void voice_unsolicited_events_context_step (GTask *task);

static void
udtmfd_ready (MMBaseModem  *self,
              GAsyncResult *res,
              GTask        *task)
{
    VoiceUnsolicitedEventsContext *ctx;
    GError                        *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_full_finish (self, res, &error)) {
        mm_obj_dbg (self, "couldn't %s +UUDTMFD reporting: '%s'",
                    ctx->enable ? "enable" : "disable",
                    error->message);
        g_error_free (error);
    }

    ctx->step++;
    voice_unsolicited_events_context_step (task);
}

static void
ucallstat_ready (MMBaseModem  *self,
                 GAsyncResult *res,
                 GTask        *task)
{
    VoiceUnsolicitedEventsContext *ctx;
    GError                        *error = NULL;

    ctx = g_task_get_task_data (task);

    if (!mm_base_modem_at_command_full_finish (self, res, &error)) {
        mm_obj_dbg (self, "couldn't %s +UCALLSTAT reporting: '%s'",
                    ctx->enable ? "enable" : "disable",
                    error->message);
        g_error_free (error);
    }

    ctx->step++;
    voice_unsolicited_events_context_step (task);
}

static void
voice_unsolicited_events_context_step (GTask *task)
{
    MMBroadbandModemUblox         *self;
    VoiceUnsolicitedEventsContext *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
    case VOICE_UNSOLICITED_EVENTS_STEP_FIRST:
        ctx->step++;
        /* fall-through */

    case VOICE_UNSOLICITED_EVENTS_STEP_UCALLSTAT_PRIMARY:
        if (ctx->primary) {
            mm_obj_dbg (self, "%s extended call status reporting in primary port...",
                        ctx->enable ? "enabling" : "disabling");
            mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                           ctx->primary,
                                           ctx->ucallstat_command,
                                           3,
                                           FALSE,
                                           FALSE,
                                           NULL,
                                           (GAsyncReadyCallback)ucallstat_ready,
                                           task);
            return;
        }
        ctx->step++;
        /* fall-through */

    case VOICE_UNSOLICITED_EVENTS_STEP_UCALLSTAT_SECONDARY:
        if (ctx->secondary) {
            mm_obj_dbg (self, "%s extended call status reporting in secondary port...",
                        ctx->enable ? "enabling" : "disabling");
            mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                           ctx->secondary,
                                           ctx->ucallstat_command,
                                           3,
                                           FALSE,
                                           FALSE,
                                           NULL,
                                           (GAsyncReadyCallback)ucallstat_ready,
                                           task);
            return;
        }
        ctx->step++;
        /* fall-through */

    case VOICE_UNSOLICITED_EVENTS_STEP_UDTMFD_PRIMARY:
        if ((self->priv->udtmfd_support == FEATURE_SUPPORTED) && (ctx->primary)) {
            mm_obj_dbg (self, "%s DTMF detection and reporting in primary port...",
                        ctx->enable ? "enabling" : "disabling");
            mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                           ctx->primary,
                                           ctx->udtmfd_command,
                                           3,
                                           FALSE,
                                           FALSE,
                                           NULL,
                                           (GAsyncReadyCallback)udtmfd_ready,
                                           task);
            return;
        }
        ctx->step++;
        /* fall-through */

    case VOICE_UNSOLICITED_EVENTS_STEP_UDTMFD_SECONDARY:
        if ((self->priv->udtmfd_support == FEATURE_SUPPORTED) && (ctx->secondary)) {
            mm_obj_dbg (self, "%s DTMF detection and reporting in secondary port...",
                        ctx->enable ? "enabling" : "disabling");
            mm_base_modem_at_command_full (MM_BASE_MODEM (self),
                                           ctx->secondary,
                                           ctx->udtmfd_command,
                                           3,
                                           FALSE,
                                           FALSE,
                                           NULL,
                                           (GAsyncReadyCallback)udtmfd_ready,
                                           task);
            return;
        }
        ctx->step++;
        /* fall-through */

    case VOICE_UNSOLICITED_EVENTS_STEP_LAST:
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

static void
common_voice_enable_disable_unsolicited_events (MMBroadbandModemUblox *self,
                                                gboolean               enable,
                                                GAsyncReadyCallback    callback,
                                                gpointer               user_data)
{
    VoiceUnsolicitedEventsContext *ctx;
    GTask                         *task;

    task = g_task_new (self, NULL, callback, user_data);

    ctx = g_slice_new0 (VoiceUnsolicitedEventsContext);
    ctx->step = VOICE_UNSOLICITED_EVENTS_STEP_FIRST;
    ctx->enable = enable;
    if (enable) {
        ctx->ucallstat_command = g_strdup ("+UCALLSTAT=1");
        ctx->udtmfd_command = g_strdup ("+UDTMFD=1,2");
    } else {
        ctx->ucallstat_command = g_strdup ("+UCALLSTAT=0");
        ctx->udtmfd_command = g_strdup ("+UDTMFD=0");
    }
    ctx->primary = mm_base_modem_get_port_primary (MM_BASE_MODEM (self));
    ctx->secondary = mm_base_modem_get_port_secondary (MM_BASE_MODEM (self));
    g_task_set_task_data (task, ctx, (GDestroyNotify) voice_unsolicited_events_context_free);

    voice_unsolicited_events_context_step (task);
}

/*****************************************************************************/
/* Enabling unsolicited events (Voice interface) */

static gboolean
modem_voice_enable_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                              GAsyncResult       *res,
                                              GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
voice_enable_unsolicited_events_ready (MMBroadbandModemUblox *self,
                                       GAsyncResult          *res,
                                       GTask                 *task)
{
    GError *error = NULL;

    if (!common_voice_enable_disable_unsolicited_events_finish (self, res, &error)) {
        mm_obj_warn (self, "Couldn't enable u-blox-specific voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_voice_enable_unsolicited_events_ready (MMIfaceModemVoice *self,
                                              GAsyncResult      *res,
                                              GTask             *task)
{
    GError *error = NULL;

    if (!iface_modem_voice_parent->enable_unsolicited_events_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    common_voice_enable_disable_unsolicited_events (MM_BROADBAND_MODEM_UBLOX (self),
                                                    TRUE,
                                                    (GAsyncReadyCallback) voice_enable_unsolicited_events_ready,
                                                    task);
}

static void
modem_voice_enable_unsolicited_events (MMIfaceModemVoice   *self,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* Chain up parent's enable */
    iface_modem_voice_parent->enable_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_voice_enable_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Disabling unsolicited events (Voice interface) */

static gboolean
modem_voice_disable_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                               GAsyncResult       *res,
                                               GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_voice_disable_unsolicited_events_ready (MMIfaceModemVoice *self,
                                               GAsyncResult      *res,
                                               GTask             *task)
{
    GError *error = NULL;

    if (!iface_modem_voice_parent->disable_unsolicited_events_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
voice_disable_unsolicited_events_ready (MMBroadbandModemUblox *self,
                                        GAsyncResult          *res,
                                        GTask                 *task)
{
    GError *error = NULL;

    if (!common_voice_enable_disable_unsolicited_events_finish (self, res, &error)) {
        mm_obj_warn (self, "Couldn't disable u-blox-specific voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    iface_modem_voice_parent->disable_unsolicited_events (
        MM_IFACE_MODEM_VOICE (self),
        (GAsyncReadyCallback)parent_voice_disable_unsolicited_events_ready,
        task);
}

static void
modem_voice_disable_unsolicited_events (MMIfaceModemVoice   *self,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    common_voice_enable_disable_unsolicited_events (MM_BROADBAND_MODEM_UBLOX (self),
                                                    FALSE,
                                                    (GAsyncReadyCallback) voice_disable_unsolicited_events_ready,
                                                    task);
}

/*****************************************************************************/
/* Common setup/cleanup voice unsolicited events */

static void
ucallstat_received (MMPortSerialAt        *port,
                    GMatchInfo            *match_info,
                    MMBroadbandModemUblox *self)
{
    static const MMCallState ublox_call_state[] = {
        [0] = MM_CALL_STATE_ACTIVE,
        [1] = MM_CALL_STATE_HELD,
        [2] = MM_CALL_STATE_DIALING,     /* Dialing  (MOC) */
        [3] = MM_CALL_STATE_RINGING_OUT, /* Alerting (MOC) */
        [4] = MM_CALL_STATE_RINGING_IN,  /* Incoming (MTC) */
        [5] = MM_CALL_STATE_WAITING,     /* Waiting  (MTC) */
        [6] = MM_CALL_STATE_TERMINATED,
        [7] = MM_CALL_STATE_ACTIVE,      /* Treated same way as ACTIVE */
    };

    MMCallInfo call_info = { 0 };
    guint      aux;

    if (!mm_get_uint_from_match_info (match_info, 1, &aux)) {
        mm_obj_warn (self, "couldn't parse call index from +UCALLSTAT");
        return;
    }
    call_info.index = aux;

    if (!mm_get_uint_from_match_info (match_info, 2, &aux) ||
        (aux >= G_N_ELEMENTS (ublox_call_state))) {
        mm_obj_warn (self, "couldn't parse call state from +UCALLSTAT");
        return;
    }
    call_info.state = ublox_call_state[aux];

    /* guess direction for some of the states */
    switch (call_info.state) {
    case MM_CALL_STATE_DIALING:
    case MM_CALL_STATE_RINGING_OUT:
        call_info.direction = MM_CALL_DIRECTION_OUTGOING;
        break;
    case MM_CALL_STATE_RINGING_IN:
    case MM_CALL_STATE_WAITING:
        call_info.direction = MM_CALL_DIRECTION_INCOMING;
        break;
    case MM_CALL_STATE_UNKNOWN:
    case MM_CALL_STATE_ACTIVE:
    case MM_CALL_STATE_HELD:
    case MM_CALL_STATE_TERMINATED:
    default:
        call_info.direction = MM_CALL_DIRECTION_UNKNOWN;
        break;
    }

    mm_iface_modem_voice_report_call (MM_IFACE_MODEM_VOICE (self), &call_info);
}

static void
udtmfd_received (MMPortSerialAt        *port,
                 GMatchInfo            *match_info,
                 MMBroadbandModemUblox *self)
{
    g_autofree gchar *dtmf = NULL;

    dtmf = g_match_info_fetch (match_info, 1);
    mm_obj_dbg (self, "received DTMF: %s", dtmf);
    /* call index unknown */
    mm_iface_modem_voice_received_dtmf (MM_IFACE_MODEM_VOICE (self), 0, dtmf);
}

static void
common_voice_setup_cleanup_unsolicited_events (MMBroadbandModemUblox *self,
                                               gboolean               enable)
{
    MMPortSerialAt *ports[2];
    guint           i;

    if (G_UNLIKELY (!self->priv->ucallstat_regex))
        self->priv->ucallstat_regex = g_regex_new ("\\r\\n\\+UCALLSTAT:\\s*(\\d+),(\\d+)\\r\\n",
                                                   G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    if (G_UNLIKELY (!self->priv->udtmfd_regex))
        self->priv->udtmfd_regex = g_regex_new ("\\r\\n\\+UUDTMFD:\\s*([0-9A-D\\*\\#])\\r\\n",
                                                G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);

    ports[0] = mm_base_modem_peek_port_primary   (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));

    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (!ports[i])
            continue;

        mm_port_serial_at_add_unsolicited_msg_handler (ports[i],
                                                       self->priv->ucallstat_regex,
                                                       enable ? (MMPortSerialAtUnsolicitedMsgFn)ucallstat_received : NULL,
                                                       enable ? self : NULL,
                                                       NULL);

        mm_port_serial_at_add_unsolicited_msg_handler (ports[i],
                                                       self->priv->udtmfd_regex,
                                                       enable ? (MMPortSerialAtUnsolicitedMsgFn)udtmfd_received : NULL,
                                                       enable ? self : NULL,
                                                       NULL);
    }
}

/*****************************************************************************/
/* Cleanup unsolicited events (Voice interface) */

static gboolean
modem_voice_cleanup_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                               GAsyncResult       *res,
                                               GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_voice_cleanup_unsolicited_events_ready (MMIfaceModemVoice *self,
                                               GAsyncResult      *res,
                                               GTask             *task)
{
    GError *error = NULL;

    if (!iface_modem_voice_parent->cleanup_unsolicited_events_finish (self, res, &error)) {
        mm_obj_warn (self, "Couldn't cleanup parent voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_voice_cleanup_unsolicited_events (MMIfaceModemVoice   *self,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* our own cleanup first */
    common_voice_setup_cleanup_unsolicited_events (MM_BROADBAND_MODEM_UBLOX (self), FALSE);

    /* Chain up parent's cleanup */
    iface_modem_voice_parent->cleanup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_voice_cleanup_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Setup unsolicited events (Voice interface) */

static gboolean
modem_voice_setup_unsolicited_events_finish (MMIfaceModemVoice  *self,
                                             GAsyncResult       *res,
                                             GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
parent_voice_setup_unsolicited_events_ready (MMIfaceModemVoice *self,
                                             GAsyncResult      *res,
                                             GTask             *task)
{
    GError  *error = NULL;

    if (!iface_modem_voice_parent->setup_unsolicited_events_finish (self, res, &error)) {
        mm_obj_warn (self, "Couldn't setup parent voice unsolicited events: %s", error->message);
        g_error_free (error);
    }

    /* our own setup next */
    common_voice_setup_cleanup_unsolicited_events (MM_BROADBAND_MODEM_UBLOX (self), TRUE);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_voice_setup_unsolicited_events (MMIfaceModemVoice   *self,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* chain up parent's setup first */
    iface_modem_voice_parent->setup_unsolicited_events (
        self,
        (GAsyncReadyCallback)parent_voice_setup_unsolicited_events_ready,
        task);
}

/*****************************************************************************/
/* Create call (Voice interface) */

static MMBaseCall *
create_call (MMIfaceModemVoice *self,
             MMCallDirection    direction,
             const gchar       *number)
{
    return mm_base_call_new (MM_BASE_MODEM (self),
                             direction,
                             number,
                             TRUE,  /* skip_incoming_timeout */
                             TRUE,  /* supports_dialing_to_ringing */
                             TRUE); /* supports_ringing_to_active */
}

/*****************************************************************************/
/* Check if Voice supported (Voice interface) */

static gboolean
modem_voice_check_support_finish (MMIfaceModemVoice  *self,
                                  GAsyncResult       *res,
                                  GError            **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
udtmfd_test_ready (MMBaseModem  *_self,
                   GAsyncResult *res,
                   GTask        *task)
{
    MMBroadbandModemUblox *self = MM_BROADBAND_MODEM_UBLOX (_self);

    self->priv->udtmfd_support = (!!mm_base_modem_at_command_finish (MM_BASE_MODEM (self), res, NULL) ?
                                  FEATURE_SUPPORTED : FEATURE_UNSUPPORTED);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_voice_check_support_ready (MMIfaceModemVoice *self,
                                  GAsyncResult      *res,
                                  GTask             *task)
{
    GError *error = NULL;

    if (!iface_modem_voice_parent->check_support_finish (self, res, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* voice is supported, check if +UDTMFD is available */
    mm_base_modem_at_command (MM_BASE_MODEM (self),
                              "+UDTMFD=?",
                              3,
                              TRUE,
                              (GAsyncReadyCallback) udtmfd_test_ready,
                              task);
}

static void
modem_voice_check_support (MMIfaceModemVoice   *self,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    /* chain up parent's setup first */
    iface_modem_voice_parent->check_support (
        self,
        (GAsyncReadyCallback)parent_voice_check_support_ready,
        task);
}

/*****************************************************************************/
/* Create Bearer (Modem interface) */

typedef enum {
    CREATE_BEARER_STEP_FIRST,
    CREATE_BEARER_STEP_CHECK_PROFILE,
    CREATE_BEARER_STEP_CHECK_MODE,
    CREATE_BEARER_STEP_CREATE_BEARER,
    CREATE_BEARER_STEP_LAST,
} CreateBearerStep;

typedef struct {
    CreateBearerStep    step;
    MMBearerProperties *properties;
    MMBaseBearer       *bearer;
    gboolean            has_net;
} CreateBearerContext;

static void
create_bearer_context_free (CreateBearerContext *ctx)
{
    g_clear_object (&ctx->bearer);
    g_object_unref (ctx->properties);
    g_slice_free (CreateBearerContext, ctx);
}

static MMBaseBearer *
modem_create_bearer_finish (MMIfaceModem  *self,
                            GAsyncResult  *res,
                            GError       **error)
{
    return MM_BASE_BEARER (g_task_propagate_pointer (G_TASK (res), error));
}

static void create_bearer_step (GTask *task);

static void
broadband_bearer_new_ready (GObject      *source,
                            GAsyncResult *res,
                            GTask        *task)
{
    MMBroadbandModemUblox *self;
    CreateBearerContext   *ctx;
    GError                *error = NULL;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    g_assert (!ctx->bearer);
    ctx->bearer = mm_broadband_bearer_new_finish (res, &error);
    if (!ctx->bearer) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "new generic broadband bearer created at DBus path '%s'", mm_base_bearer_get_path (ctx->bearer));
    ctx->step++;
    create_bearer_step (task);
}

static void
broadband_bearer_ublox_new_ready (GObject      *source,
                                  GAsyncResult *res,
                                  GTask        *task)
{
    MMBroadbandModemUblox *self;
    CreateBearerContext   *ctx;
    GError                *error = NULL;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    g_assert (!ctx->bearer);
    ctx->bearer = mm_broadband_bearer_ublox_new_finish (res, &error);
    if (!ctx->bearer) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "new u-blox broadband bearer created at DBus path '%s'", mm_base_bearer_get_path (ctx->bearer));
    ctx->step++;
    create_bearer_step (task);
}

static void
mode_check_ready (MMBaseModem  *_self,
                  GAsyncResult *res,
                  GTask        *task)
{
    MMBroadbandModemUblox *self = MM_BROADBAND_MODEM_UBLOX (_self);
    const gchar           *response;
    GError                *error = NULL;
    CreateBearerContext   *ctx;

    ctx = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (!response) {
        mm_obj_dbg (self, "couldn't load current networking mode: %s", error->message);
        g_error_free (error);
    } else if (!mm_ublox_parse_ubmconf_response (response, &self->priv->mode, &error)) {
        mm_obj_dbg (self, "couldn't parse current networking mode response '%s': %s", response, error->message);
        g_error_free (error);
    } else {
        g_assert (self->priv->mode != MM_UBLOX_NETWORKING_MODE_UNKNOWN);
        mm_obj_dbg (self, "networking mode loaded: %s", mm_ublox_networking_mode_get_string (self->priv->mode));
    }

    /* If checking networking mode isn't supported, we'll fallback to
     * assume the device is in router mode, which is the mode asking for
     * less connection setup rules from our side (just request DHCP).
     */
    if (self->priv->mode == MM_UBLOX_NETWORKING_MODE_UNKNOWN && ctx->has_net) {
        mm_obj_dbg (self, "fallback to default networking mode: router");
        self->priv->mode = MM_UBLOX_NETWORKING_MODE_ROUTER;
    }

    self->priv->mode_checked = TRUE;

    ctx->step++;
    create_bearer_step (task);
}

static void
profile_check_ready (MMBaseModem  *_self,
                     GAsyncResult *res,
                     GTask        *task)
{
    MMBroadbandModemUblox *self = MM_BROADBAND_MODEM_UBLOX (_self);
    const gchar           *response;
    GError                *error = NULL;
    CreateBearerContext   *ctx;

    ctx = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (!response) {
        mm_obj_dbg (self, "couldn't load current usb profile: %s", error->message);
        g_error_free (error);
    } else if (!mm_ublox_parse_uusbconf_response (response, &self->priv->profile, &error)) {
        mm_obj_dbg (self, "couldn't parse current usb profile response '%s': %s", response, error->message);
        g_error_free (error);
    } else {
        g_assert (self->priv->profile != MM_UBLOX_USB_PROFILE_UNKNOWN);
        mm_obj_dbg (self, "usb profile loaded: %s", mm_ublox_usb_profile_get_string (self->priv->profile));
    }

    /* Assume the operation has been performed, even if it may have failed */
    self->priv->profile_checked = TRUE;

    ctx->step++;
    create_bearer_step (task);
}

static void
create_bearer_step (GTask *task)
{
    MMBroadbandModemUblox *self;
    CreateBearerContext   *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
    case CREATE_BEARER_STEP_FIRST:
        ctx->step++;
        /* fall through */

    case CREATE_BEARER_STEP_CHECK_PROFILE:
        if (!self->priv->profile_checked) {
            mm_obj_dbg (self, "checking current USB profile...");
            mm_base_modem_at_command (
                MM_BASE_MODEM (self),
                "+UUSBCONF?",
                3,
                FALSE,
                (GAsyncReadyCallback) profile_check_ready,
                task);
            return;
        }
        ctx->step++;
        /* fall through */

    case CREATE_BEARER_STEP_CHECK_MODE:
        if (!self->priv->mode_checked) {
            mm_obj_dbg (self, "checking current networking mode...");
            mm_base_modem_at_command (
                MM_BASE_MODEM (self),
                "+UBMCONF?",
                3,
                FALSE,
                (GAsyncReadyCallback) mode_check_ready,
                task);
            return;
        }
        ctx->step++;
        /* fall through */

    case CREATE_BEARER_STEP_CREATE_BEARER:
        /* If we have a net interface, we'll create a u-blox bearer, unless for
         * any reason we have the back-compatible profile selected. */
        if ((self->priv->profile != MM_UBLOX_USB_PROFILE_BACK_COMPATIBLE) && ctx->has_net) {
            /* whenever there is a net port, we should have loaded a valid networking mode */
            g_assert (self->priv->mode != MM_UBLOX_NETWORKING_MODE_UNKNOWN);
            mm_obj_dbg (self, "creating u-blox broadband bearer (%s profile, %s mode)...",
                        mm_ublox_usb_profile_get_string (self->priv->profile),
                        mm_ublox_networking_mode_get_string (self->priv->mode));
            mm_broadband_bearer_ublox_new (
                MM_BROADBAND_MODEM (self),
                self->priv->profile,
                self->priv->mode,
                ctx->properties,
                NULL, /* cancellable */
                (GAsyncReadyCallback) broadband_bearer_ublox_new_ready,
                task);
            return;
        }

        /* If usb profile is back-compatible already, or if there is no NET port
         * available, create default generic bearer */
        mm_obj_dbg (self, "creating generic broadband bearer...");
        mm_broadband_bearer_new (MM_BROADBAND_MODEM (self),
                                 ctx->properties,
                                 NULL, /* cancellable */
                                 (GAsyncReadyCallback) broadband_bearer_new_ready,
                                 task);
        return;

    case CREATE_BEARER_STEP_LAST:
        g_assert (ctx->bearer);
        g_task_return_pointer (task, g_object_ref (ctx->bearer), g_object_unref);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }

    g_assert_not_reached ();
}

static void
modem_create_bearer (MMIfaceModem        *self,
                     MMBearerProperties  *properties,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    CreateBearerContext *ctx;
    GTask               *task;

    ctx = g_slice_new0 (CreateBearerContext);
    ctx->step = CREATE_BEARER_STEP_FIRST;
    ctx->properties = g_object_ref (properties);

    /* Flag whether this modem has exposed a network interface */
    ctx->has_net = !!mm_base_modem_peek_best_data_port (MM_BASE_MODEM (self), MM_PORT_TYPE_NET);

    task = g_task_new (self, NULL, callback, user_data);
    g_task_set_task_data (task, ctx, (GDestroyNotify) create_bearer_context_free);
    create_bearer_step (task);
}

/*****************************************************************************/
/* Create SIM (Modem interface) */

static MMBaseSim *
modem_create_sim_finish (MMIfaceModem  *self,
                         GAsyncResult  *res,
                         GError       **error)
{
    return mm_sim_ublox_new_finish (res, error);
}

static void
modem_create_sim (MMIfaceModem        *self,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
    mm_sim_ublox_new (MM_BASE_MODEM (self),
                      NULL, /* cancellable */
                      callback,
                      user_data);
}

/*****************************************************************************/
/* Setup ports (Broadband modem class) */

static void
setup_ports (MMBroadbandModem *_self)
{
    MMBroadbandModemUblox *self = MM_BROADBAND_MODEM_UBLOX (_self);
    MMPortSerialAt        *ports[2];
    guint                  i;

    /* Call parent's setup ports first always */
    MM_BROADBAND_MODEM_CLASS (mm_broadband_modem_ublox_parent_class)->setup_ports (_self);

    ports[0] = mm_base_modem_peek_port_primary   (MM_BASE_MODEM (self));
    ports[1] = mm_base_modem_peek_port_secondary (MM_BASE_MODEM (self));

    /* Configure AT ports */
    for (i = 0; i < G_N_ELEMENTS (ports); i++) {
        if (!ports[i])
            continue;

        g_object_set (ports[i],
                      MM_PORT_SERIAL_SEND_DELAY, (guint64) 0,
                      NULL);

        mm_port_serial_at_add_unsolicited_msg_handler (
            ports[i],
            self->priv->pbready_regex,
            NULL, NULL, NULL);
    }
}




typedef struct {
    GList *profiles;
} ListProfilesContext;

static void
list_profiles_context_free (ListProfilesContext *ctx)
{
    mm_3gpp_profile_list_free (ctx->profiles);
    g_slice_free (ListProfilesContext, ctx);
}

static gboolean
modem_3gpp_profile_manager_list_profiles_finish (MMIfaceModem3gppProfileManager  *self,
                                                 GAsyncResult                    *res,
                                                 GList                          **out_profiles,
                                                 GError                         **error)
{
    ListProfilesContext *ctx;

    if (!g_task_propagate_boolean (G_TASK (res), error))
        return FALSE;

    ctx = g_task_get_task_data (G_TASK (res));
    if (out_profiles)
        *out_profiles = g_steal_pointer (&ctx->profiles);
    return TRUE;
}

static gint
cmp_prefer_default_EPS_cid (gconstpointer  a, gconstpointer  b) {
    const MM3gppProfile *p2 = (const MM3gppProfile *) b;
    (void) a;

    return mm_3gpp_profile_get_profile_id (p2) == 4 ? 1 : -1;
}


static void
profile_manager_parent_list_profiles_ready (MMIfaceModem3gppProfileManager *self,
                                            GAsyncResult                   *res,
                                            GTask                          *task)
{
    ListProfilesContext *ctx;
    GError              *error = NULL;

    ctx = g_slice_new0 (ListProfilesContext);
    g_task_set_task_data (task, ctx, (GDestroyNotify) list_profiles_context_free);

    if (!iface_modem_3gpp_profile_manager_parent->list_profiles_finish (self, res, &ctx->profiles, &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    if (!ctx->profiles) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }

    ctx->profiles = g_list_sort(ctx->profiles, cmp_prefer_default_EPS_cid);

    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
modem_3gpp_profile_manager_list_profiles (MMIfaceModem3gppProfileManager *self,
                                          GAsyncReadyCallback             callback,
                                          gpointer                        user_data)
{
    GTask *task;

    task = g_task_new (self, NULL, callback, user_data);

    iface_modem_3gpp_profile_manager_parent->list_profiles (
        self,
        (GAsyncReadyCallback)profile_manager_parent_list_profiles_ready,
        task);
}

/*****************************************************************************/
/* Common initial EPS bearer info loading for both:
 *   - runtime status
 *   - configuration settings
 */

typedef struct {
    MMBearerProperties       *properties;
} CommonLoadInitialEpsContext;

static void
common_load_initial_eps_context_free (CommonLoadInitialEpsContext *ctx)
{
    g_clear_object (&ctx->properties);
    g_slice_free (CommonLoadInitialEpsContext, ctx);
}

static MMBearerProperties *
common_load_initial_eps_bearer_finish (MMIfaceModem3gpp  *self,
                                       GAsyncResult      *res,
                                       GError           **error)
{
    return MM_BEARER_PROPERTIES (g_task_propagate_pointer (G_TASK (res), error));
}


static void
common_load_initial_eps_cgdcont_ready (MMBaseModem  *_self,
                                       GAsyncResult *res,
                                       GTask        *task)
{
    MMBroadbandModemUblox *self = MM_BROADBAND_MODEM_UBLOX(_self);
    const gchar                 *response;
    CommonLoadInitialEpsContext *ctx;
    g_autoptr(GError)            error = NULL;

    ctx = (CommonLoadInitialEpsContext *) g_task_get_task_data (task);

    /* errors aren't fatal */
    response = mm_base_modem_at_command_finish (_self, res, &error);
    if (!response)
        mm_obj_dbg (self, "couldn't load context %d status: %s", 4, error->message);
    else {
        GList *context_list;

        context_list = mm_3gpp_parse_cgdcont_read_response (response, &error);
        if (!context_list)
            mm_obj_dbg (self, "couldn't parse CGDCONT response: %s", error->message);
        else {
            GList *l;

            for (l = context_list; l; l = g_list_next (l)) {
                MM3gppPdpContext *pdp = l->data;

                if (pdp->cid == 4) {
                    mm_bearer_properties_set_ip_type (ctx->properties, pdp->pdp_type);
                    mm_bearer_properties_set_apn (ctx->properties, pdp->apn ? pdp->apn : "");
                    mm_bearer_properties_set_profile_id(ctx->properties, 4);
                    break;
                }
            }
            if (!l)
                mm_obj_dbg (self, "no status reported for context 4");
            mm_3gpp_pdp_context_list_free (context_list);
        }
    }

    /* Go to next step */
    g_task_return_pointer (task, g_steal_pointer (&ctx->properties), g_object_unref);
	g_object_unref (task);
}


static MMBearerProperties *
modem_3gpp_load_initial_eps_bearer_finish (MMIfaceModem3gpp  *self,
                                           GAsyncResult      *res,
                                           GError           **error)
{
    return common_load_initial_eps_bearer_finish (self, res, error);
}

static void
modem_3gpp_load_initial_eps_bearer (MMIfaceModem3gpp    *self,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
	GTask                       *task;
	CommonLoadInitialEpsContext *ctx;

	task = g_task_new (self, NULL, callback, user_data);

	/* Setup context */
	ctx = g_slice_new0 (CommonLoadInitialEpsContext);
	ctx->properties = mm_bearer_properties_new ();
	g_task_set_task_data (task, ctx, (GDestroyNotify) common_load_initial_eps_context_free);

	mm_base_modem_at_command (
	                MM_BASE_MODEM (self),
	                "+CGDCONT?",
	                20,
	                FALSE,
	                (GAsyncReadyCallback)common_load_initial_eps_cgdcont_ready,
	                task);
}

/*****************************************************************************/
/* Set initial EPS bearer settings */

typedef enum {
    SET_INITIAL_EPS_STEP_FIRST = 0,
    SET_INITIAL_EPS_STEP_CHECK_MODE,
    SET_INITIAL_EPS_STEP_RF_OFF,
    SET_INITIAL_EPS_STEP_APN,
    SET_INITIAL_EPS_STEP_RF_ON,
    SET_INITIAL_EPS_STEP_LAST,
} SetInitialEpsStep;

typedef struct {
    MMBearerProperties *properties;
    SetInitialEpsStep   step;
    guint               initial_cfun_mode;
    gboolean            supported;
    GError             *saved_error;
} SetInitialEpsContext;

static void
set_initial_eps_context_free (SetInitialEpsContext *ctx)
{
    g_assert (!ctx->saved_error);
    g_object_unref (ctx->properties);
    g_slice_free (SetInitialEpsContext, ctx);
}

static void set_initial_eps_step (GTask *task);

static void
set_initial_eps_rf_on_ready (MMBaseModem  *self,
                             GAsyncResult *res,
                             GTask        *task)
{
    g_autoptr(GError)     error = NULL;
    SetInitialEpsContext *ctx;

    ctx = (SetInitialEpsContext *) g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        mm_obj_warn (self, "couldn't set RF back on: %s", error->message);
        if (!ctx->saved_error)
            ctx->saved_error = g_steal_pointer (&error);
    }

    /* Go to next step */
    ctx->step++;
    set_initial_eps_step (task);
}


static void
set_initial_eps_ucgdflt_ready (MMBaseModem  *self,
                               GAsyncResult *res,
                               GTask        *task)
{
    SetInitialEpsContext      *ctx;

    ctx = (SetInitialEpsContext *) g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (self, res, &ctx->saved_error)) {
        mm_obj_warn (self, "couldn't set initial default bearer settings: %s",
                     ctx->saved_error->message);
        /* Fallback to recover RF before returning the error */
        ctx->step = SET_INITIAL_EPS_STEP_RF_ON;
    } else {
        /* Go to next step */
        ctx->step++;
    }
    set_initial_eps_step (task);
}

static void
set_initial_eps_rf_off_ready (MMBaseModem  *self,
                              GAsyncResult *res,
                              GTask        *task)
{
    GError               *error = NULL;
    SetInitialEpsContext *ctx;

    ctx = (SetInitialEpsContext *) g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        mm_obj_warn (self, "couldn't set RF off: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go to next step */
    ctx->step++;
    set_initial_eps_step (task);
}

static void
set_initial_eps_cfun_mode_load_ready (MMBaseModem  *self,
                                      GAsyncResult *res,
                                      GTask        *task)
{
    GError                *error = NULL;
    const gchar           *response;
    SetInitialEpsContext  *ctx;
    guint                  mode;

    ctx = (SetInitialEpsContext *) g_task_get_task_data (task);
    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response || !mm_3gpp_parse_cfun_query_response (response, &mode, &error)) {
        mm_obj_warn (self, "couldn't load initial functionality mode: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "current functionality mode: %u", mode);
    if (mode != 1 && mode != 4) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                 "cannot setup the default LTE bearer settings: "
                                 "the SIM must be powered");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->initial_cfun_mode = mode;
    ctx->step++;
    set_initial_eps_step (task);
}

static void check_ip_type_supported (MMBearerIpFamily data, SetInitialEpsContext * ctx)
{
	MMBearerIpFamily requested_ip_family = mm_bearer_properties_get_ip_type(ctx->properties);

	if ((requested_ip_family == MM_BEARER_IP_FAMILY_NONE || requested_ip_family == MM_BEARER_IP_FAMILY_ANY) && data == MM_BEARER_IP_FAMILY_IPV4) {
		mm_obj_dbg (NULL, "ucgdflt chooses type ipv4 because requested type is unspecified");
		ctx->supported = TRUE;
	} else if (data == requested_ip_family) {
		mm_obj_dbg (NULL, "ucgdflt type %s supported!",
                    mm_3gpp_get_pdp_type_from_ip_family(mm_bearer_properties_get_ip_type(ctx->properties)));
		ctx->supported = TRUE;
	}
}

static void
set_initial_eps_ucgdflt_test_ready (MMBaseModem * self, GAsyncResult * res, GTask * task)
{
	SetInitialEpsContext      *ctx;
    const gchar * response;

    ctx  = g_task_get_task_data (task);

    response = mm_base_modem_at_command_finish (self, res, &ctx->saved_error);

    if (!response) {
        mm_obj_dbg (self, "ucgdflt command is not supported!: %s", ctx->saved_error->message);
        ctx->step = SET_INITIAL_EPS_STEP_LAST;
	} else {
		GList * types = NULL;
		mm_obj_dbg (self, "ucgdflt supported!");
		types = mm_ublox_parse_ucgdflt_test_response(response, &ctx->saved_error);
		g_list_foreach(types, (GFunc) check_ip_type_supported, ctx);
		g_list_free(types);

		if (ctx->supported) {
            ctx->step++;
		} else {
			mm_obj_dbg (self, "ucgdflt command is not supported for bearer type '%s'",
                        mm_3gpp_get_pdp_type_from_ip_family(mm_bearer_properties_get_ip_type(ctx->properties)));
			ctx->saved_error = g_error_new(MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED, "ip type not supported");
            ctx->step = SET_INITIAL_EPS_STEP_LAST;
		}
    }
    set_initial_eps_step (task);
}

static void
set_initial_eps_step (GTask *task)
{
    MMBaseModem * self;
    SetInitialEpsContext      *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
    case SET_INITIAL_EPS_STEP_FIRST:
        mm_base_modem_at_command (
                        MM_BASE_MODEM (self),
                        "+UCGDFLT=?",
                        5,
                        TRUE,
                        (GAsyncReadyCallback)set_initial_eps_ucgdflt_test_ready,
                        task);
        return;

    case SET_INITIAL_EPS_STEP_CHECK_MODE:
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "+CFUN?",
            5,
            FALSE,
            (GAsyncReadyCallback)set_initial_eps_cfun_mode_load_ready,
            task);
        return;

    case SET_INITIAL_EPS_STEP_RF_OFF:
        if (ctx->initial_cfun_mode != 4) {
            mm_base_modem_at_command (
                MM_BASE_MODEM (self),
                "+CFUN=4",
                5,
                FALSE,
                (GAsyncReadyCallback)set_initial_eps_rf_off_ready,
                task);
            return;
        }
        ctx->step++;
        /* fall through */

    case SET_INITIAL_EPS_STEP_APN: {
        const gchar        *apn;
        g_autofree gchar   *quoted_apn = NULL;
        const gchar        *user;
        g_autofree gchar   *quoted_user = NULL;
        const gchar        *password;
        g_autofree gchar   *quoted_password = NULL;
        g_autofree gchar   *apn_cmd = NULL;
        const gchar        *ip_family_str;
        g_autofree gchar   *quoted_ip_family = NULL;
        MMBearerIpFamily    ip_family;

        ip_family = mm_bearer_properties_get_ip_type (ctx->properties);
        if (ip_family == MM_BEARER_IP_FAMILY_NONE || ip_family == MM_BEARER_IP_FAMILY_ANY)
            ip_family = MM_BEARER_IP_FAMILY_IPV4;

        ip_family_str = mm_3gpp_get_pdp_type_from_ip_family (ip_family);

        apn = mm_bearer_properties_get_apn (ctx->properties);
        user = mm_bearer_properties_get_user(ctx->properties);
        password = mm_bearer_properties_get_password(ctx->properties);

        mm_obj_dbg (self, "context with APN '%s' and PDP type '%s', user '%s' and password '%s'",
                    apn, ip_family_str, user, password);
        quoted_apn = mm_port_serial_at_quote_string (apn);
        quoted_user = mm_port_serial_at_quote_string(user);
        quoted_password = mm_port_serial_at_quote_string(password);
        quoted_ip_family = mm_port_serial_at_quote_string(ip_family_str);

        apn_cmd = g_strdup_printf ("+UCGDFLT=1,%s,%s,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,0,0,0,%s,%s",
                                   quoted_ip_family, quoted_apn, quoted_user, quoted_password);
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            apn_cmd,
            20,
            FALSE,
            (GAsyncReadyCallback)set_initial_eps_ucgdflt_ready,
            task);
        return;
    }

    case SET_INITIAL_EPS_STEP_RF_ON:
        if (ctx->initial_cfun_mode == 1) {
            mm_base_modem_at_command (
                MM_BASE_MODEM (self),
                "+CFUN=1",
                5,
                FALSE,
                (GAsyncReadyCallback)set_initial_eps_rf_on_ready,
                task);
            return;
        }
        ctx->step++;
        /* fall through */

    case SET_INITIAL_EPS_STEP_LAST:
        if (ctx->saved_error)
            g_task_return_error (task, g_steal_pointer (&ctx->saved_error));
        else
            g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

static void
modem_3gpp_set_initial_eps_bearer_settings (MMIfaceModem3gpp    *self,
                                            MMBearerProperties  *properties,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data)
{
    GTask                *task;
    SetInitialEpsContext *ctx;

    task = g_task_new (self, NULL, callback, user_data);

    /* Setup context */
    ctx = g_slice_new0 (SetInitialEpsContext);
    ctx->properties = g_object_ref (properties);
    ctx->step = SET_INITIAL_EPS_STEP_FIRST;
    g_task_set_task_data (task, ctx, (GDestroyNotify) set_initial_eps_context_free);

    set_initial_eps_step (task);
}

static gboolean
modem_3gpp_set_initial_eps_bearer_settings_finish(MMIfaceModem3gpp  *_self,
                                                  GAsyncResult      *res,
                                                  GError           **error)
{
    mm_obj_dbg(_self, "Call set_initial_eps_bearer_settings_finish");
    return g_task_propagate_boolean(G_TASK(res), error);
}


static MMBearerProperties *
load_initial_eps_bearer_finish (MMIfaceModem3gpp *self,
                                GAsyncResult     *res,
                                GError           **error)
{
    return g_task_propagate_pointer (G_TASK(res), error);
}


static void
load_initial_eps_bearer_ucgdflt_ready(MMBaseModem * self, GAsyncResult * res, GTask * task)
{
    GError * error = NULL;
    const gchar * response;
    MMBearerProperties * props;

    response = mm_base_modem_at_command_finish (self, res, &error);
    props = (MMBearerProperties *) g_task_get_task_data (task);

    if (!response) {
        mm_obj_dbg (self, "couldn't load default bearer: %s", error->message);
        g_error_free (error);
        g_task_return_pointer(task, NULL, g_object_unref);
	} else {
        gchar * out_apn;
        MMBearerIpFamily out_ip_type;
        GError * inner_error = NULL;

        if (mm_ublox_parse_ucgdflt_response(response, &out_apn, &out_ip_type, &inner_error)) {
            mm_bearer_properties_set_apn(props, out_apn);
            mm_bearer_properties_set_ip_type(props, out_ip_type);
            g_task_return_pointer(task, props, g_object_unref);
        } else {
            mm_obj_dbg(self, "Failed to get apn");
            g_task_return_error(task, inner_error);
        }
    }
    g_object_unref(task);
}

static void
load_initial_eps_bearer_ucgdflt_test_ready(MMBaseModem * self, GAsyncResult * res, GTask * task)
{
    GError * error = NULL;
    const gchar * response;

    response = mm_base_modem_at_command_finish (self, res, &error);

    if (!response) {
        mm_obj_dbg (self, "ucgdflt command is not supported!: %s", error->message);
        g_error_free (error);
        g_task_return_pointer(task, NULL, g_object_unref);
	} else {
		mm_obj_dbg (self, "ucgdflt supported!");

        mm_base_modem_at_command(self, "+UCGDFLT?", 5, FALSE, (GAsyncReadyCallback) load_initial_eps_bearer_ucgdflt_ready, task);
    }
}


static void
load_initial_eps_bearer(MMIfaceModem3gpp    *_self,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
    GTask *task;
    MMBearerProperties * props;

    mm_obj_dbg(_self, "Call load_initial_eps_bearer_settings");

    props = mm_bearer_properties_new();
    task = g_task_new(_self, NULL, callback, user_data);
    g_task_set_task_data(task, props, (GDestroyNotify) g_clear_object);

    mm_base_modem_at_command (
                MM_BASE_MODEM (_self),
                "+UCGDFLT=?",
                5,
                TRUE,
                (GAsyncReadyCallback)load_initial_eps_bearer_ucgdflt_test_ready,
                task);

    return;
}



/*****************************************************************************/

MMBroadbandModemUblox *
mm_broadband_modem_ublox_new (const gchar  *device,
                              const gchar **drivers,
                              const gchar  *plugin,
                              guint16       vendor_id,
                              guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_UBLOX,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* Generic bearer (TTY) and u-blox bearer (NET) supported */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, TRUE,
                         NULL);
}

static void
iface_modem_3gpp_profile_manager_init (MMIfaceModem3gppProfileManager *iface)
{
    iface_modem_3gpp_profile_manager_parent = g_type_interface_peek_parent (iface);

    iface->list_profiles = modem_3gpp_profile_manager_list_profiles;
    iface->list_profiles_finish = modem_3gpp_profile_manager_list_profiles_finish;
}

static void
mm_broadband_modem_ublox_init (MMBroadbandModemUblox *self)
{
    /* Initialize private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_BROADBAND_MODEM_UBLOX,
                                              MMBroadbandModemUbloxPrivate);
    self->priv->profile = MM_UBLOX_USB_PROFILE_UNKNOWN;
    self->priv->mode = MM_UBLOX_NETWORKING_MODE_UNKNOWN;
    self->priv->any_allowed = MM_MODEM_MODE_NONE;
    self->priv->support_config.loaded   = FALSE;
    self->priv->support_config.method   = SETTINGS_UPDATE_METHOD_UNKNOWN;
    self->priv->support_config.uact     = FEATURE_SUPPORT_UNKNOWN;
    self->priv->support_config.ubandsel = FEATURE_SUPPORT_UNKNOWN;
    self->priv->udtmfd_support = FEATURE_SUPPORT_UNKNOWN;
    self->priv->pbready_regex = g_regex_new ("\\r\\n\\+PBREADY\\r\\n",
                                             G_REGEX_RAW | G_REGEX_OPTIMIZE, 0, NULL);
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface->create_sim = modem_create_sim;
    iface->create_sim_finish = modem_create_sim_finish;
    iface->create_bearer        = modem_create_bearer;
    iface->create_bearer_finish = modem_create_bearer_finish;
    iface->load_unlock_retries        = load_unlock_retries;
    iface->load_unlock_retries_finish = load_unlock_retries_finish;
    iface->load_power_state        = load_power_state;
    iface->load_power_state_finish = load_power_state_finish;
    iface->modem_power_up        = modem_power_up;
    iface->modem_power_up_finish = common_modem_power_operation_finish;
    iface->modem_power_down        = modem_power_down;
    iface->modem_power_down_finish = common_modem_power_operation_finish;
    iface->modem_power_off        = modem_power_off;
    iface->modem_power_off_finish = common_modem_power_operation_finish;
    iface->reset        = modem_reset;
    iface->reset_finish = common_modem_power_operation_finish;
    iface->load_supported_modes        = load_supported_modes;
    iface->load_supported_modes_finish = load_supported_modes_finish;
    iface->load_current_modes        = load_current_modes;
    iface->load_current_modes_finish = load_current_modes_finish;
    iface->set_current_modes        = set_current_modes;
    iface->set_current_modes_finish = common_set_current_modes_bands_finish;
    iface->load_supported_bands        = load_supported_bands;
    iface->load_supported_bands_finish = load_supported_bands_finish;
    iface->load_current_bands        = load_current_bands;
    iface->load_current_bands_finish = load_current_bands_finish;
    iface->set_current_bands        = set_current_bands;
    iface->set_current_bands_finish = common_set_current_modes_bands_finish;
}

static void
iface_modem_3gpp_init(MMIfaceModem3gpp * iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent(iface);

    iface->set_initial_eps_bearer_settings = modem_3gpp_set_initial_eps_bearer_settings;
    iface->set_initial_eps_bearer_settings_finish = modem_3gpp_set_initial_eps_bearer_settings_finish;
    iface->load_initial_eps_bearer_settings = load_initial_eps_bearer;
    iface->load_initial_eps_bearer_settings_finish = load_initial_eps_bearer_finish;
    iface->load_initial_eps_bearer = modem_3gpp_load_initial_eps_bearer;
    iface->load_initial_eps_bearer_finish = modem_3gpp_load_initial_eps_bearer_finish;
}

static void
iface_modem_voice_init (MMIfaceModemVoice *iface)
{
    iface_modem_voice_parent = g_type_interface_peek_parent (iface);

    iface->check_support = modem_voice_check_support;
    iface->check_support_finish = modem_voice_check_support_finish;
    iface->setup_unsolicited_events = modem_voice_setup_unsolicited_events;
    iface->setup_unsolicited_events_finish = modem_voice_setup_unsolicited_events_finish;
    iface->cleanup_unsolicited_events = modem_voice_cleanup_unsolicited_events;
    iface->cleanup_unsolicited_events_finish = modem_voice_cleanup_unsolicited_events_finish;
    iface->enable_unsolicited_events = modem_voice_enable_unsolicited_events;
    iface->enable_unsolicited_events_finish = modem_voice_enable_unsolicited_events_finish;
    iface->disable_unsolicited_events = modem_voice_disable_unsolicited_events;
    iface->disable_unsolicited_events_finish = modem_voice_disable_unsolicited_events_finish;

    iface->create_call = create_call;
}

static void
finalize (GObject *object)
{
    MMBroadbandModemUblox *self = MM_BROADBAND_MODEM_UBLOX (object);

    g_regex_unref (self->priv->pbready_regex);

    if (self->priv->ucallstat_regex)
        g_regex_unref (self->priv->ucallstat_regex);
    if (self->priv->udtmfd_regex)
        g_regex_unref (self->priv->udtmfd_regex);

    G_OBJECT_CLASS (mm_broadband_modem_ublox_parent_class)->finalize (object);
}

static void
mm_broadband_modem_ublox_class_init (MMBroadbandModemUbloxClass *klass)
{
    GObjectClass          *object_class = G_OBJECT_CLASS (klass);
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (MMBroadbandModemUbloxPrivate));

    object_class->finalize = finalize;

    broadband_modem_class->setup_ports = setup_ports;
}
