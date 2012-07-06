/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-request-http.c: http: URI request object
 *
 * Copyright (C) 2009, 2010 Red Hat, Inc.
 * Copyright (C) 2010 Igalia, S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib/gi18n-lib.h>

#define LIBSOUP_USE_UNSTABLE_REQUEST_API

#include "soup-request-http.h"
#include "soup.h"
#include "soup-cache-private.h"
#include "soup-session-private.h"

G_DEFINE_TYPE (SoupRequestHTTP, soup_request_http, SOUP_TYPE_REQUEST)

struct _SoupRequestHTTPPrivate {
	SoupMessage *msg;
	char *content_type;
};

static void content_sniffed (SoupMessage *msg,
			     const char  *content_type,
			     GHashTable  *params,
			     gpointer     user_data);

static void
soup_request_http_init (SoupRequestHTTP *http)
{
	http->priv = G_TYPE_INSTANCE_GET_PRIVATE (http, SOUP_TYPE_REQUEST_HTTP, SoupRequestHTTPPrivate);
}

static gboolean
soup_request_http_check_uri (SoupRequest  *request,
			     SoupURI      *uri,
			     GError      **error)
{
	SoupRequestHTTP *http = SOUP_REQUEST_HTTP (request);

	if (!SOUP_URI_VALID_FOR_HTTP (uri))
		return FALSE;

	http->priv->msg = soup_message_new_from_uri (SOUP_METHOD_GET, uri);
	g_signal_connect (http->priv->msg, "content-sniffed",
			  G_CALLBACK (content_sniffed), http);
	return TRUE;
}

static void
soup_request_http_finalize (GObject *object)
{
	SoupRequestHTTP *http = SOUP_REQUEST_HTTP (object);

	if (http->priv->msg) {
		g_signal_handlers_disconnect_by_func (http->priv->msg,
						      G_CALLBACK (content_sniffed),
						      http);
		g_object_unref (http->priv->msg);
	}

	g_free (http->priv->content_type);

	G_OBJECT_CLASS (soup_request_http_parent_class)->finalize (object);
}

static GInputStream *
soup_request_http_send (SoupRequest          *request,
			GCancellable         *cancellable,
			GError              **error)
{
	SoupRequestHTTP *http = SOUP_REQUEST_HTTP (request);

	return soup_session_send_request (soup_request_get_session (request),
					  http->priv->msg,
					  cancellable, error);
}


typedef struct {
	SoupRequestHTTP *http;
	GCancellable *cancellable;
	GSimpleAsyncResult *simple;

	SoupMessage *original;
	GInputStream *stream;
} SendAsyncData;

static void
free_send_async_data (SendAsyncData *sadata)
{
       g_object_unref (sadata->http);
       g_object_unref (sadata->simple);

       if (sadata->cancellable)
               g_object_unref (sadata->cancellable);
       if (sadata->stream)
               g_object_unref (sadata->stream);
       if (sadata->original)
               g_object_unref (sadata->original);

       g_slice_free (SendAsyncData, sadata);
}

static void
http_input_stream_ready_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	SendAsyncData *sadata = user_data;
	GError *error = NULL;
	GInputStream *stream;

	stream = soup_session_send_request_finish (SOUP_SESSION (source), result, &error);
	if (stream) {
		g_simple_async_result_set_op_res_gpointer (sadata->simple, stream, g_object_unref);
	} else {
		g_simple_async_result_take_error (sadata->simple, error);
	}
	g_simple_async_result_complete (sadata->simple);
	free_send_async_data (sadata);
}


static void
conditional_get_ready_cb (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
	SendAsyncData *sadata = user_data;
	GInputStream *stream;

	if (msg->status_code == SOUP_STATUS_NOT_MODIFIED) {
		SoupCache *cache = (SoupCache *)soup_session_get_feature (session, SOUP_TYPE_CACHE);

		stream = soup_cache_send_response (cache, sadata->original);
		if (stream) {
			g_simple_async_result_set_op_res_gpointer (sadata->simple, stream, g_object_unref);

			soup_message_got_headers (sadata->original);

			/* FIXME: this is wrong; the cache won't have
			 * the sniffed type.
			 */
			sadata->http->priv->content_type = g_strdup (soup_message_headers_get_content_type (msg->response_headers, NULL));

			g_simple_async_result_complete (sadata->simple);

			soup_message_finished (sadata->original);
			free_send_async_data (sadata);
			return;
		}
	}

	/* The resource was modified or the server returned a 200
	 * OK. Either way we reload it. This is far from optimal as
	 * we're donwloading the resource twice, but we will change it
	 * once the cache is integrated in the streams stack.
	 */
	soup_session_send_request_async (session, sadata->original, sadata->cancellable,
					 http_input_stream_ready_cb, sadata);
}

static gboolean
idle_return_from_cache_cb (gpointer data)
{
	SendAsyncData *sadata = data;

	g_simple_async_result_set_op_res_gpointer (sadata->simple,
						   g_object_ref (sadata->stream), g_object_unref);

	/* Issue signals  */
	soup_message_got_headers (sadata->http->priv->msg);

	sadata->http->priv->content_type = g_strdup (soup_message_headers_get_content_type (sadata->http->priv->msg->response_headers, NULL));

	g_simple_async_result_complete (sadata->simple);

	soup_message_finished (sadata->http->priv->msg);

	free_send_async_data (sadata);
	return FALSE;
}

