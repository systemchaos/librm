/**
 * The rm project
 * Copyright (c) 2012-2014 Jan-Michael Brummer
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

/**
 * \file capi/capi.c
 * \brief CAPI routines and main capi functions
 */

#include <rm/rm.h>

#include <capi.h>
#include <fax.h>
#include <phone.h>
#include <isdn-convert.h>

typedef struct {
	RmNetEvent *net_event;

	GIOChannel *channel;
	guint id;
} RmCapiPlugin;

//#define CAPI_DEBUG 1

RmDevice *capi_device = NULL;

/** The current active session */
static struct session *session = NULL;
/** Unique connection id */
static unsigned int id = 1024;
/** cancellable capi loop */
static GCancellable *capi_loop_cancel = NULL;

static GMainContext *main_context = NULL;

static gboolean
emit_connect (gpointer user_data)
{
	RmConnection *connection = user_data;

	rm_object_emit_connection_connect(connection);
	return G_SOURCE_REMOVE;
}

/**
 * \brief Connection ring handler - emit connection-established signal
 * \param connection capi connection structure
 */
void capi_connection_connect(struct capi_connection *capi_connection)
{
	RmConnection *connection = rm_connection_find_by_id(capi_connection->id);

	if (connection) {
		GSource *idle = g_idle_source_new ();
		g_source_set_callback (idle, emit_connect, connection, NULL);
		g_source_attach (idle, main_context);
	}
}

static gboolean
emit_disconnect (gpointer user_data)
{
	RmConnection *connection = user_data;

	rm_object_emit_connection_disconnect(connection);
	return G_SOURCE_REMOVE;
}

/**
 * \brief Connection ring handler - emit connection-terminated signal
 * \param connection capi connection structure
 */
void capi_connection_disconnect(struct capi_connection *capi_connection)
{
	RmConnection *connection = rm_connection_find_by_id(capi_connection->id);

	if (connection) {
		GSource *idle = g_idle_source_new ();
		g_source_set_callback (idle, emit_disconnect, connection, NULL);
		g_source_attach (idle, main_context);
	}
}

static gboolean
emit_incoming (gpointer user_data)
{
	RmConnection *connection = user_data;

	rm_object_emit_connection_incoming(connection);
	return G_SOURCE_REMOVE;
}

/**
 * \brief Connection ring handler
 * \param connection capi connection structure
 */
void connection_ring(struct capi_connection *capi_connection)
{
	RmConnection *connection;
	gchar *target = capi_connection->target;
	gchar *trg = strchr(target, '#');

	connection = rm_connection_add(&capi_phone, capi_connection->id, RM_CONNECTION_TYPE_INCOMING | RM_CONNECTION_TYPE_SOFTPHONE, trg ? trg + 1 : target, capi_connection->source);

	if (connection) {
		connection->priv = capi_connection;

		GSource *idle = g_idle_source_new ();
		g_source_set_callback (idle, emit_incoming, connection, NULL);
		g_source_attach (idle, main_context);
	}
}

/**
 * \brief Connection code handler
 * \param connection capi connection structure
 * \param code dtmf code
 */
void connection_code(struct capi_connection *connection, gint code)
{
	g_debug("connection_code(): code 0x%x", code);
}

static gboolean
emit_status (gpointer user_data)
{
	RmConnection *connection = user_data;

	g_print ("%s: FIXME, STATUS MISSING\n", __FUNCTION__);
	rm_object_emit_connection_status(0, connection);
	return G_SOURCE_REMOVE;
}

/**
 * \brief Connection status handlers - emits connection-status signal
 * \param connection capi connection structure
 * \param status status code
 */
void connection_status(struct capi_connection *capi_connection, gint status)
{
	RmConnection *connection = rm_connection_find_by_id(capi_connection->id);

	if (connection) {
		GSource *idle = g_idle_source_new ();
		g_source_set_callback (idle, emit_status, connection, NULL);
		g_source_attach (idle, main_context);
	}
}

/**
 * \brief Dump capi error (UNUSED)
 * \param error capi error number
 */
static void capi_error(long error)
{
	if (error != 0) {
		g_debug("->Error: 0x%lX", error);
		if (error == 0x3301) {
			g_warning("Protocol Error Layer 1");
		} else if (error == 0x2001) {
			g_warning("Message not supported in current state");
		}
	}
}

/**
 * \brief Set connection type, transfer and cleanup routine, b3 informations
 * \param connection capi connection
 * \param type connection type
 * \return error code: 0 on success, otherwise error
 */
static int capi_connection_set_type(struct capi_connection *connection, int type)
{
	int result = 0;

	/* Set type */
	connection->type = type;

	/* Set informations depending on type */
	switch (type) {
	case SESSION_PHONE:
		connection->init_data = capi_phone_init_data;
		connection->data = capi_phone_data;
		connection->clean = NULL;
		connection->early_b3 = 1;
		break;
	case SESSION_FAX:
		connection->init_data = capi_fax_init_data;
		connection->data = capi_fax_data;
		connection->clean = capi_fax_clean;
		connection->early_b3 = 0;
		break;
	default:
		g_debug("Unhandled session type!!");
		result = -1;
		break;
	}

	return result;
}

/**
 * \brief Return free capi connection index
 * \return free connection index or -1 on error
 */
struct capi_connection *capi_get_free_connection(void)
{
	int i;

	if (!session) {
		return NULL;
	}

	for (i = 0; i < CAPI_CONNECTIONS; i++) {
		if (session->connection[i].plci == 0 && session->connection[i].ncci == 0) {
			session->connection[i].id = id++;
			session->connection[i].state = STATE_IDLE;
			return &session->connection[i];
		}
	}

	return NULL;
}

/**
 * \brief Free capi connection
 * \param connection capi connection
 * \return error code
 */
static int capi_set_free(struct capi_connection *connection)
{
	/* reset connection */
	if (connection->priv != NULL) {
		if (connection->clean) {
			connection->clean(connection);
		} else {
			g_debug("Warning: Private data but no clean function");
		}
	}

	memset(connection, 0, sizeof(struct capi_connection));

	return 0;
}

/**
 * \brief Terminate selected connection
 * \param connection connection we want to terminate
 */
