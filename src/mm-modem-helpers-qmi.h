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
 * Copyright (C) 2012 Google, Inc.
 */

#ifndef MM_MODEM_HELPERS_QMI_H
#define MM_MODEM_HELPERS_QMI_H

#include <config.h>

#include <ModemManager.h>
#include <libqmi-glib.h>

/*****************************************************************************/
/* QMI/DMS to MM translations */

MMModemCapability mm_modem_capability_from_qmi_radio_interface (QmiDmsRadioInterface network);

MMModemLock mm_modem_lock_from_qmi_uim_pin_status (QmiDmsUimPinStatus status,
                                                       gboolean pin1);

QmiDmsUimFacility mm_3gpp_facility_to_qmi_uim_facility (MMModem3gppFacility mm);

GArray *mm_modem_bands_from_qmi_band_capabilities (QmiDmsBandCapability qmi_bands,
                                                   QmiDmsLteBandCapability qmi_lte_bands);

/*****************************************************************************/
/* QMI/NAS to MM translations */

MMModemAccessTechnology mm_modem_access_technology_from_qmi_radio_interface (QmiNasRadioInterface interface);
MMModemAccessTechnology mm_modem_access_technologies_from_qmi_radio_interface_array (GArray *radio_interfaces);

MMModemAccessTechnology mm_modem_access_technology_from_qmi_data_capability (QmiNasDataCapability cap);
MMModemAccessTechnology mm_modem_access_technologies_from_qmi_data_capability_array (GArray *data_capabilities);

MMModemMode mm_modem_mode_from_qmi_radio_technology_preference (QmiNasRadioTechnologyPreference qmi);
QmiNasRadioTechnologyPreference mm_modem_mode_to_qmi_radio_technology_preference (MMModemMode mode,
                                                                                  gboolean is_cdma);

MMModemMode mm_modem_mode_from_qmi_rat_mode_preference (QmiNasRatModePreference qmi);
QmiNasRatModePreference mm_modem_mode_to_qmi_rat_mode_preference (MMModemMode mode,
                                                                  gboolean is_cdma,
                                                                  gboolean is_3gpp);

MMModemCapability mm_modem_capability_from_qmi_rat_mode_preference (QmiNasRatModePreference qmi);

MMModemCapability mm_modem_capability_from_qmi_radio_technology_preference (QmiNasRadioTechnologyPreference qmi);

MMModemCapability mm_modem_capability_from_qmi_band_preference (QmiNasBandPreference qmi);

MMModemMode mm_modem_mode_from_qmi_gsm_wcdma_acquisition_order_preference (QmiNasGsmWcdmaAcquisitionOrderPreference qmi);
QmiNasGsmWcdmaAcquisitionOrderPreference mm_modem_mode_to_qmi_gsm_wcdma_acquisition_order_preference (MMModemMode mode);

GArray *mm_modem_bands_from_qmi_rf_band_information_array (GArray *info_array);

MMModem3gppRegistrationState mm_modem_3gpp_registration_state_from_qmi_registration_state (QmiNasAttachState attach_state,
                                                                                           QmiNasRegistrationState registration_state,
                                                                                           gboolean roaming);

MMModemCdmaRegistrationState mm_modem_cdma_registration_state_from_qmi_registration_state (QmiNasRegistrationState registration_state);

/*****************************************************************************/
/* QMI/WMS to MM translations */

QmiWmsStorageType mm_sms_storage_to_qmi_storage_type (MMSmsStorage storage);
MMSmsStorage mm_sms_storage_from_qmi_storage_type (QmiWmsStorageType qmi_storage);

MMSmsState mm_sms_state_from_qmi_message_tag (QmiWmsMessageTagType tag);

/*****************************************************************************/
/* QMI/WDS to MM translations */

QmiWdsAuthentication mm_bearer_allowed_auth_to_qmi_authentication (MMBearerAllowedAuth auth);

#endif  /* MM_MODEM_HELPERS_QMI_H */
