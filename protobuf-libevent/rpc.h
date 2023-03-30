
#ifndef __BR_RPC_H__
#define __BR_RPC_H__

#pragma pack(push)
#pragma pack(1)
typedef struct _RPC_PACKET {
	char magic[6];
	unsigned char major_version;
	unsigned char minor_version;
	unsigned int length;
} RPC_PACKET;
#pragma pack(pop)

#define RPC_MAGIC "PROTOB"
#define RPC_MAJOR_VERSION 0x01
#define RPC_MINOR_VERSION 0x01


#endif //__BR_RPC_H__