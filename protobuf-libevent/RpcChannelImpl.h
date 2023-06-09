#pragma once
#include <google/protobuf/service.h>
#include <stdint.h>
#include <queue>

namespace google {
	namespace protobuf {

		template <typename Arg1, typename Arg2, typename Arg3>
		class FunctionClosure3 : public Closure {
		public:
			typedef void(*FunctionType)(Arg1 arg1, Arg2 arg2, Arg3 arg3);

			FunctionClosure3(FunctionType function, bool self_deleting,
				Arg1 arg1, Arg2 arg2, Arg3 arg3)
				: function_(function), self_deleting_(self_deleting),
				arg1_(arg1), arg2_(arg2), arg3_(arg3) {}
			~FunctionClosure3() {}

			void Run() override {
				bool needs_delete = self_deleting_;  // read in case callback deletes
				function_(arg1_, arg2_, arg3_);
				if (needs_delete) delete this;
			}

		private:
			FunctionType function_;
			bool self_deleting_;
			Arg1 arg1_;
			Arg2 arg2_;
			Arg3 arg3_;
		};

		// See Closure.
		template <typename Arg1, typename Arg2, typename Arg3>
		inline Closure* NewCallback(void(*function)(Arg1, Arg2, Arg3),
			Arg1 arg1, Arg2 arg2, Arg3 arg3) {
			return new FunctionClosure3<Arg1, Arg2, Arg3>(
				function, true, arg1, arg2, arg3);
		}
	}
}

struct event_base;
struct bufferevent;
struct event;

struct callback_cache {
	google::protobuf::RpcController* controller;
	google::protobuf::Message* response;
	google::protobuf::Closure* done;
	struct timeval tv;
};

class RpcChannelImpl :
	public google::protobuf::RpcChannel
{
public:
	RpcChannelImpl(const char *ip_as_string);
	~RpcChannelImpl();

	virtual void CallMethod(const google::protobuf::MethodDescriptor* method,
		google::protobuf::RpcController* controller, const google::protobuf::Message* request,
		google::protobuf::Message* response, google::protobuf::Closure* done);

private:
	static void *worker(void *arg); 
	static void connect_time_cb(evutil_socket_t fd, short event, void *arg);
	static void heartbeat_time_cb(evutil_socket_t fd, short event, void *arg);
	static void request_time_cb(evutil_socket_t fd, short event, void *arg);
	static void read_cb(struct bufferevent *bev, void *arg);
	static void event_cb(struct bufferevent *bev, short events, void *arg);
	
	struct bufferevent *create_bufferevent_socket();

	void send_no_lock(const std::string &message_str);
	
	void send_heartbeat();

	void send_request_cache();

	void decode(const std::string &message_str);

	void add_request_cache(uint64_t id, const std::string &message_str);
	bool del_request_cache(uint64_t id, std::string &message_str);
	bool del_request_cache_first(std::string &message_str);
	bool has_request_cache(uint64_t id);

	void guess_callback_cache();
	void add_callback_cache(uint64_t id, const struct callback_cache &cache);
	bool del_callback_cache(uint64_t id, struct callback_cache &cache);

private:
	uint64_t			m_id;
	RPC_PACKET			m_packet;
	RPC_PACKET			m_packet_heartbeat;
	unsigned long		m_threadid;
	struct event_base	*m_base;
	struct bufferevent  *m_bev;
	void				*m_bev_lock;
	struct event		*m_connect_timer;
	struct event		*m_heartbeat_timer;
	struct event		*m_request_timer;
	struct sockaddr_storage m_connect_to_addr;
	int					m_connect_to_addrlen;

	std::map<uint64_t, std::string>				m_request_cache;
	void										*m_request_cache_lock;

	std::map<uint64_t, struct callback_cache>	m_callback_cache;
	void										*m_callback_cache_lock;
	
};

