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
 * Copyright (C) 2021-2022 Intel Corporation
 */
#include "config.h"

#include <locale.h>
#include <glib.h>
#include <gio/gio.h>

#include <libmm-glib.h>

#define SLP_PORT 7275

#define SLP_ADDRESS "AGPS_SUPL_SLP_ADDRESS"
#define TLS_CERT_NAME "AGPS_SUPL_TLS_CERT_NAME"
#define TLS_CERT "AGPS_SUPL_TLS_CERT"

#define MAX_SUPPORTED_CERTS 10

#define CERT_SIZE_MAX 8192

#define PROGRAM_NAME    "mmlocation"
#define PROGRAM_VERSION PACKAGE_VERSION

#define PROPERTY_CERT_NAME "cert-name"
#define PROPERTY_CERT_DATA "cert-data"

/* Context */
static gchar    *conf_file;
static gchar    *opc;
static gboolean  verbose_flag;
static gboolean  version_flag;

static GOptionEntry main_entries[] = {
    { "conf_file", 'p', 0, G_OPTION_ARG_STRING, &conf_file,
      "Path to AGPS Configuration file",
      "[PATH]"
    },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose_flag,
      "Run action with verbose logs",
      NULL
    },
    { "version", 'V', 0, G_OPTION_ARG_NONE, &version_flag,
      "Print version",
      NULL
    },
    { "operator_code", 'c', 0, G_OPTION_ARG_STRING, &opc,
      "3GPP Operator Code ( MCC & MNC)",
      "[MCC-MNC]"
    },
    { NULL }
};

typedef struct {
    gchar *cert_name;
    gchar *cert;
} SuplCertificateData;

typedef struct {
    gchar *group_name;
    gchar *slp_address;
    GList *cert_data;    /* SuplCertificateData */
} SuplServerConfig;

typedef struct {
    GList *supl_server_config;    /*SuplServerConfig*/
} AGPSConfig;

typedef struct {
    AGPSConfig agps_config;
} LocationContext;

typedef struct {
    MMManager       *manager;
    MMObject        *object;
    MMModemLocation *modem_location;
} MMContext;

static MMContext *ctx;

static SuplServerConfig *
get_supl_server_config (LocationContext *gnss_context)
{
    GList            *supl_list = NULL;
    SuplServerConfig *supl_config = NULL;
    SuplServerConfig *default_supl_config = NULL;

    for (supl_list = gnss_context->agps_config.supl_server_config; supl_list; supl_list = g_list_next (supl_list)) {
        /* SUPL SLP & Certificates are fetched based on the
        Operator code, if none matches from the configuration
        file, then the default configuration is used & pushed */
        supl_config = (SuplServerConfig *)supl_list->data;
        if (g_str_equal (supl_config->group_name, "Default"))
            default_supl_config = supl_config;

        if (g_str_equal (supl_config->group_name, opc))
            return supl_config;
    }

    g_print ("Default SUPL config returned\n");
    return default_supl_config;
}

static void
send_supl_server (LocationContext *gnss_context)
{
    g_autofree gchar *supl = NULL;
    SuplServerConfig *supl_config = NULL;
    gboolean          ret;

    supl_config = get_supl_server_config (gnss_context);
    if (supl_config == NULL) {
        g_print ("SUPL Configuration not available\n");
        return;
    }

    supl = g_strdup_printf ("%s:%u", supl_config->slp_address, SLP_PORT);
    g_print ("supl address to be sent is %s\n",supl);
    ret = mm_modem_location_set_supl_server_sync (ctx->modem_location,
                                                  supl,
                                                  NULL,
                                                  NULL);
    if (!ret)
        g_printerr ("failed to set supl server info\n");
}


