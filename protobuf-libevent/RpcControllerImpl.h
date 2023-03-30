#pragma once
#include <google/protobuf/service.h>

class RpcControllerImpl :
	public ::google::protobuf::RpcController
{
public:
	RpcControllerImpl();
	~RpcControllerImpl();

	virtual void Reset();

	virtual bool Failed() const;

	virtual std::string ErrorText() const;

	virtual void StartCancel();

	virtual void SetFailed(const std::string& reason);

	virtual bool IsCanceled() const;

	virtual void NotifyOnCancel(::google::protobuf::Closure* callback);

private:
	bool m_bIsCanceled;
	std::string m_errorText;
};

