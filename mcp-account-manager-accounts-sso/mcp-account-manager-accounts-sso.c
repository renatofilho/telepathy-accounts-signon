/*
 * Copyright © 2012 Collabora Ltd.
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: john.brooks@jollamobile.com
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "mcp-account-manager-accounts-sso.h"

#include <telepathy-glib/telepathy-glib.h>

#include <libaccounts-glib/ag-account.h>
#include <libaccounts-glib/ag-account-service.h>
#include <libaccounts-glib/ag-manager.h>
#include <libaccounts-glib/ag-service.h>
#include <libaccounts-glib/ag-auth-data.h>
#include <libaccounts-glib/ag-provider.h>

#include <libsignon-glib/signon-identity.h>

#include <string.h>
#include <ctype.h>

#define ACCOUNTS_SSO_PROVIDER "im.telepathy.Account.Storage.AccountsSSO"

#define PLUGIN_NAME "accounts-sso"
#define PLUGIN_PRIORITY (MCP_ACCOUNT_STORAGE_PLUGIN_PRIO_KEYRING + 10)
#define PLUGIN_DESCRIPTION "Provide Telepathy Accounts from Accounts-SSO via libaccounts-glib"
#define PLUGIN_PROVIDER ACCOUNTS_SSO_PROVIDER

#define DEBUG g_debug

#define SERVICE_TYPE "IM"
#define KEY_PREFIX "telepathy/"
#define KEY_ACCOUNT_NAME "mc-account-name"
#define KEY_READONLY_PARAMS "mc-readonly-params"

static void account_storage_iface_init (McpAccountStorageIface *iface);
static void create_account(AgAccountService *service, McpAccountManagerAccountsSso *self);

G_DEFINE_TYPE_WITH_CODE (McpAccountManagerAccountsSso, mcp_account_manager_accounts_sso,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (MCP_TYPE_ACCOUNT_STORAGE,
        account_storage_iface_init));

struct _McpAccountManagerAccountsSsoPrivate
{
  McpAccountManager *am;

  AgManager *manager;

  /* alloc'ed string -> ref'ed AgAccountService
   * The key is the account_name, an MC unique identifier.
   * Note: There could be multiple services in this table having the same
   * AgAccount, even if unlikely. */
  GHashTable *accounts;

  /* List of AgAccountService that are monitored but don't yet have an
   * associated telepathy account and identifier. A reference must be held
   * to watch signals. */
  GList *pending_accounts;

  /* Queue of owned DelayedSignalData */
  GQueue *pending_signals;

  gboolean loaded;
  gboolean ready;
};

typedef enum {
  DELAYED_CREATE,
  DELAYED_DELETE,
} DelayedSignal;

typedef struct {
  DelayedSignal signal;
  AgAccountId account_id;
} DelayedSignalData;

static gchar *
_tp_transform_to_string(GVariant *src)
{
  if (g_variant_is_of_type (src, G_VARIANT_TYPE_BOOLEAN))
    {
      if (g_variant_get_boolean (src))
        return g_strdup("true");
      else
        return g_strdup ("false");
    }
  else if (g_variant_is_of_type(src, G_VARIANT_TYPE_STRING) ||
           g_variant_is_of_type(src, G_VARIANT_TYPE_OBJECT_PATH) ||
           g_variant_is_of_type(src, G_VARIANT_TYPE_SIGNATURE))
    {
      return g_variant_dup_string(src, NULL);
    }
  else
    {
      DEBUG("VARIANT TYPE: %s", g_variant_get_type_string(src));
    }

  return NULL;
}

static gchar *
_service_dup_tp_value (AgAccountService *service,
    const gchar *key)
{
  gchar *real_key = g_strdup_printf (KEY_PREFIX "%s", key);
  GVariant *value;
  value = ag_account_service_get_variant (service, real_key, NULL);
  g_free (real_key);
  if (value == NULL)
    return NULL;

  return g_variant_dup_string(value, NULL);
}

static void
_service_set_tp_value (AgAccountService *service,
    const gchar *key,
    const gchar *value)
{
  gchar *real_key = g_strdup_printf (KEY_PREFIX "%s", key);

  if (value != NULL)
    {
      GVariant *gvariant = g_variant_new_string (value);
      ag_account_service_set_variant (service, real_key, gvariant);
    }
  else
    {
      ag_account_service_set_variant (service, real_key, NULL);
    }
    g_free(real_key);
}

