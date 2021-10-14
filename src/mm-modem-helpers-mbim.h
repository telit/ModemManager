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
 * Copyright (C) 2013 Aleksander Morgado <aleksander@gnu.org>
 */

#ifndef MM_MODEM_HELPERS_MBIM_H
#define MM_MODEM_HELPERS_MBIM_H

#include <config.h>

#include <ModemManager.h>
#include <libmbim-glib.h>

/*****************************************************************************/
/* MBIM/BasicConnect to MM translations */

MMModemCapability mm_modem_capability_from_mbim_device_caps (MbimCellularClass  caps_cellular_class,
                                                             MbimDataClass      caps_data_class,
                                                             const gchar       *caps_custom_data_class);

MMModemLock mm_modem_lock_from_mbim_pin_type (MbimPinType pin_type);

MMModem3gppRegistrationState mm_modem_3gpp_registration_state_from_mbim_register_state (MbimRegisterState state);

MMModemAccessTechnology mm_modem_access_technology_from_mbim_data_class (MbimDataClass data_class);

MMModem3gppNetworkAvailability mm_modem_3gpp_network_availability_from_mbim_provider_state (MbimProviderState state);

GList *mm_3gpp_network_info_list_from_mbim_providers (const MbimProvider *const *providers, guint n_providers);

MbimPinType mbim_pin_type_from_mm_modem_3gpp_facility (MMModem3gppFacility facility);

GError *mm_mobile_equipment_error_from_mbim_nw_error (MbimNwError nw_error,
                                                      gpointer    log_object);

MMBearerAllowedAuth mm_bearer_allowed_auth_from_mbim_auth_protocol (MbimAuthProtocol      auth_protocol);
MbimAuthProtocol    mm_bearer_allowed_auth_to_mbim_auth_protocol   (MMBearerAllowedAuth   bearer_auth,
                                                                    gpointer              log_object,
                                                                    GError              **error);
MMBearerIpFamily    mm_bearer_ip_family_from_mbim_context_ip_type  (MbimContextIpType     ip_type);
MbimContextIpType   mm_bearer_ip_family_to_mbim_context_ip_type    (MMBearerIpFamily      ip_family,
                                                                    GError              **error);
MMBearerApnType     mm_bearer_apn_type_from_mbim_context_type      (MbimContextType       context_type);
MbimContextType     mm_bearer_apn_type_to_mbim_context_type        (MMBearerApnType       apn_type,
                                                                    gpointer              log_object,
                                                                    GError              **error);

/*****************************************************************************/
/* MBIM/SMS to MM translations */

MMSmsState mm_sms_state_from_mbim_message_status (MbimSmsStatus status);

guint8 mm_get_version (MbimDevice *device);

#define MBIM_V1 (1) /* Decimal value of Mbim Version 1 in little-endian constant 0x0100 0100*/
#define MBIM_V2 (2) /* Decimal value of Mbim Version 2 in little-endian constant 0x0200 0100 */
#define MBIM_V3 (3) /* Decimal value of Mbim Version 3 in little-endian constant 0x0300 0100 */

#endif  /* MM_MODEM_HELPERS_MBIM_H */
