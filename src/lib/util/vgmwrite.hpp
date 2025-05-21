// license:BSD-3-Clause
// copyright-holders:Valley Bell
/***************************************************************************

    vgmwrite.hpp

    VGM writer/logger.

***************************************************************************/
#ifndef __VGMWRITE_HPP__
#define __VGMWRITE_HPP__

#pragma once

#include <cstdint>
#include "../../emu/emu.h"
#include <wchar.h>

class VGMLogger;

class VGMDeviceLog
{
	friend class VGMLogger;
private:
	struct VGM_PCMCACHE
	{
		uint32_t Start;
		uint32_t Next;
		uint32_t Pos;
		uint32_t CacheSize;
		uint8_t* CacheData;
	};

public:
	VGMDeviceLog();
	bool IsValid(void) const;
	void Write(uint8_t port, uint16_t r, uint8_t v);
	void WriteLargeData(uint8_t type, uint32_t blockSize, uint32_t startOfs, uint32_t dataLen, const void* data);
	void SetProperty(uint8_t attr, uint32_t data);
	void DumpSampleROM(uint8_t type, memory_region* region);
	void DumpSampleROM(uint8_t type, address_space& space);
private:
	void SetupPCMCache(uint32_t size);
	void FlushPCMCache(void);

	running_machine* _machine;
	VGMLogger* _vgmlog;
	uint16_t _fileID;

	uint8_t _chipType;
	//uint8_t _chipInst;	// chip instance
	uint8_t _hadWrite;
	VGM_PCMCACHE _pcmCache;
};

class VGMLogger
{
	friend class VGMDeviceLog;
public:
	typedef uint16_t gd3char_t;	// UTF-16 character, for VGM GD3 tag

	struct VGM_HEADER	// public, due to some static functions
	{
		uint32_t fccVGM;
		uint32_t lngEOFOffset;
		uint32_t lngVersion;
		uint32_t lngHzPSG;
		uint32_t lngHz2413;
		uint32_t lngGD3Offset;
		uint32_t lngTotalSamples;
		uint32_t lngLoopOffset;
		uint32_t lngLoopSamples;
		uint32_t lngRate;
		uint16_t shtPSG_Feedback;
		uint8_t bytPSG_SRWidth;
		uint8_t bytPSG_Flags;
		uint32_t lngHz2612;
		uint32_t lngHz2151;
		uint32_t lngDataOffset;
		uint32_t lngHzSPCM;
		uint32_t lngSPCMIntf;
		
		uint32_t lngHzRF5C68;
		uint32_t lngHz2203;
		uint32_t lngHz2608;
		uint32_t lngHz2610;
		uint32_t lngHz3812;
		uint32_t lngHz3526;
		uint32_t lngHz8950;
		uint32_t lngHz262;
		uint32_t lngHz278B;
		uint32_t lngHz271;
		uint32_t lngHz280B;
		uint32_t lngHzRF5C164;
		uint32_t lngHzPWM;
		uint32_t lngHzAY8910;
		uint8_t lngAYType;
		uint8_t lngAYFlags;
		uint8_t lngAYFlagsYM2203;
		uint8_t lngAYFlagsYM2608;
		uint8_t bytModifiers[0x04];
		
		uint32_t lngHzGBDMG;		// part of the LR35902 (GB Main CPU)
		uint32_t lngHzNESAPU;		// part of the N2A03 (NES Main CPU)
		uint32_t lngHzMultiPCM;
		uint32_t lngHzUPD7759;
		uint32_t lngHzOKIM6258;
		uint8_t bytOKI6258Flags;
		uint8_t bytK054539Flags;
		uint8_t bytC140Type;
		uint8_t bytReservedFlags;
		uint32_t lngHzOKIM6295;
		uint32_t lngHzK051649;
		uint32_t lngHzK054539;
		uint32_t lngHzHuC6280;
		uint32_t lngHzC140;
		uint32_t lngHzK053260;
		uint32_t lngHzPokey;
		uint32_t lngHzQSound;
		uint32_t lngHzSCSP;
		uint32_t lngExtraOfs;
		
		uint32_t lngHzWSwan;
		uint32_t lngHzVSU;
		uint32_t lngHzSAA1099;
		uint32_t lngHzES5503;
		uint32_t lngHzES5506;
		uint8_t bytES5503Chns;
		uint8_t bytES5506Chns;
		uint8_t bytC352ClkDiv;
		uint8_t bytESReserved;
		uint32_t lngHzX1_010;
		uint32_t lngHzC352;
		