/* Returns NULL if the account never has been imported into MC before */
static gchar *
_service_dup_tp_account_name (AgAccountService *service)
{
  return _service_dup_tp_value (service, KEY_ACCOUNT_NAME);
}

static void
_service_set_tp_account_name (AgAccountService *service,
    const gchar *account_name)
{
  _service_set_tp_value (service, KEY_ACCOUNT_NAME, account_name);
}

static void
_service_enabled_cb (AgAccountService *service,
    gboolean enabled,
    McpAccountManagerAccountsSso *self)
{
  gchar *account_name = _service_dup_tp_account_name (service);
  GList *node;

  if (account_name == NULL)
    {
      if (enabled)
        {
          create_account (service, self);

          node = g_list_find (self->priv->pending_accounts, service);
          if (node)
            {
              self->priv->pending_accounts = g_list_delete_link (self->priv->pending_accounts,
                  node);
              g_object_unref (service);
            }
        }
    }
  else
    {
      DEBUG ("Accounts SSO: account %s toggled: %s", account_name,
          enabled ? "enabled" : "disabled");

      /* FIXME: Should this update the username from signon credentials first,
       * in case that was changed? */
      g_signal_emit_by_name (self, "toggled", account_name, enabled);
    }

  g_free (account_name);
}

static void
_service_changed_cb (AgAccountService *service,
    McpAccountManagerAccountsSso *self)
{
  gchar *account_name = _service_dup_tp_account_name (service);

  if (!self->priv->ready || account_name == NULL)
    return;

  DEBUG ("Accounts SSO: account %s changed", account_name);

  /* FIXME: Should check signon credentials for changed username */
  /* FIXME: Could use ag_account_service_get_changed_fields()
   * and emit "altered-one" */
  g_signal_emit_by_name (self, "altered", account_name);

  g_free (account_name);
}

static void
_account_stored_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  AgAccount *account = AG_ACCOUNT(source_object);
  GError *error = NULL;

  if (!ag_account_store_finish (account, res, &error))
    {
      g_assert (error != NULL);
      DEBUG ("Error storing Accounts SSO account '%s': %s",
          ag_account_get_display_name (account),
          error->message);
      g_error_free(error);
    }
}

static gboolean
_add_service (McpAccountManagerAccountsSso *self,
    AgAccountService *service,
    const gchar *account_name)
{
  DEBUG ("Accounts SSO: account %s added", account_name);

  if (g_hash_table_contains (self->priv->accounts, account_name))
    {
      DEBUG ("Already exists, ignoring");
      return FALSE;
    }

  g_hash_table_insert (self->priv->accounts,
      g_strdup (account_name),
      g_object_ref (service));

  return TRUE;
}

static void
_account_create(McpAccountManagerAccountsSso *self, AgAccountService *service)
{
  AgAccount *account = ag_account_service_get_account (service);
  gchar *cm_name = _service_dup_tp_value (service, "manager");
  gchar *protocol_name = _service_dup_tp_value (service, "protocol");
  gchar *service_name = 0;
  gchar *account_name = 0;
  gchar *tmp;

  if (tp_str_empty (cm_name) || tp_str_empty (protocol_name))
    {
      g_debug ("Accounts SSO: _account_create missing manager/protocol for new account %u, ignoring", account->id);
      g_free (cm_name);
      g_free (protocol_name);
      return;
    }

  /* Generate a unique and predictable name using service name and account ID, instead of
   * mcp_account_manager_get_unique_name. Manager name and service name are escaped, and
   * dashes are replaced with underscores in protocol name and service name. This matches
   * mcp_account_manager_get_unique_name's behavior. */
  tmp = tp_escape_as_identifier (cm_name);
  g_free (cm_name);
  cm_name = tmp;

  g_strdelimit (protocol_name, "-", '_');

  service_name = tp_escape_as_identifier (ag_service_get_name (ag_account_service_get_service (service)));
  account_name = g_strdup_printf ("%s/%s/%s_%u", cm_name, protocol_name,
      service_name, account->id);

  _service_set_tp_account_name (service, account_name);
  ag_account_store_async (account, NULL, _account_stored_cb, self);

  g_debug("Accounts SSO: _account_create: %s", account_name);

  if (_add_service (self, service, account_name))
      g_signal_emit_by_name (self, "created", account_name);

  g_free (cm_name);
  g_free (protocol_name);
  g_free (service_name);
  g_free (account_name);
}

