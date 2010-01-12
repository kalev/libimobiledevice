/*
 * lockdown.c
 * libiphone built-in lockdownd client
 *
 * Copyright (c) 2008 Zach C. All Rights Reserved.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <libtasn1.h>
#include <gnutls/x509.h>
#include <plist/plist.h>

#include "property_list_service.h"
#include "lockdown.h"
#include "iphone.h"
#include "debug.h"
#include "userpref.h"

#define RESULT_SUCCESS 0
#define RESULT_FAILURE 1

const ASN1_ARRAY_TYPE pkcs1_asn1_tab[] = {
	{"PKCS1", 536872976, 0},
	{0, 1073741836, 0},
	{"RSAPublicKey", 536870917, 0},
	{"modulus", 1073741827, 0},
	{"publicExponent", 3, 0},
	{0, 0, 0}
};

/**
 * Internally used function for checking the result from lockdown's answer
 * plist to a previously sent request.
 *
 * @param dict The plist to evaluate.
 * @param query_match Name of the request to match.
 *
 * @return RESULT_SUCCESS when the result is 'Success',
 *         RESULT_FAILURE when the result is 'Failure',
 *         or a negative value if an error occured during evaluation.
 */
static int lockdown_check_result(plist_t dict, const char *query_match)
{
	int ret = -1;

	plist_t query_node = plist_dict_get_item(dict, "Request");
	if (!query_node) {
		return ret;
	}
	if (plist_get_node_type(query_node) != PLIST_STRING) {
		return ret;
	} else {
		char *query_value = NULL;
		plist_get_string_val(query_node, &query_value);
		if (!query_value) {
			return ret;
		}
		if (strcmp(query_value, query_match) != 0) {
			free(query_value);
			return ret;
		}
		free(query_value);
	}

	plist_t result_node = plist_dict_get_item(dict, "Result");
	if (!result_node) {
		return ret;
	}

	plist_type result_type = plist_get_node_type(result_node);

	if (result_type == PLIST_STRING) {

		char *result_value = NULL;

		plist_get_string_val(result_node, &result_value);

		if (result_value) {
			if (!strcmp(result_value, "Success")) {
				ret = RESULT_SUCCESS;
			} else if (!strcmp(result_value, "Failure")) {
				ret = RESULT_FAILURE;
			} else {
				debug_info("ERROR: unknown result value '%s'", result_value);
			}
		}
		if (result_value)
			free(result_value);
	}
	return ret;
}

/**
 * Adds a label key with the passed value to a plist dict node.
 *
 * @param plist The plist to add the key to
 * @param label The value for the label key
 *
 */
static void plist_dict_add_label(plist_t plist, const char *label)
{
	if (plist && label) {
		if (plist_get_node_type(plist) == PLIST_DICT)
			plist_dict_insert_item(plist, "Label", plist_new_string(label));
	}
}

/** gnutls callback for writing data to the device.
 *
 * @param transport It's really the lockdownd client, but the method signature has to match
 * @param buffer The data to send
 * @param length The length of data to send in bytes
 *
 * @return The number of bytes sent
 */
static ssize_t lockdownd_ssl_write(gnutls_transport_ptr_t transport, char *buffer, size_t length)
{
	uint32_t bytes = 0;
	lockdownd_client_t client;
	client = (lockdownd_client_t) transport;
	debug_info("pre-send length = %zi", length);
	iphone_device_send(property_list_service_get_connection(client->parent), buffer, length, &bytes);
	debug_info("post-send sent %i bytes", bytes);
	return bytes;
}

/** gnutls callback for reading data from the device.
 *
 * @param transport It's really the lockdownd client, but the method signature has to match
 * @param buffer The buffer to store data in
 * @param length The length of data to read in bytes
 *
 * @return The number of bytes read
 */
static ssize_t lockdownd_ssl_read(gnutls_transport_ptr_t transport, char *buffer, size_t length)
{
	int bytes = 0, pos_start_fill = 0;
	size_t tbytes = 0;
	int this_len = length;
	iphone_error_t res;
	lockdownd_client_t client;
	client = (lockdownd_client_t) transport;
	char *recv_buffer;

	debug_info("pre-read client wants %zi bytes", length);

	recv_buffer = (char *) malloc(sizeof(char) * this_len);

	/* repeat until we have the full data or an error occurs */
	do {
		if ((res = iphone_device_recv(property_list_service_get_connection(client->parent), recv_buffer, this_len, (uint32_t*)&bytes)) != LOCKDOWN_E_SUCCESS) {
			debug_info("ERROR: iphone_device_recv returned %d", res);
			return res;
		}
		debug_info("post-read we got %i bytes", bytes);

		// increase read count
		tbytes += bytes;

		// fill the buffer with what we got right now
		memcpy(buffer + pos_start_fill, recv_buffer, bytes);
		pos_start_fill += bytes;

		if (tbytes >= length) {
			break;
		}

		this_len = length - tbytes;
		debug_info("re-read trying to read missing %i bytes", this_len);
	} while (tbytes < length);

	if (recv_buffer) {
		free(recv_buffer);
	}

	return tbytes;
}

