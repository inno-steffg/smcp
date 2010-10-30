/*
 *  smcp_pairing.c
 *  SMCP
 *
 *  Created by Robert Quattlebaum on 8/31/10.
 *  Copyright 2010 deepdarc. All rights reserved.
 *
 */

#ifndef VERBOSE_DEBUG
#define VERBOSE_DEBUG 1
#endif

#include "assert_macros.h"

#include "smcp.h"
#include "smcp_logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ll.h"

#if SMCP_USE_BSD_SOCKETS
#include <netdb.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <net/if.h>
#elif __CONTIKI__
#include "resolv.h"
#endif

#include "smcp_pairing.h"
#include "smcp_node.h"
#include "smcp_timer.h"
#include "smcp_internal.h"

#include "smcp_variable_node.h"

#include "smcp_helpers.h"
#include "url-helpers.h"

#pragma mark -
#pragma mark Macros


#pragma mark -
#pragma mark Paring

static bt_compare_result_t
smcp_pairing_compare_for_insert(
	smcp_pairing_t lhs, smcp_pairing_t rhs
) {
	if(lhs->path == rhs->path)
		return strcmp(lhs->dest_uri, rhs->dest_uri);
	bt_compare_result_t ret = strcmp(lhs->path, rhs->path);
	return (ret == 0) ? strcmp(lhs->dest_uri, rhs->dest_uri) : ret;
}


static bt_compare_result_t
smcp_pairing_compare(
	smcp_pairing_t lhs, smcp_pairing_t rhs
) {
	if(lhs->path == rhs->path)
		return strcmp(lhs->dest_uri, rhs->dest_uri);
	bt_compare_result_t ret = strcmp(lhs->path, rhs->path);
	return (ret == 0) ? strcmp(lhs->dest_uri, rhs->dest_uri) : ret;
}

static bt_compare_result_t
smcp_pairing_compare_cstr(
	smcp_pairing_t lhs, const char* rhs
) {
	if(lhs->path == rhs)
		return 0;
	if(!lhs->path)
		return 1;
	if(!rhs)
		return -1;
	return strcmp(lhs->path, rhs);
}

void
smcp_pairing_finalize(smcp_pairing_t self) {
	if(self->path)
		free((void*)self->path);
	if(self->dest_uri)
		free((void*)self->dest_uri);
	free((void*)self);
}

smcp_status_t
smcp_daemon_pair_with_uri(
	smcp_daemon_t	self,
	const char*		path,
	const char*		uri,
	int				flags,
	uintptr_t*		idVal
) {
	smcp_status_t ret = SMCP_STATUS_OK;

	smcp_pairing_t pairing = NULL;

	require_action(path, bail, ret = SMCP_STATUS_INVALID_ARGUMENT);
	require_action(uri, bail, ret = SMCP_STATUS_INVALID_ARGUMENT);

	pairing = calloc(1, sizeof(*pairing));

	require(pairing, bail);

	pairing->seq = pairing->ack = SMCP_FUNC_RANDOM_UINT32();

	while(path[0] == '/') path++;

	pairing->path = strdup(path);       //! TODO: Fix leak
	pairing->dest_uri = strdup(uri);    //! TODO: Fix leak
	pairing->flags = flags;

	if(idVal)
		*idVal = (uintptr_t)pairing;

	bt_insert(
		    (void**)&((smcp_daemon_t)self)->pairings,
		pairing,
		    (bt_compare_func_t)smcp_pairing_compare_for_insert,
		    (bt_delete_func_t)smcp_pairing_finalize,
		NULL
	);

bail:
	return ret;
}


smcp_pairing_t
smcp_daemon_get_first_pairing_for_path(
	smcp_daemon_t self, const char* path
) {
	smcp_pairing_t ret = NULL;
	smcp_pairing_t prev = NULL;

	while(path[0] == '/') path++;

	ret = bt_find((void**)&self->pairings,
		path,
		    (bt_compare_func_t)smcp_pairing_compare_cstr,
		NULL);

	if(ret) {
		for(prev = bt_prev(ret);
		    prev && (0 == strcmp(ret->path, prev->path));
		    ret = prev, prev = bt_prev(ret)) {
		}
	}

bail:
	return ret;
}