typedef struct
{
    AgAccount *account;
    AgAccountService *service;
    McpAccountManagerAccountsSso *self;
} AccountCreateData;

static void
_account_created_signon_cb(SignonIdentity *signon,
    const SignonIdentityInfo *info,
    const GError *error,
    gpointer user_data)
{
  AccountCreateData *data = (AccountCreateData*) user_data;
  gchar *username = g_strdup (signon_identity_info_get_username (info));

  g_debug("Accounts SSO: got account signon info response");

  if (!tp_str_empty (username))
    {
      /* Must be stored for CMs */
      _service_set_tp_value (data->service, "param-account", username);
      ag_account_store_async (data->account, NULL, _account_stored_cb, data->self);

      _account_create (data->self, data->service);
    }
  else
    {
      g_debug("Accounts SSO: has no account name");
    }

  g_object_unref (data->service);
  g_object_unref (signon);
  g_free(data);
}

static void
_account_created_cb (AgManager *manager,
    AgAccountId id,
    McpAccountManagerAccountsSso *self)
{
  GList *l;
  AgAccount *account = ag_manager_get_account (self->priv->manager, id);

  if (!self->priv->ready)
    {
      DelayedSignalData *data = g_slice_new0 (DelayedSignalData);

      data->signal = DELAYED_CREATE;
      data->account_id = account->id;

      g_queue_push_tail (self->priv->pending_signals, data);
      return;
    }

  l = ag_account_list_services_by_type (account, SERVICE_TYPE);
  while (l != NULL)
    {
      AgAccountService *service = ag_account_service_new (account, l->data);
      g_signal_connect (service, "enabled",
          G_CALLBACK (_service_enabled_cb), self);
      g_signal_connect (service, "changed",
          G_CALLBACK (_service_changed_cb), self);

      if (ag_account_get_enabled (account))
        {
          create_account (service, self);
        }
      else
        {
          self->priv->pending_accounts = g_list_prepend (self->priv->pending_accounts,
              g_object_ref (service));
        }

      g_object_unref (service);
      ag_service_unref (l->data);
      l = g_list_delete_link (l, l);
    }

  g_object_unref (account);
}

static void
create_account(AgAccountService *service,
    McpAccountManagerAccountsSso *self)
{
  gchar *account_name = _service_dup_tp_account_name (service);

  /* If this is the first time we see this service, we have to generate an
   * account_name for it. */
  if (account_name == NULL)
    {
      gchar *username = _service_dup_tp_value(service, "param-account");
      if (!username)
        {
          /* Request auth data to get the username from signon; it's not available
           * from the account. */
          AgAuthData *auth_data = ag_account_service_get_auth_data (service);
          if (!auth_data)
            {
              DEBUG("Accounts SSO: account is missing auth data; ignored");
              return;
            }

          guint cred_id = ag_auth_data_get_credentials_id (auth_data);
          ag_auth_data_unref(auth_data);

          SignonIdentity *signon = signon_identity_new_from_db (cred_id);
          if (!signon)
            {
              DEBUG("Accounts SSO: cannot create signon identity from account (cred_id %u); ignored", cred_id);
              return;
            }

          /* Callback frees/unrefs data */
          AccountCreateData *data = g_new(AccountCreateData, 1);
          data->account = ag_account_service_get_account (service);
          data->service = g_object_ref (service);
          data->self = self;

          DEBUG("Accounts SSO: querying account info from signon");
          signon_identity_query_info(signon, _account_created_signon_cb, data);
          return;
        }
      else
        {
          _account_create (self, service);
          g_free (username);
        }
    }
  else
    {
      if (_add_service (self, service, account_name))
        g_signal_emit_by_name (self, "created", account_name);
    }

  g_free (account_name);
}

static void
_account_deleted_cb (AgManager *manager,
    AgAccountId id,
    McpAccountManagerAccountsSso *self)
{
  GHashTableIter iter;
  gpointer value;
  GList *node;

  if (!self->priv->ready)
    {
      DelayedSignalData *data = g_slice_new0 (DelayedSignalData);

      data->signal = DELAYED_DELETE;
      data->account_id = id;

      g_queue_push_tail (self->priv->pending_signals, data);
      return;
    }

  g_hash_table_iter_init (&iter, self->priv->accounts);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      AgAccountService *service = value;
      AgAccount *account = ag_account_service_get_account (service);
      gchar *account_name;

      if (account->id != id)
        continue;

      account_name = _service_dup_tp_account_name (service);
      if (account_name == NULL)
        continue;

      DEBUG ("Accounts SSO: account %s deleted", account_name);

      g_hash_table_iter_remove (&iter);
      g_signal_emit_by_name (self, "deleted", account_name);

      g_free (account_name);
    }

  node = self->priv->pending_accounts;
  while (node)
    {
      GList *next = g_list_next (node);
      AgAccountService *service = node->data;
      AgAccount *account = ag_account_service_get_account (service);

      if (account->id == id)
        {
          g_object_unref (service);
          self->priv->pending_accounts = g_list_delete_link (self->priv->pending_accounts, node);
        }

      node = next;
    }
}

