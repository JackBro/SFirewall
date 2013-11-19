#include "comm.h"
#include <windows.h>
#include <stdio.h>
#include <process.h>
#include "../include/userioctrl.h" //do not change include order.

HANDLE g_hEvent;
PVOID g_ShareMem;
HANDLE g_kEvent;
HANDLE g_hFile;
int bExit = 1;
void LogToDB(PacketRecord *record);
int RuleToDB(RULE);

DWORD __stdcall workthread(PVOID param)
{
	while(bExit)
	{
		PacketRecord *record;
		WaitForSingleObject(g_kEvent,INFINITE);
		if (g_ShareMem)
		{
			//get the log
			char stat[8] = {0};
			record = (PacketRecord *)g_ShareMem;
			if (record->status == PacketDrop)
				strcpy_s(stat,sizeof(stat),"denied");
			else if (record->status == PacketPass)
				strcpy_s(stat,sizeof(stat),"pass");
			else 
				strcpy_s(stat,sizeof(stat),"error");
			if (record->etherType == IP_TYPE)
			{
				if (record->protocol == TCP_PROTOCOL)
					printf("TCP--->%s\n",stat);
				else if (record->protocol == UDP_PROTOCOL)
					printf("UDP--->%s\n",stat);
				else
					printf("IP--->%s\n",stat);

				printf("%d.%d.%d.%d:%d[%02x:%02x:%02x:%02x:%02x:%02x]-->%d.%d.%d.%d:%d[%02x:%02x:%02x:%02x:%02x:%02x]\n",
				record->srcIP[0],record->srcIP[1],record->srcIP[2],record->srcIP[3],record->srcPort,
				record->srcMac[0],record->srcMac[1],record->srcMac[2],record->srcMac[3],record->srcMac[4],record->srcMac[5],
				record->dstIP[0],record->dstIP[1],record->dstIP[2],record->dstIP[3],record->dstPort,
				record->dstMac[0],record->dstMac[1],record->dstMac[2],record->dstMac[3],record->dstMac[4],record->dstMac[5]);
			}
			else if (record->etherType == ARP_TYPE)
			{
				printf("ARP--->%s\n",stat);
				printf("%d - [%02x:%02x:%02x:%02x:%02x:%02x]-->%d - [%02x:%02x:%02x:%02x:%02x:%02x]\n",
					record->srcPort,
					record->srcMac[0],record->srcMac[1],record->srcMac[2],record->srcMac[3],record->srcMac[4],record->srcMac[5],
					record->dstPort,
					record->dstMac[0],record->dstMac[1],record->dstMac[2],record->dstMac[3],record->dstMac[4],record->dstMac[5]);
			}
			else
			{
				printf("Other--->%s\n",stat);
				printf("%d - [%02x:%02x:%02x:%02x:%02x:%02x]-->%d - [%02x:%02x:%02x:%02x:%02x:%02x]\n",
					record->srcPort,
					record->srcMac[0],record->srcMac[1],record->srcMac[2],record->srcMac[3],record->srcMac[4],record->srcMac[5],
					record->dstPort,
					record->dstMac[0],record->dstMac[1],record->dstMac[2],record->dstMac[3],record->dstMac[4],record->dstMac[5]);
			}
			
			LogToDB(record);
			SetEvent(g_hEvent);
		}
		
	}
	return 0;
}

