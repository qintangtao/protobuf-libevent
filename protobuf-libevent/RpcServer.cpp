#include "pch.h"
#include "RpcServer.h"
#include "EchoServiceImpl.h"
#include "RpcControllerImpl.h"
#include<iostream>
#include<fstream>
#include<sstream>

static int use_thread_pool = 0; 
static struct timeval tv_read = { 30, 0 };

#define RPC_CLIENT_CACHE_LOCK(b)        \
	do {                             \
		if (b)                       \
			EVLOCK_LOCK(b->m_client_cache_lock, 0); \
	} while (0)

#define RPC_CLIENT_CACHE_UNLOCK(b)        \
	do {                               \
		if (b)                         \
			EVLOCK_UNLOCK(b->m_client_cache_lock, 0); \
	} while (0)

class RpcServerClosure : public google::protobuf::Closure {
public:
	typedef void(*FunctionType)(
		const std::string &addr,
		RpcMessage &message,
		RpcServer *server,
		google::protobuf::RpcController* controller, 
		google::protobuf::Message *request, 
		google::protobuf::Message *response);
	
	RpcServerClosure(FunctionType function, RpcServer *server, const std::string &addr, bool self_deleting=true)
		: function_(function), server_(server), addr_(addr), self_deleting_(self_deleting)
		, controller(NULL), request(NULL), response(NULL) {}
	~RpcServerClosure() {}

	void Run() override {
		bool needs_delete = self_deleting_;  // read in case callback deletes
		function_(addr_, message, server_, controller, request, response);
		if (needs_delete) delete this;
	}

	RpcMessage message;
	google::protobuf::RpcController* controller;
	google::protobuf::Message *request;
	google::protobuf::Message *response;

private:
	FunctionType function_;
	bool self_deleting_;
	std::string addr_;
	RpcServer *server_;
};


static std::string get_bev_addr(struct bufferevent *bev)
{
	struct sockaddr_storage ss;
	evutil_socket_t fd = bufferevent_getfd(bev);
	ev_socklen_t socklen = sizeof(ss);
	char addrbuf[128];
	void *inaddr = NULL;
	const char *addr;
	uint16_t got_port = -1;

	memset(&ss, 0, sizeof(ss));
	if (getpeername(fd, (struct sockaddr *)&ss, &socklen)) {
		perror("getsockname() failed");
		exit(EXIT_FAILURE);
		return std::string();
	}

	if (ss.ss_family == AF_INET) {
		got_port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
		inaddr = &((struct sockaddr_in *)&ss)->sin_addr;
	}
	else if (ss.ss_family == AF_INET6) {
		got_port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
		inaddr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
	}

	addr = evutil_inet_ntop(ss.ss_family, inaddr, addrbuf, sizeof(addrbuf));
	if (addr) {
		//printf("%s:%d\n", addr, got_port);
		std::stringstream sss;
		sss << addr << ":" << got_port;
		std::string str = sss.str();
		return str;
	}
	else {
		fprintf(stderr, "evutil_inet_ntop failed\n");
		exit(EXIT_FAILURE);
		return std::string();
	}
}

RpcServer::RpcServer(unsigned short port)
	: m_base(NULL)
	, m_listener(NULL)
	, m_threadid(0)
{
	struct sockaddr_in sin = { 0 };
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);

	m_spServiceMap.insert(std::make_pair("EchoService", new EchoServiceImpl()));
	
	EVTHREAD_ALLOC_LOCK(m_client_cache_lock, EVTHREAD_LOCKTYPE_READWRITE);
	
	m_base = event_base_new();
	if (!m_base) {
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		exit(EXIT_FAILURE);
	}

	m_listener = evconnlistener_new_bind(m_base, accept_socket_cb, (void *)this,
		LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
		(struct sockaddr *)&sin, sizeof(sin));
	if (!m_listener) {
		fprintf(stderr, "Could not create a listener!\n");
		exit(EXIT_FAILURE);
	}

	m_threadid = sys_os_create_thread(&RpcServer::worker, this);
}

RpcServer::~RpcServer()
{
	if (m_base)
		event_base_loopbreak(m_base);

	while (m_threadid != 0)
		USleep(1000);

	if (m_listener)
		evconnlistener_free(m_listener);

	if (m_base)
		event_base_free(m_base);

	EVTHREAD_FREE_LOCK(m_client_cache_lock, EVTHREAD_LOCKTYPE_READWRITE);
}

