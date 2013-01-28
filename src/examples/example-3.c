/*!	@page smcp-example-3 example-3.c: Using the node router
**
**	src/examples/example-3.c
**
**	This example shows how to respond to a request for specific resources
**	using the node router.
**
**	## Results ##
**
**	    $ smcpctl
**	    Listening on port 61617.
**	    coap://localhost/> ls
**	    hello-world
**	    coap://localhost/> cat hello-world
**	    Hello world!
**	    coap://localhost/> cat hello-world -i
**	    CoAP/1.0 2.05 CONTENT tt=ACK(2) msgid=0xCBE1
**	    Token: CB E1
**	    Content-type: text/plain;charset=utf-8
**	    Payload-Size: 12
**
**	    Hello world!
**	    coap://localhost/>
**
**	@sa @ref smcp-node-router
**
*/

#include <stdio.h>
#include <smcp/smcp.h>
#include <smcp/smcp-node.h>

static smcp_status_t
request_handler(void* context) {

	printf("Got a request!\n");

	// Only handle GET requests for now.
	if(smcp_inbound_get_code() != COAP_METHOD_GET)
		return SMCP_STATUS_NOT_IMPLEMENTED;

	// Begin describing the response.
	smcp_outbound_begin_response(COAP_RESULT_205_CONTENT);

	smcp_outbound_add_option_uint(
		COAP_OPTION_CONTENT_TYPE,
		COAP_CONTENT_TYPE_TEXT_PLAIN
	);

	smcp_outbound_append_content("Hello world!", SMCP_CSTR_LEN);

	return smcp_outbound_send();
}

int
main(void) {
	smcp_node_t root_node = smcp_node_init(NULL,NULL,NULL);

	smcp_t instance = smcp_create(0);
	if(!instance) {
		perror("Unable to create SMCP instance");
		abort();
	}

	smcp_set_default_request_handler(instance, &smcp_node_router_handler, (void*)root_node);

	smcp_node_t hello_node = smcp_node_init(NULL,root_node,"hello-world");
	hello_node->request_handler = &request_handler;
	hello_node->context = NULL;

	printf("Listening on port %d\n",smcp_get_port(instance));

	while(1) {
		smcp_process(instance, CMS_DISTANT_FUTURE);
	}

	smcp_release(instance);

	return 0;
}