void capi_hangup(struct capi_connection *connection)
{
	_cmsg cmsg1;
	guint info = 0;

	if (connection == NULL) {
		return;
	}

	switch (connection->state) {
	case STATE_CONNECT_WAIT:
	case STATE_CONNECT_ACTIVE:
	case STATE_DISCONNECT_B3_REQ:
	case STATE_DISCONNECT_B3_WAIT:
	case STATE_DISCONNECT_ACTIVE:
	case STATE_INCOMING_WAIT:
		g_debug("REQ: DISCONNECT - plci %ld", connection->plci);

		isdn_lock();
		info = DISCONNECT_REQ(&cmsg1, session->appl_id, 1, connection->plci, NULL, NULL, NULL, NULL);
		isdn_unlock();

		if (info != 0) {
			connection->state = STATE_IDLE;
			connection_status(connection, info);
		} else {
			connection->state = STATE_DISCONNECT_ACTIVE;
		}
		break;
	case STATE_CONNECT_B3_WAIT:
	case STATE_CONNECTED:
		g_debug("REQ: DISCONNECT_B3 - ncci %ld", connection->ncci);

		isdn_lock();
		info = DISCONNECT_B3_REQ(&cmsg1, session->appl_id, 1, connection->ncci, NULL);
		isdn_unlock();

		if (info != 0) {
			/* retry with disconnect on whole connection */
			isdn_lock();
			info = DISCONNECT_REQ(&cmsg1, session->appl_id, 1, connection->plci, NULL, NULL, NULL, NULL);
			isdn_unlock();
			if (info != 0) {
				connection->state = STATE_IDLE;
				connection_status(connection, info);
			} else {
				connection->state = STATE_DISCONNECT_ACTIVE;
			}
		} else {
			connection->state = STATE_DISCONNECT_B3_REQ;
		}
		break;
	case STATE_RINGING:
		/* reject the call */
		g_debug("RESP: CONNECT - plci %ld", connection->plci);

		isdn_lock();
		info = CONNECT_RESP(&cmsg1, session->appl_id, session->message_number++, connection->plci, 3, 0, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
		isdn_unlock();
		connection->state = STATE_IDLE;
		if (info != 0) {
			connection_status(connection, info);
		}

		break;
	case STATE_IDLE:
		break;
	default:
		g_debug("Unexpected state 0x%x on disconnect", connection->state);
		break;
	}
}

/**
 * \brief Call number from target using CIP value
 * \param controller controller id
 * \param src_no source number
 * \param trg_no target number
 * \param call_anonymous call anonymous flag
 * \param type connection type
 * \param cip caller id
 * \return error code
 */
struct capi_connection *capi_call(
	unsigned controller,
	const char *src_no,
	const char *trg_no,
	unsigned call_anonymous,
	unsigned type,
	unsigned cip,
	_cword b1_protocol,
	_cword b2_protocol,
	_cword b3_protocol,
	_cstruct b1_configuration,
	_cstruct b2_configuration,
	_cstruct b3_configuration)
{
	_cmsg cmsg;
	unsigned char called_party_number[70];
	unsigned char calling_party_number[70];
	unsigned char bc[4];
	unsigned char llc[3];
	unsigned char hlc[3];
	struct capi_connection *connection = NULL;
	int err = 0;
	int intern = (trg_no[0] == '*') || (trg_no[0] == '#');

	if (!session) {
		return NULL;
	}

	if (src_no == NULL || strlen(src_no) < 1 || trg_no == NULL || strlen(trg_no) < 1) {
		g_debug("Wrong phone numbers!");
		return connection;
	}

	/* Say hello */
	g_debug("REQ: CONNECT (%s->%s)", src_no, trg_no);

	/* get free connection */
	connection = capi_get_free_connection();
	if (connection == NULL) {
		return connection;
	}

	/* set connection type */
	capi_connection_set_type(connection, type);

	/* TargetNo */
	called_party_number[0] = 1 + strlen(trg_no);
	called_party_number[1] = 0x80;
	strncpy((char*)&called_party_number[2], trg_no, sizeof(called_party_number) - 3);

	/* MSN */
	calling_party_number[1] = 0x00;
	calling_party_number[2] = 0x80;

	if (call_anonymous) {
		calling_party_number[2] = 0xA0;
	}

	if (intern) {
		calling_party_number[0] = 2 + 5;
		strncpy((char*)&calling_party_number[3], "**981", sizeof(calling_party_number) - 4);

		strncpy((char*)bc, "\x03\xE0\x90\xA3", sizeof(bc));
	} else {
		calling_party_number[0] = 2 + strlen(src_no);
		strncpy((char*)&calling_party_number[3], src_no, sizeof(calling_party_number) - 4);

		memset(bc, 0, sizeof(bc));
	}
	strncpy((char*)llc, "\x02\x80\x90", sizeof(llc));

	if (cip == 0x04) {
		strncpy((char*)hlc, "\x02\x91\x81", sizeof(hlc));
	} else if (cip == 0x11) {
		//strncpy((char *) hlc, "\x02\x91\x84", sizeof(hlc));
		//strncpy((char *) bc, "\x03\x90\x90\xA3", sizeof(bc));
		memset(bc, 0, sizeof(bc));
		memset(llc, 0, sizeof(llc));
		memset(hlc, 0, sizeof(hlc));
	}

	/* Request connect */
	isdn_lock();
	err = CONNECT_REQ(
		/* CAPI Message */
		&cmsg,
		/* Application ID */
		session->appl_id,
		/* Message Number */
		0,
		/* Controller */
		controller,
		/* CIP (Voice/Fax/...) */
		cip,
		/* Called party number */
		(unsigned char*)called_party_number,
		/* Calling party number */
		(unsigned char*)calling_party_number,
		/* NULL */
		NULL,
		/* NULL */
		NULL,
		/* B1 Protocol */
		b1_protocol,
		/* B2 Protocol */
		b2_protocol,
		/* B3 Protocol */
		b3_protocol,
		/* B1 Configuration */
		b1_configuration,
		/* B2 Confguration */
		b2_configuration,
		/* B3 Configuration */
		b3_configuration,
		/* Rest... */
		NULL,
		/* BC */
		(unsigned char*)bc,
		/* LLC */
		(unsigned char*)llc,
		/* HLC */
		(unsigned char*)hlc,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL);
	isdn_unlock();

	/* Error? */
	if (err) {
		g_debug("(%d) Unable to send CONNECT_REQ!", err);
		capi_error(err);
		capi_set_free(connection);
		connection = NULL;
		return connection;
	}

	connection->target = g_strdup(trg_no);
	connection->source = g_strdup(src_no);

	return connection;
}

/**
 * \brief Pickup an incoming call
 * \param connection incoming capi connection
 * \param type handle connection as this type
 * \return error code: 0 on success, otherwise error
 */
int capi_pickup(struct capi_connection *connection, int type)
{
	_cmsg message;
	unsigned char local_num[4];
	struct session *session = capi_get_session();

	capi_connection_set_type(connection, type);

	if (connection->state != STATE_RINGING) {
		g_debug("CAPI Pickup called, even if not ringing");
		return -1;
	} else {
		local_num[0] = 0x00;
		local_num[1] = 0x00;
		local_num[2] = 0x00;
		local_num[3] = 0x00;

		isdn_lock();
		g_debug("RESP: CAPI_CONNECT_RESP - plci %ld", connection->plci);
		CONNECT_RESP(&message, session->appl_id, session->message_number++, connection->plci, 0, 1, 1, 0, 0, 0, 0, &local_num[0], NULL, NULL, NULL, NULL, NULL, NULL, NULL);
		isdn_unlock();

		/* connection initiated, wait for CONNECT_ACTIVE_IND */
		connection->state = STATE_INCOMING_WAIT;
	}

	return 0;
}

/**
 * \brief Get the calling party number on CAPI_CONNECT
 * \param cmsg CAPI message
 * \param number buffer to store number
 */
static void capi_get_source_no(_cmsg *cmsg, char number[256])
{
	unsigned char *pnX = CONNECT_IND_CALLINGPARTYNUMBER(cmsg);
	unsigned int len = 0;

	memset(number, 0, 256);

	if (pnX == NULL) {
		pnX = INFO_IND_INFOELEMENT(cmsg);

		if (pnX != NULL) {
			len = (int)pnX[0];
		}
	} else {
		len = *CONNECT_IND_CALLINGPARTYNUMBER(cmsg);
	}

	if (len <= 1) {
		strcpy(number, "unknown");
	} else {
		/* get number */
		if (pnX[2] & 128) {
			number[strlen(number) + pnX[0] - 1] = 0;
			number[strlen(number) + pnX[0] - 2] = 0;

			memcpy(number + strlen(number), pnX + 3, (size_t)(pnX[0] - 2));
		} else {
			number[strlen(number) + pnX[0]] = 0;
			number[strlen(number) + pnX[0] - 1] = 0;
			memcpy(number + strlen(number), pnX + 2, (size_t)(pnX[0] - 1));
		}
	}

	if (!strlen(number)) {
		strcpy(number, "anonymous");
	}
}

/**
 * \brief Get the called party number on CAPI_CONNECT
 * \param cmsg CAPI message
 * \param number buffer to store number
 */
static void capi_get_target_no(_cmsg *cmsg, char number[256])
{
	unsigned char *x = CONNECT_IND_CALLEDPARTYNUMBER(cmsg);
	unsigned int len = 0;

	memset(number, 0, 256);

	if (x == NULL) {
		x = INFO_IND_INFOELEMENT(cmsg);
		if (x != NULL) {
			len = (int)x[0];
		}
	} else {
		len = *CONNECT_IND_CALLEDPARTYNUMBER(cmsg);

		if (CONNECT_IND_CALLEDPARTYNUMBER(cmsg)[0] == 0) {
			len = 0;
		}
	}

	if (len <= 1) {
		strcpy(number, "unknown");
	} else {
		number[strlen(number) + (size_t)x[0]] = 0;
		number[strlen(number) + (size_t)x[0] - 1] = 0;
		memcpy(number + strlen(number), x + 2, (size_t)(x[0] - 1));
	}

	if (!strlen(number)) {
		strcpy(number, "anonymous");
	}
}

/**
 * \brief Find capi connection by PLCI
 * \param plci plci
 * \return capi connection or NULL on error
 */
static struct capi_connection *capi_find_plci(int plci)
{
	int index;

	for (index = 0; index < CAPI_CONNECTIONS; index++) {
		if (session->connection[index].plci == plci) {
			return &session->connection[index];
		}
	}

	return NULL;
}

/**
 * \brief Find newly created capi connection
 * \return capi connection or NULL on error
 */
static struct capi_connection *capi_find_new(void)
{
	int index;

	for (index = 0; index < CAPI_CONNECTIONS; index++) {
		if (session->connection[index].plci == 0 && session->connection[index].type != 0) {
			return &session->connection[index];
		}
	}

	return NULL;
}

/**
 * \brief Find capi connection by NCCI
 * \param ncci ncci
 * \return capi connection or NULL on error
 */
static struct capi_connection *capi_find_ncci(int ncci)
{
	int index;

	for (index = 0; index < CAPI_CONNECTIONS; index++) {
		if (session->connection[index].ncci == ncci) {
			return &session->connection[index];
		}
	}

	return NULL;
}

/**
 * \brief Close capi
 * \return error code
 */
static int capi_close(void)
{
	int index;

	if (session != NULL && session->appl_id != -1) {
		for (index = 0; index < CAPI_CONNECTIONS; index++) {
			if (session->connection[index].plci != 0 || session->connection[index].ncci != 0) {
				capi_hangup(&session->connection[index]);
				g_usleep(25);
			}
		}

		CAPI20_RELEASE(session->appl_id);
		session->appl_id = -1;
	}

	return 0;
}

/**
 * \brief CAPI respond connection
 * \param plci plci
 * \param nIgnore ignore connection
 */
static void capi_resp_connection(int plci, unsigned int ignore)
{
	_cmsg cmsg1;

	if (!ignore) {
		/* *ring* */
		g_debug("REQ: ALERT - plci %d", plci);
		isdn_lock();
		ALERT_REQ(&cmsg1, session->appl_id, 0, plci, NULL, NULL, NULL, NULL, NULL);
		isdn_unlock();
	} else {
		/* ignore */
		isdn_lock();
		CONNECT_RESP(&cmsg1, session->appl_id, session->message_number++, plci, ignore, 1, 1, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
		isdn_unlock();
	}
}

/**
 * \brief Enable DTMF support.
 * \param isdn isdn device structure.
 * \param ncci NCCI
 */
static void capi_enable_dtmf(struct capi_connection *connection)
{
	_cmsg message;
	_cbyte facility[11];

	/* Message length */
	facility[0] = 10;
	/* DTMF ON: 0x01, DTMF OFF: 0x02 */
	facility[1] = (_cbyte)0x01;
	/* NULL */
	facility[2] = 0x00;
	/* DTMF Duration */
	facility[3] = 0x40;
	/* NULL */
	facility[4] = 0x00;
	/* DTMF Duration */
	facility[5] = 0x40;
	/* NULL */
	facility[6] = 0x00;
	/* NULL */
	facility[7] = 0x00;
	/* 2 */
	facility[8] = 0x02;
	/* NULL */
	facility[9] = 0x00;
	/* NULL */
	facility[10] = 0x00;

	g_debug("Enable DTMF for PLCI %ld", connection->plci);

	/* 0x01 = DTMF selector */
	isdn_lock();
	FACILITY_REQ(&message, session->appl_id, 0 /*isdn->message_number++*/, connection->plci, 0x01, (unsigned char*)facility);
	isdn_unlock();
}

/**
 * \brief Signal DTMF code to application
 * \param connection active capi connection
 * \param dtmf DTMF code
 */
static void capi_get_dtmf_code(struct capi_connection *connection, unsigned char dtmf)
{
	if (dtmf == 0) {
		return;
	}

	if (!isdigit(dtmf)) {
		if (dtmf != '#' && dtmf != '*') {
			return;
		}
	}

	connection_code(connection, dtmf);
}

/**
 * \brief Send DTMF to remote
 * \param connection active capi connection
 * \param dtmf DTMF code we want to send
 */
void capi_send_dtmf_code(struct capi_connection *connection, unsigned char dtmf)
{
	_cmsg message;
	_cbyte facility[32];

	g_debug("dtmf: %c", dtmf);

	/* Message length */
	facility[0] = 0x08;
	/* Send DTMF 0x03 */
	facility[1] = (_cbyte)0x03;
	/* NULL */
	facility[2] = 0x00;
	/* DTMF Duration */
	facility[3] = 0x30;
	/* NULL */
	facility[4] = 0x00;
	/* DTMF Duration */
	facility[5] = 0x30;
	/* NULL */
	facility[6] = 0x00;
	/* NULL */
	facility[7] = 0x01;
	/* NULL */
	facility[8] = dtmf;

	g_debug("Sending DTMF code for NCCI %ld", connection->ncci);

	/* 0x01 = DTMF selector */
	isdn_lock();
	FACILITY_REQ(&message, session->appl_id, 0 /*isdn->message_number++*/, connection->ncci, 0x01, (unsigned char*)facility);
	isdn_unlock();
}

/**
 * \brief Send display message to remote
 * \param connection active capi connection
 * \param pnDtmf text we want to send
 */
void capi_send_display_message(struct capi_connection *connection, char *text)
{
	_cmsg message;
	_cbyte facility[62 + 3];
	int len = 31;

	g_debug("Sending text: '%s'", text);
	memset(facility, 0, sizeof(facility));

	if (strlen(text) < 31) {
		len = strlen(text);
	}

	/* Complete length */
	facility[0] = len + 2;
	/* Send DTMF 0x03 */
	facility[1] = (_cbyte)0x28;
	/* Message length */
	facility[0] = len;

	strncpy((char*)facility + 3, text, len);

	isdn_lock();
	INFO_REQ(&message, session->appl_id, 0, connection->plci, (unsigned char*)"", (unsigned char*)"", (unsigned char*)"", (unsigned char*)"", (unsigned char*)facility, NULL);
	isdn_unlock();
}

/**
 * \brief CAPI indication
 * \param capi_message capi message structure
 * \return error code
 */
static int capi_indication(_cmsg capi_message)
{
	_cmsg cmsg1;
	int plci = -1;
	int ncci = -1;
	char source_phone_number[256];
	char target_phone_number[256];
	int cip = -1;
	struct capi_connection *connection = NULL;
	int reject = 0;
	int info;
	char info_element[128];
	int index;
	int nTmp;
	unsigned char *ncpi;

	switch (capi_message.Command) {
	case CAPI_CONNECT:
		/* CAPI_CONNECT - Connect indication when called from remote phone */
		plci = CONNECT_IND_PLCI(&capi_message);
		cip = CONNECT_IND_CIPVALUE(&capi_message);

		capi_get_source_no(&capi_message, source_phone_number);
		capi_get_target_no(&capi_message, target_phone_number);

		g_debug("IND: CAPI_CONNECT - plci %d, source %s, target %s, cip: %d", plci, (source_phone_number), (target_phone_number), cip);

		reject = 0;

		if (cip != 16 && cip != 1 && cip != 4 && cip != 17) {
			/* not telephony nor fax, ignore */
			reject = 1;
		}

#ifdef ACCEPT_INTERN
		if (reject && strncmp(source_phone_number, "**", 2)) {
#else
		if (reject) {
#endif
			/* Ignore */
			g_debug("IND: CAPI_CONNECT - plci: %d, ncci: %d - IGNORING (%s <- %s)", plci, 0, target_phone_number, source_phone_number);
			capi_resp_connection(plci, 1);
		} else {
			connection = capi_get_free_connection();

			connection->type = SESSION_NONE;
			connection->state = STATE_RINGING;
			connection->plci = plci;
			connection->source = g_strdup(source_phone_number);
			connection->target = g_strdup(target_phone_number);

			capi_resp_connection(plci, 0);
		}

		break;

	/* CAPI_CONNECT_ACTIVE - Active */
	case CAPI_CONNECT_ACTIVE:
		plci = CONNECT_ACTIVE_IND_PLCI(&capi_message);

		g_debug("IND: CAPI_CONNECT_ACTIVE - plci %d", plci);

		g_debug("RESP: CAPI_CONNECT_ACTIVE - plci %d", plci);
		isdn_lock();
		CONNECT_ACTIVE_RESP(&cmsg1, session->appl_id, session->message_number++, plci);
		isdn_unlock();

		connection = capi_find_plci(plci);
		if (connection == NULL) {
			g_debug("Wrong PLCI 0x%x", plci);
			break;
		}
		g_debug("IND: CAPI_CONNECT_ACTIVE - connection: %d, plci: %ld", connection->id, connection->plci);

		/* Request B3 when sending... */
		if (connection->state == STATE_INCOMING_WAIT) {
			connection->connect_time = time(NULL);

			connection->state = STATE_CONNECT_ACTIVE;
			if (connection->type == SESSION_PHONE) {
				RmAudio *audio = rm_profile_get_audio(rm_profile_get_active());

				connection->audio = rm_audio_open(audio, NULL);
				if (!connection->audio) {
					g_warning("Could not open audio. Hangup");
					capi_hangup(connection);
					connection->audio = NULL;
				}
			}
		} else if (connection->early_b3 == 0) {
			g_debug("REQ: CONNECT_B3 - nplci %d", plci);
			isdn_lock();
			info = CONNECT_B3_REQ(&cmsg1, session->appl_id, 0, plci, 0);
			isdn_unlock();

			if (info != 0) {
				connection_status(connection, info);
				/* initiate hangup on PLCI */
				capi_hangup(connection);
			} else {
				/* wait for CONNECT_B3, then announce result to application via callback */
				connection->connect_time = time(NULL);

				connection->state = STATE_CONNECT_ACTIVE;
				if (connection->type == SESSION_PHONE) {
					RmAudio *audio = rm_profile_get_audio(rm_profile_get_active());

					connection->audio = rm_audio_open(audio, NULL);
					if (!connection->audio) {
						g_warning("Could not open audio. Hangup");
						rm_object_emit_message("Audio error", "Could not open audio. Hangup");
						capi_hangup(connection);
						connection->audio = NULL;
					}
				}
			}
		}

		break;

	/* CAPI_CONNECT_B3 - data connect */
	case CAPI_CONNECT_B3:
		g_debug("IND: CAPI_CONNECT_B3");
		ncci = CONNECT_B3_IND_NCCI(&capi_message);
		plci = ncci & 0x0000ffff;

		connection = capi_find_plci(plci);
		if (connection == NULL) {
			break;
		}

		/* Answer the info message */
		isdn_lock();
		CONNECT_B3_RESP(&cmsg1, session->appl_id, session->message_number++, ncci, 0, (_cstruct)NULL);
		isdn_unlock();

		if (connection->state == STATE_CONNECT_ACTIVE) {
			connection->ncci = ncci;
			connection->state = STATE_CONNECT_B3_WAIT;
		} else {
			/* Wrong connection state for B3 connect, trigger disconnect */
			capi_hangup(connection);
		}
		break;

	/* CAPI_CONNECT_B3_ACTIVE - data active */
	case CAPI_CONNECT_B3_ACTIVE:
		g_debug("IND: CAPI_CONNECT_B3_ACTIVE");
		ncci = CONNECT_B3_ACTIVE_IND_NCCI(&capi_message);
		plci = ncci & 0x0000ffff;
		ncpi = capi_message.NCPI;

		connection = capi_find_plci(plci);
		if (connection == NULL) {
			g_debug("Wrong NCCI, got 0x%x", ncci);
			break;
		}

		connection->ncci = ncci;
		if (1) {
			int len = ncpi[0] + 1;
			int tmp;
			g_debug("NCPI len: %d", len);
			connection->ncpi = g_malloc0(len);
			memcpy(connection->ncpi, ncpi, len);
			for (tmp = 0; tmp < len; tmp++) {
				g_debug("%2.2x <-> %c", connection->ncpi[tmp], connection->ncpi[tmp]);
			}
		}

		isdn_lock();
		CONNECT_B3_ACTIVE_RESP(&cmsg1, session->appl_id, session->message_number++, ncci);
		isdn_unlock();

		connection->state = STATE_CONNECTED;

		capi_enable_dtmf(connection);
		if (connection->init_data) {
			connection->init_data(connection);
		}

		/* notify application about successful call establishment */
		capi_connection_connect(connection);
		break;

	/* CAPI_DATA_B3 - data - receive/send */
	case CAPI_DATA_B3:
#ifdef CAPI_DEBUG
		g_debug("IND: CAPI_DATA_B3");
#endif
		ncci = DATA_B3_IND_NCCI(&capi_message);

		connection = capi_find_ncci(ncci);
		g_assert(connection != NULL);

#ifdef CAPI_DEBUG
		g_debug("IND: CAPI_DATA_B3 - connection: %d, plci: %ld, ncci: %ld", connection->id, connection->plci, connection->ncci);
#endif
		connection->data(connection, capi_message);
		isdn_lock();
		DATA_B3_RESP(&cmsg1, session->appl_id, session->message_number++, connection->ncci, DATA_B3_IND_DATAHANDLE(&capi_message));
		isdn_unlock();

		break;

	/* CAPI_FACILITY - Facility (DTMF) */
	case CAPI_FACILITY:
		g_debug("IND: CAPI_FACILITY");
		ncci = CONNECT_B3_IND_NCCI(&capi_message);
		plci = ncci & 0x0000ffff;

		isdn_lock();
		FACILITY_RESP(&cmsg1, session->appl_id, session->message_number++, plci, FACILITY_IND_FACILITYSELECTOR(&capi_message), FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message));
		isdn_unlock();

		connection = capi_find_plci(plci);
		if (connection == NULL) {
			break;
		}

		g_debug("IND: CAPI_FACILITY %d", FACILITY_IND_FACILITYSELECTOR(&capi_message));
		switch (FACILITY_IND_FACILITYSELECTOR(&capi_message)) {
		case 0x0001:
			/* DTMF */
			capi_get_dtmf_code(connection, (unsigned char)FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[1]);
			break;
		case 0x0003:
			/* Supplementary Services */
			nTmp = (unsigned int)(((unsigned int)FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[1]) | ((unsigned int)FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[3] << 8));

			g_debug("%x %x %x %x %x %x", FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[0],
				FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[1],
				FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[2],
				FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[3],
				FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[4],
				FACILITY_IND_FACILITYINDICATIONPARAMETER(&capi_message)[5]);
			if (nTmp == 0x0203) {
				/* Retrieve */
				g_debug("FACILITY: RETRIEVE");
				isdn_lock();
				info = CONNECT_B3_REQ(&cmsg1, session->appl_id, 0, plci, 0);
				isdn_unlock();

				if (info != 0) {
					connection_status(connection, info);
					/* initiate hangup on PLCI */
					capi_hangup(connection);
				} else {
					/* wait for CONNECT_B3, then announce result to application via callback */
					connection->state = STATE_CONNECT_ACTIVE;
				}
			} else if (nTmp == 0x0202) {
				/* Hold */
				g_debug("FACILITY: HOLD");
			} else {
				g_debug("FACILITY: Unknown %x", nTmp);
			}
			break;
		default:
			g_debug("Unhandled facility selector!! %x", FACILITY_IND_FACILITYSELECTOR(&capi_message));
			break;
		}
		break;

	/* CAPI_INFO */
	case CAPI_INFO:
		plci = INFO_IND_PLCI(&capi_message);
		info = INFO_IND_INFONUMBER(&capi_message);

		/* Respond to INFO */
		isdn_lock();
		INFO_RESP(&cmsg1, session->appl_id, session->message_number++, plci);
		isdn_unlock();

		memset(info_element, 0, sizeof(info_element));
		for (index = 0; index < sizeof(info_element); index++) {
			info_element[index] = INFO_IND_INFOELEMENT(&capi_message)[index];
		}

		switch (info) {
		case 0x0008:
			/* Cause */
			g_debug("CAPI_INFO - CAUSE");
			g_debug("Hangup cause: 0x%x", info_element[2] & 0x7F);
			break;
		case 0x00014:
			/* Call state */
			g_debug("CAPI_INFO - CALL STATE (0x%02x)", info_element[0]);
			break;
		case 0x0018:
			/* Channel identification */
			g_debug("CAPI_INFO - CHANNEL IDENTIFICATION (0x%02x)", info_element[0]);
			break;
		case 0x001C:
			/* Facility Q.932 */
			g_debug("CAPI_INFO - FACILITY Q.932");
			break;
		case 0x001E:
			/* Progress Indicator */
			g_debug("CAPI_INFO - PROGRESS INDICATOR (0x%02x)", info_element[0]);
			if (info_element[0] < 2) {
				g_debug("CAPI_INFO - Progress description missing");
			} else {
				switch (info_element[2] & 0x7F) {
				case 0x01:
					g_debug("CAPI_INFO - Not end-to-end ISDN");
					break;
				case 0x02:
					g_debug("CAPI_INFO - Destination is non ISDN");
					break;
				case 0x03:
					g_debug("CAPI_INFO - Origination is non ISDN");
					break;
				case 0x04:
					g_debug("CAPI_INFO - Call returned to ISDN");
					break;
				case 0x05:
					g_debug("CAPI_INFO - Interworking occurred");
					break;
				case 0x08:
					g_debug("CAPI_INFO - In-band information available");
					break;
				default:
					g_debug("CAPI_INFO - Unknown progress description 0x%02x", info_element[2]);
					break;
				}
			}
			break;
		case 0x0027:
			/* Notification Indicator */
			switch ((unsigned int)info_element[0]) {
			case 0:
				g_debug("CAPI_INFO - NI - CALL SUSPENDED (%d)", info_element[0]);
				break;
			case 1:
				g_debug("CAPI_INFO - NI - CALL RESUMED (%d)", info_element[0]);
				break;
			case 2:
				g_debug("CAPI_INFO - NI - BEARER SERVICE CHANGED (%d)", info_element[0]);
				break;
			case 0xF9:
				g_debug("CAPI_INFO - NI - PUT ON HOLD (%d)", info_element[0]);
				break;
			case 0xFA:
				g_debug("CAPI_INFO - NI - RETRIEVED FROM HOLD (%d)", info_element[0]);
				break;
			default:
				g_debug("CAPI_INFO - NI - UNKNOWN (%d)", info_element[0]);
				break;
			}
			break;
		case 0x0028:
			/* Display */
			g_debug("CAPI_INFO - DISPLAY");
			break;
		case 0x0029:
			/* DateTime */
			g_debug("CAPI_INFO - DATE/TIME (%02d/%02d/%02d %02d:%02d)",
				info_element[0], info_element[1], info_element[2], info_element[3], info_element[4]);
			break;
		case 0x002C:
			/* Keypad facility */
			g_debug("CAPI_INFO - KEYPAD FACILITY");
			break;
		case 0x006C: {
			/* Caller party number */
			//int tmp;

			//g_debug("CAPI_INFO - CALLER PARTY NUMBER (%.%s)", info_element[0], &info_element[1]);
			g_debug("CAPI_INFO - CALLER PARTY NUMBER");

			/*for (tmp = 0; tmp < sizeof(info_element); tmp++) {
			        g_debug("InfoElement (%d): %x (%c)", tmp, info_element[tmp], info_element[tmp]);
			   }*/
			break;
		}
		case 0x0070:
			/* Called Party Number */
			g_debug("CAPI_INFO - CALLED PARTY NUMBER");
			break;
		case 0x0074:
			/* Redirecting Number */
			g_debug("CAPI_INFO - REDIRECTING NUMBER");
			break;
		case 0x00A1:
			/* Sending complete */
			g_debug("CAPI_INFO - SENDING COMPLETE");
			break;
		case 0x4000:
			/* Charge in Units */
			g_debug("CAPI_INFO - CHARGE IN UNITS");
			break;
		case 0x4001:
			/* Charge in Currency */
			g_debug("CAPI_INFO - CHARGE IN CURRENCY");
			break;
		case 0x8001:
			/* Alerting */
			g_debug("CAPI_INFO - ALERTING (Setup early...)");
			break;
		case 0x8002:
			/* Call Proceeding */
			g_debug("CAPI_INFO - CALL PROCEEDING");
			break;
		case 0x8003:
			/* Progress */
			g_debug("CAPI_INFO - PROGRESS (Setup early...)");
			break;
		case 0x8005:
			/* Setup */
			g_debug("CAPI_INFO - SETUP");
			break;
		case 0x8007:
			/* Connect */
			g_debug("CAPI_INFO - CONNECT");
			break;
		case 0x800D:
			/* Setup ACK */
			g_debug("CAPI_INFO - SETUP ACK");
			break;
		case 0x800F:
			/* Connect ACK */
			g_debug("CAPI_INFO - CONNECT ACK");
			break;
		case 0x8045:
			/* Disconnect */
			connection = capi_find_plci(plci);

			g_debug("CAPI_INFO - DISCONNECT");

			if (connection == NULL) {
				break;
			}

			if (connection->state == STATE_CONNECTED && connection->type == SESSION_FAX) {
				g_debug("CAPI_INFO - Fax mode and we are connected, wait for DISCONNECT_B3_IND");
				break;
			}

			capi_hangup(connection);
			break;
		case 0x804D:
			/* Release */
			g_debug("CAPI_INFO - RELEASE");
			break;
		case 0x805A:
			/* Release Complete */
			g_debug("CAPI_INFO - RELEASE COMPLETE");
			break;
		case 0x8062:
			/* Facility */
			g_debug("CAPI_INFO - FACILITY");
			break;
		case 0x806E:
			/* Notify */
			g_debug("CAPI_INFO - NOTIFY");
			break;
		case 0x807B:
			/* Information */
			g_debug("CAPI_INFO - INFORMATION");
			break;
		case 0x807D:
			/* status */
			g_debug("CAPI_INFO - STATUS");
			break;
		default:
			/* Unknown */
			g_debug("CAPI_INFO - UNKNOWN INFO (0x%02x)", info);
			break;
		}

		connection = capi_find_plci(plci);
		if (connection != NULL) {
			if (connection->early_b3 != 0 && connection->state == STATE_CONNECT_WAIT && info == 0x001E) {
				g_debug("REQ: CONNECT_B3 - Early-B3");

				isdn_lock();
				CONNECT_B3_REQ(&cmsg1, session->appl_id, 0, plci, 0);
				isdn_unlock();

				connection->connect_time = time(NULL);
				if (connection->type == SESSION_PHONE) {
					RmAudio *audio = rm_profile_get_audio(rm_profile_get_active());

					connection->audio = rm_audio_open(audio, NULL);
					if (!connection->audio) {
						g_warning("Could not open audio. Hangup");
						rm_object_emit_message("Audio error", "Could not open audio. Hangup");
						capi_hangup(connection);
						connection->audio = NULL;
					} else {
						connection->state = STATE_CONNECT_ACTIVE;
					}
				} else {
					connection->state = STATE_CONNECT_ACTIVE;
				}
			}
		}
		break;

	/* CAPI_DISCONNECT_B3 - Disconnect data */
	case CAPI_DISCONNECT_B3:
		g_debug("IND: DISCONNECT_B3");
		ncci = DISCONNECT_B3_IND_NCCI(&capi_message);

		isdn_lock();
		DISCONNECT_B3_RESP(&cmsg1, session->appl_id, session->message_number++, ncci);
		isdn_unlock();

		connection = capi_find_ncci(ncci);
		if (connection == NULL) {
			g_debug("connection not found");
			break;
		}

		connection->reason_b3 = DISCONNECT_B3_IND_REASON_B3(&capi_message);
		connection->ncci = 0;
		if (connection->state == STATE_CONNECTED || connection->state == STATE_CONNECT_B3_WAIT) {
			/* passive disconnect, DISCONNECT_IND comes later */
			connection->state = STATE_DISCONNECT_ACTIVE;
		} else {
			/* active disconnect, needs to send DISCONNECT_REQ */
			capi_hangup(connection);
		}

		g_debug("IND: CAPI_DISCONNECT_B3 - connection: %d, plci: %ld, ncci: %ld", connection->id, connection->plci, connection->ncci);
		break;

	/* CAPI_DISCONNECT - Disconnect */
	case CAPI_DISCONNECT:
		plci = DISCONNECT_IND_PLCI(&capi_message);

		g_debug("IND: DISCONNECT - plci %d", plci);

		g_debug("RESP: DISCONNECT - plci %d", plci);
		isdn_lock();
		DISCONNECT_RESP(&cmsg1, session->appl_id, session->message_number++, plci);
		isdn_unlock();

		connection = capi_find_plci(plci);
		if (connection == NULL) {
			g_debug("Connection not found, IGNORING");
			break;
		}

		/* CAPI-Error code */
		connection->reason = DISCONNECT_IND_REASON(&capi_message);
		connection->state = STATE_IDLE;
		connection->ncci = 0;
		connection->plci = 0;

		switch (connection->type) {
		case SESSION_PHONE: {
			RmAudio *audio = rm_profile_get_audio(rm_profile_get_active());

			if (session->input_thread_state == 1) {
				session->input_thread_state++;
				do {
					g_usleep(10);
				} while (session->input_thread_state != 0);
			}
			rm_audio_close(audio, connection->audio);
			break;
		}
		case SESSION_FAX:
			break;
		default:
			break;
		}

		capi_connection_disconnect(connection);

		capi_set_free(connection);
		break;
	default:
		g_debug("Unhandled command 0x%x", capi_message.Command);
		break;
	}

	return 0;
}

/**
 * \brief CAPI confirmation
 * \param capi_message capi message structure
 */
static void capi_confirmation(_cmsg capi_message)
{
	struct capi_connection *connection = NULL;
	unsigned int info;
	unsigned int plci;
	unsigned int ncci;
#ifdef CAPI_DEBUG
	int controller;
#endif

	switch (capi_message.Command) {
	case CAPI_FACILITY:
		/* Facility */
		g_debug("CNF: CAPI_FACILITY; Info: %d", capi_message.Info);
		break;
	case CAPI_LISTEN:
		/* Listen confirmation */
#ifdef CAPI_DEBUG
		controller = LISTEN_CONF_CONTROLLER(&capi_message);
		g_debug("CNF: CAPI_LISTEN: controller %d, info %d", controller, capi_message.Info);
#endif
		break;
	case CAPI_ALERT:
		/* Alert message */
		g_debug("CNF: CAPI_ALERT");
		info = ALERT_CONF_INFO(&capi_message);
		plci = ALERT_CONF_PLCI(&capi_message);

		g_debug("CNF: CAPI_ALERT: info %d, plci %d", info, plci);

		connection = capi_find_plci(plci);

		if (info != 0 && info != 3) {
			if (connection != NULL) {
				connection->state = STATE_IDLE;
			}
		} else {
			connection_ring(connection);
		}
		break;
	case CAPI_DATA_B3:
		/* Sent data acknowledge, NOP */
#ifdef CAPI_DEBUG
		g_debug("CNF: DATA_B3");
#endif
		ncci = DATA_B3_CONF_NCCI(&capi_message);

#ifdef CAPI_DEBUG
		info = DATA_B3_CONF_INFO(&capi_message);
		g_debug("CNF: CAPI_ALERT: info %d, ncci %d", info, ncci);
#endif

		connection = capi_find_ncci(ncci);
		if (connection && connection->use_buffers && connection->buffers) {
			connection->buffers--;
		}
		break;
	case CAPI_INFO:
		/* Info, NOP */
		g_debug("CNF: CAPI_INFO: info %d", capi_message.Info);
		break;
	case CAPI_CONNECT:
		/* Physical channel connection is being established */
		plci = CONNECT_CONF_PLCI(&capi_message);
		info = CONNECT_CONF_INFO(&capi_message);

		g_debug("CNF: CAPI_CONNECT - (plci: %d, info: %d)", plci, info);
		/* .. or new outgoing call? get plci. */
		connection = capi_find_new();
		if (connection == NULL) {
			g_debug("CND: CAPI_CONNECT - Warning! Received confirmation but we didn't requested a connect!!!");
			break;
		}

		if (info != 0) {
			/* Connection error */
			connection->state = STATE_IDLE;

			connection_status(connection, info);

			capi_set_free(connection);
		} else {
			/* CONNECT_ACTIVE_IND comes later, when connection actually established */
			connection->plci = plci;
			connection->state = STATE_CONNECT_WAIT;
		}
		break;
	case CAPI_CONNECT_B3:
		g_debug("CNF: CAPI_CONNECT_B3");
		capi_error(capi_message.Info);
		break;
	case CAPI_DISCONNECT:
		g_debug("CNF: CAPI_DISCONNECT");
		break;
	case CAPI_DISCONNECT_B3:
		g_debug("CNF: CAPI_DISCONNECT_B3");
		break;
	default:
		g_debug("Unhandled confirmation, command 0x%x", capi_message.Command);
		break;
	}
}

static int capi_init(int controller);

/**
 * \brief Our connection seems to be broken - reconnect
 * \param session capi session pointer
 * \return error code
 */
static void capi_reconnect(struct session *session)
{
	isdn_lock();
	capi_close();

	session->appl_id = capi_init(-1);

	isdn_unlock();
}

/**
 * \brief Main capi loop function
 * \param user_data unused pointer
 * \return NULL
 */
static gpointer capi_loop(void *user_data)
{
	struct timeval time_val;
	unsigned int info;
	unsigned int ret;
	_cmsg capi_message;
	GCancellable *loop_cancel = user_data;

	while (!g_cancellable_is_cancelled(loop_cancel)) {
		time_val.tv_sec = 1;
		time_val.tv_usec = 0;

		ret = CAPI20_WaitforMessage(session->appl_id, &time_val);
		if (ret == CapiNoError) {
			isdn_lock();
			info = capi_get_cmsg(&capi_message, session->appl_id);
			isdn_unlock();

			switch (info) {
			case CapiNoError:
				switch (capi_message.Subcommand) {
				/* Indication */
				case CAPI_IND:
					capi_indication(capi_message);
					break;
				/* Confirmation */
				case CAPI_CONF:
					capi_confirmation(capi_message);
					break;
				}
				break;
			case CapiReceiveQueueEmpty:
				g_warning("Empty queue, even if message pending.. reconnecting");
				g_usleep(1 * G_USEC_PER_SEC);
				capi_reconnect(session);
				break;
			default:
				return NULL;
			}
		} else if (!g_cancellable_is_cancelled(loop_cancel)) {
			if (session == NULL || session->appl_id == -1) {
				g_usleep(1 * G_USEC_PER_SEC);
			} else {
				g_usleep(1);
			}
		}
	}

	session = NULL;

	return NULL;
}

/**
 * \brief get capi profile
 * Convert capi_profile data from wire format to host format
 * \param Controller capi controller
 * \param host host formated capi profile pointer
 * \return error code
 */
static int get_capi_profile(unsigned controller, struct capi_profile *host)
{
	int ret_val = CAPI20_GET_PROFILE(controller, (unsigned char*)host);

	if (ret_val == 0) {
	}

	return ret_val;
}

/**
 * \brief Initialize CAPI controller
 * \param controller controller id
 * \return error code
 */
static int capi_init(int controller)
{
	CAPI_REGISTER_ERROR error_code = 0;
	_cmsg capi_message;
	unsigned int appl_id = -1;
#ifdef CAPI_DEBUG
	unsigned char buffer[64];
#endif
	int index;
	int start = 0;
	int end = 0;
	int num_controllers = 0;
	struct capi_profile profile;

	/* Check if capi is installed */
	error_code = CAPI20_ISINSTALLED();
	if (error_code != 0) {
		g_warning("CAPI 2.0: not installed, RC=0x%x", error_code);
		return -1;
	}

	/* Fetch controller/bchannel count */
	error_code = get_capi_profile(0, &profile);
	if (error_code != 0) {
		g_warning("CAPI 2.0: Error getting profile, RC=0x%x", error_code);
		return -1;
	}

	/* If there are no available controllers something went wrong, abort */
	num_controllers = profile.ncontroller;
	if (num_controllers == 0) {
		g_warning("CAPI 2.0: No ISDN controllers installed");
		return -1;
	}

#ifdef CAPI_DEBUG
	for (index = 1; index <= num_controllers; index++) {
		get_capi_profile(index, &profile);

		g_debug("CAPI 2.0: Controller: %d, Options: 0x%x", index, profile.goptions);

		int channels = profile.nbchannel;
		int dtmf = profile.goptions & 0x08 ? 1 : 0;
		int supp_serv = profile.goptions & 0x10;
		int echo = profile.goptions & 0x200;
		int intern = profile.goptions & 0x01;
		int extrn = profile.goptions & 0x02;
		int transp;
		int fax;
		int fax_ext;

		if (profile.support1 & 0x02 && profile.support2 & 0x02 && profile.support3 & 0x01) {
			transp = 1;
		} else {
			transp = 0;
		}

		if (profile.support1 & 0x10 && profile.support2 & 0x10 && profile.support3 & 0x10) {
			fax = 1;
		} else {
			fax = 0;
		}

		if (profile.support1 & 0x10 && profile.support2 & 0x10 && profile.support3 & 0x20) {
			fax_ext = 1;
		} else {
			fax_ext = 0;
		}

		g_debug("CAPI 2.0: B-Channels %d, DTMF %d, FAX %d/%d, Transp %d, SuppServ %d",
			channels, dtmf, fax, fax_ext, transp, supp_serv);
		g_debug("CAPI 2.0: Echo: %d, Intern: %d, extrn: %d", echo, intern, extrn);

		//pnManu = ( unsigned char * ) profile.manu;

		//opDebug( FOP_DEBUG, "manufactor profile: 0x%x, 0x%x, 0x%x, 0x%x\n", pnManu[0], pnManu[1], pnManu[2], pnManu[3]);
		//FopDebug( FOP_DEBUG, "Found nController #%d with %d B-channel(s)\n", nIndex, profile.nbchannel );

		g_debug("CAPI 2.0: B1 support = 0x%x", profile.support1);
		g_debug("CAPI 2.0: B2 support = 0x%x", profile.support2);
		g_debug("CAPI 2.0: B3 support = 0x%x", profile.support3);
	}

	/* Read manufacturer and version from device (entry 0) */
	g_debug("CAPI 2.0: Controllers found: %d", num_controllers);
	if (capi20_get_manufacturer(0, buffer)) {
		g_debug("CAPI 2.0: Manufacturer: %s", buffer);
	}
	if (capi20_get_version(0, buffer)) {
		g_debug("CAPI 2.0: Version %d.%d/%d.%d",
			buffer[0], buffer[1], buffer[2], buffer[3]);
	}
#endif

	/* Listen to all (<=0) or single controller (>=1) */
	if (controller <= 0) {
		start = 1;
		end = num_controllers;
	} else {
		start = controller;
		end = controller;
	}

	/* Register with CAPI */
	if (appl_id == -1) {
		error_code = CAPI20_REGISTER(CAPI_BCHANNELS, CAPI_BUFFERCNT, CAPI_PACKETS, &appl_id);
		if (error_code != 0 || appl_id == 0) {
			g_debug("Error while registering application, RC=0x%x", error_code);
			/* registration error! */
			return -2;
		}
	}

	/* Listen to CAPI controller(s) */
	for (index = start; index <= end; index++) {
		error_code = LISTEN_REQ(&capi_message, appl_id, 0, index, 0x3FF, 0x1FFF03FF, 0, NULL, NULL);
		if (error_code != 0) {
			g_debug("LISTEN_REQ failed, RC=0x%x", error_code);
			return -3;
		}

		g_debug("Listen to controller #%d ...", index);
#ifdef CAPI_DEBUG
		g_debug("Listen to controller #%d ...", index);
#endif
	}

	g_debug("CAPI connection established!");

	/* ok! */
	return appl_id;
}

void setHostName(const char *);

/**
 * \brief Initialize capi structure
 * \param host host name of router
 * \param controller listen controller or -1 for all
 * \return session pointer or NULL on error
 */
struct session *capi_session_init(const char *host, gint controller)
{
	int appl_id = -1;

	if (session != NULL) {
		return session;
	}

	if (host != NULL) {
//#if HAVE_CAPI_36
		capi20ext_set_driver("fritzbox");
		capi20ext_set_host((char*)host);
		capi20ext_set_port(5031);
		capi20ext_set_tracelevel(0);
//#else
//		setHostName(host);
//#endif
	}

	appl_id = capi_init(controller);
	if (appl_id <= 0) {
		g_debug("Initialization failed! Error %d!", appl_id);

		return NULL;
	}

	create_table_buffer();

	session = g_slice_alloc0(sizeof(struct session));

	g_mutex_init(&session->isdn_mutex);

	session->appl_id = appl_id;

	/* start capi transmission loop */
	capi_loop_cancel = g_cancellable_new();
	main_context = g_main_context_get_thread_default ();
	g_thread_new("capi", capi_loop, capi_loop_cancel);

	return session;
}

/**
 * \brief Destroy capi
 * \param force force flag for capi_close()
 * \return error code 0
 */
int capi_session_close(int force)
{
	/* Close capi connection */
	capi_close();

	if (capi_loop_cancel) {
		g_cancellable_cancel(capi_loop_cancel);
	}

	session = NULL;

	return 0;
}

/**
 * \brief Get active capi session
 * \return session pointer or NULL on error
 */
struct session *capi_get_session(void)
{
	return session;
}

/**
 * \brief Capi connect
 * \param user_data capi plugin pointer
 * \return error code
 */
gboolean capi_session_connect(gpointer user_data)
{
	RmProfile *profile = rm_profile_get_active();
	gboolean retry = TRUE;

again:
	session = capi_session_init(rm_router_get_host(profile), g_settings_get_int(profile->settings, "phone-controller") + 1);
	if (!session && retry) {
		/* Maybe the port is closed, try to activate it and try again */
		g_usleep(G_USEC_PER_SEC * 2);
		retry = FALSE;
		goto again;
	}

	return session != NULL;
}

gboolean capi_session_disconnect(gpointer user_data)
{
	return TRUE;
}

/**
 * \brief Activate plugin (add net event)
 * \param plugin plugin
 */
static gboolean capi_plugin_init(RmPlugin *plugin)
{
	RmCapiPlugin *capi_plugin = g_slice_alloc0(sizeof(RmCapiPlugin));

	plugin->priv = capi_plugin;
	/* Add network event */
	capi_plugin->net_event = rm_netmonitor_add_event("CAPI", capi_session_connect, capi_session_disconnect, capi_plugin);

	capi_device = rm_device_register("CAPI");
	capi_phone_init(capi_device);
	capi_fax_init(capi_device);

	return TRUE;
}

/**
 * \brief Deactivate plugin (remote net event)
 * \param plugin plugin
 */
static gboolean capi_plugin_shutdown(RmPlugin *plugin)
{
	RmCapiPlugin *capi_plugin = plugin->priv;

	rm_device_unregister(capi_device);
	capi_phone_shutdown();

	/* Remove network event */
	rm_netmonitor_remove_event(capi_plugin->net_event);

	return TRUE;
}

RM_PLUGIN(capi);
