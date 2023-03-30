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

	void decode(const std::string &message_in, std::string &message_out);

private:
	std::map<std::string, google::protobuf::Service *> m_spServiceMap;

	unsigned long		  m_threadid;
	struct event_base	  *m_base;
	struct evconnlistener *m_listener;
};