smcp_pairing_t
smcp_daemon_next_pairing(
	smcp_daemon_t self, smcp_pairing_t pairing
) {
	smcp_pairing_t ret = NULL;

	require(pairing != NULL, bail);

	ret = bt_next((void*)pairing);
	if(ret && (0 != strcmp(pairing->path, ret->path)))
		ret = 0;

bail:
	return ret;
}


smcp_status_t
smcp_daemon_trigger_event(
	smcp_daemon_t		self,
	const char*			path,
	const char*			content,
	size_t				content_length,
	coap_content_type_t content_type
) {
	smcp_status_t ret = 0;
	smcp_pairing_t iter;
	char cseq[24] = "dummy";

	memset(cseq, 0, sizeof(cseq));
	coap_header_item_t headers[7] = {};
	coap_transaction_id_t tid = SMCP_FUNC_RANDOM_UINT32();

	if(!path)
		path = "/";

	util_add_header(headers,
		sizeof(headers) / sizeof(*headers),
		SMCP_HEADER_CSEQ,
		cseq,
		COAP_HEADER_CSTR_LEN);
	headers[0].value_len = COAP_HEADER_CSTR_LEN; // Force recalculation of length when we send.

	util_add_header(headers,
		sizeof(headers) / sizeof(*headers),
		COAP_HEADER_CONTENT_TYPE,
		    (const void*)&content_type,
		1);

	// Move past any preceding slashes.
	while(path && *path == '/') path++;

	util_add_header(headers,
		sizeof(headers) / sizeof(*headers),
		SMCP_HEADER_ORIGIN,
		path,
		HEADER_CSTR_LEN);

	for(iter = smcp_daemon_get_first_pairing_for_path(self, path);
	    iter;
	    iter = smcp_daemon_next_pairing(self, iter)) {
		smcp_pairing_seq_t seq = smcp_pairing_get_next_seq(iter);

		DEBUG_PRINTF(CSTR(
				"%p: sending stuff for pairing \"%p\"..."), self, iter);

		snprintf(cseq,
			sizeof(cseq) - 1,
			"%lX POST",
			    (long unsigned int)seq);

		smcp_daemon_send_request_to_url(
			self,
			tid,
			COAP_METHOD_POST,
			iter->dest_uri,
			headers,
			content,
			content_length
		);

		// TODO: Handle retransmits for reliable pairings!
	}

bail:
	return ret;
}

smcp_status_t
smcp_daemon_trigger_event_with_node(
	smcp_daemon_t		self,
	smcp_node_t			node,
	const char*			subpath,
	const char*			content,
	size_t				content_length,
	coap_content_type_t content_type
) {
	char path[SMCP_MAX_PATH_LENGTH + 1];

	path[sizeof(path) - 1] = 0;

	smcp_node_get_path(node, path, sizeof(path));

	if(subpath)
		strncat(path, subpath, sizeof(path) - 1);

	{   // Remove trailing slashes.
		size_t len = strlen(path);
		while(len && path[len - 1] == '/')
			path[--len] = 0;
	}

	return smcp_daemon_trigger_event(self,
		path,
		content,
		content_length,
		content_type);
}


smcp_status_t
smcp_daemon_refresh_variable(
	smcp_daemon_t self, smcp_variable_node_t node
) {
	smcp_status_t ret = 0;
	char *content = NULL;
	size_t content_length = sizeof(content);
	coap_content_type_t content_type = COAP_CONTENT_TYPE_TEXT_PLAIN;

	require_action(self != NULL, bail, ret = SMCP_STATUS_INVALID_ARGUMENT);
	require_action(node != NULL, bail, ret = SMCP_STATUS_INVALID_ARGUMENT);

	if(node->get_func) {
		char tmp_content[SMCP_MAX_CONTENT_LENGTH]; // TODO: This is really hard on the stack! Investigate alternatives.
		ret =
		    (*node->get_func)(node, tmp_content, &content_length,
			&content_type);
		require(ret == 0, bail);
		if(content_length) {
			content = (char*)malloc(content_length);
			memcpy(content, tmp_content, content_length);
		}
	}

	ret = smcp_daemon_trigger_event_with_node(
		self,
		    (smcp_node_t)node,
		NULL,
		content,
		content_length,
		content_type
	    );

bail:
	if(content)
		free(content);

	return ret;
}


