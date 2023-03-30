#include "pch.h"
#include "RpcChannelImpl.h"
#include "RpcControllerImpl.h"
#include "rpc_message.pb.h"

static struct timeval tv_read = { 30, 0 };
static struct timeval tv_connect = { 5, 0 };
static struct timeval tv_heartbeat = { 10, 0 };


RpcChannelImpl::RpcChannelImpl(const char *ip_as_string)
	: m_id(0)
	, m_base(NULL)
	, m_bev(NULL)
	, m_connect_timer(NULL)
	, m_heartbeat_timer(NULL)
	, m_threadid(0)
{ 
	memcpy(m_packet.magic, RPC_MAGIC, sizeof(m_packet.magic));
	m_packet.major_version = RPC_MAJOR_VERSION;
	m_packet.minor_version = RPC_MINOR_VERSION;

	memset(&m_connect_to_addr, 0, sizeof(m_connect_to_addr));
	m_connect_to_addrlen = sizeof(m_connect_to_addr);
	if (evutil_parse_sockaddr_port(ip_as_string, (struct sockaddr *)&m_connect_to_addr, &m_connect_to_addrlen) < 0) {
		fprintf(stderr, "Couldn't parse sockaddr: exiting\n");
		exit(EXIT_FAILURE);
	}

	m_base = event_base_new();
	if (!m_base) {
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		exit(EXIT_FAILURE);
	}

	m_bev = create_bufferevent_socket();
	if (!m_bev) {
		fprintf(stderr, "Can't create bev socket\n");
		exit(EXIT_FAILURE);
	}

	m_connect_timer = evtimer_new(m_base, &RpcChannelImpl::connect_time_cb, this);
	if (!m_connect_timer) {
		fprintf(stderr, "Can't create connect timer\n");
		exit(EXIT_FAILURE);
	}

	m_heartbeat_timer = evtimer_new(m_base, &RpcChannelImpl::heartbeat_time_cb, this);
	if (!m_heartbeat_timer) {
		fprintf(stderr, "Can't create heartbeat timer\n");
		exit(EXIT_FAILURE);
	}

	evtimer_add(m_heartbeat_timer, &tv_heartbeat);

	m_threadid = sys_os_create_thread(&RpcChannelImpl::worker, this);
}

RpcChannelImpl::~RpcChannelImpl()
{
	if (m_base)
		event_base_loopbreak(m_base);

	while (m_threadid != 0)
		USleep(1000);

	if (m_connect_timer)
		event_free(m_connect_timer);

	if (m_heartbeat_timer)
		event_free(m_heartbeat_timer);

	if (m_bev)
		bufferevent_free(m_bev);

	if (m_base)
		event_base_free(m_base);
}

void RpcChannelImpl::CallMethod(const google::protobuf::MethodDescriptor* method,
	google::protobuf::RpcController* controller, const google::protobuf::Message* request,
	google::protobuf::Message* response, google::protobuf::Closure* done)
{
	if (m_bev)
	{
		RpcMessage message;
		message.set_type(RPC_TYPE_REQUEST);
		message.set_id(++m_id);
		message.set_service(method->service()->name());
		message.set_method(method->name());
		if (request) {
			message.set_request(request->SerializeAsString());
		}

		std::string message_str;
		message.SerializeToString(&message_str);

		// add to map
		struct request_cache cache = { controller, response, done };
		m_mapRequests.insert(std::make_pair(message.id(), cache));

		m_packet.length = (unsigned int)message_str.size();

		struct evbuffer *output = bufferevent_get_output(m_bev);
		evbuffer_add(output, &m_packet, sizeof(RPC_PACKET));
		evbuffer_add(output, message_str.c_str(), message_str.size());
	}
	else
	{
		if (done)
			done->Run();
	}
}

void *RpcChannelImpl::worker(void *arg)
{
	RpcChannelImpl *pChannel = (RpcChannelImpl *)arg;

	event_base_dispatch(pChannel->m_base);

	pChannel->m_threadid = 0;

	return NULL;
}

void RpcChannelImpl::connect_time_cb(evutil_socket_t fd, short event, void *arg)
{
	RpcChannelImpl *pChannel = (RpcChannelImpl *)arg;

	pChannel->m_bev = pChannel->create_bufferevent_socket();
	if (pChannel->m_bev)
		return;

	if (pChannel->m_connect_timer)
		evtimer_add(pChannel->m_connect_timer, &tv_connect);
}

void RpcChannelImpl::heartbeat_time_cb(evutil_socket_t fd, short which, void *arg)
{
	RpcChannelImpl *pChannel = (RpcChannelImpl *)arg;
	struct evbuffer *output;

	if (!pChannel->m_bev)
		return;

	RPC_PACKET packet;
	memcpy(packet.magic, RPC_MAGIC, sizeof(packet.magic));
	packet.major_version = RPC_MAJOR_VERSION;
	packet.minor_version = RPC_MINOR_VERSION;
	packet.length = 0;

	output = bufferevent_get_output(pChannel->m_bev);
	evbuffer_add(output, &packet, sizeof(RPC_PACKET));

	evtimer_add(pChannel->m_heartbeat_timer, &tv_heartbeat);
}