static void
soup_request_http_send_async (SoupRequest          *request,
			      GCancellable         *cancellable,
			      GAsyncReadyCallback   callback,
			      gpointer              user_data)
{
	SoupRequestHTTP *http = SOUP_REQUEST_HTTP (request);
	SendAsyncData *sadata;
	GInputStream *stream;
	SoupSession *session;
	SoupCache *cache;

	sadata = g_slice_new0 (SendAsyncData);
	sadata->http = g_object_ref (http);
	sadata->cancellable = cancellable ? g_object_ref (cancellable) : NULL;
	sadata->simple = g_simple_async_result_new (G_OBJECT (request), callback, user_data,
						    soup_request_http_send_async);

	session = soup_request_get_session (request);
	cache = (SoupCache *)soup_session_get_feature (session, SOUP_TYPE_CACHE);

	if (cache) {
		SoupCacheResponse response;

		response = soup_cache_has_response (cache, http->priv->msg);
		if (response == SOUP_CACHE_RESPONSE_FRESH) {
			stream = soup_cache_send_response (cache, http->priv->msg);

			/* Cached resource file could have been deleted outside */
			if (stream) {
				/* Do return the stream asynchronously as in
				 * the other cases. It's not enough to use
				 * g_simple_async_result_complete_in_idle as
				 * the signals must be also emitted
				 * asynchronously
				 */
				sadata->stream = stream;
				soup_add_completion (soup_session_get_async_context (session),
						     idle_return_from_cache_cb, sadata);
				return;
			}
		} else if (response == SOUP_CACHE_RESPONSE_NEEDS_VALIDATION) {
			SoupMessage *conditional_msg;

			conditional_msg = soup_cache_generate_conditional_request (cache, http->priv->msg);

			if (conditional_msg) {
				sadata->original = g_object_ref (http->priv->msg);
				soup_session_queue_message (session, conditional_msg,
							    conditional_get_ready_cb,
							    sadata);
				return;
			}
		}
	}

	soup_session_send_request_async (session, http->priv->msg, cancellable,
					 http_input_stream_ready_cb, sadata);
}

static GInputStream *
soup_request_http_send_finish (SoupRequest   *request,
			       GAsyncResult  *result,
			       GError       **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (request), soup_request_http_send_async), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;
	return g_object_ref (g_simple_async_result_get_op_res_gpointer (simple));
}

static goffset
soup_request_http_get_content_length (SoupRequest *request)
{
	SoupRequestHTTP *http = SOUP_REQUEST_HTTP (request);

	return soup_message_headers_get_content_length (http->priv->msg->response_headers);
}

static void
content_sniffed (SoupMessage *msg,
		 const char  *content_type,
		 GHashTable  *params,
		 gpointer     user_data)
{
	SoupRequestHTTP *http = user_data;
	GString *sniffed_type;

	sniffed_type = g_string_new (content_type);
	if (params) {
		GHashTableIter iter;
		gpointer key, value;

		g_hash_table_iter_init (&iter, params);
		while (g_hash_table_iter_next (&iter, &key, &value)) {
			g_string_append (sniffed_type, "; ");
			soup_header_g_string_append_param (sniffed_type, key, value);
		}
	}
	g_free (http->priv->content_type);
	http->priv->content_type = g_string_free (sniffed_type, FALSE);
}

static const char *
soup_request_http_get_content_type (SoupRequest *request)
{
	SoupRequestHTTP *http = SOUP_REQUEST_HTTP (request);

	return http->priv->content_type;
}

static const char *http_schemes[] = { "http", "https", NULL };

static void
soup_request_http_class_init (SoupRequestHTTPClass *request_http_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (request_http_class);
	SoupRequestClass *request_class =
		SOUP_REQUEST_CLASS (request_http_class);

	g_type_class_add_private (request_http_class, sizeof (SoupRequestHTTPPrivate));

	request_class->schemes = http_schemes;

	object_class->finalize = soup_request_http_finalize;

	request_class->check_uri = soup_request_http_check_uri;
	request_class->send = soup_request_http_send;
	request_class->send_async = soup_request_http_send_async;
	request_class->send_finish = soup_request_http_send_finish;
	request_class->get_content_length = soup_request_http_get_content_length;
	request_class->get_content_type = soup_request_http_get_content_type;
}

/**
 * soup_request_http_get_message:
 * @http: a #SoupRequestHTTP object
 *
 * Gets a new reference to the #SoupMessage associated to this SoupRequest
 *
 * Returns: (transfer full): a new reference to the #SoupMessage
 *
 * Since: 2.34
 */
SoupMessage *
soup_request_http_get_message (SoupRequestHTTP *http)
{
	g_return_val_if_fail (SOUP_IS_REQUEST_HTTP (http), NULL);

	return g_object_ref (http->priv->msg);
}