/** Starts communication with lockdownd after the iPhone has been paired,
 *  and if the device requires it, switches to SSL mode.
 *
 * @param client The lockdownd client
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
static lockdownd_error_t lockdownd_ssl_start_session(lockdownd_client_t client)
{
	lockdownd_error_t ret = LOCKDOWN_E_SSL_ERROR;
	uint32_t return_me = 0;

	// Set up GnuTLS...
	debug_info("enabling SSL mode");
	errno = 0;
	gnutls_global_init();
	gnutls_certificate_allocate_credentials(&client->ssl_certificate);
	gnutls_certificate_set_x509_trust_file(client->ssl_certificate, "hostcert.pem", GNUTLS_X509_FMT_PEM);
	gnutls_init(&client->ssl_session, GNUTLS_CLIENT);
	{
		int protocol_priority[16] = { GNUTLS_SSL3, 0 };
		int kx_priority[16] = { GNUTLS_KX_ANON_DH, GNUTLS_KX_RSA, 0 };
		int cipher_priority[16] = { GNUTLS_CIPHER_AES_128_CBC, GNUTLS_CIPHER_AES_256_CBC, 0 };
		int mac_priority[16] = { GNUTLS_MAC_SHA1, GNUTLS_MAC_MD5, 0 };
		int comp_priority[16] = { GNUTLS_COMP_NULL, 0 };

		gnutls_cipher_set_priority(client->ssl_session, cipher_priority);
		gnutls_compression_set_priority(client->ssl_session, comp_priority);
		gnutls_kx_set_priority(client->ssl_session, kx_priority);
		gnutls_protocol_set_priority(client->ssl_session, protocol_priority);
		gnutls_mac_set_priority(client->ssl_session, mac_priority);
	}
	gnutls_credentials_set(client->ssl_session, GNUTLS_CRD_CERTIFICATE, client->ssl_certificate);	// this part is killing me.

	debug_info("GnuTLS step 1...");
	gnutls_transport_set_ptr(client->ssl_session, (gnutls_transport_ptr_t) client);
	debug_info("GnuTLS step 2...");
	gnutls_transport_set_push_function(client->ssl_session, (gnutls_push_func) & lockdownd_ssl_write);
	debug_info("GnuTLS step 3...");
	gnutls_transport_set_pull_function(client->ssl_session, (gnutls_pull_func) & lockdownd_ssl_read);
	debug_info("GnuTLS step 4 -- now handshaking...");
	if (errno)
		debug_info("WARN: errno says %s before handshake!", strerror(errno));
	return_me = gnutls_handshake(client->ssl_session);
	debug_info("GnuTLS handshake done...");

	if (return_me != GNUTLS_E_SUCCESS) {
		debug_info("GnuTLS reported something wrong.");
		gnutls_perror(return_me);
		debug_info("oh.. errno says %s", strerror(errno));
	} else {
		client->ssl_enabled = 1;
		ret = LOCKDOWN_E_SUCCESS;
		debug_info("SSL mode enabled");
	}

	return ret;
}

/**
 * Shuts down the SSL session by performing a close notify, which is done
 * by "gnutls_bye".
 *
 * @param client The lockdown client
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
static lockdownd_error_t lockdownd_ssl_stop_session(lockdownd_client_t client)
{
	if (!client) {
		debug_info("invalid argument!");
		return LOCKDOWN_E_INVALID_ARG;
	}
	lockdownd_error_t ret = LOCKDOWN_E_SUCCESS;

	if (client->ssl_enabled) {
		debug_info("sending SSL close notify");
		gnutls_bye(client->ssl_session, GNUTLS_SHUT_RDWR);
	}
	if (client->ssl_session) {
		gnutls_deinit(client->ssl_session);
	}
	if (client->ssl_certificate) {
		gnutls_certificate_free_credentials(client->ssl_certificate);
	}
	client->ssl_enabled = 0;

	if (client->session_id)
		free(client->session_id);
	client->session_id = NULL;

	debug_info("SSL mode disabled");

	return ret;
}

/**
 * Closes the lockdownd communication session, by sending the StopSession
 * Request to the device.
 *
 * @see lockdownd_start_session
 *
 * @param control The lockdown client
 * @param session_id The id of a running session
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_stop_session(lockdownd_client_t client, const char *session_id)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	if (!session_id) {
		debug_info("no session_id given, cannot stop session");
		return LOCKDOWN_E_INVALID_ARG;
	}

	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	plist_t dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"Request", plist_new_string("StopSession"));
	plist_dict_insert_item(dict,"SessionID", plist_new_string(session_id));

	debug_info("stopping session %s", session_id);

	ret = lockdownd_send(client, dict);

	plist_free(dict);
	dict = NULL;

	ret = lockdownd_recv(client, &dict);

	if (!dict) {
		debug_info("LOCKDOWN_E_PLIST_ERROR");
		return LOCKDOWN_E_PLIST_ERROR;
	}

	ret = LOCKDOWN_E_UNKNOWN_ERROR;
	if (lockdown_check_result(dict, "StopSession") == RESULT_SUCCESS) {
		debug_info("success");
		ret = LOCKDOWN_E_SUCCESS;
	}
	plist_free(dict);
	dict = NULL;

	/* stop ssl session */
	lockdownd_ssl_stop_session(client);

	return ret;
}

/** Closes the lockdownd client and does the necessary housekeeping.
 *
 * @param client The lockdown client
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_client_free(lockdownd_client_t client)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	if (client->session_id)
		lockdownd_stop_session(client, client->session_id);

	if (client->parent) {
		lockdownd_goodbye(client);

		if (property_list_service_client_free(client->parent) == PROPERTY_LIST_SERVICE_E_SUCCESS) {
			ret = LOCKDOWN_E_SUCCESS;
		}
	}

	if (client->uuid) {
		free(client->uuid);
	}
	if (client->label) {
		free(client->label);
	}

	free(client);
	return ret;
}

/**
 * Sets the label to send for requests to lockdownd.
 *
 * @param client The lockdown client
 * @param label The label to set or NULL to disable sending a label
 *
 */
