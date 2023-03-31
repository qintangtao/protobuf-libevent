#include "pch.h"
#include "RpcChannelImpl.h"
#include "RpcControllerImpl.h"
#include "rpc_message.pb.h"


static struct timeval tv_read		= { 30, 0 };
static struct timeval tv_connect	= { 5, 0 };
static struct timeval tv_heartbeat	= { 10, 0 };
static struct timeval tv_request	= { 6, 0 };

#define CHANNEL_BEV_LOCK(b)        \
	do {                             \
		if (b)                       \
			EVLOCK_LOCK(b->m_bev_lock, 0); \
	} while (0)

#define CHANNEL_BEV_UNLOCK(b)        \
	do {                               \
		if (b)                         \
			EVLOCK_UNLOCK(b->m_bev_lock, 0); \
	} while (0)

#define CHANNEL_REQUEST_CACHE_LOCK(b)        \
	do {                             \
		if (b)                       \
			EVLOCK_LOCK(b->m_request_cache_lock, 0); \
	} while (0)

#define CHANNEL_REQUEST_CACHE_UNLOCK(b)        \
	do {                               \
		if (b)                         \
			EVLOCK_UNLOCK(b->m_request_cache_lock, 0); \
	} while (0)

#define CHANNEL_CALLBACK_CACHE_LOCK(b)        \
	do {                             \
		if (b)                       \
			EVLOCK_LOCK(b->m_callback_cache_lock, 0); \
	} while (0)

#define CHANNEL_CALLBACK_CACHE_UNLOCK(b)        \
	do {                               \
		if (b)                         \
			EVLOCK_UNLOCK(b->m_callback_cache_lock, 0); \
	} while (0)


static const struct error_entry {
	const enum ErrorCode code;
	const char *error_str;
} error_str_table[] = {
	{RPC_ERR_OK, "RPC_ERR_OK"},
	{RPC_ERR_NO_SERVICE, "RPC_ERR_NO_SERVICE"},
	{RPC_ERR_NO_METHOD, "RPC_ERR_NO_METHOD"},
	{RPC_ERR_INVALID_REQUEST, "RPC_ERR_INVALID_REQUEST"},
	{RPC_ERR_INVALID_RESPONSE, "RPC_ERR_INVALID_RESPONSE"},
	{RPC_ERR_REQUEST_TIMEOUT, "RPC_ERR_REQUEST_TIMEOUT"},
	{RPC_ERR_NO_NETWORK, "RPC_ERR_NO_NETWORK"},
	{ErrorCode_INT_MAX_SENTINEL_DO_NOT_USE_, "RPC_ERR_UNKNOWN"},
}; 

static const char *
guess_error_str(enum ErrorCode code)
{
	const struct error_entry *ent;
	for (ent = &error_str_table[0]; ent->code != ErrorCode_INT_MAX_SENTINEL_DO_NOT_USE_; ++ent) {
		if (code == ent->code)
			return ent->error_str;
	}

	return ent->error_str;
}

RpcChannelImpl::RpcChannelImpl(const char *ip_as_string)
	: m_id(0)
	, m_base(NULL)
	, m_bev(NULL)
	, m_bev_lock(NULL)
	, m_connect_timer(NULL)
	, m_heartbeat_timer(NULL)
	, m_threadid(0)
{ 
	memcpy(m_packet.magic, RPC_MAGIC, sizeof(m_packet.magic));
	m_packet.major_version = RPC_MAJOR_VERSION;
	m_packet.minor_version = RPC_MINOR_VERSION;

	memcpy(m_packet_heartbeat.magic, RPC_MAGIC, sizeof(m_packet_heartbeat.magic));
	m_packet_heartbeat.major_version = RPC_MAJOR_VERSION;
	m_packet_heartbeat.minor_version = RPC_MINOR_VERSION;
	m_packet_heartbeat.length = 0;

	EVTHREAD_ALLOC_LOCK(m_bev_lock, EVTHREAD_LOCKTYPE_READWRITE);
	EVTHREAD_ALLOC_LOCK(m_request_cache_lock, EVTHREAD_LOCKTYPE_READWRITE);
	EVTHREAD_ALLOC_LOCK(m_callback_cache_lock, EVTHREAD_LOCKTYPE_READWRITE);
	
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
	// 在event_cb中重新设置
	m_bev = NULL;

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

	m_request_timer = evtimer_new(m_base, &RpcChannelImpl::request_time_cb, this);
	if (!m_request_timer) {
		fprintf(stderr, "Can't create request timer\n");
		exit(EXIT_FAILURE);
	}
	evtimer_add(m_request_timer, &tv_request);

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

	if (m_request_timer)
		event_free(m_request_timer);
	
	if (m_bev)
		bufferevent_free(m_bev);

	if (m_base)
		event_base_free(m_base);

	EVTHREAD_FREE_LOCK(m_bev_lock, EVTHREAD_LOCKTYPE_READWRITE);
	EVTHREAD_FREE_LOCK(m_request_cache_lock, EVTHREAD_LOCKTYPE_READWRITE);
	EVTHREAD_FREE_LOCK(m_callback_cache_lock, EVTHREAD_LOCKTYPE_READWRITE);
}