smcp_status_t
smcp_daemon_handle_pair(
	smcp_daemon_t	self,
	smcp_node_t		node,
	smcp_method_t	method,
	const char*		path,
	const char*		content,
	size_t			content_length
) {
	smcp_status_t ret = 0;
	uintptr_t idVal;
	char reply[3 + sizeof(uintptr_t) * 2];
	char full_path[SMCP_MAX_PATH_LENGTH + 1];

	full_path[sizeof(full_path) - 1] = 0;

	require_action(node, bail, ret = SMCP_STATUS_FAILURE);

	smcp_node_get_path(node, full_path, sizeof(full_path));
	strncat(full_path, path, sizeof(full_path) - 1);
	ret = smcp_daemon_pair_with_uri(
		self,
		full_path,
		content,
		0,
		&idVal
	    );

	snprintf(reply, sizeof(reply), "%p", (void*)idVal);

	check_string(ret == SMCP_STATUS_OK, smcp_status_to_cstr(ret));
	ret = smcp_daemon_send_response(
		self,
		smcp_convert_status_to_result_code(
			ret
		),
		NULL,
		reply,
		ret ? 0 : strlen(reply)
	    );
bail:
	return ret;
}

extern smcp_status_t
smcp_daemon_delete_pairing(
	smcp_daemon_t self, smcp_pairing_t pairing
) {
	bt_remove(
		    (void**)&((smcp_daemon_t)self)->pairings,
		pairing,
		    (bt_compare_func_t)smcp_pairing_compare_for_insert,
		    (bt_delete_func_t)smcp_pairing_finalize,
		NULL
	);
	return 0;
}


#pragma mark -
#pragma mark Pairing Node Interface

static smcp_status_t
smcp_pairing_node_list(
	smcp_daemon_t self, smcp_pairing_t first_pairing
) {
	smcp_status_t ret = 0;
	coap_header_item_t replyHeaders[5] = {};
	char replyContent[128];
	coap_code_t response_code = COAP_RESULT_CODE_OK;

	memset(replyContent, 0, sizeof(replyContent));
	size_t content_break_threshold = 60;
	uint16_t start_index = 0, i = 0;

	smcp_pairing_t
	    pairing_iter = first_pairing ? first_pairing : bt_first(
		self->pairings),
	    pairing_prev = NULL;

	{
		const coap_header_item_t *iter =
		    smcp_daemon_get_current_request_headers(self);
		const coap_header_item_t *end = iter +
		    smcp_daemon_get_current_request_header_count(self);
		for(; iter != end; ++iter) {
			if((iter->key == COAP_HEADER_NEXT) &&
			        (iter->value_len == sizeof(start_index))) {
				memcpy(&start_index, iter->value, sizeof(start_index));
				start_index = ntohs(start_index);
			}
		}
	}

	if(content_break_threshold >= sizeof(replyContent) - 1)
		// Trim the content break threshold to something we support.
		content_break_threshold = sizeof(replyContent) - 2;


	for(i = 0;
	    pairing_iter && (i < start_index);
	    i++, pairing_iter = bt_next(pairing_iter)) {
	}

	while(pairing_iter) {
		const char* name;

		if(first_pairing)
			name = pairing_iter->dest_uri;
		else
			name = pairing_iter->path;

		if(!name)
			break;

		if((strlen(name) + 4) >
		        (content_break_threshold - strlen(replyContent) - 2))
			break;

		strlcat(replyContent, "<", sizeof(replyContent));

		{
			size_t len = strlen(replyContent);
			url_encode_cstr(replyContent + len,
				name,
				sizeof(replyContent) - len - 2);
		}

		strlcat(replyContent, ">", sizeof(replyContent));

		if(first_pairing) {
		} else {
			strlcat(replyContent, ";ct=40", sizeof(replyContent));
		}

		pairing_prev = pairing_iter;
		pairing_iter = bt_next(pairing_iter);
		start_index++;
		if(first_pairing) {
			if(pairing_iter &&
			        (0 != strcmp(first_pairing->path, pairing_iter->path)))
				pairing_iter = NULL;
		} else {
			if(pairing_iter) {
				while(pairing_iter &&
				        (0 ==
				        strcmp(pairing_prev->path, pairing_iter->path))) {
					pairing_prev = pairing_iter;
					pairing_iter = bt_next(pairing_iter);
					start_index++;
				}
			}
		}

		strlcat(replyContent, "," + (!pairing_iter), sizeof(replyContent));
	}

	// If there are more nodes, indicate as such in the headers.
	if(pairing_iter) {
		response_code = COAP_RESULT_CODE_PARTIAL_CONTENT;
		start_index = htons(start_index);
		util_add_header(replyHeaders, SMCP_MAX_HEADERS, COAP_HEADER_NEXT,
			    (void*)&start_index, sizeof(start_index));
	}

	coap_content_type_t content_type =
	    COAP_CONTENT_TYPE_APPLICATION_LINK_FORMAT;

	util_add_header(
		replyHeaders,
		SMCP_MAX_HEADERS,
		COAP_HEADER_CONTENT_TYPE,
		    (const void*)&content_type,
		1
	);

	smcp_daemon_send_response(self,
		response_code,
		replyHeaders,
		replyContent,
		strlen(replyContent));
bail:
	return ret;
}