void lockdownd_client_set_label(lockdownd_client_t client, const char *label)
{
	if (client) {
		if (client->label)
			free(client->label);

		client->label = (label != NULL) ? strdup(label): NULL;
	}
}

/** Polls the iPhone for lockdownd data.
 *
 * @param control The lockdownd client
 * @param plist The plist to store the received data
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_recv(lockdownd_client_t client, plist_t *plist)
{
	if (!client || !plist || (plist && *plist))
		return LOCKDOWN_E_INVALID_ARG;
	lockdownd_error_t ret = LOCKDOWN_E_SUCCESS;
	property_list_service_error_t err;

	if (!client->ssl_enabled) {
		err = property_list_service_receive_plist(client->parent, plist);
		if (err != PROPERTY_LIST_SERVICE_E_SUCCESS) {
			ret = LOCKDOWN_E_UNKNOWN_ERROR;
		}
	} else {
		err = property_list_service_receive_encrypted_plist(client->ssl_session, plist);
		if (err != PROPERTY_LIST_SERVICE_E_SUCCESS) {
			return LOCKDOWN_E_SSL_ERROR;
		}
	}

	if (!*plist)
		ret = LOCKDOWN_E_PLIST_ERROR;

	return ret;
}

/** Sends lockdownd data to the iPhone
 *
 * @note This function is low-level and should only be used if you need to send
 *        a new type of message.
 *
 * @param client The lockdownd client
 * @param plist The plist to send
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_send(lockdownd_client_t client, plist_t plist)
{
	if (!client || !plist)
		return LOCKDOWN_E_INVALID_ARG;

	lockdownd_error_t ret = LOCKDOWN_E_SUCCESS;
	iphone_error_t err;

	if (!client->ssl_enabled) {
		err = property_list_service_send_xml_plist(client->parent, plist);
		if (err != PROPERTY_LIST_SERVICE_E_SUCCESS) {
			ret = LOCKDOWN_E_UNKNOWN_ERROR;
		}
	} else {
		err = property_list_service_send_encrypted_xml_plist(client->ssl_session, plist);
		if (err != PROPERTY_LIST_SERVICE_E_SUCCESS) {
			ret = LOCKDOWN_E_SSL_ERROR;
		}
	}
	return ret;
}

/** Query the type of the service daemon. Depending on whether the device is
 * queried in normal mode or restore mode, different types will be returned.
 *
 * @param client The lockdownd client
 * @param type The type returned by the service daemon. Can be NULL to ignore.
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_query_type(lockdownd_client_t client, char **type)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	plist_t dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"Request", plist_new_string("QueryType"));

	debug_info("called");
	ret = lockdownd_send(client, dict);

	plist_free(dict);
	dict = NULL;

	ret = lockdownd_recv(client, &dict);

	if (LOCKDOWN_E_SUCCESS != ret)
		return ret;

	ret = LOCKDOWN_E_UNKNOWN_ERROR;
	if (lockdown_check_result(dict, "QueryType") == RESULT_SUCCESS) {
		/* return the type if requested */
		if (type != NULL) {
			plist_t type_node = plist_dict_get_item(dict, "Type");
			plist_get_string_val(type_node, type);
		}
		debug_info("success with type %s", *type);
		ret = LOCKDOWN_E_SUCCESS;
	}
	plist_free(dict);
	dict = NULL;

	return ret;
}

/** Retrieves a preferences plist using an optional domain and/or key name.
 *
 * @param client an initialized lockdownd client.
 * @param domain the domain to query on or NULL for global domain
 * @param key the key name to request or NULL to query for all keys
 * @param value a plist node representing the result value node
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_get_value(lockdownd_client_t client, const char *domain, const char *key, plist_t *value)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	plist_t dict = NULL;
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	/* setup request plist */
	dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	if (domain) {
		plist_dict_insert_item(dict,"Domain", plist_new_string(domain));
	}
	if (key) {
		plist_dict_insert_item(dict,"Key", plist_new_string(key));
	}
	plist_dict_insert_item(dict,"Request", plist_new_string("GetValue"));

	/* send to device */
	ret = lockdownd_send(client, dict);

	plist_free(dict);
	dict = NULL;

	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	/* Now get device's answer */
	ret = lockdownd_recv(client, &dict);
	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	if (lockdown_check_result(dict, "GetValue") == RESULT_SUCCESS) {
		debug_info("success");
		ret = LOCKDOWN_E_SUCCESS;
	}
	if (ret != LOCKDOWN_E_SUCCESS) {
		plist_free(dict);
		return ret;
	}

	plist_t value_node = plist_dict_get_item(dict, "Value");

	if (value_node) {
		debug_info("has a value");
		*value = plist_copy(value_node);
	}

	plist_free(dict);
	return ret;
}

