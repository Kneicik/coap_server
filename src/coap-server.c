/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(net_coap_server_sample, LOG_LEVEL_DBG);

#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>

#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/udp.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_link_format.h>
#include <zephyr/drivers/can.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/socketcan.h>
#include <zephyr/net/socketcan_utils.h>
#include <zephyr/drivers/gpio.h>

#include "net_private.h"

#define MAX_RETRANSMIT_COUNT 2

#define MAX_COAP_MSG_LEN 256

#define MY_COAP_PORT 5683

#define BLOCK_WISE_TRANSFER_SIZE_GET 2048

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define ADDRLEN(sock) \
	(((struct sockaddr *)sock)->sa_family == AF_INET ? \
		sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6))

#define NUM_OBSERVERS 3

#define NUM_PENDINGS 10

/* CoAP socket fd */
static int sock;

static struct coap_observer observers[NUM_OBSERVERS];

static struct coap_pending pendings[NUM_PENDINGS];

static struct k_work_delayable observer_work;

static int obs_counter;

static struct coap_resource *resource_to_notify;

static struct k_work_delayable retransmit_work;

#if defined(CONFIG_NET_IPV4)
static int start_coap_server(void)
{
	struct sockaddr_in addr;
	int r;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(MY_COAP_PORT);

	sock = socket(addr.sin_family, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("Failed to create UDP socket %d", errno);
		return -errno;
	}

	r = bind(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (r < 0) {
		LOG_ERR("Failed to bind UDP socket %d", errno);
		return -errno;
	}

	return 0;
}
#endif

static int send_coap_reply(struct coap_packet *cpkt,
			   const struct sockaddr *addr,
			   socklen_t addr_len)
{
	int r;

	net_hexdump("Response", cpkt->data, cpkt->offset);

	r = sendto(sock, cpkt->data, cpkt->offset, 0, addr, addr_len);
	if (r < 0) {
		LOG_ERR("Failed to send %d", errno);
		r = -errno;
	}

	return r;
}

static int well_known_core_get(struct coap_resource *resource,
			       struct coap_packet *request,
			       struct sockaddr *addr, socklen_t addr_len)
{
	struct coap_packet response;
	uint8_t *data;
	int r;

	data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	r = coap_well_known_core_get(resource, request, &response,
				     data, MAX_COAP_MSG_LEN);
	if (r < 0) {
		goto end;
	}

	r = send_coap_reply(&response, addr, addr_len);

end:
	k_free(data);

	return r;
}


static int led_put(struct coap_resource *resource,
		    struct coap_packet *request,
		    struct sockaddr *addr, socklen_t addr_len)
{
	struct coap_packet response;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	const uint8_t *payload;
	uint8_t *data;
	uint16_t payload_len;
	uint8_t code;
	uint8_t type;
	uint8_t tkl;
	uint16_t id;
	int r;
	int ret = 0;

	code = coap_header_get_code(request);
	type = coap_header_get_type(request);
	id = coap_header_get_id(request);
	tkl = coap_header_get_token(request, token);

	LOG_INF("*******");
	LOG_INF("type: %u code %u id %u", type, code, id);
	LOG_INF("*******");

	payload = coap_packet_get_payload(request, &payload_len);
	if (payload) {
		net_hexdump("PUT Payload", payload, payload_len);
	}
	if (*payload == 49){
		printk("payload: %d\n %d\n",*payload,payload_len);
	if (!device_is_ready(led.port)) {
		return ret;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}
	} else{
		if(*payload == 48){
			gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
		}
		
	}
	if (type == COAP_TYPE_CON) {
		type = COAP_TYPE_ACK;
	} else {
		type = COAP_TYPE_NON_CON;
	}

	data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	r = coap_packet_init(&response, data, MAX_COAP_MSG_LEN,
			     COAP_VERSION_1, type, tkl, token,
			     COAP_RESPONSE_CODE_CHANGED, id);
	if (r < 0) {
		goto end;
	}

	r = send_coap_reply(&response, addr, addr_len);

end:
	k_free(data);

	return r;
}

static void schedule_next_retransmission(void)
{
	struct coap_pending *pending;
	int32_t remaining;
	uint32_t now = k_uptime_get_32();

	/* Get the first pending retransmission to expire after cycling. */
	pending = coap_pending_next_to_expire(pendings, NUM_PENDINGS);
	if (!pending) {
		return;
	}

	remaining = pending->t0 + pending->timeout - now;
	if (remaining < 0) {
		remaining = 0;
	}

	k_work_reschedule(&retransmit_work, K_MSEC(remaining));
}

static void remove_observer(struct sockaddr *addr);

static void retransmit_request(struct k_work *work)
{
	struct coap_pending *pending;
	int r;

	pending = coap_pending_next_to_expire(pendings, NUM_PENDINGS);
	if (!pending) {
		return;
	}

	if (!coap_pending_cycle(pending)) {
		remove_observer(&pending->addr);
		k_free(pending->data);
		coap_pending_clear(pending);
	} else {
		net_hexdump("Retransmit", pending->data, pending->len);

		r = sendto(sock, pending->data, pending->len, 0,
			   &pending->addr, ADDRLEN(&pending->addr));
		if (r < 0) {
			LOG_ERR("Failed to send %d", errno);
		}
	}

	schedule_next_retransmission();
}

static void update_counter(struct k_work *work)
{
	obs_counter++;

	if (resource_to_notify) {
		coap_resource_notify(resource_to_notify);
	}

	k_work_reschedule(&observer_work, K_SECONDS(0.1));
}

static int create_pending_request(struct coap_packet *response,
				  const struct sockaddr *addr)
{
	struct coap_pending *pending;
	int r;

	pending = coap_pending_next_unused(pendings, NUM_PENDINGS);
	if (!pending) {
		return -ENOMEM;
	}

	r = coap_pending_init(pending, response, addr, MAX_RETRANSMIT_COUNT);
	if (r < 0) {
		return -EINVAL;
	}

	coap_pending_cycle(pending);

	schedule_next_retransmission();

	return 0;
}

static int send_notification_packet(const struct sockaddr *addr,
				    socklen_t addr_len,
				    uint16_t age, uint16_t id,
				    const uint8_t *token, uint8_t tkl,
				    bool is_response)
{
	struct coap_packet response;
	char payload[14];
	uint8_t *data;
	uint8_t type;
	int r;

	if (is_response) {
		type = COAP_TYPE_ACK;
	} else {
		type = COAP_TYPE_CON;
	}

	if (!is_response) {
		id = coap_next_id();
	}

	data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	r = coap_packet_init(&response, data, MAX_COAP_MSG_LEN,
			     COAP_VERSION_1, type, tkl, token,
			     COAP_RESPONSE_CODE_CONTENT, id);
	if (r < 0) {
		goto end;
	}

	if (age >= 2U) {
		r = coap_append_option_int(&response, COAP_OPTION_OBSERVE, age);
		if (r < 0) {
			goto end;
		}
	}

	r = coap_append_option_int(&response, COAP_OPTION_CONTENT_FORMAT,
				   COAP_CONTENT_FORMAT_TEXT_PLAIN);
	if (r < 0) {
		goto end;
	}

	r = coap_packet_append_payload_marker(&response);
	if (r < 0) {
		goto end;
	}

	/* The response that coap-client expects */
	r = snprintk((char *) payload, sizeof(payload),
		     "Counter: %d\n", obs_counter);
	if (r < 0) {
		goto end;
	}

	r = coap_packet_append_payload(&response, (uint8_t *)payload,
				       strlen(payload));
	if (r < 0) {
		goto end;
	}

	if (type == COAP_TYPE_CON) {
		r = create_pending_request(&response, addr);
		if (r < 0) {
			goto end;
		}
	}

	k_work_reschedule(&observer_work, K_SECONDS(5));

	r = send_coap_reply(&response, addr, addr_len);

	/* On successful creation of pending request, do not free memory */
	if (type == COAP_TYPE_CON) {
		return r;
	}

end:
	k_free(data);

	return r;
}

static int obs_get(struct coap_resource *resource,
		   struct coap_packet *request,
		   struct sockaddr *addr, socklen_t addr_len)
{
	struct coap_observer *observer;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint16_t id;
	uint8_t code;
	uint8_t type;
	uint8_t tkl;
	bool observe = true;

	if (!coap_request_is_observe(request)) {
		if (coap_get_option_int(request, COAP_OPTION_OBSERVE) == 1) {
			remove_observer(addr);
		}
		observe = false;
		goto done;
	}

	observer = coap_observer_next_unused(observers, NUM_OBSERVERS);
	if (!observer) {
		LOG_ERR("Not enough observer slots.");
		return -ENOMEM;
	}

	coap_observer_init(observer, request, addr);

	coap_register_observer(resource, observer);

	resource_to_notify = resource;

done:
	code = coap_header_get_code(request);
	type = coap_header_get_type(request);
	id = coap_header_get_id(request);
	tkl = coap_header_get_token(request, token);

	LOG_INF("*******");
	LOG_INF("type: %u code %u id %u", type, code, id);
	LOG_INF("*******");

	return send_notification_packet(addr, addr_len,
					observe ? resource->age : 0,
					id, token, tkl, true);
}

static void obs_notify(struct coap_resource *resource,
		       struct coap_observer *observer)
{
	send_notification_packet(&observer->addr,
				 sizeof(observer->addr),
				 resource->age, 0,
				 observer->token, observer->tkl, false);
}

static int core_get(struct coap_resource *resource,
		    struct coap_packet *request,
		    struct sockaddr *addr, socklen_t addr_len)
{
	static const char dummy_str[] = "Just a test\n";
	struct coap_packet response;
	uint8_t token[COAP_TOKEN_MAX_LEN];
	uint8_t *data;
	uint16_t id;
	uint8_t tkl;
	int r;

	id = coap_header_get_id(request);
	tkl = coap_header_get_token(request, token);

	data = (uint8_t *)k_malloc(MAX_COAP_MSG_LEN);
	if (!data) {
		return -ENOMEM;
	}

	r = coap_packet_init(&response, data, MAX_COAP_MSG_LEN,
			     COAP_VERSION_1, COAP_TYPE_ACK, tkl, token,
			     COAP_RESPONSE_CODE_CONTENT, id);
	if (r < 0) {
		r = -EINVAL;
		goto end;
	}

	r = coap_packet_append_payload_marker(&response);
	if (r < 0) {
		r = -EINVAL;
		goto end;
	}

	r = coap_packet_append_payload(&response, (uint8_t *)dummy_str,
				       sizeof(dummy_str));
	if (r < 0) {
		r = -EINVAL;
		goto end;
	}

	r = send_coap_reply(&response, addr, addr_len);

end:
	k_free(data);

	return r;
}

static const char * const led_path[] = { "led", NULL };

static const char * const obs_path[] = { "obs", NULL };
static const char * const core_1_path[] = { "core1", NULL };
static const char * const core_1_attributes[] = {
	"title=\"Core 1\"",
	"rt=core1",
	NULL };

static const char * const core_2_path[] = { "core2", NULL };
static const char * const core_2_attributes[] = {
	"title=\"Core 1\"",
	"rt=core1",
	NULL };

static struct coap_resource resources[] = {
	{ .get = well_known_core_get,
	  .path = COAP_WELL_KNOWN_CORE_PATH,
	},
	{ .put = led_put,
	  .path = led_path,
	},
	{ .path = obs_path,
	  .get = obs_get,
	  .notify = obs_notify,
	},
	{ .get = core_get,
	  .path = core_1_path,
	  .user_data = &((struct coap_core_metadata) {
			 .attributes = core_1_attributes,
			 }),
	},
	{ .get = core_get,
	  .path = core_2_path,
	  .user_data = &((struct coap_core_metadata) {
			 .attributes = core_2_attributes,
			 }),
	},
	{ },
};

static struct coap_resource *find_resource_by_observer(
		struct coap_resource *resources, struct coap_observer *o)
{
	struct coap_resource *r;

	for (r = resources; r && r->path; r++) {
		sys_snode_t *node;

		SYS_SLIST_FOR_EACH_NODE(&r->observers, node) {
			if (&o->list == node) {
				return r;
			}
		}
	}

	return NULL;
}

static void remove_observer(struct sockaddr *addr)
{
	struct coap_resource *r;
	struct coap_observer *o;

	o = coap_find_observer_by_addr(observers, NUM_OBSERVERS, addr);
	if (!o) {
		return;
	}

	r = find_resource_by_observer(resources, o);
	if (!r) {
		LOG_ERR("Observer found but Resource not found\n");
		return;
	}

	LOG_INF("Removing observer %p", o);

	coap_remove_observer(r, o);
	memset(o, 0, sizeof(struct coap_observer));
}

static void process_coap_request(uint8_t *data, uint16_t data_len,
				 struct sockaddr *client_addr,
				 socklen_t client_addr_len)
{
	struct coap_packet request;
	struct coap_pending *pending;
	struct coap_option options[16] = { 0 };
	uint8_t opt_num = 16U;
	uint8_t type;
	int r;

	r = coap_packet_parse(&request, data, data_len, options, opt_num);
	if (r < 0) {
		LOG_ERR("Invalid data received (%d)\n", r);
		return;
	}

	type = coap_header_get_type(&request);

	pending = coap_pending_received(&request, pendings, NUM_PENDINGS);
	if (!pending) {
		goto not_found;
	}

	/* Clear CoAP pending request */
	if (type == COAP_TYPE_ACK || type == COAP_TYPE_RESET) {
		k_free(pending->data);
		coap_pending_clear(pending);

		if (type == COAP_TYPE_RESET) {
			remove_observer(client_addr);
		}
	}

	return;

not_found:
	r = coap_handle_request(&request, resources, options, opt_num,
				client_addr, client_addr_len);
	if (r < 0) {
		LOG_WRN("No handler for such request (%d)\n", r);
	}
}

static int process_client_request(void)
{
	int received;
	struct sockaddr client_addr;
	socklen_t client_addr_len;
	uint8_t request[MAX_COAP_MSG_LEN];

	do {
		client_addr_len = sizeof(client_addr);
		received = recvfrom(sock, request, sizeof(request), 0,
				    &client_addr, &client_addr_len);
		if (received < 0) {
			LOG_ERR("Connection error %d", errno);
			return -errno;
		}

		process_coap_request(request, received, &client_addr,
				     client_addr_len);
	} while (true);

	return 0;
}

void main(void)
{
	int r;

	LOG_DBG("Start CoAP-server sample");

#if defined(CONFIG_NET_IPV6)
	bool res;

	res = join_coap_multicast_group();
	if (!res) {
		goto quit;
	}
#endif

	r = start_coap_server();
	if (r < 0) {
		goto quit;
	}

	k_work_init_delayable(&retransmit_work, retransmit_request);
	k_work_init_delayable(&observer_work, update_counter);

	while (1) {
		r = process_client_request();
		if (r < 0) {
			goto quit;
		}
	}

	LOG_DBG("Done"); 
	return;

quit:
	LOG_ERR("Quit");
}
