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
 * Copyright (C) 2008 - 2009 Novell, Inc.
 * Copyright (C) 2009 - 2012 Red Hat, Inc.
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 */

#ifndef MM_BROADBAND_MODEM_MOTOROLA_H
#define MM_BROADBAND_MODEM_MOTOROLA_H

#include "mm-broadband-modem.h"

#define MM_TYPE_BROADBAND_MODEM_MOTOROLA            (mm_broadband_modem_motorola_get_type ())
#define MM_BROADBAND_MODEM_MOTOROLA(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), MM_TYPE_BROADBAND_MODEM_MOTOROLA, MMBroadbandModemMotorola))
#define MM_BROADBAND_MODEM_MOTOROLA_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  MM_TYPE_BROADBAND_MODEM_MOTOROLA, MMBroadbandModemMotorolaClass))
#define MM_IS_BROADBAND_MODEM_MOTOROLA(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), MM_TYPE_BROADBAND_MODEM_MOTOROLA))
#define MM_IS_BROADBAND_MODEM_MOTOROLA_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  MM_TYPE_BROADBAND_MODEM_MOTOROLA))
#define MM_BROADBAND_MODEM_MOTOROLA_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  MM_TYPE_BROADBAND_MODEM_MOTOROLA, MMBroadbandModemMotorolaClass))

typedef struct _MMBroadbandModemMotorola MMBroadbandModemMotorola;
typedef struct _MMBroadbandModemMotorolaClass MMBroadbandModemMotorolaClass;

struct _MMBroadbandModemMotorola {
    MMBroadbandModem parent;
};

struct _MMBroadbandModemMotorolaClass{
    MMBroadbandModemClass parent;
};

GType mm_broadband_modem_motorola_get_type (void);

MMBroadbandModemMotorola *mm_broadband_modem_motorola_new (const gchar *device,
                                                           const gchar **drivers,
                                                           const gchar *plugin,
                                                           guint16 vendor_id,
                                                           guint16 product_id);

#endif /* MM_BROADBAND_MODEM_MOTOROLA_H */
