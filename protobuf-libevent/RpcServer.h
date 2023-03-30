#pragma once
#include "rpc_message.pb.h"
#include <google/protobuf/service.h>

struct event_base;
struct evconnlistener;

class RpcServer
{
public:
	RpcServer(unsigned short port);
	~RpcServer();

	void decode(const std::string &message_in, std::string &message_out);

	inline struct event_base *base() const
	{ return m_base; }

private:
	static void *worker(void *arg);

private:
	std::map<std::string, google::protobuf::Service *> m_spServiceMap;

	unsigned long		  m_threadid;
	struct event_base	  *m_base;
	struct evconnlistener *m_listener;
};