static void
mcp_account_manager_accounts_sso_dispose (GObject *object)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) object;

  tp_clear_object (&self->priv->am);
  tp_clear_object (&self->priv->manager);
  tp_clear_pointer (&self->priv->accounts, g_hash_table_unref);

  g_list_free_full (self->priv->pending_accounts, g_object_unref);
  self->priv->pending_accounts = NULL;

  G_OBJECT_CLASS (mcp_account_manager_accounts_sso_parent_class)->dispose (object);
}

static void
mcp_account_manager_accounts_sso_init (McpAccountManagerAccountsSso *self)
{
  DEBUG ("Accounts SSO: MC plugin initialised");

  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      MCP_TYPE_ACCOUNT_MANAGER_ACCOUNTS_SSO, McpAccountManagerAccountsSsoPrivate);

  self->priv->accounts = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, g_object_unref);
  self->priv->pending_accounts = NULL;
  self->priv->pending_signals = g_queue_new ();

  self->priv->manager = ag_manager_new_for_service_type (SERVICE_TYPE);
  g_return_if_fail (self->priv->manager != NULL);

  g_signal_connect (self->priv->manager, "account-created",
      G_CALLBACK (_account_created_cb), self);
  g_signal_connect (self->priv->manager, "account-deleted",
      G_CALLBACK (_account_deleted_cb), self);
}

static void
mcp_account_manager_accounts_sso_class_init (McpAccountManagerAccountsSsoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = mcp_account_manager_accounts_sso_dispose;

  g_type_class_add_private (gobject_class,
      sizeof (McpAccountManagerAccountsSsoPrivate));
}

static void
_ensure_loaded (McpAccountManagerAccountsSso *self)
{
  GList *services;

  if (self->priv->loaded)
    return;

  self->priv->loaded = TRUE;

  g_assert (!self->priv->ready);

  services = ag_manager_get_account_services (self->priv->manager);
  while (services != NULL)
    {
      AgAccountService *service = services->data;
      AgAccount *account = ag_account_service_get_account (service);
      gchar *account_name = _service_dup_tp_account_name (service);

      if (account_name != NULL)
        {
          /* This service was already known, we can add it now */
          _add_service (self, service, account_name);
          g_signal_connect (service, "enabled",
              G_CALLBACK (_service_enabled_cb), self);
          g_signal_connect (service, "changed",
              G_CALLBACK (_service_changed_cb), self);
          g_free (account_name);
        }
      else
        {
          DelayedSignalData *data = g_slice_new0 (DelayedSignalData);

          /* This service was created while MC was not running, delay its
           * creation until MC is ready */
          data->signal = DELAYED_CREATE;
          data->account_id = account->id;

          g_queue_push_tail (self->priv->pending_signals, data);
        }

      g_object_unref (services->data);
      services = g_list_delete_link (services, services);
    }
}

static GList *
account_manager_accounts_sso_list (const McpAccountStorage *storage,
    const McpAccountManager *am)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) storage;
  GList *accounts = NULL;
  GHashTableIter iter;
  gpointer key;

  DEBUG (G_STRFUNC);

  g_return_val_if_fail (self->priv->manager != NULL, NULL);

  _ensure_loaded (self);

  g_hash_table_iter_init (&iter, self->priv->accounts);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    accounts = g_list_prepend (accounts, g_strdup (key));

  return accounts;
}

static const gchar *
provider_to_tp_service_name (const gchar *provider_name)
{
  /* Well known services are defined in Telepathy spec:
   * http://telepathy.freedesktop.org/spec/Account.html#Property:Service */
  if (!tp_strdiff (provider_name, "google"))
    return "google-talk";

  return provider_name;
}

