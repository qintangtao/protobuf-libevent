#include "pch.h"
#include "RpcServer.h"
#include "EchoServiceImpl.h"

static int use_thread_pool = 0; 
static struct timeval tv_read = { 30, 0 };

RpcServer::RpcServer(unsigned short port)
	: m_base(NULL)
	, m_listener(NULL)
	, m_threadid(0)
{
	m_spServiceMap.insert(std::make_pair("EchoService", new EchoServiceImpl()));
	
	m_base = event_base_new();
	if (!m_base) {
		fprintf(stderr, "Couldn't create an event_base: exiting\n");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in sin = { 0 };
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);

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

void
RpcServer::accept_socket_cb(struct evconnlistener *listener, evutil_socket_t fd,
	struct sockaddr *sa, int socklen, void *arg)
{
	RpcServer *pServer = (RpcServer *)arg;

	if (use_thread_pool) {
		//struct eveasy_thread_pool *pool = arg;
		//eveasy_thread_pool_assign(pool, fd, sa, socklen);
	}
	else {
		create_bufferevent_socket(pServer->m_base, fd, arg);
	}
}


void RpcServer::read_cb(struct bufferevent *bev, void *arg)
{
	RpcServer *pServer = (RpcServer *)arg;
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

		if (pServer) {

			std::string message_in((const char *)body, packet.length);
			std::string message_out;

			pServer->decode(message_in, message_out);

			if (!message_out.empty()) {

				size = packet.length;

				packet.length = (unsigned int)message_out.size();

				evbuffer_add(output, &packet, sizeof(RPC_PACKET));
				evbuffer_add(output, message_out.c_str(), message_out.size());

				packet.length = (unsigned int)size;
			}
		}

		// remove packet body data
		evbuffer_drain(input, packet.length);
	}
}

void RpcServer::event_cb(struct bufferevent *bev, short events, void *arg)
{
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

	/* None of the other events can happen here, since we haven't enabled
	 * timeouts */
	bufferevent_free(bev);
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

void RpcServer::decode(const std::string &message_in, std::string &message_out)
{
	std::map<std::string, google::protobuf::Service *>::iterator iter;
	const google::protobuf::ServiceDescriptor *serviceDesc;
	const google::protobuf::MethodDescriptor *methodDesc;
	google::protobuf::Service *service;
	google::protobuf::Message *request = NULL;
	google::protobuf::Message *response = NULL;
	enum ErrorCode errCode = RPC_ERR_OK;

	RpcMessage message;
	if (!message.ParseFromString(message_in)) {
		std::cout << "Parse message failed!" << std::endl;
		return;
	}

#ifndef NDEBUG
	std::cout << "server -> "
		<< "size: " << message_in.size()
		<< ", type: " << message.type()
		<< ", id: " << message.id()
		<< ", service: " << message.service()
		<< ", method: " << message.method()
		<< ", error: " << message.error()
		<< ", request: " << message.request().size()
		<< std::endl;
#endif

	do
	{
		iter = m_spServiceMap.find(message.service());
		if (iter == m_spServiceMap.end()) {
			std::cout << "Do not Find, service: " << message.service() << std::endl;
			errCode = RPC_ERR_NO_SERVICE;
			break;
		}

		service = iter->second;

		serviceDesc = service->GetDescriptor();
		if (!serviceDesc) {
			std::cout << "Do not Find Descriptor, service: " << message.service() << std::endl;
			errCode = RPC_ERR_NO_SERVICE;
			break;
		}

		methodDesc = serviceDesc->FindMethodByName(message.method());
		if (!methodDesc) {
			std::cout << "Do not Find, method: " << message.method() << std::endl;
			errCode = RPC_ERR_NO_METHOD;
			break;
		}

		// 构造 request & response 对象
		request = service->GetRequestPrototype(methodDesc).New();
		response = service->GetResponsePrototype(methodDesc).New();

		if (!request->ParseFromString(message.request())) {
			std::cout << "Parse reqeust failed!" << std::endl;
			errCode = RPC_ERR_INVALID_REQUEST;
			break;
		}
	
		service->CallMethod(methodDesc, NULL, request, response, NULL);

	} while (0);

	message.set_type(RPC_TYPE_RESPONSE);
	message.set_error(errCode);
	message.set_request(std::string());
	if (response) message.set_response(response->SerializeAsString());
	message.SerializeToString(&message_out);

	if (request)
		delete request;
	if (response)
		delete response;
}