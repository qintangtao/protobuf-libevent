#pragma once
#include "rpc_message.pb.h"
#include <google/protobuf/service.h>

struct event_base;
struct bufferevent;
struct evconnlistener;

class RpcServer
{
public:
	RpcServer(unsigned short port);
	~RpcServer();

private:
	static void *worker(void *arg);
	static void accept_socket_cb(struct evconnlistener *listener, evutil_socket_t fd,
		struct sockaddr *sa, int socklen, void *arg);
	static void read_cb(struct bufferevent *bev, void *arg); 
	static void event_cb(struct bufferevent *bev, short events, void *arg);
	static struct bufferevent *create_bufferevent_socket(struct event_base *base, evutil_socket_t fd, void *arg);
	static void done_cb(const std::string &addr, RpcMessage &message, RpcServer *server,
		google::protobuf::RpcController* controller, google::protobuf::Message *request, google::protobuf::Message *response);

	void send_no_lock(struct bufferevent *bev, const std::string &message_str);

	void decode(const std::string &addr, const std::string &message_str);

	void add_client_cache(const std::string &addr, struct bufferevent *bev);
	struct bufferevent *del_client_cache(const std::string &addr);
	struct bufferevent *lookup_client_cache_no_lock(const std::string &addr);


private:
	std::map<std::string, google::protobuf::Service *> m_spServiceMap;

	std::map<std::string, struct bufferevent *> m_client_cache;
	void									    *m_client_cache_lock;

	unsigned long		  m_threadid;
	struct event_base	  *m_base;
	struct evconnlistener *m_listener;
};