/** Sets a preferences value using a plist and optional domain and/or key name.
 *
 * @param client an initialized lockdownd client.
 * @param domain the domain to query on or NULL for global domain
 * @param key the key name to set the value or NULL to set a value dict plist
 * @param value a plist node of any node type representing the value to set
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_set_value(lockdownd_client_t client, const char *domain, const char *key, plist_t value)
{
	if (!client || !value)
		return LOCKDOWN_E_INVALID_ARG;

	plist_t dict = NULL;
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	/* setup request plist */
	dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	if (domain) {
		plist_dict_insert_item(dict,"Domain", plist_new_string(domain));
	}
	if (key) {
		plist_dict_insert_item(dict,"Key", plist_new_string(key));
	}
	plist_dict_insert_item(dict,"Request", plist_new_string("SetValue"));
	plist_dict_insert_item(dict,"Value", value);

	/* send to device */
	ret = lockdownd_send(client, dict);

	plist_free(dict);
	dict = NULL;

	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	/* Now get device's answer */
	ret = lockdownd_recv(client, &dict);
	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	if (lockdown_check_result(dict, "SetValue") == RESULT_SUCCESS) {
		debug_info("success");
		ret = LOCKDOWN_E_SUCCESS;
	}

	if (ret != LOCKDOWN_E_SUCCESS) {
		plist_free(dict);
		return ret;
	}

	plist_free(dict);
	return ret;
}

/** Removes a preference node on the device by domain and/or key name
 *
 * @note: Use with caution as this could remove vital information on the device
 *
 * @param client an initialized lockdownd client.
 * @param domain the domain to query on or NULL for global domain
 * @param key the key name to remove or NULL remove all keys for the current domain
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_remove_value(lockdownd_client_t client, const char *domain, const char *key)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	plist_t dict = NULL;
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	/* setup request plist */
	dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	if (domain) {
		plist_dict_insert_item(dict,"Domain", plist_new_string(domain));
	}
	if (key) {
		plist_dict_insert_item(dict,"Key", plist_new_string(key));
	}
	plist_dict_insert_item(dict,"Request", plist_new_string("RemoveValue"));

	/* send to device */
	ret = lockdownd_send(client, dict);

	plist_free(dict);
	dict = NULL;

	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	/* Now get device's answer */
	ret = lockdownd_recv(client, &dict);
	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	if (lockdown_check_result(dict, "RemoveValue") == RESULT_SUCCESS) {
		debug_info("success");
		ret = LOCKDOWN_E_SUCCESS;
	}

	if (ret != LOCKDOWN_E_SUCCESS) {
		plist_free(dict);
		return ret;
	}

	plist_free(dict);
	return ret;
}

/** Asks for the device's unique id. Part of the lockdownd handshake.
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_get_device_uuid(lockdownd_client_t client, char **uuid)
{
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;
	plist_t value = NULL;

	ret = lockdownd_get_value(client, NULL, "UniqueDeviceID", &value);
	if (ret != LOCKDOWN_E_SUCCESS) {
		return ret;
	}
	plist_get_string_val(value, uuid);

	plist_free(value);
	value = NULL;
	return ret;
}

/** Askes for the device's public key. Part of the lockdownd handshake.
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_get_device_public_key(lockdownd_client_t client, gnutls_datum_t * public_key)
{
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;
	plist_t value = NULL;
	char *value_value = NULL;
	uint64_t size = 0;

	ret = lockdownd_get_value(client, NULL, "DevicePublicKey", &value);
	if (ret != LOCKDOWN_E_SUCCESS) {
		return ret;
	}
	plist_get_data_val(value, &value_value, &size);
	public_key->data = (unsigned char*)value_value;
	public_key->size = size;

	plist_free(value);
	value = NULL;

	return ret;
}

/** Askes for the device's name.
 *
 * @param client The pointer to the location of the new lockdownd_client
 *
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_get_device_name(lockdownd_client_t client, char **device_name)
{
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;
	plist_t value = NULL;

	ret = lockdownd_get_value(client, NULL, "DeviceName", &value);
	if (ret != LOCKDOWN_E_SUCCESS) {
		return ret;
	}
	plist_get_string_val(value, device_name);

	plist_free(value);
	value = NULL;

	return ret;
}

/** Creates a lockdownd client for the device.
 *
 * @param phone The device to create a lockdownd client for
 * @param client The pointer to the location of the new lockdownd_client
 * @param label The label to use for communication. Usually the program name
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_client_new(iphone_device_t device, lockdownd_client_t *client, const char *label)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	lockdownd_error_t ret = LOCKDOWN_E_SUCCESS;

	property_list_service_client_t plistclient = NULL;
	if (property_list_service_client_new(device, 0xf27e, &plistclient) != PROPERTY_LIST_SERVICE_E_SUCCESS) {
		debug_info("could not connect to lockdownd (device %s)", device->uuid);
		return LOCKDOWN_E_MUX_ERROR;
	}

	lockdownd_client_t client_loc = (lockdownd_client_t) malloc(sizeof(struct lockdownd_client_int));
	client_loc->parent = plistclient;
	client_loc->ssl_session = NULL;
	client_loc->ssl_certificate = NULL;
	client_loc->ssl_enabled = 0;
	client_loc->session_id = NULL;
	client_loc->uuid = NULL;
	client_loc->label = NULL;
	if (label != NULL)
		strdup(label);

	if (LOCKDOWN_E_SUCCESS == ret) {
		*client = client_loc;
	} else {
		lockdownd_client_free(client_loc);
	}

	return ret;
}

/** Creates a lockdownd client for the device and starts initial handshake.
 * The handshake consists of query_type, validate_pair, pair and
 * start_session calls.
 *
 * @param phone The device to create a lockdownd client for
 * @param client The pointer to the location of the new lockdownd_client
 * @param label The label to use for communication. Usually the program name
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_client_new_with_handshake(iphone_device_t device, lockdownd_client_t *client, const char *label)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	lockdownd_error_t ret = LOCKDOWN_E_SUCCESS;
	lockdownd_client_t client_loc = NULL;
	char *host_id = NULL;
	char *type = NULL;


	ret = lockdownd_client_new(device, &client_loc, label);

	/* perform handshake */
	if (LOCKDOWN_E_SUCCESS != lockdownd_query_type(client_loc, &type)) {
		debug_info("QueryType failed in the lockdownd client.");
		ret = LOCKDOWN_E_NOT_ENOUGH_DATA;
	} else {
		if (strcmp("com.apple.mobile.lockdown", type)) {
			debug_info("Warning QueryType request returned \"%s\".", type);
		}
		if (type)
			free(type);
	}

	ret = iphone_device_get_uuid(device, &client_loc->uuid);
	if (LOCKDOWN_E_SUCCESS != ret) {
		debug_info("failed to get device uuid.");
	}
	debug_info("device uuid: %s", client_loc->uuid);

	userpref_get_host_id(&host_id);
	if (LOCKDOWN_E_SUCCESS == ret && !host_id) {
		ret = LOCKDOWN_E_INVALID_CONF;
	}

	if (LOCKDOWN_E_SUCCESS == ret && !userpref_has_device_public_key(client_loc->uuid))
		ret = lockdownd_pair(client_loc, host_id);

	/* in any case, we need to validate pairing to receive trusted host status */
	ret = lockdownd_validate_pair(client_loc, host_id);

	if (LOCKDOWN_E_SUCCESS == ret) {
		ret = lockdownd_start_session(client_loc, host_id, NULL, NULL);
		if (LOCKDOWN_E_SUCCESS != ret) {
			ret = LOCKDOWN_E_SSL_ERROR;
			debug_info("SSL Session opening failed.");
		}

		if (host_id) {
			free(host_id);
			host_id = NULL;
		}
	}
	
	if (LOCKDOWN_E_SUCCESS == ret) {
		*client = client_loc;
	} else {
		lockdownd_client_free(client_loc);
	}

	return ret;
}

