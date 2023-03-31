#pragma once
#include "rpc_message.pb.h"
#include <google/protobuf/service.h>

struct event_base;
struct bufferevent;
struct evconnlistener;
struct bufferevent_client;

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
	static void done_cb(RpcMessage &message, struct bufferevent_client *client,
		google::protobuf::RpcController* controller, google::protobuf::Message *request, google::protobuf::Message *response);

	void decode(struct bufferevent_client *client, const std::string &message_str);

private:
	std::map<std::string, google::protobuf::Service *> m_spServiceMap;

	unsigned long		  m_threadid;
	struct event_base	  *m_base;
	struct evconnlistener *m_listener;
};