static void
send_supl_digital_certificate (LocationContext *gnss_context)
{
    g_autoptr(MMModemLocation)     cert = NULL;
    GList               *cert_list = NULL;
    SuplCertificateData *cert_data = NULL;
    SuplServerConfig    *supl_config = NULL;
    gboolean             ret;
    GError          *error = NULL;

    supl_config = get_supl_server_config (gnss_context);
    if (supl_config == NULL) {
        g_print ("SUPL Configuration not available\n");
        return;
    }

    for (cert_list = supl_config->cert_data; cert_list; cert_list = g_list_next (cert_list)) {
        cert_data = (SuplCertificateData *)cert_list->data;

        cert = mm_location_profile_new();
        if (!cert)
            return;
        
        mm_location_profile_consume_string(cert, PROPERTY_CERT_NAME,cert_data->cert_name,&error);
        mm_location_profile_consume_string(cert, PROPERTY_CERT_DATA,cert_data->cert,&error);
        mm_location_set_supl_digital_certificate_get_dictionary(cert);

        ret = mm_modem_location_set_supl_digital_certificate_sync (ctx->modem_location,
                                                                   cert,
                                                                   NULL,
                                                                   NULL);
        if (!ret)
            g_printerr ("failed to set SUPL digital certificate\n");
    }
}

static void
store_supl_server_config (GKeyFile        *keyfile,
                          gchar           *group_name,
                          LocationContext *gnss_context)
{
    gsize                n_cert_names = 0, n_certs = 0, i = 0, cert_len;
    GError              *error = NULL;
    SuplCertificateData *cert_data = NULL;
    SuplServerConfig    *supl_config = NULL;
    gchar              **cert_names = NULL;
    gchar              **certs = NULL;

    supl_config = g_slice_new0 (SuplServerConfig);
    if (!supl_config)
        return;

    supl_config->group_name  = g_strdup (group_name);
    supl_config->slp_address = g_key_file_get_string (keyfile, group_name, SLP_ADDRESS,  NULL);

    cert_names = g_key_file_get_string_list (keyfile, group_name, TLS_CERT_NAME, &n_cert_names, &error);
    certs = g_key_file_get_string_list (keyfile, group_name, TLS_CERT, &n_certs, &error);

    if ((supl_config->group_name == NULL) || (supl_config->slp_address == NULL) ||
          (n_cert_names != n_certs) || (n_cert_names == 0) ||
          (n_certs > MAX_SUPPORTED_CERTS)) {
        g_print ("Ignoring the Supl Server Info for Group [%s]\n", group_name);
        goto free_mem;
    }

    for (i = 0; i < n_certs; i++) {
        cert_data = g_slice_new0 (SuplCertificateData);
        cert_data->cert_name = g_strdup_printf ("%s", cert_names[i]);
        cert_data->cert = g_strdup_printf ("%s", certs[i]);

        /* Remove the double quotes from the certificate name and certificate data at the start and end */
        cert_len = strnlen (cert_data->cert, CERT_SIZE_MAX);
        cert_data->cert[cert_len-1] = '\0';
        cert_data->cert_name[(strnlen (cert_data->cert_name, CERT_SIZE_MAX))-1] = '\0';

        cert_data->cert = cert_data->cert + 1;
        cert_data->cert_name = cert_data->cert_name + 1;
        supl_config->cert_data = g_list_append (supl_config->cert_data, cert_data);
    }

    if (supl_config->cert_data == NULL)  {
        g_print ("discarding Group as there are no valid certificates\n");
        goto free_mem;
    }

    gnss_context->agps_config.supl_server_config =
        g_list_append (gnss_context->agps_config.supl_server_config, supl_config);
    g_print ("store SUPL server config: Group Name [%s] , SLP address [%s]\n",
        supl_config->group_name, supl_config->slp_address);
    return;

free_mem:
    g_free (supl_config->group_name);
    g_free (supl_config->slp_address);
    g_slice_free (SuplServerConfig, supl_config);
    g_strfreev (cert_names);
    g_strfreev (certs);
    if (error != NULL)
        g_error_free(error);
}