/** Function used internally by lockdownd_pair() and lockdownd_validate_pair()
 *
 * @param client The lockdown client to pair with.
 * @param host_id The HostID to use for pairing. If NULL is passed, then
 *    the HostID of the current machine is used. A new HostID will be
 *    generated automatically when pairing is done for the first time.
 * @param verb This is either "Pair" or "ValidatePair".
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
static lockdownd_error_t lockdownd_do_pair(lockdownd_client_t client, char *host_id, const char *verb)
{
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;
	plist_t dict = NULL;
	plist_t dict_record = NULL;

	gnutls_datum_t device_cert = { NULL, 0 };
	gnutls_datum_t host_cert = { NULL, 0 };
	gnutls_datum_t root_cert = { NULL, 0 };
	gnutls_datum_t public_key = { NULL, 0 };

	char *host_id_loc = host_id;

	ret = lockdownd_get_device_public_key(client, &public_key);
	if (ret != LOCKDOWN_E_SUCCESS) {
		debug_info("device refused to send public key.");
		return ret;
	}
	debug_info("device public key follows:\n%s", public_key.data);

	ret = lockdownd_gen_pair_cert(public_key, &device_cert, &host_cert, &root_cert);
	if (ret != LOCKDOWN_E_SUCCESS) {
		free(public_key.data);
		return ret;
	}

	if (!host_id) {
		userpref_get_host_id(&host_id_loc);
	}

	/* Setup Pair request plist */
	dict = plist_new_dict();
	dict_record = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"PairRecord", dict_record);

	plist_dict_insert_item(dict_record, "DeviceCertificate", plist_new_data((const char*)device_cert.data, device_cert.size));
	plist_dict_insert_item(dict_record, "HostCertificate", plist_new_data((const char*)host_cert.data, host_cert.size));
	plist_dict_insert_item(dict_record, "HostID", plist_new_string(host_id_loc));
	plist_dict_insert_item(dict_record, "RootCertificate", plist_new_data((const char*)root_cert.data, root_cert.size));

	plist_dict_insert_item(dict, "Request", plist_new_string(verb));

	/* send to iPhone */
	ret = lockdownd_send(client, dict);
	plist_free(dict);
	dict = NULL;

	if (!host_id) {
		free(host_id_loc);
	}

	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	/* Now get iPhone's answer */
	ret = lockdownd_recv(client, &dict);

	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	if (lockdown_check_result(dict, verb) != RESULT_SUCCESS) {
		ret = LOCKDOWN_E_PAIRING_FAILED;
	}

	/* if pairing succeeded */
	if (ret == LOCKDOWN_E_SUCCESS) {
		debug_info("%s success", verb);
		if (!strcmp("Unpair", verb)) {
			/* remove public key from config */
			userpref_remove_device_public_key(client->uuid);
		} else {
			/* store public key in config */
			userpref_set_device_public_key(client->uuid, public_key);
		}
	} else {
		debug_info("%s failure", verb);
		plist_t error_node = NULL;
		/* verify error condition */
		error_node = plist_dict_get_item(dict, "Error");
		if (error_node) {
			char *value = NULL;
			plist_get_string_val(error_node, &value);
			/* the first pairing fails if the device is password protected */
			if (value && !strcmp(value, "PasswordProtected")) {
				ret = LOCKDOWN_E_PASSWORD_PROTECTED;
				free(value);
			}
			plist_free(error_node);
			error_node = NULL;
		}
	}
	plist_free(dict);
	dict = NULL;
	free(public_key.data);
	return ret;
}