static gboolean
account_manager_accounts_sso_get (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *account_name,
    const gchar *key)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) storage;
  AgAccountService *service;
  AgAccount *account;
  AgService *s;
  gboolean handled = FALSE;

  g_return_val_if_fail (self->priv->manager != NULL, FALSE);

  service = g_hash_table_lookup (self->priv->accounts, account_name);
  if (service == NULL)
    return FALSE;

  DEBUG ("%s: %s, %s", G_STRFUNC, account_name, key);

  account = ag_account_service_get_account (service);
  s = ag_account_service_get_service (service);

  /* NULL key means we want all settings */
  if (key == NULL)
    {
      AgAccountSettingIter iter;
      const gchar *k;
      GVariant *v;

      ag_account_service_settings_iter_init (service, &iter, KEY_PREFIX);
      while (ag_account_settings_iter_get_next (&iter, &k, &v))
        {
          gchar *value = _tp_transform_to_string (v);
          if (value)
            {
              mcp_account_manager_set_value (am, account_name, k, value);
              g_free (value);
            }
        }
    }

  /* Some special keys that are not stored in setting */
  if (key == NULL || !tp_strdiff (key, "Enabled"))
    {
      mcp_account_manager_set_value (am, account_name, "Enabled",
          ag_account_service_get_enabled (service) ? "true" : "false");
      handled = TRUE;
    }

  if (key == NULL || !tp_strdiff (key, "DisplayName"))
    {
      mcp_account_manager_set_value (am, account_name, "DisplayName",
          ag_account_get_display_name (account));
      handled = TRUE;
    }

  if (key == NULL || !tp_strdiff (key, "Service"))
    {
      mcp_account_manager_set_value (am, account_name, "Service",
          provider_to_tp_service_name (ag_account_get_provider_name (account)));
      handled = TRUE;
    }

  if (key == NULL || !tp_strdiff (key, "Icon"))
    {
      /* Try loading the icon from service, if that's empty, load the provider */
      const gchar *icon_name = ag_service_get_icon_name (s);
      if (strlen(icon_name) == 0)
        {
          AgProvider *provider = ag_manager_get_provider (self->priv->manager,
                                                          ag_account_get_provider_name (account));
          icon_name = ag_provider_get_icon_name (provider);
          ag_provider_unref(provider);
        }
      mcp_account_manager_set_value (am, account_name, "Icon",
          icon_name);
      handled = TRUE;
    }

  /* If it was none of the above, then just lookup in service' settings */
  if (!handled)
    {
      gchar *value = _service_dup_tp_value (service, key);

      mcp_account_manager_set_value (am, account_name, key, value);
      g_free (value);
    }

  return TRUE;
}

static gboolean
account_manager_accounts_sso_set (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *account_name,
    const gchar *key,
    const gchar *val)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) storage;
  AgAccountService *service;
  AgAccount *account;

  g_return_val_if_fail (self->priv->manager != NULL, FALSE);

  service = g_hash_table_lookup (self->priv->accounts, account_name);
  if (service == NULL)
    return FALSE;

  account = ag_account_service_get_account (service);

  DEBUG ("%s: %s, %s, %s", G_STRFUNC, account_name, key, val);

  if (!tp_strdiff (key, "Enabled"))
    {
      /* Enabled is a global setting on the account, not per-services,
       * unfortunately */
      ag_account_select_service (account, NULL);
      ag_account_set_enabled (account, !tp_strdiff (val, "true"));
    }
  else if (!tp_strdiff (key, "DisplayName"))
    {
      ag_account_set_display_name (account, val);
    }
  else
    {
      _service_set_tp_value (service, key, val);
    }

  return TRUE;
}

static gchar *
account_manager_accounts_sso_create (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *cm_name,
    const gchar *protocol_name,
    GHashTable *params,
    GError **error)
{
  /* We don't want account creation for this plugin. */
  return NULL;
}

static gboolean
account_manager_accounts_sso_delete (const McpAccountStorage *storage,
    const McpAccountManager *am,
    const gchar *account_name,
    const gchar *key)
{
  return FALSE;
}

static gboolean
account_manager_accounts_sso_commit (const McpAccountStorage *storage,
    const McpAccountManager *am)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) storage;
  GHashTableIter iter;
  gpointer value;

  DEBUG (G_STRFUNC);

  g_return_val_if_fail (self->priv->manager != NULL, FALSE);

  g_hash_table_iter_init (&iter, self->priv->accounts);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      AgAccountService *service = value;
      AgAccount *account = ag_account_service_get_account (service);

      ag_account_store_async (account, NULL, _account_stored_cb, self);
    }

  return TRUE;
}