void *RpcServer::worker(void *arg)
{
	RpcServer *pServer = (RpcServer *)arg;

	event_base_dispatch(pServer->m_base);

	pServer->m_threadid = 0;

	return NULL;
}

void RpcServer::accept_socket_cb(struct evconnlistener *listener, 
	evutil_socket_t fd, struct sockaddr *sa, int socklen, void *arg)
{
	RpcServer *pServer = (RpcServer *)arg;
	struct bufferevent *bev;

	if (use_thread_pool) {
		//struct eveasy_thread_pool *pool = arg;
		//eveasy_thread_pool_assign(pool, fd, sa, socklen);
	}
	else {
		bev = create_bufferevent_socket(pServer->m_base, fd, arg);
		if (bev) {
			pServer->add_client_cache(get_bev_addr(bev), bev);
		}
	}
}

void RpcServer::read_cb(struct bufferevent *bev, void *arg)
{
	RpcServer *pServer = (RpcServer *)arg;
	struct evbuffer *input = bufferevent_get_input(bev);
	struct evbuffer *output = bufferevent_get_output(bev);
	std::string addr = get_bev_addr(bev);
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
			evbuffer_add(output, &packet, sizeof(RPC_PACKET));
			continue;
		}

		// read packet body
		body = evbuffer_pullup(input, packet.length);
		if (!body) {
			fprintf(stderr, "major_version:%d, minor_version:%d, length:%d evbuffer pullup\n",
				packet.major_version, packet.minor_version, packet.length);
			// clear buffer
			evbuffer_drain(input, evbuffer_get_length(input));
			break;
		}

		if (pServer)
			pServer->decode(addr, std::string((const char *)body, packet.length));

		// remove packet body data
		evbuffer_drain(input, packet.length);
	}
}

void RpcServer::event_cb(struct bufferevent *bev, short events, void *arg)
{
	RpcServer *pServer = (RpcServer *)arg;

	if (events & BEV_EVENT_EOF) {
		printf("Connection closed.\n");
	}
	else if (events & BEV_EVENT_ERROR) {
		printf("Got an error on the connection\n"); /*XXX win32*/
	}
	else if (events & BEV_EVENT_TIMEOUT) {
		printf("Connection timeout.\n");
	}
	else {
		return;
	}

	pServer->del_client_cache(get_bev_addr(bev));

	/* None of the other events can happen here, since we haven't enabled
	 * timeouts */
	bufferevent_free(bev);
}

void RpcServer::done_cb(const std::string &addr, RpcMessage &message, RpcServer *server,
	google::protobuf::RpcController* controller, google::protobuf::Message *request, google::protobuf::Message *response)
{
	struct bufferevent *bev;

	do 
	{
		if (!server)
			break;

		if (controller && controller->IsCanceled())
			break;

		if (response)
			message.set_response(response->SerializeAsString());

		std::string message_str;
		message.SerializeToString(&message_str);

		// send to client
		RPC_CLIENT_CACHE_LOCK(server);
		bev = server->lookup_client_cache_no_lock(addr);
		if (bev)
			server->send_no_lock(bev, message_str);
		RPC_CLIENT_CACHE_UNLOCK(server);

	} while (0);

	if (controller)
		delete controller;
	if (request)
		delete request;
	if (response)
		delete response;
}

struct bufferevent *RpcServer::create_bufferevent_socket(struct event_base *base, evutil_socket_t fd, void *arg)
{
	struct bufferevent *bev;

	bev = bufferevent_socket_new(base, fd,
		BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_THREADSAFE);
	if (!bev) {
		fprintf(stderr, "Couldn't new bufferevent socket.\n");
		goto err;
	}

	bufferevent_setcb(bev, &RpcServer::read_cb, NULL, &RpcServer::event_cb, arg);
	bufferevent_set_timeouts(bev, &tv_read, NULL);
	bufferevent_enable(bev, EV_READ | EV_WRITE);

	return bev;

err:
	evutil_closesocket(fd);

	return NULL;
}

