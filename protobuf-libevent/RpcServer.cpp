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

#define BEV_CLIENT_LOCK(b)        \
	do {                             \
		if (b)                       \
			EVLOCK_LOCK(b->lock, 0); \
	} while (0)

#define BEV_CLIENT_UNLOCK(b)        \
	do {                               \
		if (b)                         \
			EVLOCK_UNLOCK(b->lock, 0); \
	} while (0)


struct bufferevent_client {
	struct bufferevent *bev;
	int refcnt;
	void *lock;
	void *arg;
};

static struct bufferevent_client *bufferevent_client_new(struct bufferevent *bev, void *arg)
{
	struct bufferevent_client *client;

	client = (struct bufferevent_client *)malloc(sizeof(struct bufferevent_client));
	if (!client)
		return NULL;

	client->bev = bev;
	client->refcnt = 1;
	client->arg = arg;

	EVTHREAD_ALLOC_LOCK(client->lock, EVTHREAD_LOCKTYPE_READWRITE);

	return client;
}

static void bufferevent_client_free(struct bufferevent_client *client)
{
	BEV_CLIENT_LOCK(client);

	if (--client->refcnt > 0) {
		BEV_CLIENT_UNLOCK(client);
		return;
	}

	BEV_CLIENT_UNLOCK(client);
	if (client->lock)
		EVTHREAD_FREE_LOCK(client->lock, EVTHREAD_LOCKTYPE_READWRITE);

	free(client);
}

static void bufferevent_client_add_reference(struct bufferevent_client *client)
{
	BEV_CLIENT_LOCK(client);
	client->refcnt++;
	BEV_CLIENT_UNLOCK(client);
}

static void bufferevent_client_set_bev(struct bufferevent_client *client, struct bufferevent *bev)
{
	BEV_CLIENT_LOCK(client);
	client->bev = bev;
	BEV_CLIENT_UNLOCK(client);
}

static void send_rpc_message(struct bufferevent *bev, const std::string &message_str)
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


class RpcServerClosure : public google::protobuf::Closure {
public:
	typedef void(*FunctionType)(
		RpcMessage &message,
		struct bufferevent_client *client,
		google::protobuf::RpcController* controller,
		google::protobuf::Message *request,
		google::protobuf::Message *response);

	RpcServerClosure(FunctionType function, struct bufferevent_client *client, bool self_deleting = true)
		: function_(function), client_(client), self_deleting_(self_deleting)
		, controller(NULL), request(NULL), response(NULL) {
		if (client_)
			bufferevent_client_add_reference(client_);
	}
	~RpcServerClosure() {
		if (client_)
			bufferevent_client_free(client_);
	}

	void Run() override {
		bool needs_delete = self_deleting_;  // read in case callback deletes
		function_(message, client_, controller, request, response);
		if (needs_delete) delete this;
	}

	RpcMessage message;
	google::protobuf::RpcController* controller;
	google::protobuf::Message *request;
	google::protobuf::Message *response;

private:
	FunctionType function_;
	bool self_deleting_;
	struct bufferevent_client *client_;
};

RpcServer::RpcServer(unsigned short port)
	: m_base(NULL)
	, m_listener(NULL)
	, m_threadid(0)
{
	struct sockaddr_in sin = { 0 };
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);

	m_spServiceMap.insert(std::make_pair("EchoService", new EchoServiceImpl()));
	
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
	struct bufferevent_client *client;

	if (use_thread_pool) {
		//struct eveasy_thread_pool *pool = arg;
		//eveasy_thread_pool_assign(pool, fd, sa, socklen);
	}
	else {
		client = bufferevent_client_new(NULL, arg);
		if (client) {
			bev = create_bufferevent_socket(pServer->m_base, fd, client);
			if (bev)
				bufferevent_client_set_bev(client, bev);
			else
				bufferevent_client_free(client);
		} else {
			evutil_closesocket(fd);
		}
	}
}

void RpcServer::read_cb(struct bufferevent *bev, void *arg)
{
	struct bufferevent_client *client = (struct bufferevent_client *)arg;
	RpcServer *pServer = (RpcServer *)client->arg;
	struct evbuffer *input = bufferevent_get_input(bev);
	struct evbuffer *output = bufferevent_get_output(bev);
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
			//如果是多线程，此处可能会造成数据混乱
			BEV_CLIENT_LOCK(client);
			evbuffer_add(output, &packet, sizeof(RPC_PACKET));
			BEV_CLIENT_UNLOCK(client);
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

		if (pServer) {
			// 此处可优化，防止内存拷贝
			pServer->decode(client, std::string((const char *)body, packet.length));
		}

		// remove packet body data
		evbuffer_drain(input, packet.length);
	}
}

void RpcServer::event_cb(struct bufferevent *bev, short events, void *arg)
{
	struct bufferevent_client *client = (struct bufferevent_client *)arg;
	RpcServer *pServer = (RpcServer *)client->arg;

	if (events & BEV_EVENT_EOF) {
#ifndef NDEBUG
		fprintf(stderr, "Connection closed.\n");
#endif
	}
	else if (events & BEV_EVENT_ERROR) {
#ifndef NDEBUG
		fprintf(stderr, "Got an error on the connection\n"); /*XXX win32*/
#endif
	}
	else if (events & BEV_EVENT_TIMEOUT) {
#ifndef NDEBUG
		fprintf(stderr, "Connection timeout.\n");
#endif
	}
	else {
		return;
	}

	bufferevent_client_set_bev(client, NULL);
	bufferevent_client_free(client);

	/* None of the other events can happen here, since we haven't enabled
	 * timeouts */
	bufferevent_free(bev);
}

void RpcServer::done_cb(RpcMessage &message, struct bufferevent_client *client,
	google::protobuf::RpcController* controller, google::protobuf::Message *request, google::protobuf::Message *response)
{
	do 
	{
		if (controller && controller->IsCanceled())
			break;

		if (response)
			message.set_response(response->SerializeAsString());

		std::string message_str;
		if (!message.SerializeToString(&message_str)) {
#ifndef NDEBUG
			std::cout << "server.done_cb -> "
				<< ", type: " << message.type()
				<< ", id: " << message.id()
				<< ", service: " << message.service()
				<< ", method: " << message.method()
				<< ", error: " << message.error()
				<< std::endl;
#endif
			break;
		}

		// send to client
		BEV_CLIENT_LOCK(client);
		if (client->bev) 
			send_rpc_message(client->bev, message_str);
		BEV_CLIENT_UNLOCK(client);

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

void RpcServer::decode(struct bufferevent_client *client, const std::string &message_str)
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

		done = new RpcServerClosure(&RpcServer::done_cb, client);
		done->message.set_type(RPC_TYPE_RESPONSE);
		done->message.set_id(message.id());
		done->message.set_service(message.service());
		done->message.set_method(message.method());
		done->message.set_error(RPC_ERR_OK);

		// 可以对任务计数统计，当任务量达到一定数量是不进行处理
		// 此处增加任务计数 request_cnt++;   需加锁
		// 再done->Run()回调中减少任务计数 request_cnt--;
#if 0
		static uint64_t request_cnt = 1;
		if (request_cnt > 1000) {
			done->message.set_error(RPC_ERR_BUSY);
			break;
		}
#endif

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