		uint32_t lngHzGA20;
		uint32_t lngHzMikey;
		uint32_t lngHzK007232;
		uint32_t lngHzK005289;
		uint32_t lngHzOKIM5205;
		uint8_t bytReserved[0x0C];
	};	// -> 0x100 Bytes
	struct GD3_TAG
	{
		uint32_t fccGD3;
		uint32_t lngVersion;
		uint32_t lngTagLength;
		gd3char_t strTrackNameE[0x70];
		gd3char_t strTrackNameJ[0x10];	// Japanese Names are not used
		gd3char_t strGameNameE[0x70];
		gd3char_t strGameNameJ[0x10];
		gd3char_t strSystemNameE[0x30];
		gd3char_t strSystemNameJ[0x10];
		gd3char_t strAuthorNameE[0x30];
		gd3char_t strAuthorNameJ[0x10];
		gd3char_t strReleaseDate[0x10];
		gd3char_t strCreator[0x20];
		gd3char_t strNotes[0x50];
	};	// -> 0x200 Bytes

private:
	struct VGM_ROM_DATA
	{
		uint8_t type;
		uint8_t dstart_msb;
		uint32_t dataSize;
		const void* data;
	};
	struct VGM_INIT_CMD
	{
		uint8_t CmdLen;
		uint8_t Data[0x08];
	};
	struct VGM_INF
	{
		FILE* hFile;
		VGM_HEADER header;
		uint8_t WroteHeader;
		uint32_t HeaderBytes;
		uint32_t BytesWrt;
		uint32_t SmplsWrt;
		uint32_t EvtDelay;
		
		uint32_t DataCount;
		VGM_ROM_DATA DataBlk[0x20];	// TODO: make std:.vector
		//uint32_t CmdAlloc;
		uint32_t CmdCount;
		VGM_INIT_CMD Commands[0x100];	// TODO: make std:.vector
		
		uint8_t NesMemEmpty;
		uint8_t NesMem[0x4000];
	};
public:
	VGMLogger(running_machine &machine);
	void Start(void);
	void Stop(void);
	VGMDeviceLog* OpenDevice(uint8_t chipType, int clock);
	VGMDeviceLog* GetChip(uint8_t chipType, uint8_t instance);
	static VGMDeviceLog* GetDummyChip(void);
	void ChangeROMData(uint32_t oldsize, const void* olddata, uint32_t newsize, const void* newdata);
	
private:
	void Header_PostWrite(VGM_INF& vf);
	void Header_SizeCheck(VGM_INF& vf, uint32_t minVer, uint32_t minSize);
	void Header_Clear(VGM_INF& vf);
	void CloseFile(VGM_INF& vf);
	void WriteDelay(VGM_INF& vf);
	uint8_t NES_RAMCheck(VGM_INF& vf, uint32_t datasize, uint32_t* value1, uint32_t* value2, const uint8_t* data);
	
	running_machine* _machine;
	emu_timer* _timer;
	TIMER_CALLBACK_MEMBER(timerCallack);

	uint8_t _logging = 0x00;
	std::string _fNameBase;
	std::vector<VGM_INF> _files;
	std::vector<VGMDeviceLog> _chips;
	GD3_TAG _tag;
};

// VGM Chip Constants
enum ChipTypes : uint8_t
{
	// v1.00
	VGMC_SN76496	= 0x00,
	VGMC_YM2413		= 0x01,
	VGMC_YM2612		= 0x02,
	VGMC_YM2151		= 0x03,
	// v1.51
	VGMC_SEGAPCM	= 0x04,
	VGMC_RF5C68		= 0x05,
	VGMC_YM2203		= 0x06,
	VGMC_YM2608		= 0x07,
	VGMC_YM2610		= 0x08,
	VGMC_YM3812		= 0x09,
	VGMC_YM3526		= 0x0A,
	VGMC_Y8950		= 0x0B,
	VGMC_YMF262		= 0x0C,
	VGMC_YMF278B	= 0x0D,
	VGMC_YMF271		= 0x0E,
	VGMC_YMZ280B	= 0x0F,
	VGMC_T6W28		= 0x7F,	// note: emulated via 2xSN76496
	VGMC_RF5C164	= 0x10,
	VGMC_PWM		= 0x11,
	VGMC_AY8910		= 0x12,
	// v1.61
	VGMC_GBSOUND	= 0x13,
	VGMC_NESAPU		= 0x14,
	VGMC_MULTIPCM	= 0x15,
	VGMC_UPD7759	= 0x16,
	VGMC_OKIM6258	= 0x17,
	VGMC_OKIM6295	= 0x18,
	VGMC_K051649	= 0x19,
	VGMC_K054539	= 0x1A,
	VGMC_C6280		= 0x1B,
	VGMC_C140		= 0x1C,
	VGMC_K053260	= 0x1D,
	VGMC_POKEY		= 0x1E,
	VGMC_QSOUND		= 0x1F,
	// v1.71
	VGMC_SCSP		= 0x20,
	VGMC_WSWAN		= 0x21,
	VGMC_VSU		= 0x22,
	VGMC_SAA1099	= 0x23,
	VGMC_ES5503		= 0x24,
	VGMC_ES5506		= 0x25,
	VGMC_X1_010		= 0x26,
	VGMC_C352		= 0x27,
	VGMC_GA20		= 0x28,
	// v1.72
	VGMC_MIKEY		= 0x29,
	VGMC_K007232	= 0x2A,
	VGMC_K005289	= 0x2B,
	VGMC_OKIM5205	= 0x2C,

	//VGMC_OKIM6376	= 0xFF,
};

#endif // __VGMWRITE_HPP__