/** 
 * Pairs the device with the given HostID.
 * It's part of the lockdownd handshake.
 *
 * @param client The lockdown client to pair with.
 * @param host_id The HostID to use for pairing. If NULL is passed, then
 *    the HostID of the current machine is used. A new HostID will be
 *    generated automatically when pairing is done for the first time.
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_pair(lockdownd_client_t client, char *host_id)
{
	return lockdownd_do_pair(client, host_id, "Pair");
}

/** 
 * Pairs the device with the given HostID. The difference to lockdownd_pair()
 * is that the specified host will become trusted host of the device.
 * It's part of the lockdownd handshake.
 *
 * @param client The lockdown client to pair with.
 * @param host_id The HostID to use for pairing. If NULL is passed, then
 *    the HostID of the current machine is used. A new HostID will be
 *    generated automatically when pairing is done for the first time.
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_validate_pair(lockdownd_client_t client, char *host_id)
{
	return lockdownd_do_pair(client, host_id, "ValidatePair");
}

/** 
 * Unpairs the device with the given HostID and removes the pairing records
 * from the device and host.
 *
 * @param client The lockdown client to pair with.
 * @param host_id The HostID to use for unpairing. If NULL is passed, then
 *    the HostID of the current machine is used.
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_unpair(lockdownd_client_t client, char *host_id)
{
	return lockdownd_do_pair(client, host_id, "Unpair");
}

/**
 * Tells the device to immediately enter recovery mode.
 *
 * @param client The lockdown client
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_enter_recovery(lockdownd_client_t client)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	plist_t dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"Request", plist_new_string("EnterRecovery"));

	debug_info("telling device to enter recovery mode");

	ret = lockdownd_send(client, dict);
	plist_free(dict);
	dict = NULL;

	ret = lockdownd_recv(client, &dict);

	if (lockdown_check_result(dict, "EnterRecovery") == RESULT_SUCCESS) {
		debug_info("success");
		ret = LOCKDOWN_E_SUCCESS;
	}
	plist_free(dict);
	dict = NULL;
	return ret;
}

/**
 * Performs the Goodbye Request to tell the device the communication
 * session is now closed.
 *
 * @param client The lockdown client
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_goodbye(lockdownd_client_t client)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	plist_t dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"Request", plist_new_string("Goodbye"));

	debug_info("called");

	ret = lockdownd_send(client, dict);
	plist_free(dict);
	dict = NULL;

	ret = lockdownd_recv(client, &dict);
	if (!dict) {
		debug_info("did not get goodbye response back");
		return LOCKDOWN_E_PLIST_ERROR;
	}

	if (lockdown_check_result(dict, "Goodbye") == RESULT_SUCCESS) {
		debug_info("success");
		ret = LOCKDOWN_E_SUCCESS;
	}
	plist_free(dict);
	dict = NULL;
	return ret;
}

/** Generates the device certificate from the public key as well as the host
 *  and root certificates.
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_gen_pair_cert(gnutls_datum_t public_key, gnutls_datum_t * odevice_cert,
									   gnutls_datum_t * ohost_cert, gnutls_datum_t * oroot_cert)
{
	if (!public_key.data || !odevice_cert || !ohost_cert || !oroot_cert)
		return LOCKDOWN_E_INVALID_ARG;
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;
	userpref_error_t uret = USERPREF_E_UNKNOWN_ERROR;

	gnutls_datum_t modulus = { NULL, 0 };
	gnutls_datum_t exponent = { NULL, 0 };

	/* now decode the PEM encoded key */
	gnutls_datum_t der_pub_key;
	if (GNUTLS_E_SUCCESS == gnutls_pem_base64_decode_alloc("RSA PUBLIC KEY", &public_key, &der_pub_key)) {

		/* initalize asn.1 parser */
		ASN1_TYPE pkcs1 = ASN1_TYPE_EMPTY;
		if (ASN1_SUCCESS == asn1_array2tree(pkcs1_asn1_tab, &pkcs1, NULL)) {

			ASN1_TYPE asn1_pub_key = ASN1_TYPE_EMPTY;
			asn1_create_element(pkcs1, "PKCS1.RSAPublicKey", &asn1_pub_key);

			if (ASN1_SUCCESS == asn1_der_decoding(&asn1_pub_key, der_pub_key.data, der_pub_key.size, NULL)) {

				/* get size to read */
				int ret1 = asn1_read_value(asn1_pub_key, "modulus", NULL, (int*)&modulus.size);
				int ret2 = asn1_read_value(asn1_pub_key, "publicExponent", NULL, (int*)&exponent.size);

				modulus.data = gnutls_malloc(modulus.size);
				exponent.data = gnutls_malloc(exponent.size);

				ret1 = asn1_read_value(asn1_pub_key, "modulus", modulus.data, (int*)&modulus.size);
				ret2 = asn1_read_value(asn1_pub_key, "publicExponent", exponent.data, (int*)&exponent.size);
				if (ASN1_SUCCESS == ret1 && ASN1_SUCCESS == ret2)
					ret = LOCKDOWN_E_SUCCESS;
			}
			if (asn1_pub_key)
				asn1_delete_structure(&asn1_pub_key);
		}
		if (pkcs1)
			asn1_delete_structure(&pkcs1);
	}

	/* now generate certificates */
	if (LOCKDOWN_E_SUCCESS == ret && 0 != modulus.size && 0 != exponent.size) {

		gnutls_global_init();
		gnutls_datum_t essentially_null = { (unsigned char*)strdup("abababababababab"), strlen("abababababababab") };

		gnutls_x509_privkey_t fake_privkey, root_privkey, host_privkey;
		gnutls_x509_crt_t dev_cert, root_cert, host_cert;

		gnutls_x509_privkey_init(&fake_privkey);
		gnutls_x509_crt_init(&dev_cert);
		gnutls_x509_crt_init(&root_cert);
		gnutls_x509_crt_init(&host_cert);

		if (GNUTLS_E_SUCCESS ==
			gnutls_x509_privkey_import_rsa_raw(fake_privkey, &modulus, &exponent, &essentially_null, &essentially_null,
											   &essentially_null, &essentially_null)) {

			gnutls_x509_privkey_init(&root_privkey);
			gnutls_x509_privkey_init(&host_privkey);

			uret = userpref_get_keys_and_certs(root_privkey, root_cert, host_privkey, host_cert);

			if (USERPREF_E_SUCCESS == uret) {
				/* generate device certificate */
				gnutls_x509_crt_set_key(dev_cert, fake_privkey);
				gnutls_x509_crt_set_serial(dev_cert, "\x00", 1);
				gnutls_x509_crt_set_version(dev_cert, 3);
				gnutls_x509_crt_set_ca_status(dev_cert, 0);
				gnutls_x509_crt_set_activation_time(dev_cert, time(NULL));
				gnutls_x509_crt_set_expiration_time(dev_cert, time(NULL) + (60 * 60 * 24 * 365 * 10));
				gnutls_x509_crt_sign(dev_cert, root_cert, root_privkey);

				if (LOCKDOWN_E_SUCCESS == ret) {
					/* if everything went well, export in PEM format */
					size_t export_size = 0;
					gnutls_datum_t dev_pem = { NULL, 0 };
					gnutls_x509_crt_export(dev_cert, GNUTLS_X509_FMT_PEM, NULL, &export_size);
					dev_pem.data = gnutls_malloc(export_size);
					gnutls_x509_crt_export(dev_cert, GNUTLS_X509_FMT_PEM, dev_pem.data, &export_size);
					dev_pem.size = export_size;

					gnutls_datum_t pem_root_cert = { NULL, 0 };
					gnutls_datum_t pem_host_cert = { NULL, 0 };

					uret = userpref_get_certs_as_pem(&pem_root_cert, &pem_host_cert);

					if (USERPREF_E_SUCCESS == uret) {
						/* copy buffer for output */
						odevice_cert->data = malloc(dev_pem.size);
						memcpy(odevice_cert->data, dev_pem.data, dev_pem.size);
						odevice_cert->size = dev_pem.size;

						ohost_cert->data = malloc(pem_host_cert.size);
						memcpy(ohost_cert->data, pem_host_cert.data, pem_host_cert.size);
						ohost_cert->size = pem_host_cert.size;

						oroot_cert->data = malloc(pem_root_cert.size);
						memcpy(oroot_cert->data, pem_root_cert.data, pem_root_cert.size);
						oroot_cert->size = pem_root_cert.size;

						g_free(pem_root_cert.data);
						g_free(pem_host_cert.data);
					}
				}
			}

			switch(uret) {
			case USERPREF_E_INVALID_ARG:
				ret = LOCKDOWN_E_INVALID_ARG;
				break;
			case USERPREF_E_INVALID_CONF:
				ret = LOCKDOWN_E_INVALID_CONF;
				break;
			case USERPREF_E_SSL_ERROR:
				ret = LOCKDOWN_E_SSL_ERROR;
			default:
				break;
			}
		}
	}

	gnutls_free(modulus.data);
	gnutls_free(exponent.data);

	gnutls_free(der_pub_key.data);

	return ret;
}