int setup_comm(int *error)
{
	DWORD RetBytes;
	HANDLE m_hEvent,m_kEvent;
	DWORD addr = 0;
	g_hFile = CreateFile("\\\\.\\s7fw",GENERIC_READ|GENERIC_WRITE,0,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
	if (g_hFile == INVALID_HANDLE_VALUE)
	{
		if (GetLastError() == ERROR_FILE_NOT_FOUND)
			fprintf(stderr,"Installing driver...\n");
		else
		{
			fprintf(stderr,"Open Symbol Link failed:%d\n",GetLastError());
			*error = 1;
		}
		return false;
	}

	//Create event to be sent to kernel
	m_hEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
	m_kEvent = CreateEvent(NULL,FALSE,FALSE,NULL);
	if (!DeviceIoControl(g_hFile,IOCTL_SET_EVENT,&m_hEvent,sizeof(HANDLE),NULL,0,&RetBytes,NULL))
	{
		fprintf(stderr,"Send Event 1 to kernel failed:%d\n",GetLastError());
		goto FAIL_EXIT;
	}

	if (!DeviceIoControl(g_hFile,IOCTL_SET_EVENT_K,&m_kEvent,sizeof(HANDLE),NULL,0,&RetBytes,NULL))
	{
		fprintf(stderr,"Send Event to 2 kernel failed:%d\n",GetLastError());
		goto FAIL_EXIT;
	}

	// get shared memory from kernel
	if (!DeviceIoControl(g_hFile,IOCTL_GET_SHARE_ADDR,NULL,0,&addr,sizeof(addr),&RetBytes,NULL))
	{
		fprintf(stderr,"Get Shared Address failed:%d\n",GetLastError());
		goto FAIL_EXIT;
	}

	g_ShareMem = (PVOID)addr;
	if (!g_ShareMem)
	{
		fprintf(stderr,"get addr:%p,failed.\n",addr);
		goto FAIL_EXIT;
	}
	g_hEvent = m_hEvent;
	g_kEvent = m_kEvent;

	//Create thread to handle comm
	if (-1 == _beginthreadex(NULL,0,workthread,NULL,0,0))
	{
		fprintf(stderr,"Create work thread errorno:%d\n",errno);
		goto FAIL_EXIT;
	}

	return TRUE;

FAIL_EXIT:
	CloseHandle(g_hFile);
	CloseHandle(m_hEvent);
	CloseHandle(m_kEvent);
	*error = 1;
	return FALSE;
}

int DeliveryRule(RULE r)
{
	DWORD retBytes;
	PktFltRule rule;
	DWORD temp_ip;

	ZeroMemory(&rule,sizeof(PktFltRule));
	temp_ip = inet_addr(r.src_ip);
	if (temp_ip != 0xffffffff)
	{
		rule.srcIpAddr[3] = LOBYTE((temp_ip & 0xff000000) >> 24);
		rule.srcIpAddr[2] = LOBYTE((temp_ip & 0x00ff0000) >> 16);
		rule.srcIpAddr[1] = LOBYTE((temp_ip & 0x0000ff00) >> 8);
		rule.srcIpAddr[0] = LOBYTE((temp_ip & 0x000000ff) >> 0);
	}
	temp_ip = inet_addr(r.dst_ip);
	if (temp_ip != 0xffffffff)
	{
		rule.dstIpAddr[3] = LOBYTE((temp_ip & 0xff000000) >> 24);
		rule.dstIpAddr[2] = LOBYTE((temp_ip & 0x00ff0000) >> 16);
		rule.dstIpAddr[1] = LOBYTE((temp_ip & 0x0000ff00) >> 8);
		rule.dstIpAddr[0] = LOBYTE((temp_ip & 0x000000ff) >> 0);
	}
	if (!strcmp(r.type,"TCP"))
		rule.protocol = TCP_PROTOCOL;
	else if (!strcmp(r.type,"UDP"))
		rule.protocol = UDP_PROTOCOL;
	else if (!strcmp(r.type,"ICMP"))
		rule.protocol = ICMP_PROTOCOL;
	else if (!strcmp(r.type,"IP"))
		rule.etherType = IP_TYPE;
	else if (!strcmp(r.type,"ARP"))
		rule.etherType = IP_TYPE;
	else if (!strcmp(r.type,"RARP"))
		rule.etherType = IP_TYPE;

	if (!strcmp(r.op,"Pass"))
		rule.status = PacketPass;
	else if (!strcmp(r.op,"Denied"))
		rule.status = PacketDrop;

	rule.srcPort = atoi(r.src_port);
	rule.dstPort = atoi(r.dst_port);

	if (!DeviceIoControl(g_hFile,IOCTL_ADD_RULE,&rule,sizeof(rule),NULL,0,&retBytes,NULL))
	{
		fprintf(stderr,"Send rule to kernel failed:%d\n",GetLastError());
		return KERNEL_COMM_ERROR;
	}
	if (!RuleToDB(r))
		return SQL_ERROR;
	return SUCCESS;
}