void RpcChannelImpl::CallMethod(const google::protobuf::MethodDescriptor* method,
	google::protobuf::RpcController* controller, const google::protobuf::Message* request,
	google::protobuf::Message* response, google::protobuf::Closure* done)
{
	if (!controller || !done)
		return;

	RpcMessage message;
	message.set_type(RPC_TYPE_REQUEST);
	message.set_id(++m_id);
	message.set_service(method->service()->name());
	message.set_method(method->name());
	if (request) message.set_request(request->SerializeAsString());

	std::string message_str;
	message.SerializeToString(&message_str);

	// add to cache
	struct callback_cache cache = { controller, response, done };
	evutil_gettimeofday(&cache.tv, NULL);
	add_callback_cache(m_id, cache);

	CHANNEL_BEV_LOCK(this);
	if (m_bev)
	{		
		send_no_lock(message_str);
		CHANNEL_BEV_UNLOCK(this);
	}
	else
	{
		CHANNEL_BEV_UNLOCK(this);

		// 等待重连网络后发送，如果超时则移除
		add_request_cache(m_id, message_str);
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

	// try reconnect
	struct bufferevent *bev = pChannel->create_bufferevent_socket();
	if (bev)
		return;

	if (pChannel->m_connect_timer)
		evtimer_add(pChannel->m_connect_timer, &tv_connect);
}

void RpcChannelImpl::heartbeat_time_cb(evutil_socket_t fd, short event, void *arg)
{
	RpcChannelImpl *pChannel = (RpcChannelImpl *)arg;

	pChannel->send_heartbeat();

	evtimer_add(pChannel->m_heartbeat_timer, &tv_heartbeat);
}

void RpcChannelImpl::request_time_cb(evutil_socket_t fd, short event, void *arg)
{
	RpcChannelImpl *pChannel = (RpcChannelImpl *)arg;

	pChannel->guess_callback_cache();

	evtimer_add(pChannel->m_request_timer, &tv_request);
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
			//fprintf(stderr, "major_version:%d, minor_version:%d, length:%d\n",
			//	packet.major_version, packet.minor_version, packet.length);
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
	} else if (events & BEV_EVENT_ERROR) {
		fprintf(stdout, "Got an error on the connection\n"); /*XXX win32*/
	} else if (events & BEV_EVENT_TIMEOUT) {
		fprintf(stdout, "Connection timeout.\n");
	} else if (events & BEV_EVENT_CONNECTED) {
		CHANNEL_BEV_LOCK(pChannel);
		pChannel->m_bev = bev;
		CHANNEL_BEV_UNLOCK(pChannel);

		pChannel->send_request_cache();

		return;
	} else {
		return;
	}

	CHANNEL_BEV_LOCK(pChannel);
	pChannel->m_bev = NULL;
	CHANNEL_BEV_UNLOCK(pChannel);

	bufferevent_free(bev);

	if (pChannel->m_connect_timer)
		evtimer_add(pChannel->m_connect_timer, &tv_connect);
}

struct bufferevent *RpcChannelImpl::create_bufferevent_socket()
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

void RpcChannelImpl::send_no_lock(const std::string &message_str)
{
	struct evbuffer *output = bufferevent_get_output(m_bev);

	m_packet.length = (unsigned int)message_str.size();
	
	evbuffer_add(output, &m_packet, sizeof(RPC_PACKET));
	evbuffer_add(output, message_str.c_str(), message_str.size());
}

void RpcChannelImpl::send_heartbeat()
{
	struct evbuffer *output;

	CHANNEL_BEV_LOCK(this);
	if (m_bev) {
		output = bufferevent_get_output(m_bev);
		evbuffer_add(output, &m_packet_heartbeat, sizeof(RPC_PACKET));
	}
	CHANNEL_BEV_UNLOCK(this);
}

void RpcChannelImpl::send_request_cache()
{
	std::string message_str;

	CHANNEL_BEV_LOCK(this);
	if (m_bev) {
		while (del_request_cache_first(message_str))
			send_no_lock(message_str);
	}
	CHANNEL_BEV_UNLOCK(this);
}

