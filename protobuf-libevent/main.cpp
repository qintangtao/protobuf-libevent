// protobuf_demo.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "pch.h"
#include <iostream>
#include "echo_service.pb.h"
#include "RpcChannelImpl.h"
#include "RpcControllerImpl.h"
#include "RpcServer.h"

void DoneCallback(google::protobuf::RpcController* controller, google::protobuf::Message *reqeust, google::protobuf::Message *response)
{
	do 
	{
		EchoRequest *echoReqeust = dynamic_cast<EchoRequest *>(reqeust);
		EchoResponse *echoResponse = dynamic_cast<EchoResponse *>(response);
		if (echoReqeust && echoResponse)
		{
			std::cout << "echoReqeust: ";

			std::cout << echoReqeust->message() << std::endl;

			std::cout << "echoResponse: ";

			if (controller->IsCanceled())
			{
				std::cout << "Canceled" << std::endl;
			}
			else if (controller->Failed())
			{
				std::cout << controller->ErrorText() << std::endl;
			}
			else
			{
				std::cout << echoResponse->message() << std::endl;
			}
			break;
		}

		AddRequest *addReqeust = dynamic_cast<AddRequest *>(reqeust);
		AddResponse *addResponse = dynamic_cast<AddResponse *>(response);
		if (addReqeust && addResponse)
		{
			std::cout << "AddRequest: ";

			std::cout << addReqeust->a() << " + " << addReqeust->b() << std::endl;

			std::cout << "addResponse: ";

			if (controller->IsCanceled())
			{
				std::cout << "Canceled" << std::endl;
			}
			else if (controller->Failed())
			{
				std::cout << controller->ErrorText() << std::endl;
			}
			else
			{
				std::cout << addResponse->result() << std::endl;
			}
			break;
		}

	} while (0);
	
	if (controller)
		delete controller;
	if (reqeust)
		delete reqeust;
	if (response)
		delete response;
}

int main()
{
#ifdef _WIN32
	WSADATA wsa_data;
	WSAStartup(MAKEWORD(2, 2), &wsa_data);
#else
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		fprintf(stderr, "Couldn't signal SIGPIP SIG_IGN\n");
		goto err;
	}
#endif
#ifdef _WIN32
#ifdef EVTHREAD_USE_WINDOWS_THREADS_IMPLEMENTED
	evthread_use_windows_threads();
#endif
#else
#ifdef EVTHREAD_USE_PTHREADS_IMPLEMENTED
	evthread_use_pthreads();
#endif
#endif

	RpcServer *server = new RpcServer(9206);
	RpcChannelImpl *channel = new RpcChannelImpl("127.0.0.1:9206");
	

	USleep(3 * 1000 * 1000);


	EchoService_Stub service(channel);
	{
		EchoRequest *request = new EchoRequest();
		EchoResponse *response = new EchoResponse();
		google::protobuf::RpcController* controller = new RpcControllerImpl();
		google::protobuf::Closure* done = google::protobuf::NewCallback(&DoneCallback, controller, (google::protobuf::Message *)request, (google::protobuf::Message *)response);

		request->set_message("hello!");

		service.Echo(controller, request, response, done);
	}

	USleep(3 * 1000 * 1000);

#if 1
	for (int i = 0; i < 20; i++)
	{
		AddRequest *request = new AddRequest();
		AddResponse *response = new AddResponse();
		google::protobuf::RpcController* controller = new RpcControllerImpl();
		google::protobuf::Closure* done = google::protobuf::NewCallback(&DoneCallback, controller, (google::protobuf::Message *)request, (google::protobuf::Message *)response);

		request->set_a(22 + i*i);
		request->set_b(44 + i * i * 2);

		service.Add(controller, request, response, done);
	}
#endif


	getchar();

	delete server;
	delete channel;

#ifdef _WIN32
	WSACleanup();
#endif
}
