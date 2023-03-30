#include "pch.h"
#include "EchoServiceImpl.h"


EchoServiceImpl::EchoServiceImpl()
{
}


EchoServiceImpl::~EchoServiceImpl()
{
}

void EchoServiceImpl::Echo(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
	const ::EchoRequest* request,
	::EchoResponse* response,
	::google::protobuf::Closure* done)
{
	response->set_message(request->message() + ", welcome!");

	if (done)
		done->Run();
}

void EchoServiceImpl::Add(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
	const ::AddRequest* request,
	::AddResponse* response,
	::google::protobuf::Closure* done)
{
	int32_t a = request->a();
	int32_t b = request->b();

	response->set_result(a + b);

	if (done)
		done->Run();
}