void RpcChannelImpl::decode(const std::string &message_str)
{
	std::map<uint64_t, struct request_cache>::iterator iter;
	struct callback_cache cache;
	RpcMessage message;

	do 
	{
		if (!message.ParseFromString(message_str)) {
			std::cout << "Parse message failed!" << std::endl;
			break;
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

		if (!del_callback_cache(message.id(), cache))
			break;

		if (cache.controller && cache.controller->IsCanceled()) {
			// Canceled
		} else {
			if (message.error() == RPC_ERR_OK) {
				if (cache.response && !cache.response->ParseFromString(message.response())) {
					if (cache.controller)
						cache.controller->SetFailed(guess_error_str(RPC_ERR_INVALID_RESPONSE));
				}
			} else {
				if (cache.controller)
					cache.controller->SetFailed(guess_error_str(message.error()));
			}
		}
		
		if (cache.done)
			cache.done->Run();

	} while (0);
}

void RpcChannelImpl::add_request_cache(uint64_t id, const std::string &message_str)
{
	CHANNEL_REQUEST_CACHE_LOCK(this);
	m_request_cache.insert(std::make_pair(id, message_str));
	CHANNEL_REQUEST_CACHE_UNLOCK(this);
}

bool RpcChannelImpl::del_request_cache(uint64_t id, std::string &message_str)
{
	std::map<uint64_t, std::string>::iterator iter;

	CHANNEL_REQUEST_CACHE_LOCK(this);
	iter = m_request_cache.find(id);
	if (iter == m_request_cache.end()) {
		//std::cout << "Do not Find, id: " << id << std::endl;
		CHANNEL_REQUEST_CACHE_UNLOCK(this);
		return false;
	}

	message_str = iter->second;

	// del from cache
	m_request_cache.erase(iter);

	CHANNEL_REQUEST_CACHE_UNLOCK(this);

	return true;
}

bool RpcChannelImpl::del_request_cache_first(std::string &message_str)
{
	std::map<uint64_t, std::string>::iterator iter;

	CHANNEL_REQUEST_CACHE_LOCK(this);
	iter = m_request_cache.begin();
	if (iter == m_request_cache.end()) {
		CHANNEL_REQUEST_CACHE_UNLOCK(this);
		return false;
	}

	message_str = iter->second;

	// del from cache
	m_request_cache.erase(iter);

	CHANNEL_REQUEST_CACHE_UNLOCK(this);

	return true;
}

bool RpcChannelImpl::has_request_cache(uint64_t id)
{
	std::map<uint64_t, std::string>::iterator iter;

	CHANNEL_REQUEST_CACHE_LOCK(this);
	iter = m_request_cache.find(id);
	if (iter == m_request_cache.end()) {
		//std::cout << "Do not Find, id: " << id << std::endl;
		CHANNEL_REQUEST_CACHE_UNLOCK(this);
		return false;
	}

	CHANNEL_REQUEST_CACHE_UNLOCK(this);

	return true;
}

void RpcChannelImpl::guess_callback_cache()
{
	std::map<uint64_t, struct callback_cache>::iterator	iter;
	std::string message_str;
	struct timeval te, ts;

	evutil_gettimeofday(&te, NULL);

	CHANNEL_CALLBACK_CACHE_LOCK(this);
	while ((iter = m_callback_cache.begin()) != m_callback_cache.end())
	{
		uint64_t id = iter->first;
		struct callback_cache &cache = iter->second;

		if (cache.controller && cache.controller->IsCanceled()) {
			// Canceled

			// del from request cache
			del_request_cache(id, message_str);

		} else {
			// check request timeout
			evutil_timersub(&te, &cache.tv, &ts);

			if ((ts.tv_sec * 1000000L + ts.tv_usec) < 12000000L)
				break;

			// del from request cache
			if (del_request_cache(id, message_str))
				cache.controller->SetFailed(guess_error_str(RPC_ERR_NO_NETWORK));
			else
				cache.controller->SetFailed(guess_error_str(RPC_ERR_REQUEST_TIMEOUT));
		}

		if (cache.done)
			cache.done->Run();

		// del from callback cache
		m_callback_cache.erase(iter);
	}
	CHANNEL_CALLBACK_CACHE_UNLOCK(this);
}

void RpcChannelImpl::add_callback_cache(uint64_t id, const struct callback_cache &cache)
{
	CHANNEL_CALLBACK_CACHE_LOCK(this);
	m_callback_cache.insert(std::make_pair(id, cache));
	CHANNEL_CALLBACK_CACHE_UNLOCK(this);
}

bool RpcChannelImpl::del_callback_cache(uint64_t id, struct callback_cache &cache)
{
	std::map<uint64_t, struct callback_cache>::iterator iter;

	CHANNEL_CALLBACK_CACHE_LOCK(this);
	iter = m_callback_cache.find(id);
	if (iter == m_callback_cache.end()) {
		//std::cout << "Do not Find, id: " << id << std::endl;
		CHANNEL_CALLBACK_CACHE_UNLOCK(this);
		return false;
	}

	cache = iter->second;

	// del from cache
	m_callback_cache.erase(iter);

	CHANNEL_CALLBACK_CACHE_UNLOCK(this);

	return true;
}