static smcp_status_t
smcp_pairing_node_request_handler(
	smcp_daemon_t	self,
	smcp_node_t		node,
	smcp_method_t	method,
	const char*		path,
	const char*		content,
	size_t			content_length
) {
	smcp_status_t ret = 0;

	if(!path || (path[0] == 0)) {
		if(method == COAP_METHOD_GET) {
			ret = smcp_pairing_node_list(self, NULL);
		} else {
			smcp_daemon_send_response(self,
				COAP_RESULT_CODE_METHOD_NOT_ALLOWED,
				NULL,
				NULL,
				0);
		}
	} else {
		char* name = strsep((char**)&path, "/");
		smcp_pairing_t pairing;
		url_decode_cstr_inplace(name);

		pairing = smcp_daemon_get_first_pairing_for_path(self, name);

		if(!path || (path[0] == 0)) {
			require_action(pairing != NULL,
				bail,
				ret = SMCP_STATUS_NOT_FOUND);
			if(method == COAP_METHOD_GET) {
				ret = smcp_pairing_node_list(self, pairing);
			}
/*
            } else if((method==COAP_METHOD_PUT) || (method==COAP_METHOD_POST)) {
                require_action(content_length,bail,ret=SMCP_STATUS_UNSUPPORTED_URI);
                ret = smcp_daemon_pair_with_uri(self,name,content,0,NULL);
                if((ret==SMCP_STATUS_OK)) {
                    smcp_daemon_send_response(self, COAP_RESULT_CODE_CREATED, NULL, NULL, 0);
                }
 */
			else {
				smcp_daemon_send_response(self,
					COAP_RESULT_CODE_METHOD_NOT_ALLOWED,
					NULL,
					NULL,
					0);
			}
		} else {
			char* uri = strsep((char**)&path, "/");
			url_decode_cstr_inplace(uri);
			while(pairing && (0 != strcmp(uri, pairing->dest_uri))) {
				pairing = smcp_daemon_next_pairing(self, pairing);
			}

			if(method == COAP_METHOD_GET) {
				char rcontent[128];
				size_t rcontent_length;

				require_action(pairing != NULL,
					bail,
					ret = SMCP_STATUS_NOT_FOUND);

				rcontent_length = snprintf(
					rcontent,
					sizeof(rcontent),
					"DestURI: %s\n"
					"Flags: 0x%02X\n"
					"Seq: 0x%08X\n"
					"Ack: 0x%08X\n"
					"",
					pairing->dest_uri,
					pairing->flags,
					pairing->seq,
					pairing->ack
				    );
				smcp_daemon_send_response(self,
					COAP_RESULT_CODE_OK,
					NULL,
					rcontent,
					rcontent_length);
			} else if((method == COAP_METHOD_PUT) ||
			        (method == COAP_METHOD_POST)) {
				ret = smcp_daemon_pair_with_uri(self, name, uri, 0, NULL);
				if((ret == SMCP_STATUS_OK) && !pairing) {
					smcp_daemon_send_response(self,
						COAP_RESULT_CODE_CREATED,
						NULL,
						NULL,
						0);
				}
			} else if(method == COAP_METHOD_DELETE) {
				ret = smcp_daemon_delete_pairing(self, pairing);
			} else {
				smcp_daemon_send_response(self,
					COAP_RESULT_CODE_METHOD_NOT_ALLOWED,
					NULL,
					NULL,
					0);
			}
		}
	}

bail:
	return ret;
}

smcp_node_t smcp_pairing_node_init(
	smcp_node_t self, smcp_node_t parent, const char* name
) {
	require(self = smcp_node_init(self, parent, name), bail);

	self->request_handler = &smcp_pairing_node_request_handler;

bail:
	return self;
}
