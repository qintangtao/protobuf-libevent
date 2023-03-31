#include "pch.h"
#include "EchoServiceImpl.h"


EchoServiceImpl::EchoServiceImpl()
{
}


EchoServiceImpl::~EchoServiceImpl()
{
}

static int sec = 5;

void *echo_worker(void *arg)
{
	::google::protobuf::Closure* done = (::google::protobuf::Closure*)arg;

	sec += 3;
	
	USleep(sec * 1000 * 1000);

	done->Run();

	return NULL;
}


void EchoServiceImpl::Echo(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
	const ::EchoRequest* request,
	::EchoResponse* response,
	::google::protobuf::Closure* done)
{
	response->set_message(request->message() + ", welcome!");

	sys_os_create_thread(&echo_worker, done);
}

void EchoServiceImpl::Add(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
	const ::AddRequest* request,
	::AddResponse* response,
	::google::protobuf::Closure* done)
{
	int32_t a = request->a();
	int32_t b = request->b();

	response->set_result(a + b);

	done->Run();
}