static void
account_manager_accounts_sso_ready (const McpAccountStorage *storage,
    const McpAccountManager *am)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) storage;
  DelayedSignalData *data;

  g_return_if_fail (self->priv->manager != NULL);

  if (self->priv->ready)
    return;

  DEBUG (G_STRFUNC);

  self->priv->ready = TRUE;
  self->priv->am = g_object_ref (G_OBJECT (am));

  while ((data = g_queue_pop_head (self->priv->pending_signals)) != NULL)
    {
      switch (data->signal)
        {
          case DELAYED_CREATE:
            _account_created_cb (self->priv->manager, data->account_id, self);
            break;
          case DELAYED_DELETE:
            _account_deleted_cb (self->priv->manager, data->account_id, self);
            break;
          default:
            g_assert_not_reached ();
        }

      g_slice_free (DelayedSignalData, data);
    }

  g_queue_free (self->priv->pending_signals);
  self->priv->pending_signals = NULL;

}

static void
account_manager_accounts_sso_get_identifier (const McpAccountStorage *storage,
    const gchar *account_name,
    GValue *identifier)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) storage;
  AgAccountService *service;
  AgAccount *account;

  g_return_if_fail (self->priv->manager != NULL);

  service = g_hash_table_lookup (self->priv->accounts, account_name);
  if (service == NULL)
    return;

  account = ag_account_service_get_account (service);

  g_value_init (identifier, G_TYPE_UINT);
  g_value_set_uint (identifier, account->id);
}

static GHashTable *
account_manager_accounts_sso_get_additional_info (const McpAccountStorage *storage,
    const gchar *account_name)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) storage;
  AgAccountService *service;
  AgAccount *account;
  AgProvider *provider;
  GHashTable *ret = NULL;

  /* If we don't know this account, we cannot do anything */
  service = g_hash_table_lookup (self->priv->accounts, account_name);
  if (service == NULL)
    return ret;

  account = ag_account_service_get_account (service);
  provider = ag_manager_get_provider (self->priv->manager, ag_account_get_provider_name (account));

  ret = tp_asv_new (
      "providerDisplayName", G_TYPE_STRING, ag_provider_get_display_name (provider),
      "accountDisplayName", G_TYPE_STRING, ag_account_get_display_name (account),
      NULL);

  ag_provider_unref(provider);
  return ret;
}

static guint
account_manager_accounts_sso_get_restrictions (const McpAccountStorage *storage,
    const gchar *account_name)
{
  McpAccountManagerAccountsSso *self = (McpAccountManagerAccountsSso *) storage;
  AgAccountService *service;
  guint restrictions = TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_SERVICE;
  GVariant *value;

  g_return_val_if_fail (self->priv->manager != NULL, 0);

  /* If we don't know this account, we cannot do anything */
  service = g_hash_table_lookup (self->priv->accounts, account_name);
  if (service == NULL)
    return G_MAXUINT;

  value = ag_account_service_get_variant (service,
      KEY_PREFIX KEY_READONLY_PARAMS, NULL);

  if (value != NULL && g_variant_get_boolean (value))
    restrictions |= TP_STORAGE_RESTRICTION_FLAG_CANNOT_SET_PARAMETERS;

  /* FIXME: We can't set Icon either, but there is no flag for that */
  return restrictions;
}

static void
account_storage_iface_init (McpAccountStorageIface *iface)
{
  iface->name = PLUGIN_NAME;
  iface->desc = PLUGIN_DESCRIPTION;
  iface->priority = PLUGIN_PRIORITY;
  iface->provider = PLUGIN_PROVIDER;

#define IMPLEMENT(x) iface->x = account_manager_accounts_sso_##x
  IMPLEMENT (get);
  IMPLEMENT (list);
  IMPLEMENT (set);
  IMPLEMENT (create);
  IMPLEMENT (delete);
  IMPLEMENT (commit);
  IMPLEMENT (ready);
  IMPLEMENT (get_identifier);
  IMPLEMENT (get_restrictions);
  IMPLEMENT (get_additional_info);
#undef IMPLEMENT
}

McpAccountManagerAccountsSso *
mcp_account_manager_accounts_sso_new (void)
{
  return g_object_new (MCP_TYPE_ACCOUNT_MANAGER_ACCOUNTS_SSO, NULL);
}
