#include "pch.h"
#include "RpcControllerImpl.h"


RpcControllerImpl::RpcControllerImpl()
	: m_bIsCanceled(false)
{
}


RpcControllerImpl::~RpcControllerImpl()
{
}


void RpcControllerImpl::Reset()
{
	m_bIsCanceled = false;
	m_errorText.clear();
}

bool RpcControllerImpl::Failed() const
{
	return !m_errorText.empty();
}

std::string RpcControllerImpl::ErrorText() const
{
	return m_errorText;
}

void RpcControllerImpl::StartCancel()
{
	m_bIsCanceled = true;
}

void RpcControllerImpl::SetFailed(const std::string& reason)
{
	m_errorText = reason;
}

bool RpcControllerImpl::IsCanceled() const
{
	return m_bIsCanceled;
}

void RpcControllerImpl::NotifyOnCancel(::google::protobuf::Closure* callback)
{

}