void RpcServer::send_no_lock(struct bufferevent *bev, const std::string &message_str)
{
	struct evbuffer *output = bufferevent_get_output(bev);

	RPC_PACKET packet;
	memcpy(packet.magic, RPC_MAGIC, sizeof(packet.magic));
	packet.major_version = RPC_MAJOR_VERSION;
	packet.minor_version = RPC_MINOR_VERSION;
	packet.length = (unsigned int)message_str.size();

	// 多线程中调用，会存在数据不连续的情况（使用了两次evbuffer_add， 最好改成1次）
	evbuffer_add(output, &packet, sizeof(RPC_PACKET));
	evbuffer_add(output, message_str.c_str(), message_str.size());
}

void RpcServer::decode(const std::string &addr, const std::string &message_str)
{
	std::map<std::string, google::protobuf::Service *>::iterator iter;
	const google::protobuf::ServiceDescriptor *serviceDesc;
	const google::protobuf::MethodDescriptor *methodDesc;
	google::protobuf::Service *service;
	RpcServerClosure* done = NULL;
	RpcMessage message;

	do
	{
		if (!message.ParseFromString(message_str)) {
			std::cout << "Parse message failed!" << std::endl;
			break;
		}

#ifndef NDEBUG
		std::cout << "server -> "
			<< "size: " << message_str.size()
			<< ", type: " << message.type()
			<< ", id: " << message.id()
			<< ", service: " << message.service()
			<< ", method: " << message.method()
			<< ", error: " << message.error()
			<< ", request: " << message.request().size()
			<< std::endl;
#endif

		done = new RpcServerClosure(&RpcServer::done_cb, this, addr);
		done->message.set_type(RPC_TYPE_RESPONSE);
		done->message.set_id(message.id());
		done->message.set_service(message.service());
		done->message.set_method(message.method());
		done->message.set_error(RPC_ERR_OK);

		iter = m_spServiceMap.find(message.service());
		if (iter == m_spServiceMap.end()) {
			std::cout << "Do not Find, service: " << message.service() << std::endl;
			done->message.set_error(RPC_ERR_NO_SERVICE);
			break;
		}

		service = iter->second;

		serviceDesc = service->GetDescriptor();
		if (!serviceDesc) {
			std::cout << "Do not Find Descriptor, service: " << message.service() << std::endl;
			done->message.set_error(RPC_ERR_NO_SERVICE);
			break;
		}

		methodDesc = serviceDesc->FindMethodByName(message.method());
		if (!methodDesc) {
			std::cout << "Do not Find, method: " << message.method() << std::endl;
			done->message.set_error(RPC_ERR_NO_METHOD);
			break;
		}

		// 构造 request & response 对象
		done->request = service->GetRequestPrototype(methodDesc).New();
		done->response = service->GetResponsePrototype(methodDesc).New();

		if (!done->request->ParseFromString(message.request())) {
			std::cout << "Parse reqeust failed!" << std::endl;
			done->message.set_error(RPC_ERR_INVALID_REQUEST);
			break;
		}

		done->controller = new RpcControllerImpl();

		// 优化，可以放入工作线程池中（可以分成网络线程池和工作线程池）
		service->CallMethod(methodDesc, done->controller, done->request, done->response, done);

		return;

	} while (0);

	if (done)
		done->Run();
}

void RpcServer::add_client_cache(const std::string &addr, struct bufferevent *bev)
{
	RPC_CLIENT_CACHE_LOCK(this);
	m_client_cache.insert(std::make_pair(addr, bev));
	RPC_CLIENT_CACHE_UNLOCK(this);
}

struct bufferevent *RpcServer::del_client_cache(const std::string &addr)
{
	std::map<std::string, struct bufferevent *>::iterator iter;
	struct bufferevent *bev;

	RPC_CLIENT_CACHE_LOCK(this);
	iter = m_client_cache.find(addr);
	if (iter == m_client_cache.end()) {
		RPC_CLIENT_CACHE_UNLOCK(this);
		return NULL;
	}

	bev = iter->second;

	// del from cache
	m_client_cache.erase(iter);

	RPC_CLIENT_CACHE_UNLOCK(this);

	return bev;
}

struct bufferevent *RpcServer::lookup_client_cache_no_lock(const std::string &addr)
{
	std::map<std::string, struct bufferevent *>::iterator iter;
	
	iter = m_client_cache.find(addr);
	if (iter != m_client_cache.end())
		return iter->second;
	
	return NULL;
}