/** Starts communication with lockdownd after the iPhone has been paired,
 *  and if the device requires it, switches to SSL mode.
 *
 * @param client The lockdownd client
 * @param host_id The HostID of the computer
 * @param session_id The session_id of the created session
 * @param ssl_enabled Whether SSL communication is used in the session
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_start_session(lockdownd_client_t client, const char *host_id, char **session_id, int *ssl_enabled)
{
	lockdownd_error_t ret = LOCKDOWN_E_SUCCESS;
	plist_t dict = NULL;

	if (!client || !host_id)
		ret = LOCKDOWN_E_INVALID_ARG;

	/* if we have a running session, stop current one first */
	if (client->session_id) {
		lockdownd_stop_session(client, client->session_id);
	}

	/* setup request plist */
	dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"HostID", plist_new_string(host_id));
	plist_dict_insert_item(dict,"Request", plist_new_string("StartSession"));

	ret = lockdownd_send(client, dict);
	plist_free(dict);
	dict = NULL;

	if (ret != LOCKDOWN_E_SUCCESS)
		return ret;

	ret = lockdownd_recv(client, &dict);

	if (!dict)
		return LOCKDOWN_E_PLIST_ERROR;

	if (lockdown_check_result(dict, "StartSession") == RESULT_FAILURE) {
		plist_t error_node = plist_dict_get_item(dict, "Error");
		if (error_node && PLIST_STRING == plist_get_node_type(error_node)) {
			char *error = NULL;
			plist_get_string_val(error_node, &error);
			if (!strcmp(error, "InvalidHostID")) {
				ret = LOCKDOWN_E_INVALID_HOST_ID;
			}
			free(error);
		}
	} else {
		uint8_t use_ssl = 0;

		plist_t enable_ssl = plist_dict_get_item(dict, "EnableSessionSSL");
		if (enable_ssl && (plist_get_node_type(enable_ssl) == PLIST_BOOLEAN)) {
			plist_get_bool_val(enable_ssl, &use_ssl);
		}
		debug_info("Session startup OK");

		if (ssl_enabled != NULL)
			*ssl_enabled = use_ssl;

		/* store session id, we need it for StopSession */
		plist_t session_node = plist_dict_get_item(dict, "SessionID");
		if (session_node && (plist_get_node_type(session_node) == PLIST_STRING)) {
			plist_get_string_val(session_node, &client->session_id);
		}
		if (client->session_id) {
			debug_info("SessionID: %s", client->session_id);
			if (session_id != NULL)
				*session_id = strdup(client->session_id);
		} else {
			debug_info("Failed to get SessionID!");
		}
		debug_info("Enable SSL Session: %s", (use_ssl?"true":"false"));
		if (use_ssl) {
			ret = lockdownd_ssl_start_session(client);
		} else {
			client->ssl_enabled = 0;
			ret = LOCKDOWN_E_SUCCESS;
		}
	}

	plist_free(dict);
	dict = NULL;

	return ret;
}

