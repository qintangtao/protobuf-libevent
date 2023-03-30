// pch.cpp: 与预编译标头对应的源文件；编译成功所必需的

#include "pch.h"
#include <windows.h>
#include <stdio.h>

// 一般情况下，忽略此文件，但如果你使用的是预编译标头，请保留它。

unsigned long
sys_os_create_thread(void *(*func)(void *), void *arg)
{
#ifdef _WIN32
	HANDLE hThread;
	DWORD threadid = 0;
	hThread =
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, &threadid);
	if (hThread == INVALID_HANDLE_VALUE) {
		fprintf(stderr, "Can't create thread: %d\n", GetLastError());
		goto error;
	}
	CloseHandle(hThread);

	return (unsigned long)threadid;
#else
	int ret;
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	if ((ret = pthread_create(&thread, &attr, func, arg)) != 0) {
		fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
		goto error;
	}
	pthread_detach(thread);

	return (unsigned long)thread;
#endif

error:
	return 0;
}