void RpcChannelImpl::read_cb(struct bufferevent *bev, void *arg)
{
	RpcChannelImpl *pChannel = (RpcChannelImpl *)arg;
	struct evbuffer *input = bufferevent_get_input(bev);
	RPC_PACKET packet;
	unsigned char *body;
	ev_ssize_t total, size;

	while ((total = evbuffer_get_length(input)) >= sizeof(RPC_PACKET)) {

		// copy packet header
		size = evbuffer_copyout(input, &packet, sizeof(RPC_PACKET));
		if (size < sizeof(RPC_PACKET))
			break;

		if (memcmp(packet.magic, RPC_MAGIC, sizeof(packet.magic)) != 0) {
			fprintf(stderr, "magic error %02x %02x %02x %02x %02x %02x\n",
				packet.magic[0],
				packet.magic[1],
				packet.magic[2],
				packet.magic[3],
				packet.magic[4],
				packet.magic[5]);
			evbuffer_drain(input, 1);
			continue;
		}

		if (packet.major_version != RPC_MAJOR_VERSION ||
			packet.minor_version != RPC_MINOR_VERSION) {
			fprintf(stderr, "check version: %d-%d\n", packet.major_version, packet.minor_version);
			evbuffer_drain(input, sizeof(RPC_PACKET));
			continue;
		}

		if (total < (ev_ssize_t)(packet.length + sizeof(RPC_PACKET))) {
			// continue to read
			break;
		}

		// remove packet header data
		evbuffer_drain(input, sizeof(RPC_PACKET));

		if (packet.length == 0) {
			fprintf(stderr, "major_version:%d, minor_version:%d, length:%d\n",
				packet.major_version, packet.minor_version, packet.length);
			continue;
		}

		// read packet body (h264 data)
		body = evbuffer_pullup(input, packet.length);
		if (!body) {
			fprintf(stderr, "major_version:%d, minor_version:%d, length:%d evbuffer pullup\n",
				packet.major_version, packet.minor_version, packet.length);
			// clear buffer
			evbuffer_drain(input, evbuffer_get_length(input));
			break;
		}

		if (pChannel)
			pChannel->decode(std::string((const char *)body, packet.length));

		// remove packet body data
		evbuffer_drain(input, packet.length);
	}

}

void RpcChannelImpl::event_cb(struct bufferevent *bev, short events, void *arg)
{
	RpcChannelImpl *pChannel = (RpcChannelImpl *)arg;

	if (events & BEV_EVENT_EOF) {
		fprintf(stdout, "Connection closed.\n");
	}
	else if (events & BEV_EVENT_ERROR) {
		fprintf(stdout, "Got an error on the connection\n"); /*XXX win32*/
	}
	else if (events & BEV_EVENT_TIMEOUT) {
		fprintf(stdout, "Connection timeout.\n");
	}
	else {
		return;
	}

	pChannel->m_bev = NULL;
	bufferevent_free(bev);

	if (pChannel->m_connect_timer)
		evtimer_add(pChannel->m_connect_timer, &tv_connect);
}

struct bufferevent *
	RpcChannelImpl::create_bufferevent_socket()
{
	struct bufferevent *bev = NULL;

	if (!m_base)
		goto err;

	bev = bufferevent_socket_new(m_base, -1,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
	if (!bev) {
		fprintf(stderr, "Couldn't open listener.\n");
		goto err;
	}

	if (bufferevent_socket_connect(
		bev, (struct sockaddr *)&m_connect_to_addr, m_connect_to_addrlen) < 0) {
		perror("bufferevent socket connect");
		goto err;
	}

	bufferevent_setcb(bev, &RpcChannelImpl::read_cb, NULL, &RpcChannelImpl::event_cb, this);
	bufferevent_set_timeouts(bev, &tv_read, NULL);
	bufferevent_enable(bev, EV_READ | EV_WRITE);

	return bev;

err:
	if (bev)
		bufferevent_free(bev);

	return NULL;
}


void RpcChannelImpl::decode(const std::string &message_str)
{
	std::map<uint64_t, struct request_cache>::iterator iter;
	google::protobuf::RpcController* controller = NULL;
	google::protobuf::Message *response = NULL;
	google::protobuf::Closure* done = NULL;
	struct request_cache cache;
	RpcMessage message;

	if (!message.ParseFromString(message_str)) {
		std::cout << "Parse message failed!" << std::endl;
		return;
	}

#ifndef NDEBUG
	std::cout << "client -> "
		<< "size: " << message_str.size()
		<< ", type: " << message.type()
		<< ", id: " << message.id()
		<< ", service: " << message.service()
		<< ", method: " << message.method()
		<< ", error: " << message.error()
		<< ", response: " << message.response().size()
		<< std::endl;
#endif

	do 
	{
		iter = m_mapRequests.find(message.id());
		if (iter == m_mapRequests.end()) {
			std::cout << "Do not Find, id: " << message.id() << std::endl;
			break;
		}

		cache = iter->second;
		controller = cache.controller;
		response = cache.response;
		done = cache.done;

		// del from map
		m_mapRequests.erase(iter);

		if (controller && controller->IsCanceled())
			break;

		if (!response)
			break;

		if (!done)
			break;

		switch (message.error())
		{
		case RPC_ERR_OK:
		{
			if (!response->ParseFromString(message.response())) {
				if (controller)
					controller->SetFailed("Parse response failed");
			}
		}
			break;
		case RPC_ERR_NO_SERVICE:
		{
			if (controller)
				controller->SetFailed("RPC_ERR_NO_SERVICE");
		}
			break;
		case RPC_ERR_NO_METHOD:
		{
			if (controller)
				controller->SetFailed("RPC_ERR_NO_METHOD");
		}
			break;
		case RPC_ERR_INVALID_REQUEST:
		{
			if (controller)
				controller->SetFailed("RPC_ERR_INVALID_REQUEST");
		}
			break;
		case RPC_ERR_INVALID_RESPONSE:
		{
			if (controller)
				controller->SetFailed("RPC_ERR_INVALID_RESPONSE");
		}
			break;
		default:
		{
			if (controller)
				controller->SetFailed("RPC_ERR_UNKNOWN");
		}
			break;
		}
		
		done->Run();

	} while (0);
}