/** Command to start the desired service
 *
 * @param client The lockdownd client
 * @param service The name of the service to start
 * @param port The port number the service was started on
 
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_start_service(lockdownd_client_t client, const char *service, int *port)
{
	if (!client || !service || !port)
		return LOCKDOWN_E_INVALID_ARG;

	char *host_id = NULL;
	userpref_get_host_id(&host_id);
	if (!host_id)
		return LOCKDOWN_E_INVALID_CONF;
	if (!client->session_id)
		return LOCKDOWN_E_NO_RUNNING_SESSION;

	plist_t dict = NULL;
	uint32_t port_loc = 0;
	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	free(host_id);
	host_id = NULL;

	dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"Request", plist_new_string("StartService"));
	plist_dict_insert_item(dict,"Service", plist_new_string(service));

	/* send to iPhone */
	ret = lockdownd_send(client, dict);
	plist_free(dict);
	dict = NULL;

	if (LOCKDOWN_E_SUCCESS != ret)
		return ret;

	ret = lockdownd_recv(client, &dict);

	if (LOCKDOWN_E_SUCCESS != ret)
		return ret;

	if (!dict)
		return LOCKDOWN_E_PLIST_ERROR;

	ret = LOCKDOWN_E_UNKNOWN_ERROR;
	if (lockdown_check_result(dict, "StartService") == RESULT_SUCCESS) {
		plist_t port_value_node = plist_dict_get_item(dict, "Port");

		if (port_value_node && (plist_get_node_type(port_value_node) == PLIST_UINT)) {
			uint64_t port_value = 0;
			plist_get_uint_val(port_value_node, &port_value);

			if (port_value) {
				port_loc = port_value;
				ret = LOCKDOWN_E_SUCCESS;
			}
			if (port && ret == LOCKDOWN_E_SUCCESS)
				*port = port_loc;
		}
	}
	else
		ret = LOCKDOWN_E_START_SERVICE_FAILED;

	plist_free(dict);
	dict = NULL;
	return ret;
}

/**
 * Activates the device. Only works within an open session.
 * The ActivationRecord plist dictionary must be obtained using the
 * activation protocol requesting from Apple's https webservice.
 *
 * @see http://iphone-docs.org/doku.php?id=docs:protocols:activation
 *
 * @param control The lockdown client
 * @param activation_record The activation record plist dictionary
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_activate(lockdownd_client_t client, plist_t activation_record) 
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	if (!client->session_id)
		return LOCKDOWN_E_NO_RUNNING_SESSION;

	if (!activation_record)
		return LOCKDOWN_E_INVALID_ARG;

	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	plist_t dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"Request", plist_new_string("Activate"));
	plist_dict_insert_item(dict,"ActivationRecord", activation_record);

	ret = lockdownd_send(client, dict);
	plist_free(dict);
	dict = NULL;

	ret = lockdownd_recv(client, &dict);
	if (!dict) {
		debug_info("LOCKDOWN_E_PLIST_ERROR");
		return LOCKDOWN_E_PLIST_ERROR;
	}

	ret = LOCKDOWN_E_ACTIVATION_FAILED;
	if (lockdown_check_result(dict, "Activate") == RESULT_SUCCESS) {
		debug_info("success");
		ret = LOCKDOWN_E_SUCCESS;
	}
	plist_free(dict);
	dict = NULL;

	return ret;
}

/**
 * Deactivates the device, returning it to the locked
 * “Activate with iTunes” screen.
 *
 * @param control The lockdown client
 *
 * @return an error code (LOCKDOWN_E_SUCCESS on success)
 */
lockdownd_error_t lockdownd_deactivate(lockdownd_client_t client)
{
	if (!client)
		return LOCKDOWN_E_INVALID_ARG;

	if (!client->session_id)
		return LOCKDOWN_E_NO_RUNNING_SESSION;

	lockdownd_error_t ret = LOCKDOWN_E_UNKNOWN_ERROR;

	plist_t dict = plist_new_dict();
	plist_dict_add_label(dict, client->label);
	plist_dict_insert_item(dict,"Request", plist_new_string("Deactivate"));

	ret = lockdownd_send(client, dict);
	plist_free(dict);
	dict = NULL;

	ret = lockdownd_recv(client, &dict);
	if (!dict) {
		debug_info("LOCKDOWN_E_PLIST_ERROR");
		return LOCKDOWN_E_PLIST_ERROR;
	}

	ret = LOCKDOWN_E_UNKNOWN_ERROR;
	if (lockdown_check_result(dict, "Deactivate") == RESULT_SUCCESS) {
		debug_info("success");
		ret = LOCKDOWN_E_SUCCESS;
	}
	plist_free(dict);
	dict = NULL;

	return ret;
}