static gboolean
store_agps_config (LocationContext *gnss_context)
{
    guint     index    = 0;
    gsize     n_groups = 0;
    g_autoptr(GKeyFile) keyfile  = NULL;
    GError   *error    = NULL;
    gchar  ** groups   = NULL;

    keyfile = g_key_file_new ();
    if (!g_key_file_load_from_file (keyfile, conf_file, G_KEY_FILE_NONE, &error)) {
        g_printerr ("No file found to read the data\n");
        goto cleanup;
    }

    g_key_file_set_list_separator (keyfile, ',');

    groups = g_key_file_get_groups (keyfile, &n_groups);
    if (groups == NULL) {
        g_print ("No Groups in Location AGPS configuration file\n");
        goto cleanup;
    }

    for (index = 0; index < n_groups; index++) {
        store_supl_server_config (keyfile,  groups[index],gnss_context);
        g_free (groups[index]);
    }

    g_free (groups);
    if (error != NULL)
        g_error_free(error);
    return TRUE;

cleanup:
    if (error != NULL)
        g_error_free(error);
    return FALSE;
}

static void
modem_added (MMManager *manager,
             MMObject  *object)
{
    MMModem *mdm = NULL;

    mdm = MM_MODEM (mm_object_peek_modem (object));
    if (NULL == mdm)
        return;

    ctx->modem_location = mm_object_get_modem_location (object);
    if (ctx->modem_location == NULL) {
        g_printerr ("modem_location is NULL\n");
        return;
    }
}

static void
print_version_and_exit (void)
{
    g_print ("\n"
             PROGRAM_NAME " " PROGRAM_VERSION "\n"
             "Copyright (2022) Aleksander Morgado\n"
             "License GPLv2+: GNU GPL version 2 or later <http://gnu.org/licenses/gpl-2.0.html>\n"
             "This is free software: you are free to change and redistribute it.\n"
             "There is NO WARRANTY, to the extent permitted by law.\n"
             "\n");
    exit (EXIT_SUCCESS);
}

gint
main (gint argc, gchar **argv)
{
    guint8              count = 0;
    GList              *modem_list, *l;
    GOptionContext     *context;
    GDBusObjectManager *obj_manager = NULL;
    GDBusConnection    *connection;
    LocationContext    *gnss_context = NULL;
    GError             *error = NULL;

    setlocale (LC_ALL, "");

    /* Setup option context, process it and destroy it */
    context = g_option_context_new ("- ModemManager Location DBus API testing");
    g_option_context_add_main_entries (context, main_entries, NULL);
    g_option_context_parse (context, &argc, &argv, NULL);
    g_option_context_free (context);

    if (version_flag)
        print_version_and_exit ();

    /* No config file path given? */
    if (!conf_file) {
        g_printerr ("error: no config file specified\n");
        exit (EXIT_FAILURE);
    }

    /* No operator code given? */
    if (!opc) {
        g_printerr ("error: no operator code specified\n");
        exit (EXIT_FAILURE);
    }

    /* set up the context */
    ctx = g_new0 (MMContext,1);

    /* Setup dbus connection to use */
    connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!connection) {
        g_printerr ("error: couldn't get bus: %s\n",
                    error ? error->message : "unknown error");
        exit (EXIT_FAILURE);
    }

    ctx->manager = mm_manager_new_sync (connection,
                                        G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
                                        NULL,
                                        &error);
    if (!ctx->manager) {
        g_printerr ("error: couldn't create manager: %s\n", error ? error->message : "unknown error");
        exit (EXIT_FAILURE);
    }

    obj_manager = G_DBUS_OBJECT_MANAGER (ctx->manager);
    if (!obj_manager) {
        g_printerr ("error: obj manager returned NULL: %s\n", error ? error->message : "unknown error");
        exit (EXIT_FAILURE);
    }

    modem_list = g_dbus_object_manager_get_objects (obj_manager);
    if (!modem_list) {
        g_printerr ("modem_list is NULL\n");
        exit (EXIT_FAILURE);
    }

    for (l = modem_list, count = 0; l; l = g_list_next (l), count++) {
        modem_added (ctx->manager, MM_OBJECT (l->data));
    }

    gnss_context = g_slice_new0 (LocationContext);
    if (store_agps_config (gnss_context) == FALSE) {
        exit (EXIT_FAILURE);
    }

    send_supl_server (gnss_context);
    send_supl_digital_certificate (gnss_context);
    g_free (ctx);

    return EXIT_SUCCESS;
}
