#pragma once
#include "echo_service.pb.h"
#include <google/protobuf/port_def.inc>

class EchoServiceImpl :
	public EchoService
{
public:
	EchoServiceImpl();
	~EchoServiceImpl();

	virtual void Echo(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
		const ::EchoRequest* request,
		::EchoResponse* response,
		::google::protobuf::Closure* done);
	virtual void Add(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
		const ::AddRequest* request,
		::AddResponse* response,
		::google::protobuf::Closure* done);
};

