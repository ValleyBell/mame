// license:BSD-3-Clause
// copyright-holders:Valley Bell
/*
    vgmwrite.cpp

    VGM output module.
*/
#pragma GCC diagnostic ignored "-Wunused-function"

#include "../../emu/emu.h"
#include "../../emu/emuopts.h"
#include "../../emu/main.h" // for emulator_info::
#include "../../emu/softlist.h"
#include "osdcomm.h"
#include <wchar.h>
#include "vgmwrite.hpp"

#include <string>
#include <vector>

#define DYNAMIC_HEADER_SIZE

//#define print_info	osd_printf_info
//#define print_warn	osd_printf_warning
//#define print_err	osd_printf_error
#define print_info	_machine->logerror
#define print_warn	_machine->logerror
#define print_err	_machine->logerror


// Function Prototypes
static size_t str2utf16(VGMLogger::gd3char_t* dststr, const char* srcstr, size_t max);
static size_t wcs2utf16(VGMLogger::gd3char_t* dststr, const wchar_t* srcstr, size_t max);
static size_t utf16len(const VGMLogger::gd3char_t* str);


static VGMDeviceLog dummyDevice;
static bool dumpEmptyBlocks = true;

static size_t str2utf16(VGMLogger::gd3char_t* dststr, const char* srcstr, size_t max)
{
	size_t srclen;
	size_t wlen;
	wchar_t* wstr;
	
	srclen = strlen(srcstr);
	wstr = (wchar_t*)calloc(srclen + 1, sizeof(wchar_t));
	
	mbstowcs(wstr, srcstr, srclen + 1);
	wlen = wcs2utf16(dststr, wstr, max);
	
	free(wstr);
	return wlen;
}

static size_t wcs2utf16(VGMLogger::gd3char_t* dststr, const wchar_t* srcstr, size_t max)
{
	size_t wlen;
	
	max --;
	wlen = wcslen(srcstr);
	if (wlen > max)
		wlen = max;
	
	if (sizeof(VGMLogger::gd3char_t) == sizeof(wchar_t))
	{
		wcsncpy((wchar_t*)dststr, srcstr, wlen + 1);
		return wlen;
	}
	else
	{
		size_t idxSrc;
		size_t idxDst;
		
		// return number of copied bytes
		idxDst = 0;
		for (idxSrc = 0; idxSrc < wlen && idxDst < max; idxSrc ++)
		{
			if (srcstr[idxSrc] >= 0x10000)
			{
#define HI_SURROGATE_START	0xD800
#define LO_SURROGATE_START	0xDC00
				// surrogate conversion from http://unicode.org/faq/utf_bom.html
				VGMLogger::gd3char_t X, W;
				
				X = (VGMLogger::gd3char_t)srcstr[idxSrc];
				W = ((srcstr[idxSrc] >> 16) & 0x1F) - 1;
				VGMLogger::gd3char_t HiSurrogate = HI_SURROGATE_START | (W << 6) | (X >> 10);
				VGMLogger::gd3char_t LoSurrogate = LO_SURROGATE_START | (X & 0x3FF);
				
				dststr[idxDst] = HiSurrogate;	idxDst ++;
				dststr[idxDst] = LoSurrogate;	idxDst ++;
			}
			else
			{
				// just truncate/copy
				dststr[idxDst] = (VGMLogger::gd3char_t)srcstr[idxSrc];	idxDst ++;
			}
		}
		dststr[idxDst] = (VGMLogger::gd3char_t)L'\0';
		return idxDst;
	}
}

static size_t utf16len(const VGMLogger::gd3char_t* str)
{
	const VGMLogger::gd3char_t* end_ptr = str;
	
	while(*end_ptr != 0)
		end_ptr ++;
	return end_ptr - str;
}

static void DumpEmptyData(FILE* hFile, size_t dataLen, uint8_t fill)
{
	constexpr size_t BLOCK_SIZE = 4096;
	uint8_t buffer[BLOCK_SIZE];
	size_t remBytes = dataLen;
	memset(buffer, fill, BLOCK_SIZE);
	while(remBytes > 0)
	{
		size_t wrtBytes = (BLOCK_SIZE <= remBytes) ? BLOCK_SIZE : remBytes;
		size_t ret = fwrite(buffer, 0x01, wrtBytes, hFile);
		if (ret < wrtBytes)
			break;
		remBytes -= ret;
	}
	return;
}

static bool PatchROMBlock(uint8_t romType, uint32_t& dataSize, const void*& data, uint32_t startOfs,
	std::vector<uint8_t>& patchBuffer, const VGMLogger::VGM_HEADER& vh)
{
	const uint8_t* dbytes = static_cast<const uint8_t*>(data);
	
	switch(romType)
	{
	case 0x8D:	// Type: C140 ROM Data
		if (vh.bytC140Type == 0x02)
			return false;	// no patching for C219 ROM
		
		dataSize /= 2;
		startOfs /= 2;
		if (data == nullptr || startOfs >= dataSize)
			return true;
		
		patchBuffer.resize(dataSize);
		for (uint32_t curPos = 0x00; curPos < dataSize; curPos ++)
			patchBuffer[curPos] = dbytes[curPos * 2 + 1];	// 16-bit word access (upper-byte only) -> 8-bit ROM
		
		data = patchBuffer.data();
		return true;
	}
	return false;
}

VGMDeviceLog::VGMDeviceLog()
	: _machine(nullptr)
	, _vgmlog(nullptr)
{
}

bool VGMDeviceLog::IsValid(void) const
{
	return (_vgmlog != nullptr);
}

VGMLogger::VGMLogger(running_machine &machine)
{
	_machine = &machine;
	dummyDevice._machine = _machine;
	return;
}

void VGMLogger::Start(void)
{
	_logging = (uint8_t)_machine->options().vgm_write();
	print_info("VGM logging mode: %02X\n", _logging);
	
	// start the timer
	// (done here because it makes save states with vgmwrite off compatible with
	//  saves that have it on)
	_timer = _machine->scheduler().timer_alloc(timer_expired_delegate(FUNC(VGMLogger::timerCallack), this));
	_timer->adjust(attotime::from_hz(44100), 0, attotime::from_hz(44100));
	
	if (! _logging)
		return;
	
	_chips.reserve(0x20);	// reserve space, so that we don't reallocate/move pointers later

	// Get the Game Information and write the GD3 Tag
	const game_driver* gamedrv = &_machine->system();
	const device_image_interface* devimg = image_interface_enumerator(_machine->root_device()).first();
	const software_info* swinfo = (devimg != nullptr) ? devimg->software_entry() : nullptr;
	
	if (gamedrv)
		_fNameBase = gamedrv->type.shortname();
	else
		_fNameBase = "vgmlog";
	_fNameBase += "_";
	
	_tag.fccGD3 = 0x20336447;	// 'Gd3 '
	_tag.lngVersion = 0x100;
	wcs2utf16(_tag.strTrackNameE, L"", 0x70);
	wcs2utf16(_tag.strTrackNameJ, L"", 0x10);
	wcs2utf16(_tag.strGameNameE, L"", 0x70);
	wcs2utf16(_tag.strGameNameJ, L"", 0x10);
	wcs2utf16(_tag.strSystemNameE, L"", 0x30);
	wcs2utf16(_tag.strSystemNameJ, L"", 0x10);
	wcs2utf16(_tag.strAuthorNameE, L"", 0x30);
	wcs2utf16(_tag.strAuthorNameJ, L"", 0x10);
	wcs2utf16(_tag.strReleaseDate, L"", 0x10);
	wcs2utf16(_tag.strCreator, L"", 0x20);
	wcs2utf16(_tag.strNotes, L"", 0x50);
	
	if (/*gamedrv->flags & MACHINE_TYPE_ARCADE*/ swinfo == nullptr) // TODO flag removed with commit 0562745
	{
		wcs2utf16(_tag.strSystemNameE, L"Arcade Machine", 0x30);
		str2utf16(_tag.strGameNameE, gamedrv->type.fullname(), 0x70);
		str2utf16(_tag.strAuthorNameE, gamedrv->manufacturer, 0x30);
		str2utf16(_tag.strReleaseDate, gamedrv->year, 0x10);
	}
	else
	{
		str2utf16(_tag.strSystemNameE, gamedrv->type.fullname(), 0x30);
		if (swinfo != nullptr)
		{
			if (swinfo->longname().length() > 0)
				str2utf16(_tag.strGameNameE, swinfo->longname().c_str(), 0x70);
			else
				str2utf16(_tag.strGameNameE, devimg->basename_noext(), 0x70);
			str2utf16(_tag.strAuthorNameE, swinfo->publisher().c_str(), 0x30);
			str2utf16(_tag.strReleaseDate, swinfo->year().c_str(), 0x10);
		}
	}
	
	wchar_t vgmNotes[0x50];
	swprintf(vgmNotes, sizeof(vgmNotes) / sizeof(wchar_t), L"Generated by %s %s",
			emulator_info::get_appname(), emulator_info::get_build_version());
	wcs2utf16(_tag.strNotes, vgmNotes, 0x50);
	
	_tag.lngTagLength = utf16len(_tag.strTrackNameE) + 1 +
				utf16len(_tag.strTrackNameJ) + 1 +
				utf16len(_tag.strGameNameE) + 1 +
				utf16len(_tag.strGameNameJ) + 1 +
				utf16len(_tag.strSystemNameE) + 1 +
				utf16len(_tag.strSystemNameJ) + 1 +
				utf16len(_tag.strAuthorNameE) + 1 +
				utf16len(_tag.strAuthorNameJ) + 1 +
				utf16len(_tag.strReleaseDate) + 1 +
				utf16len(_tag.strCreator) + 1 +
				utf16len(_tag.strNotes) + 1;
	_tag.lngTagLength *= sizeof(gd3char_t);	// String Length -> Byte Length
	
	print_info("VGM logging started ...\n");
	
	return;
}

void VGMLogger::Stop(void)
{
	//delete _timer;	_timer = nullptr;

	if (! _logging)
		return;
	
	// For all "dual" chips, make sure the first chip's instance is marked as "used".
	// Else it will incorrectly remove its clock value.
	for (size_t curChp = 0; curChp < _chips.size(); curChp ++)
	{
		VGMDeviceLog& vcBase = _chips[curChp];
		if (vcBase._chipType & 0x80)
		{
			for (size_t curC2 = 0; curC2 < curChp; curC2 ++)
			{
				VGMDeviceLog& vc2 = _chips[curC2];
				if (vc2._chipType == (vcBase._chipType & 0x7F))
					vc2._hadWrite = 0x01;
			}
		}
	}
	
	size_t chip_unused = 0x00;
	for (VGMDeviceLog& vc : _chips)
	{
		if (! vc._hadWrite)
		{
			VGM_HEADER& vh = _files[vc._fileID].header;
			chip_unused ++;
			// clock_mask - remove either the dual-chip bit or the entire clock
			uint32_t clock_mask = (vc._chipType & 0x80) ? ~0x40000000 : 0x00000000;
			
			switch(vc._chipType & 0x7F)
			{
			case VGMC_T6W28:
				clock_mask = 0x00000000;
				[[fallthrough]];	// fall through, as it is handled as 2xSN76496
			case VGMC_SN76496:
				vh.lngHzPSG &= clock_mask;
				if (! clock_mask)
				{
					vh.shtPSG_Feedback = 0x0000;
					vh.bytPSG_SRWidth = 0x00;
					vh.bytPSG_Flags = 0x00;
				}
				break;
			case VGMC_YM2413:
				vh.lngHz2413 &= clock_mask;
				break;
			case VGMC_YM2612:
				vh.lngHz2612 &= clock_mask;
				break;
			case VGMC_YM2151:
				vh.lngHz2151 &= clock_mask;
				break;
			case VGMC_SEGAPCM:
				vh.lngHzSPCM &= clock_mask;
				if (! clock_mask)
					vh.lngSPCMIntf = 0x00;
				break;
			case VGMC_RF5C68:
				vh.lngHzRF5C68 &= clock_mask;
				break;
			case VGMC_YM2203:
				vh.lngHz2203 &= clock_mask;
				if (! clock_mask)
					vh.lngAYFlagsYM2203 = 0x00;
				break;
			case VGMC_YM2608:
				vh.lngHz2608 &= clock_mask;
				if (! clock_mask)
					vh.lngAYFlagsYM2608 = 0x00;
				break;
			case VGMC_YM2610:
				vh.lngHz2610 &= clock_mask;
				break;
			case VGMC_YM3812:
				vh.lngHz3812 &= clock_mask;
				break;
			case VGMC_YM3526:
				vh.lngHz3526 &= clock_mask;
				break;
			case VGMC_Y8950:
				vh.lngHz8950 &= clock_mask;
				break;
			case VGMC_YMF262:
				vh.lngHz262 &= clock_mask;
				break;
			case VGMC_YMF278B:
				vh.lngHz278B &= clock_mask;
				break;
			case VGMC_YMF271:
				vh.lngHz271 &= clock_mask;
				break;
			case VGMC_YMZ280B:
				vh.lngHz280B &= clock_mask;
				break;
			case VGMC_RF5C164:
				vh.lngHzRF5C164 &= clock_mask;
				break;
			case VGMC_PWM:
				vh.lngHzPWM &= clock_mask;
				break;
			case VGMC_AY8910:
				vh.lngHzAY8910 &= clock_mask;
				if (! clock_mask)
				{
					vh.lngAYFlags = 0x00;
					vh.lngAYType = 0x00;
				}
				break;
			case VGMC_GBSOUND:
				vh.lngHzGBDMG &= clock_mask;
				break;
			case VGMC_NESAPU:
				vh.lngHzNESAPU &= clock_mask;
				break;
			case VGMC_MULTIPCM:
				vh.lngHzMultiPCM &= clock_mask;
				break;
			case VGMC_UPD7759:
				vh.lngHzUPD7759 &= clock_mask;
				break;
			case VGMC_OKIM6258:
				vh.lngHzOKIM6258 &= clock_mask;
				if (! clock_mask)
					vh.bytOKI6258Flags = 0x00;
				break;
			case VGMC_OKIM6295:
				vh.lngHzOKIM6295 &= clock_mask;
				break;
			case VGMC_K051649:
				vh.lngHzK051649 &= clock_mask;
				break;
			case VGMC_K054539:
				vh.lngHzK054539 &= clock_mask;
				if (! clock_mask)
					vh.bytK054539Flags = 0x00;
				break;
			case VGMC_C6280:
				vh.lngHzHuC6280 &= clock_mask;
				break;
			case VGMC_C140:
				vh.lngHzC140 &= clock_mask;
				if (! clock_mask)
					vh.bytC140Type = 0x00;
				break;
			case VGMC_K053260:
				vh.lngHzK053260 &= clock_mask;
				break;
			case VGMC_POKEY:
				vh.lngHzPokey &= clock_mask;
				break;
			case VGMC_QSOUND:
				vh.lngHzQSound &= clock_mask;
				break;
			case VGMC_SCSP:
				vh.lngHzSCSP &= clock_mask;
				break;
			case VGMC_WSWAN:
				vh.lngHzWSwan &= clock_mask;
				break;
			case VGMC_VSU:
				vh.lngHzVSU &= clock_mask;
				break;
			case VGMC_SAA1099:
				vh.lngHzSAA1099 &= clock_mask;
				break;
			case VGMC_ES5503:
				vh.lngHzES5503 &= clock_mask;
				if (! clock_mask)
					vh.bytES5503Chns = 0x00;
				break;
			case VGMC_ES5506:
				vh.lngHzES5506 &= clock_mask;
				if (! clock_mask)
					vh.bytES5506Chns = 0x00;
				break;
			case VGMC_X1_010:
				vh.lngHzX1_010 &= clock_mask;
				break;
			case VGMC_C352:
				vh.lngHzC352 &= clock_mask;
				if (! clock_mask)
					vh.bytC352ClkDiv = 0x00;
				break;
			case VGMC_GA20:
				vh.lngHzGA20 &= clock_mask;
				break;
			case VGMC_MIKEY:
				vh.lngHzMikey &= clock_mask;
				break;
			case VGMC_K007232:
				vh.lngHzK007232 &= clock_mask;
				break;
			case VGMC_K005289:
				vh.lngHzK005289 &= clock_mask;
				break;
			case VGMC_OKIM5205:
				vh.lngHzOKIM5205 &= clock_mask;
				break;
		//	case VGMC_OKIM6376:
		//		vh.lngHzOKIM6376 &= clock_mask;
		//		break;
			}
		}
		if (vc._pcmCache.CacheData != nullptr)
		{
			vc._pcmCache.CacheSize = 0x00;
			free(vc._pcmCache.CacheData);
			vc._pcmCache.CacheData = nullptr;
		}
	}
	if (chip_unused)
		print_info("Header Data of %hu unused Chips removed.\n", chip_unused);
	_chips.clear();
	
	for (VGM_INF& vf : _files)
	{
		if (vf.hFile != nullptr)
			CloseFile(vf);
	}
	_files.clear();
	print_info("VGM stopped.\n");
	
	return;
}

TIMER_CALLBACK_MEMBER(VGMLogger::timerCallack)
{
	if (! _logging)
		return;
	
	for (VGM_INF& vf : _files)
	{
		if (vf.hFile != nullptr)
			vf.EvtDelay ++;
	}
	
	return;
}

void VGMLogger::Header_PostWrite(VGM_INF& vf)
{
	if (vf.WroteHeader)
		return;
	
	VGM_HEADER& vh = vf.header;
	uint32_t curcmd;
	std::vector<uint8_t> romPatchBuf;	// temporary buffer for patched ROM data
	
	print_info("Writing VGM Header ...\n");
	fseek(vf.hFile, 0x00, SEEK_SET);
	vf.BytesWrt = 0x00;
	
	fwrite(&vh, 0x01, vf.HeaderBytes, vf.hFile);
	vf.BytesWrt += vf.HeaderBytes;
	
	for (curcmd = 0x00; curcmd < vf.DataCount; curcmd ++)
	{
		VGM_ROM_DATA& vrd = vf.DataBlk[curcmd];
		uint32_t dataLen = vrd.dataSize & 0x7FFFFFFF;
		const void* data = vrd.data;
		uint32_t blkSize = vrd.dataSize & 0x80000000;
		uint32_t startOfs = 0x00;
		
		PatchROMBlock(vrd.type, dataLen, data, startOfs, romPatchBuf, vh);
		if (data != nullptr || dumpEmptyBlocks)
			blkSize += dataLen;
		
		fputc(0x67, vf.hFile);
		fputc(0x66, vf.hFile);
		fputc(vrd.type, vf.hFile);
		switch(vrd.type & 0xE0)
		{
		case 0x80:	// ROM Image
		case 0xA0:
			blkSize += 0x08;
			fwrite(&blkSize, 0x04, 0x01, vf.hFile);		// Data Block Size
			fwrite(&dataLen, 0x04, 0x01, vf.hFile);		// ROM Size
			startOfs |= (vrd.dstart_msb << 24);
			fwrite(&startOfs, 0x04, 0x01, vf.hFile);	// Data Base Address
			break;
		case 0xC0:	// RAM Writes (16-bit address)
			blkSize += 0x02;
			fwrite(&blkSize, 0x04, 0x01, vf.hFile);		// Data Block Size
			fwrite(&startOfs, 0x02, 0x01, vf.hFile);	// Data Address
			break;
		case 0xE0:	// RAM Writes (32-bit address)
			blkSize += 0x04;
			fwrite(&blkSize, 0x04, 0x01, vf.hFile);		// Data Block Size
			fwrite(&startOfs, 0x04, 0x01, vf.hFile);	// Data Address
			break;
		default:
			fwrite(&blkSize, 0x04, 0x01, vf.hFile);		// Data Block Size
			break;
		}
		if (data == nullptr && dumpEmptyBlocks)
			DumpEmptyData(vf.hFile, dataLen, 0x00);
		else if (data != nullptr)
		{
			size_t wrtByt = fwrite(data, 0x01, dataLen, vf.hFile);
			if (wrtByt != dataLen)
				print_info("Warning VGM Header_PostWrite: wrote only 0x%X bytes instead of 0x%X!\n", (uint32_t)wrtByt, dataLen);
		}
		vf.BytesWrt += 0x07 + (blkSize & 0x7FFFFFFF);
	}
	for (curcmd = 0x00; curcmd < vf.CmdCount; curcmd ++)
	{
		VGM_INIT_CMD& vicmd = vf.Commands[curcmd];
		fwrite(vicmd.Data, 0x01, vicmd.CmdLen, vf.hFile);
		vf.BytesWrt += vicmd.CmdLen;
	}
	vf.WroteHeader = 0x01;
	
	return;
}

void VGMLogger::Header_SizeCheck(VGM_INF& vf, uint32_t minVer, uint32_t minSize)
{
	if (vf.hFile == nullptr)
		return;
	
	VGM_HEADER& vh = vf.header;
	if (vh.lngVersion < minVer)
		vh.lngVersion = minVer;
	if (vf.HeaderBytes < minSize)
		vf.HeaderBytes = minSize;
	
	return;
}

void VGMLogger::Header_Clear(VGM_INF& vf)
{
	if (vf.hFile == nullptr)
		return;
	
	VGM_HEADER& vh = vf.header;
	memset(&vh, 0x00, sizeof(VGM_HEADER));
	vh.fccVGM = 0x206D6756;	// 'Vgm '
	vh.lngEOFOffset = 0x00000000;
	vh.lngVersion = 0x151;	 // "base" version
	//vh.lngGD3Offset = 0x00000000;
	//vh.lngTotalSamples = 0;
	//vh.lngLoopOffset = 0x00000000;
	//vh.lngLoopSamples = 0;
#ifdef DYNAMIC_HEADER_SIZE
	vf.HeaderBytes = 0x38;
	vf.WroteHeader = 0x00;
#else
	vf.HeaderBytes = sizeof(VGM_HEADER);
	vf.WroteHeader = 0x01;
#endif
	vh.lngDataOffset = vf.HeaderBytes - 0x34;
	
	//fseek(vf.hFile, 0x00, SEEK_SET);
	//fwrite(header, 0x01, sizeof(VGM_HEADER), vf.hFile);
	//vf.BytesWrt += sizeof(VGM_HEADER);
	vf.BytesWrt = 0x00;
	
	return;
}

VGMDeviceLog* VGMLogger::OpenDevice(uint8_t chipType, int clock)
{
	print_info("vgm_open - Chip Type %02X, Clock %u\n", chipType, clock);
	if (! _logging || chipType == 0xFF)
		return &dummyDevice;
	
	if (_logging != 0xDD)
	{
		// prevent it from logging chips with known audible errors in VGM logs
		if (chipType == VGMC_ES5506)
			return &dummyDevice;	// logging needs to be reworked for this one
		else if (chipType == VGMC_SCSP)
			return &dummyDevice;	// timing is horribly off (see Fighting Vipers)
	}
	
	VGM_INF* vfPtr = nullptr;
	for (VGM_INF& vf : _files)
	{
		if (vf.hFile == nullptr)
			continue;
		
		uint32_t chip_val = 0;
		uint8_t use_two = 0x01;
		VGM_HEADER& vh = vf.header;
		switch(chipType)
		{
		case VGMC_SN76496:
			chip_val = vh.lngHzPSG;
			break;
		case VGMC_YM2413:
			chip_val = vh.lngHz2413;
			break;
		case VGMC_YM2612:
			chip_val = vh.lngHz2612;
			break;
		case VGMC_YM2151:
			chip_val = vh.lngHz2151;
			break;
		case VGMC_SEGAPCM:
			chip_val = vh.lngHzSPCM;
			break;
		case VGMC_RF5C68:
			chip_val = vh.lngHzRF5C68;
			use_two = 0x00;
			break;
		case VGMC_YM2203:
			chip_val = vh.lngHz2203;
			break;
		case VGMC_YM2608:
			chip_val = vh.lngHz2608;
			break;
		case VGMC_YM2610:
			chip_val = vh.lngHz2610;
			break;
		case VGMC_YM3812:
			chip_val = vh.lngHz3812;
			break;
		case VGMC_YM3526:
			chip_val = vh.lngHz3526;
			break;
		case VGMC_Y8950:
			chip_val = vh.lngHz8950;
			break;
		case VGMC_YMF262:
			chip_val = vh.lngHz262;
			break;
		case VGMC_YMF278B:
			chip_val = vh.lngHz278B;
			break;
		case VGMC_YMF271:
			chip_val = vh.lngHz271;
			break;
		case VGMC_YMZ280B:
			chip_val = vh.lngHz280B;
			break;
		case VGMC_T6W28:
			chip_val = vh.lngHzPSG;
			use_two = 0x00;
			break;
		case VGMC_RF5C164:
			chip_val = vh.lngHzRF5C164;
			use_two = 0x00;
			break;
		case VGMC_PWM:
			chip_val = vh.lngHzPWM;
			use_two = 0x00;
			break;
		case VGMC_AY8910:
			chip_val = vh.lngHzAY8910;
			break;
		case VGMC_GBSOUND:
			chip_val = vh.lngHzGBDMG;
			break;
		case VGMC_NESAPU:
			chip_val = vh.lngHzNESAPU;
			break;
		case VGMC_MULTIPCM:
			chip_val = vh.lngHzMultiPCM;
			break;
		case VGMC_UPD7759:
			chip_val = vh.lngHzUPD7759;
			break;
		case VGMC_OKIM6258:
			chip_val = vh.lngHzOKIM6258;
			break;
		case VGMC_OKIM6295:
			chip_val = vh.lngHzOKIM6295;
			break;
		case VGMC_K051649:
			chip_val = vh.lngHzK051649;
			break;
		case VGMC_K054539:
			chip_val = vh.lngHzK054539;
			break;
		case VGMC_C6280:
			chip_val = vh.lngHzHuC6280;
			break;
		case VGMC_C140:
			chip_val = vh.lngHzC140;
			break;
		case VGMC_K053260:
			chip_val = vh.lngHzK053260;
			break;
		case VGMC_POKEY:
			chip_val = vh.lngHzPokey;
			break;
		case VGMC_QSOUND:
			chip_val = vh.lngHzQSound;
			use_two = 0x00;
			break;
		case VGMC_SCSP:
			chip_val = vh.lngHzSCSP;
			//use_two = 0x00;
			break;
		case VGMC_WSWAN:
			chip_val = vh.lngHzWSwan;
			break;
		case VGMC_VSU:
			chip_val = vh.lngHzVSU;
			break;
		case VGMC_SAA1099:
			chip_val = vh.lngHzSAA1099;
			break;
		case VGMC_ES5503:
			chip_val = vh.lngHzES5503;
			break;
		case VGMC_ES5506:
			chip_val = vh.lngHzES5506;
			break;
		case VGMC_X1_010:
			chip_val = vh.lngHzX1_010;
			break;
		case VGMC_C352:
			chip_val = vh.lngHzC352;
			use_two = 0x00;
			break;
		case VGMC_GA20:
			chip_val = vh.lngHzGA20;
			break;
		case VGMC_MIKEY:
			chip_val = vh.lngHzMikey;
			break;
		case VGMC_K007232:
			chip_val = vh.lngHzK007232;
			break;
		case VGMC_K005289:
			chip_val = vh.lngHzK005289;
			break;
		case VGMC_OKIM5205:
			chip_val = vh.lngHzOKIM5205;
			break;
	//	case VGMC_OKIM6376:
	//		chip_val = vh.lngHzOKIM6376;
	//		use_two = 0x00;
	//		break;
		default:
			return &dummyDevice;	// unknown chip - don't log
		}
		if (! chip_val)
		{
			vfPtr = &vf;
			break;
		}
		else if (use_two)
		{
			// _logging == 1 -> log up to 2 instances of the same chip into one VGM
			// _logging == 2 -> log multiple instances into separate files
			if (! (chip_val & 0x40000000) && (_logging & 0x01))
			{
				if (clock != (chip_val & 0x3FFFFFF))
					print_warn("VGM Log: Warning - 2-chip mode, but chip clocks different!\n");
				vfPtr = &vf;
				clock = 0x40000000 | chip_val;
				chipType |= 0x80;
				break;
			}
		}
	}	// end for(vf)
	
	if (vfPtr == nullptr)	// no file found?
	{
		// try to create a new one
		std::string vgmName(_fNameBase.length() + 0x10, '\0');
		snprintf(&vgmName[0], vgmName.size(), "%s%zX.vgm", _fNameBase.c_str(), _files.size());
		
		print_info("Opening %s ...\t", vgmName.c_str());
		FILE* hFile = fopen(vgmName.c_str(), "wb");
		if (hFile != nullptr)
		{
			print_info("OK\n");
			_files.push_back(VGM_INF());
			vfPtr = &_files.back();
			vfPtr->hFile = hFile;
			vfPtr->BytesWrt = 0;
			vfPtr->SmplsWrt = 0;
			vfPtr->EvtDelay = 0;
			Header_Clear(*vfPtr);
		}
		else
		{
			print_info("Failed to create the file!\n");
		}
	}
	if (vfPtr == nullptr)
		return &dummyDevice;
	
	_chips.push_back(VGMDeviceLog());
	VGMDeviceLog& vc = _chips.back();
	vc._machine = _machine;
	vc._vgmlog = this;
	vc._fileID = vfPtr - &_files[0];
	vc._chipType = chipType;
	vc._hadWrite = 0x00;
	vc._pcmCache.CacheSize = 0x00;
	vc._pcmCache.CacheData = nullptr;
	//printf("Chip Init: chip %p, vgmlog %p, fileID %u, chipID 0x%02X\n", &vc, vc._vgmlog, vc._fileID, vc._chipType);
	
	VGM_HEADER& vh = vfPtr->header;
	switch(chipType & 0x7F)
	{
	case VGMC_SN76496:
		vh.lngHzPSG = clock;
		break;
	case VGMC_YM2413:
		vh.lngHz2413 = clock;
		break;
	case VGMC_YM2612:
		vh.lngHz2612 = clock;
		break;
	case VGMC_YM2151:
		vh.lngHz2151 = clock;
		break;
	case VGMC_SEGAPCM:
		vh.lngHzSPCM = clock;
		break;
	case VGMC_RF5C68:
		vh.lngHzRF5C68 = clock;
		vc.SetupPCMCache(0x400);
		break;
	case VGMC_YM2203:
		vh.lngHz2203 = clock;
		break;
	case VGMC_YM2608:
		vh.lngHz2608 = clock;
		break;
	case VGMC_YM2610:
		vh.lngHz2610 = clock;
		break;
	case VGMC_YM3812:
		vh.lngHz3812 = clock;
		break;
	case VGMC_YM3526:
		vh.lngHz3526 = clock;
		break;
	case VGMC_Y8950:
		vh.lngHz8950 = clock;
		break;
	case VGMC_YMF262:
		vh.lngHz262 = clock;
		break;
	case VGMC_YMF278B:
		vh.lngHz278B = clock;
		break;
	case VGMC_YMF271:
		vh.lngHz271 = clock;
		break;
	case VGMC_YMZ280B:
		vh.lngHz280B = clock;
		break;
	case VGMC_T6W28:
		vh.lngHzPSG = clock | 0xC0000000;	// Cheat to use 2 SN76489 chips
		break;
	case VGMC_RF5C164:
		vh.lngHzRF5C164 = clock;
		vc.SetupPCMCache(0x400);
		break;
	case VGMC_PWM:
		vh.lngHzPWM = clock;
		break;
	case VGMC_AY8910:
		vh.lngHzAY8910 = clock;
		break;
	case VGMC_GBSOUND:
		vh.lngHzGBDMG = clock;
		break;
	case VGMC_NESAPU:
		vh.lngHzNESAPU = clock;
		break;
	case VGMC_MULTIPCM:
		vh.lngHzMultiPCM = clock;
		break;
	case VGMC_UPD7759:
		vh.lngHzUPD7759 = clock;
		break;
	case VGMC_OKIM6258:
		vh.lngHzOKIM6258 = clock;
		break;
	case VGMC_OKIM6295:
		vh.lngHzOKIM6295 = clock;
		break;
	case VGMC_K051649:
		vh.lngHzK051649 = clock;
		break;
	case VGMC_K054539:
		vh.lngHzK054539 = clock;
		break;
	case VGMC_C6280:
		vh.lngHzHuC6280 = clock;
		break;
	case VGMC_C140:
		vh.lngHzC140 = clock;
		break;
	case VGMC_K053260:
		vh.lngHzK053260 = clock;
		break;
	case VGMC_POKEY:
		vh.lngHzPokey = clock;
		break;
	case VGMC_QSOUND:
		vh.lngHzQSound = clock;
		break;
	case VGMC_SCSP:
		vh.lngHzSCSP = clock;
		vc.SetupPCMCache(0x4000);
		break;
	case VGMC_WSWAN:
		vh.lngHzWSwan = clock;
		break;
	case VGMC_VSU:
		vh.lngHzVSU = clock;
		break;
	case VGMC_SAA1099:
		vh.lngHzSAA1099 = clock;
		break;
	case VGMC_ES5503:
		vh.lngHzES5503 = clock;
		vc.SetupPCMCache(0x1000);
		break;
	case VGMC_ES5506:
		vh.lngHzES5506 = clock;
		break;
	case VGMC_X1_010:
		vh.lngHzX1_010 = clock;
		break;
	case VGMC_C352:
		vh.lngHzC352 = clock;
		break;
	case VGMC_GA20:
		vh.lngHzGA20 = clock;
		break;
	case VGMC_MIKEY:
		vh.lngHzMikey = clock;
		break;
	case VGMC_K007232:
		vh.lngHzK007232 = clock;
		break;
	case VGMC_K005289:
		vh.lngHzK005289 = clock;
		break;
	case VGMC_OKIM5205:
		vh.lngHzOKIM5205 = clock;
		break;
//	case VGMC_OKIM6376:
//		vh.lngHzOKIM6376 = clock;
//		break;
	}
	
	switch(chipType & 0x7F)
	{
	case VGMC_SN76496:
	case VGMC_YM2413:
	case VGMC_YM2612:
	case VGMC_YM2151:
	case VGMC_SEGAPCM:
	case VGMC_T6W28:
		Header_SizeCheck(*vfPtr, 0x151, 0x40);
		break;
	case VGMC_RF5C68:
	case VGMC_YM2203:
	case VGMC_YM2608:
	case VGMC_YM2610:
	case VGMC_YM3812:
	case VGMC_YM3526:
	case VGMC_Y8950:
	case VGMC_YMF262:
	case VGMC_YMF278B:
	case VGMC_YMF271:
	case VGMC_YMZ280B:
	case VGMC_RF5C164:
	case VGMC_PWM:
	case VGMC_AY8910:
		Header_SizeCheck(*vfPtr, 0x151, 0x80);
		break;
	case VGMC_GBSOUND:
	case VGMC_NESAPU:
	case VGMC_MULTIPCM:
	case VGMC_UPD7759:
	case VGMC_OKIM6258:
	case VGMC_OKIM6295:
	case VGMC_K051649:
	case VGMC_K054539:
	case VGMC_C6280:
	case VGMC_C140:
	case VGMC_K053260:
	case VGMC_POKEY:
	case VGMC_QSOUND:
//	case VGMC_OKIM6376:
		Header_SizeCheck(*vfPtr, 0x161, 0xC0);
		break;
	case VGMC_SCSP:
		Header_SizeCheck(*vfPtr, 0x171, 0xC0);
		break;
	case VGMC_WSWAN:
	case VGMC_VSU:
	case VGMC_SAA1099:
	case VGMC_ES5503:
	case VGMC_ES5506:
	case VGMC_X1_010:
	case VGMC_C352:
		Header_SizeCheck(*vfPtr, 0x171, 0xE0);
		break;
	case VGMC_GA20:
		Header_SizeCheck(*vfPtr, 0x171, 0x100);
		break;
	case VGMC_MIKEY:
	case VGMC_K007232:
	case VGMC_K005289:
	case VGMC_OKIM5205:
		Header_SizeCheck(*vfPtr, 0x172, 0x100);
		break;
	}
	
	return &vc;
}

void VGMDeviceLog::SetupPCMCache(uint32_t size)
{
	_pcmCache.CacheSize = size;
	if (_pcmCache.CacheData != nullptr)
		free(_pcmCache.CacheData);
	_pcmCache.CacheData = (uint8_t*)malloc(size);
	_pcmCache.Start = 0xFFFFFFFF;
	_pcmCache.Pos = 0x00;
	_pcmCache.Next = 0x00;
	
	return;
}

void VGMDeviceLog::SetProperty(uint8_t attr, uint32_t data)
{
	if (_machine != nullptr)
		print_info("Property Set to Chip 0x%02X, Attr 0x%02X, Data 0x%02X\n", _chipType, attr, data);
	if (_vgmlog == nullptr || ! _vgmlog->_logging)
		return;
	
	uint8_t bitcnt;
	VGMLogger::VGM_INF& vf = _vgmlog->_files[_fileID];
	VGMLogger::VGM_HEADER& vh = vf.header;

	switch(_chipType & 0x7F)	// write the header data
	{
	case VGMC_SN76496:
	case VGMC_T6W28:
		switch(attr)
		{
		case 0x00:	// Reserved
			break;
		case 0x01:	// Shift Register Width (Feedback Mask)
			bitcnt = 0x00;	// Convert the BitMask to BitCount
			while(data)
			{
				data >>= 1;
				bitcnt ++;
			}
			vh.bytPSG_SRWidth = bitcnt;
			break;
		case 0x02:	// Feedback Pattern (White Noise Tap #1)
			vh.shtPSG_Feedback = (uint16_t)data;
			break;
		case 0x03:	// Feedback Pattern (White Noise Tap #2)
			// must be called after #1
			vh.shtPSG_Feedback |= (uint16_t)data;
			break;
		case 0x04:	// Negate Channels Flag
			vh.bytPSG_Flags &= ~(0x01 << 1);
			vh.bytPSG_Flags |= (data & 0x01) << 1;
			break;
		case 0x05:	// Stereo Flag (On/Off)
			// 0 is Stereo and 1 is mono
			vh.bytPSG_Flags &= ~(0x01 << 2);
			vh.bytPSG_Flags |= (~data & 0x01) << 2;
			break;
		case 0x06:	// Clock Divider (On/Off)
			vh.bytPSG_Flags &= ~(0x01 << 3);
			bitcnt = (data == 1) ? 0x01 : 0x00;
			vh.bytPSG_Flags |= (bitcnt & 0x01) << 3;
			break;
		case 0x07:	// Freq 0 is Maximum Frequency
			vh.bytPSG_Flags &= ~(0x01 << 0);
			vh.bytPSG_Flags |= (data & 0x01) << 0;
			break;
		case 0x08:	// NCR mode
			vh.bytPSG_Flags &= ~(0x10 << 0);
			vh.bytPSG_Flags |= (data & 0x10) << 0;
			break;
		}
		break;
	case VGMC_YM2413:
		break;
	case VGMC_YM2612:
		switch(attr)
		{
		case 0x00:	// Chip Type (set YM3438 mode)
			vh.lngHz2612 = (vh.lngHz2612 & 0x7FFFFFFF) | (data << 31);
			break;
		}
		break;
	case VGMC_YM2151:
		break;
	case VGMC_SEGAPCM:
		switch(attr)
		{
		case 0x00:	// Reserved
			break;
		case 0x01:	// Sega PCM Interface
			vh.lngSPCMIntf = data;
			break;
		}
		break;
	case VGMC_RF5C68:
		break;
	case VGMC_YM2203:
		switch(attr)
		{
		case 0x00:	// Reserved
			break;
		case 0x01:	// Flags
			vh.lngAYFlagsYM2203 = data & 0xFF;
			break;
		}
		break;
	case VGMC_YM2608:
		switch(attr)
		{
		case 0x00:	// Reserved
			break;
		case 0x01:	// Flags
			vh.lngAYFlagsYM2608 = data & 0xFF;
			break;
		}
		break;
	case VGMC_YM2610:
		switch(attr)
		{
		case 0x00:	// Chip Type (set YM2610B mode)
			vh.lngHz2610 = (vh.lngHz2610 & 0x7FFFFFFF) | (data << 31);
			break;
		}
		break;
	case VGMC_YM3812:
		break;
	case VGMC_YM3526:
		break;
	case VGMC_Y8950:
		break;
	case VGMC_YMF262:
		switch(attr)
		{
		case 0x00:	// is Part of OPL4
			if (data)
			{
				_chipType = VGMC_YMF278B;
				vh.lngHz262 = 0x00;
			}
			break;
		}
		break;
	case VGMC_YMF278B:
		break;
	case VGMC_YMF271:
		break;
	case VGMC_YMZ280B:
		break;
	case VGMC_RF5C164:
		break;
	case VGMC_PWM:
		break;
	case VGMC_AY8910:
		switch(attr)
		{
		case 0x00:	// Device Type
			vh.lngAYType = data & 0xFF;
			break;
		case 0x01:	// Flags
			vh.lngAYFlags = data & 0xFF;
			break;
		case 0x10:	// Resistor Loads
		case 0x11:
		case 0x12:
			print_info("AY8910: Resistor Load %hu = %u\n", attr & 0x0F, data);
			break;
		}
		break;
	case VGMC_GBSOUND:
		switch(attr)
		{
		case 0x00:	// GameBoy Color mode
			vh.lngHzGBDMG = (vh.lngHzGBDMG & 0x7FFFFFFF) | (data << 31);
			break;
		}
		break;
	case VGMC_NESAPU:
		switch(attr)
		{
		case 0x00:	// FDS Enable
			vh.lngHzNESAPU = (vh.lngHzNESAPU & 0x7FFFFFFF) | (data << 31);
			break;
		}
		break;
	case VGMC_MULTIPCM:
		break;
	case VGMC_UPD7759:
		switch(attr)
		{
		case 0x00:	// Chip Type (set master/slave mode)
			vh.lngHzUPD7759 = (vh.lngHzUPD7759 & 0x7FFFFFFF) | (data << 31);
			break;
		case 0x01:	// uPD7759/56 mode
			// not stored into VGMs yet
			break;
		}
		break;
	case VGMC_OKIM6258:
		switch(attr)
		{
		case 0x00:	// Reserved
			break;
		case 0x01:	// Clock Divider
			vh.bytOKI6258Flags &= ~(0x03 << 0);
			vh.bytOKI6258Flags |= (data & 0x03) << 0;
			break;
		case 0x02:	// ADPCM Type
			vh.bytOKI6258Flags &= ~(0x01 << 2);
			vh.bytOKI6258Flags |= (data & 0x01) << 2;
			break;
		case 0x03:	// 12-Bit Output
			vh.bytOKI6258Flags &= ~(0x01 << 3);
			vh.bytOKI6258Flags |= (data & 0x01) << 3;
			break;
		}
		break;
	case VGMC_OKIM6295:
		switch(attr)
		{
		case 0x00:	// Chip Type (pin 7 state)
			vh.lngHzOKIM6295 = (vh.lngHzOKIM6295 & 0x7FFFFFFF) | (data << 31);
			break;
		}
		break;
	case VGMC_K051649:
		switch(attr)
		{
		case 0x00:	// Chip Type (K051649/SCC or K052539/SCC+)
			vh.lngHzK051649 = (vh.lngHzK051649 & 0x7FFFFFFF) | (data << 31);
			break;
		}
		break;
	case VGMC_K054539:
		switch(attr)
		{
		case 0x01:	// Control Flags
			vh.bytK054539Flags = data;
			break;
		}
		break;
	case VGMC_C6280:
		break;
	case VGMC_C140:
		switch(attr)
		{
		case 0x01:	// Banking Type
			vh.bytC140Type = data;
			break;
		}
		break;
	case VGMC_K053260:
		break;
	case VGMC_POKEY:
		break;
	case VGMC_QSOUND:
		break;
	case VGMC_SCSP:
		break;
	case VGMC_WSWAN:
		break;
	case VGMC_VSU:
		break;
	case VGMC_SAA1099:
		break;
	case VGMC_ES5503:
		switch(attr)
		{
		case 0x00:	// Reserved
			break;
		case 0x01:	// Channels
			vh.bytES5503Chns = data & 0xFF;
			break;
		}
		break;
	case VGMC_ES5506:
		switch(attr)
		{
		case 0x00:	// Chip Type (5505/5506)
			vh.lngHzES5506 = (vh.lngHzES5506 & 0x7FFFFFFF) | (data << 31);
			break;
		case 0x01:	// Channels
			vh.bytES5506Chns = data & 0xFF;
			break;
		}
		break;
	case VGMC_X1_010:
		break;
	case VGMC_C352:
		switch(attr)
		{
		case 0x00:	// Reserved
			break;
		case 0x01:	// Clock Divider
			vh.bytC352ClkDiv = (data / 4) & 0xFF;
			break;
		}
		break;
	case VGMC_GA20:
		break;
	case VGMC_MIKEY:
		break;
	case VGMC_K007232:
		break;
	case VGMC_K005289:
		break;
	case VGMC_OKIM5205:
		switch(attr)
		{
		case 0x00:	// Chip Type (0 = MSM5205, 1 = MSM6585)
			vh.lngHzOKIM5205 = (vh.lngHzK051649 & 0x7FFFFFFF) | (data << 31);
			break;
		#if 0
		case 0x01:	// Clock Divider
			vh.bytOKI5205Flags &= ~(0x03 << 0);
			vh.bytOKI5205Flags |= (data & 0x03) << 0;
			break;
		case 0x02:	// ADPCM Type
			vh.bytOKI5205Flags &= ~(0x01 << 2);
			vh.bytOKI5205Flags |= (data & 0x01) << 2;
			break;
		#endif
		}
		break;
//	case VGMC_OKIM6376:
//		break;
	}
	
	return;
}

void VGMLogger::CloseFile(VGM_INF& vf)
{
	VGM_HEADER& vh = vf.header;
	if (! vf.WroteHeader)
	{
		fclose(vf.hFile);
		vf.hFile = nullptr;
		return;
	}
	
	WriteDelay(vf);
	fputc(0x66, vf.hFile);	// Write EOF Command
	vf.BytesWrt ++;
	
	// GD3 Tag
	vh.lngGD3Offset = vf.BytesWrt - 0x00000014;
	fwrite(&_tag.fccGD3, 0x04, 0x01, vf.hFile);
	fwrite(&_tag.lngVersion, 0x04, 0x01, vf.hFile);
	fwrite(&_tag.lngTagLength, 0x04, 0x01, vf.hFile);
	fwrite(_tag.strTrackNameE, sizeof(gd3char_t), utf16len(_tag.strTrackNameE) + 1, vf.hFile);
	fwrite(_tag.strTrackNameJ, sizeof(gd3char_t), utf16len(_tag.strTrackNameJ) + 1, vf.hFile);
	fwrite(_tag.strGameNameE, sizeof(gd3char_t), utf16len(_tag.strGameNameE) + 1, vf.hFile);
	fwrite(_tag.strGameNameJ, sizeof(gd3char_t), utf16len(_tag.strGameNameJ) + 1, vf.hFile);
	fwrite(_tag.strSystemNameE, sizeof(gd3char_t), utf16len(_tag.strSystemNameE) + 1, vf.hFile);
	fwrite(_tag.strSystemNameJ, sizeof(gd3char_t), utf16len(_tag.strSystemNameJ) + 1, vf.hFile);
	fwrite(_tag.strAuthorNameE, sizeof(gd3char_t), utf16len(_tag.strAuthorNameE) + 1, vf.hFile);
	fwrite(_tag.strAuthorNameJ, sizeof(gd3char_t), utf16len(_tag.strAuthorNameJ) + 1, vf.hFile);
	fwrite(_tag.strReleaseDate, sizeof(gd3char_t), utf16len(_tag.strReleaseDate) + 1, vf.hFile);
	fwrite(_tag.strCreator, sizeof(gd3char_t), utf16len(_tag.strCreator) + 1, vf.hFile);
	fwrite(_tag.strNotes, sizeof(gd3char_t), utf16len(_tag.strNotes) + 1, vf.hFile);
	vf.BytesWrt += 0x0C + _tag.lngTagLength;
	
	// Rewrite vh
	vh.lngTotalSamples = vf.SmplsWrt;
	vh.lngEOFOffset = vf.BytesWrt - 0x00000004;
	vh.lngDataOffset = vf.HeaderBytes - 0x34;
	
	fseek(vf.hFile, 0x00, SEEK_SET);
	fwrite(&vh, 0x01, vf.HeaderBytes, vf.hFile);
	
	fclose(vf.hFile);
	vf.hFile = nullptr;
	
	print_info("VGM %02hX closed. %u Bytes, %u Samples written.\n", &vf - &_files[0], vf.BytesWrt, vf.SmplsWrt);
	
	return;
}

void VGMLogger::WriteDelay(VGM_INF& vf)
{
	if (vf.EvtDelay)
	{
		if (! vf.WroteHeader)
			Header_PostWrite(vf);	// write post-header data
		
		for (VGMDeviceLog& vc : _chips)
		{
			if (vc._chipType != 0xFF)
				vc.FlushPCMCache();
		}
	}
	
	while(vf.EvtDelay)
	{
		uint16_t delaywrite;
		
		if (vf.EvtDelay > 0x0000FFFF)
			delaywrite = 0xFFFF;
		else
			delaywrite = (uint16_t)vf.EvtDelay;
		
		if (delaywrite <= 0x0010)
		{
			fputc(0x70 + (delaywrite - 1), vf.hFile);
			vf.BytesWrt += 0x01;
		}
		else
		{
			fputc(0x61, vf.hFile);
			fwrite(&delaywrite, 0x02, 0x01, vf.hFile);
			vf.BytesWrt += 0x03;
		}
		vf.SmplsWrt += delaywrite;
		
		vf.EvtDelay -= delaywrite;
	}
	
	return;
}

void VGMDeviceLog::Write(uint8_t port, uint16_t r, uint8_t v)
{
	//print_info("Write to Chip 0x%02X, Port 0x%02X, Reg 0x%02X, Data 0x%02X\n", _chipType, port, r, v);
	if (_vgmlog == nullptr || ! _vgmlog->_logging)
		return;
	
	VGMLogger::VGM_INF& vf = _vgmlog->_files[_fileID];
	if (vf.hFile == nullptr)
		return;
	
	if (! _hadWrite)
		_hadWrite = 0x01;
	
	VGMLogger::VGM_INIT_CMD wrtCmd;
	int8_t cm;	// "Cheat Mode" to support 2 instances of 1 chip within 1 file
	uint32_t mem_addr;

	wrtCmd.CmdLen = 0x00;
	cm = (_chipType & 0x80) ? 0x50 : 0x00;
	switch(_chipType & 0x7F)	// Write the data
	{
	case VGMC_T6W28:
		cm = ~port & 0x01;
		port = 0x00;
		[[fallthrough]];
	case VGMC_SN76496:
		switch(port)
		{
		case 0x00:
			cm = cm ? -0x20 : 0x00;
			wrtCmd.Data[0x00] = 0x50 + cm;
			wrtCmd.Data[0x01] = r;
			wrtCmd.CmdLen = 0x02;
			break;
		case 0x01:
			cm = cm ? -0x10 : 0x00;
			wrtCmd.Data[0x00] = 0x4F + cm;
			wrtCmd.Data[0x01] = r;
			wrtCmd.CmdLen = 0x02;
			break;
		}
		break;
	case VGMC_YM2413:
		wrtCmd.Data[0x00] = 0x51 + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_YM2612:
		wrtCmd.Data[0x00] = 0x52 + (port & 0x01) + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_YM2151:
		wrtCmd.Data[0x00] = 0x54 + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_SEGAPCM:
		r |= (_chipType & 0x80) << 8;
		wrtCmd.Data[0x00] = 0xC0;				// Write Memory
		wrtCmd.Data[0x01] = (r >> 0) & 0xFF;	// offset low
		wrtCmd.Data[0x02] = (r >> 8) & 0xFF;	// offset high
		wrtCmd.Data[0x03] = v;					// data
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_RF5C68:
		switch(port)
		{
		case 0x00:
			wrtCmd.Data[0x00] = 0xB0;				// Write Register
			wrtCmd.Data[0x01] = r;					// Register
			wrtCmd.Data[0x02] = v;					// Value
			wrtCmd.CmdLen = 0x03;
			break;
		case 0x01:
			wrtCmd.Data[0x00] = 0xC1;				// Write Memory
			wrtCmd.Data[0x01] = (r >> 0) & 0xFF;	// offset low
			wrtCmd.Data[0x02] = (r >> 8) & 0xFF;	// offset high
			wrtCmd.Data[0x03] = v;					// Data
			wrtCmd.CmdLen = 0x04;
			break;
		}
		break;
	case VGMC_YM2203:
		wrtCmd.Data[0x00] = 0x55 + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_YM2608:
		wrtCmd.Data[0x00] = 0x56 + (port & 0x01) + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_YM2610:
		wrtCmd.Data[0x00] = 0x58 + (port & 0x01) + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_YM3812:
		wrtCmd.Data[0x00] = 0x5A + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_YM3526:
		wrtCmd.Data[0x00] = 0x5B + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_Y8950:
		wrtCmd.Data[0x00] = 0x5C + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_YMF262:
		wrtCmd.Data[0x00] = 0x5E + (port & 0x01) + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_YMF278B:
		wrtCmd.Data[0x00] = 0xD0;
		wrtCmd.Data[0x01] = port | (_chipType & 0x80);
		wrtCmd.Data[0x02] = r;
		wrtCmd.Data[0x03] = v;
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_YMF271:
		wrtCmd.Data[0x00] = 0xD1;
		wrtCmd.Data[0x01] = port | (_chipType & 0x80);
		wrtCmd.Data[0x02] = r;
		wrtCmd.Data[0x03] = v;
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_YMZ280B:
		wrtCmd.Data[0x00] = 0x5D + cm;
		wrtCmd.Data[0x01] = r;
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_RF5C164:
		switch(port)
		{
		case 0x00:
			wrtCmd.Data[0x00] = 0xB1;				// Write Register
			wrtCmd.Data[0x01] = r;					// Register
			wrtCmd.Data[0x02] = v;					// Value
			wrtCmd.CmdLen = 0x03;
			break;
		case 0x01:
			wrtCmd.Data[0x00] = 0xC2;				// Write Memory
			wrtCmd.Data[0x01] = (r >> 0) & 0xFF;	// offset low
			wrtCmd.Data[0x02] = (r >> 8) & 0xFF;	// offset high
			wrtCmd.Data[0x03] = v;					// Data
			wrtCmd.CmdLen = 0x04;
			break;
		}
		break;
	case VGMC_PWM:
		wrtCmd.Data[0x00] = 0xB2;
		wrtCmd.Data[0x01] = (port << 4) | ((r & 0xF00) >> 8);
		wrtCmd.Data[0x02] = r & 0xFF;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_AY8910:
		wrtCmd.Data[0x00] = 0xA0;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_GBSOUND:
		wrtCmd.Data[0x00] = 0xB3;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_NESAPU:
		wrtCmd.Data[0x00] = 0xB4;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_MULTIPCM:
		switch(port)
		{
		case 0x00:	// Register Write
			wrtCmd.Data[0x00] = 0xB5;
			wrtCmd.Data[0x01] = r | (_chipType & 0x80);
			wrtCmd.Data[0x02] = v;
			wrtCmd.CmdLen = 0x03;
			break;
		case 0x01:	// Bank Write
			wrtCmd.Data[0x00] = 0xC3;
			wrtCmd.Data[0x01] = v | (_chipType & 0x80);	// Both/Left/Right Offset
			wrtCmd.Data[0x02] = (r >> 0) & 0xFF;		// offset low
			wrtCmd.Data[0x03] = (r >> 8) & 0xFF;		// offset high
			wrtCmd.CmdLen = 0x04;
			break;
		}
		break;
	case VGMC_UPD7759:
		wrtCmd.Data[0x00] = 0xB6;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_OKIM6258:
		wrtCmd.Data[0x00] = 0xB7;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_OKIM6295:
		wrtCmd.Data[0x00] = 0xB8;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_K051649:
		wrtCmd.Data[0x00] = 0xD2;
		wrtCmd.Data[0x01] = port | (_chipType & 0x80);
		wrtCmd.Data[0x02] = r;
		wrtCmd.Data[0x03] = v;
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_K054539:
		wrtCmd.Data[0x00] = 0xD3;
		wrtCmd.Data[0x01] = (r >> 8) | (_chipType & 0x80);
		wrtCmd.Data[0x02] = r & 0xFF;
		wrtCmd.Data[0x03] = v;
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_C6280:
		wrtCmd.Data[0x00] = 0xB9;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_C140:
		wrtCmd.Data[0x00] = 0xD4;
		wrtCmd.Data[0x01] = (r >> 8) | (_chipType & 0x80);
		wrtCmd.Data[0x02] = r & 0xFF;
		wrtCmd.Data[0x03] = v;
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_K053260:
		wrtCmd.Data[0x00] = 0xBA;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_POKEY:
		wrtCmd.Data[0x00] = 0xBB;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_QSOUND:
		wrtCmd.Data[0x00] = 0xC4;
		wrtCmd.Data[0x01] = (r & 0xFF00) >> 8;	// Data MSB
		wrtCmd.Data[0x02] = (r & 0x00FF) >> 0;	// Data LSB
		wrtCmd.Data[0x03] = v;					// Command
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_SCSP:
		switch(port & 0x80)
		{
		case 0x00:	// Register Write
			wrtCmd.Data[0x00] = 0xC5;
			wrtCmd.Data[0x01] = ((r & 0xFF00) >> 8) | (_chipType & 0x80);	// Data MSB
			wrtCmd.Data[0x02] = (r & 0x00FF) >> 0;							// Data LSB
			wrtCmd.Data[0x03] = v;											// Command
			wrtCmd.CmdLen = 0x04;
			break;
		case 0x80:	// Memory Write
			mem_addr = ((port & 0x7F) << 16) | (r << 0);
			
			// optimize consecutive Memory Writes
			if (_pcmCache.Start == 0xFFFFFFFF || mem_addr != _pcmCache.Next)
			{
				// flush cache to file
				FlushPCMCache();
				_vgmlog->WriteDelay(vf);
				//printf("Mem Cache Start: %06X\n", mem_addr);
				_pcmCache.Start = mem_addr;
				_pcmCache.Next = _pcmCache.Start;
				_pcmCache.Pos = 0x00;
			}
			_pcmCache.CacheData[_pcmCache.Pos] = v;
			_pcmCache.Pos ++;
			_pcmCache.Next ++;
			if (_pcmCache.Pos >= _pcmCache.CacheSize)
				_pcmCache.Next = 0xFFFFFFFF;
			return;
		}
		break;
	case VGMC_WSWAN:
		switch(port)
		{
		case 0x00:
			wrtCmd.Data[0x00] = 0xBC;
			wrtCmd.Data[0x01] = r | (_chipType & 0x80);
			wrtCmd.Data[0x02] = v;
			wrtCmd.CmdLen = 0x03;
			break;
		case 0x01:
			r |= (_chipType & 0x80) << 8;
			wrtCmd.Data[0x00] = 0xC6;				// Write Memory
			wrtCmd.Data[0x01] = (r >> 8) & 0xFF;	// offset high
			wrtCmd.Data[0x02] = (r >> 0) & 0xFF;	// offset low
			wrtCmd.Data[0x03] = v;					// Data
			wrtCmd.CmdLen = 0x04;
			break;
		}
		break;
	case VGMC_VSU:
		r |= (_chipType & 0x80) << 8;
		wrtCmd.Data[0x00] = 0xC7;
		wrtCmd.Data[0x01] = (r & 0xFF00) >> 8;	// Offset MSB
		wrtCmd.Data[0x02] = (r & 0x00FF) >> 0;	// Offset LSB
		wrtCmd.Data[0x03] = v;					// Data
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_SAA1099:
		wrtCmd.Data[0x00] = 0xBD;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_ES5503:
		switch(port & 0x80)
		{
		case 0x00:	// register write
			r |= (_chipType & 0x80) << 8;
			wrtCmd.Data[0x00] = 0xD5;
			wrtCmd.Data[0x01] = (r & 0xFF00) >> 8;	// Chip Select
			wrtCmd.Data[0x02] = (r & 0x00FF) >> 0;	// Register
			wrtCmd.Data[0x03] = v;
			wrtCmd.CmdLen = 0x04;
			break;
		case 0x80:	// RAM write
			mem_addr = ((port & 0x7F) << 16) | (r << 0);
			
			// optimize consecutive Memory Writes
			if (_pcmCache.Start == 0xFFFFFFFF || mem_addr != _pcmCache.Next)
			{
				// flush cache to file
				FlushPCMCache();
				_vgmlog->WriteDelay(vf);
				_pcmCache.Start = mem_addr;
				_pcmCache.Next = _pcmCache.Start;
				_pcmCache.Pos = 0x00;
			}
			_pcmCache.CacheData[_pcmCache.Pos] = v;
			_pcmCache.Pos ++;
			_pcmCache.Next ++;
			if (_pcmCache.Pos >= _pcmCache.CacheSize)
				_pcmCache.Next = 0xFFFFFFFF;
			return;
		}
		break;
	case VGMC_ES5506:
		switch(port & 0x80)
		{
		case 0x00:	// 8-bit register write
			wrtCmd.Data[0x00] = 0xBE;
			wrtCmd.Data[0x01] = r | (_chipType & 0x80);
			wrtCmd.Data[0x02] = v;
			wrtCmd.CmdLen = 0x03;
			break;
		case 0x80:		// 16-bit register write
			wrtCmd.Data[0x00] = 0xD6;
			wrtCmd.Data[0x01] = v | (_chipType & 0x80);
			wrtCmd.Data[0x02] = (r & 0xFF00) >> 8;	// Data MSB
			wrtCmd.Data[0x03] = (r & 0x00FF) >> 0;	// Data LSB
			wrtCmd.CmdLen = 0x04;
			break;
		}
		break;
	case VGMC_X1_010:
		r |= (_chipType & 0x80) << 8;
		wrtCmd.Data[0x00] = 0xC8;
		wrtCmd.Data[0x01] = (r & 0xFF00) >> 8;	// Offset MSB
		wrtCmd.Data[0x02] = (r & 0x00FF) >> 0;	// Offset LSB
		wrtCmd.Data[0x03] = v;
		wrtCmd.CmdLen = 0x04;
		break;
	case VGMC_C352:
		port |= (_chipType & 0x80);
		wrtCmd.Data[0x00] = 0xE1;
		wrtCmd.Data[0x01] = port;				// Register MSB
		wrtCmd.Data[0x02] = v;					// Register LSB
		wrtCmd.Data[0x03] = (r & 0xFF00) >> 8;	// Data MSB
		wrtCmd.Data[0x04] = (r & 0x00FF) >> 0;	// Data LSB
		wrtCmd.CmdLen = 0x05;
		break;
	case VGMC_GA20:
		wrtCmd.Data[0x00] = 0xBF;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_MIKEY:
		wrtCmd.Data[0x00] = 0x40;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_K007232:
		wrtCmd.Data[0x00] = 0x41;
		wrtCmd.Data[0x01] = r | (_chipType & 0x80);
		wrtCmd.Data[0x02] = v;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_K005289:
		wrtCmd.Data[0x00] = 0x42;
		wrtCmd.Data[0x01] = (_chipType & 0x80) | (port << 4) | ((r & 0xF00) >> 8);
		wrtCmd.Data[0x02] = r & 0xFF;
		wrtCmd.CmdLen = 0x03;
		break;
	case VGMC_OKIM5205:
		wrtCmd.Data[0x00] = 0x32;
		wrtCmd.Data[0x01] = (_chipType & 0x80) | ((r & 0x07) << 4) | (v & 0x0F);
		wrtCmd.CmdLen = 0x02;
		break;
//	case VGMC_OKIM6376:
//		wrtCmd.Data[0x00] = 0x31;
//		wrtCmd.Data[0x01] = v;
//		wrtCmd.CmdLen = 0x02;
//		break;
	}
	
	_vgmlog->WriteDelay(vf);
	
	if (! vf.WroteHeader)
	{
		/*if (vf.CmdCount >= vf.CmdAlloc)
		{
			vf.CmdAlloc += 0x100;
			vf.Commands = (VGM_INIT_CMD*)realloc(vf.Commands, sizeof(VGM_INIT_CMD) * vf.CmdAlloc);
		}*/
		
		// most commands sent at time 0 come from soundchip_reset(),
		// so I check if the command is "worth" being written
		cm = 0x00;
		switch(_chipType & 0x7F)
		{
		case VGMC_YM2203:
		case VGMC_YM2608:	// (not on YM2610 and YM2612)
			if (r >= 0x2D && r <= 0x2F)
				cm = 0x01;	// Prescaler Select
			break;
		case VGMC_OKIM6258:
			if (r >= 0x08 && r <= 0x0F)
				cm = 0x01;	// OKIM6258 clock change
			break;
		case VGMC_OKIM6295:
			if (r >= 0x08 && r <= 0x0F)
				cm = 0x01;	// OKIM6295 clock change and configuration
			break;
		}
		
		if (cm && vf.CmdCount < 0x100)
		{
			vf.Commands[vf.CmdCount] = wrtCmd;
			vf.CmdCount ++;
		}
		return;
	}
	
	fwrite(wrtCmd.Data, 0x01, wrtCmd.CmdLen, vf.hFile);
	vf.BytesWrt += wrtCmd.CmdLen;
	
	return;
}

void VGMDeviceLog::WriteLargeData(uint8_t type, uint32_t blockSize, uint32_t startOfs, uint32_t dataLen, const void* data)
{
	// blockSize - ROM/RAM size
	// startOfs - Start Address
	// dataLen - Bytes to Write (0 -> write from Start Address to end of ROM/RAM)
	
	if (_vgmlog == nullptr || ! _vgmlog->_logging)
		return;

	VGMLogger::VGM_INF& vf = _vgmlog->_files[_fileID];
	if (vf.hFile == nullptr)
		return;
	
	uint8_t blkType = 0x00;
	uint8_t dstart_msb = 0x00;
	switch(_chipType & 0x7F)	// Write the data
	{
	case VGMC_SN76496:
	case VGMC_T6W28:
		break;
	case VGMC_YM2413:
		break;
	case VGMC_YM2612:
		break;
	case VGMC_YM2151:
		break;
	case VGMC_SEGAPCM:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Image
			blkType = 0x80;	// Type: SegaPCM ROM Image
			break;
		}
		break;
	case VGMC_RF5C68:
		break;
	case VGMC_YM2203:
		break;
	case VGMC_YM2608:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// PCM ROM Data
			blkType = 0x81;	// Type: YM2608 DELTA-T ROM Data
			break;
		}
		break;
	case VGMC_YM2610:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// PCM ROM Data A
			blkType = 0x82;	// Type: YM2610 ADPCM ROM Data
			break;
		case 0x02:	// PCM ROM Data B
			blkType = 0x83;	// Type: YM2610 DELTA-T ROM Data
			break;
		}
		break;
	case VGMC_YM3812:
		break;
	case VGMC_YM3526:
		break;
	case VGMC_Y8950:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// DELTA-T Memory
			blkType = 0x88;	// Type: Y8950 DELTA-T ROM Data
			break;
		}
		break;
	case VGMC_YMF262:
		break;
	case VGMC_YMF278B:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x84;	// Type: YMF278B ROM Data
			break;
		case 0x02:	// RAM Data
			blkType = 0x87;	// Type: YMF278B RAM Data
			break;
		}
		break;
	case VGMC_YMF271:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x85;	// Type: YMF271 ROM Data
			break;
		}
		break;
	case VGMC_YMZ280B:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x86;	// Type: YMZ280B ROM Data
			break;
		}
		break;
	case VGMC_RF5C164:
		break;
	case VGMC_PWM:
		break;
	case VGMC_AY8910:
		break;
	case VGMC_GBSOUND:
		break;
	case VGMC_NESAPU:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// RAM Data
			blkType = 0xC2;
			{
				uint8_t ret = _vgmlog->NES_RAMCheck(vf, blockSize, &startOfs, &dataLen,
								static_cast<const uint8_t*>(data));
				if (! ret)
					return;
				if (ret == 0x02)
				{
					data = static_cast<const uint8_t*>(data) - startOfs + 0xC000;
					startOfs = 0xC000;
					dataLen = 0x4000;
				}
			}
			break;
		}
		break;
	case VGMC_MULTIPCM:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x89;	// Type: MultiPCM ROM Data
			break;
		}
		break;
	case VGMC_UPD7759:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x8A;	// Type: UPD7759 ROM Data
			break;
		}
		break;
	case VGMC_OKIM6258:
		break;
	case VGMC_OKIM6295:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x8B;	// Type: OKIM6295 ROM Data
			break;
		}
		break;
	case VGMC_K051649:
		break;
	case VGMC_K054539:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x8C;	// Type: K054539 ROM Data
			break;
		}
		break;
	case VGMC_C6280:
		break;
	case VGMC_C140:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x8D;	// Type: C140 ROM Data
			break;
		}
		break;
	case VGMC_K053260:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x8E;	// Type: K053260 ROM Data
			break;
		}
		break;
	case VGMC_POKEY:
		break;
	case VGMC_QSOUND:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x8F;	// Type: QSound ROM Data
			break;
		}
		break;
	case VGMC_SCSP:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// RAM Data
			FlushPCMCache();
			blkType = 0xE0;	// Type: YMF292/SCSP RAM Data
			break;
		case 0x02:	// ROM Data
			blkType = 0x06;	// Type: unused/invalid
			break;
		}
		break;
	case VGMC_WSWAN:
		break;
	case VGMC_VSU:
		break;
	case VGMC_SAA1099:
		break;
	case VGMC_ES5503:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// RAM Data
			FlushPCMCache();
			blkType = 0xE1;	// Type: ES5503 RAM Data
			break;
		}
		break;
	case VGMC_ES5506:
		// "type" tells us the actual region
		blkType = 0x90;	// Type: ES5506 ROM Data
		dstart_msb = type << 4;
		break;
	case VGMC_X1_010:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x91;	// Type: X1-010 ROM Data
			break;
		}
		break;
	case VGMC_C352:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x92;	// Type: C352 ROM Data
			break;
		}
		break;
	case VGMC_GA20:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x93;	// Type: GA20 ROM Data
			break;
		}
		break;
	case VGMC_MIKEY:
		break;
	case VGMC_K007232:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0x94;	// Type: K007232 ROM Data
			break;
		}
		break;
	case VGMC_K005289:
		switch(type)
		{
		case 0x00:
			break;
		case 0x01:	// ROM Data
			blkType = 0xC3;	// Type: K005289 ROM Data (treated as RAM due to being small and fixed-size)
			break;
		}
		break;
	case VGMC_OKIM5205:
		break;
//	case VGMC_OKIM6376:
//		switch(type)
//		{
//		case 0x00:
//			break;
//		case 0x01:	// ROM Data
//			//blkType = 0x8C;	// Type: OKIM6376 ROM Data
//			break;
//		}
//		break;
	}
	
	if (! blkType)
		return;
	
	if (data == nullptr)
		print_info("ROM Data %02X: (0x%X bytes) is NULL!\n", blkType, blockSize);
	
	_vgmlog->WriteDelay(vf);
	
	if (! vf.WroteHeader)
	{
		bool doWrite = false;
		switch(blkType & 0xC0)
		{
		case 0x80:	// ROM Image
			doWrite = true;
			break;
		case 0xC0:	// RAM Writes
			if (blkType == 0xC3)
				doWrite = true;	// save/rewrite this as well
			break;
		}
		if (doWrite && vf.DataCount < 0x20)
		{
			VGMLogger::VGM_ROM_DATA& vrd = vf.DataBlk[vf.DataCount];
			vf.DataCount ++;
			
			vrd.type = blkType;
			vrd.dstart_msb = dstart_msb;
			vrd.dataSize = blockSize | ((_chipType & 0x80) << 24);
			vrd.data = data;
		}
		return;
	}
	
	fputc(0x67, vf.hFile);
	fputc(0x66, vf.hFile);
	fputc(blkType, vf.hFile);
	
	uint32_t finalSize;
	std::vector<uint8_t> romPatchBuf;	// temporary buffer for patched ROM data
	switch(blkType & 0xC0)
	{
	case 0x00:	// Normal Data Block
		if (! dataLen)
			dataLen = blockSize - startOfs;
		if (data == nullptr)
		{
			startOfs = 0x00;
			dataLen = 0x00;
		}
		finalSize = dataLen;
		finalSize |= (_chipType & 0x80) << 24;
		
		fwrite(&finalSize, 0x04, 0x01, vf.hFile);	// Data Block Size
		if (data == nullptr && dumpEmptyBlocks)
			DumpEmptyData(vf.hFile, dataLen, 0x00);
		else
			fwrite(data, 0x01, dataLen, vf.hFile);
		vf.BytesWrt += 0x07 + (finalSize & 0x7FFFFFFF);
		break;
	case 0x80:	// ROM Image
		// Value 1 & 2 are used to write parts of the image (and save space)
		if (! dataLen)
			dataLen = blockSize - startOfs;
		if (data == nullptr && !dumpEmptyBlocks)
		{
			startOfs = 0x00;
			dataLen = 0x00;
		}
		
		PatchROMBlock(blkType, dataLen, data, startOfs, romPatchBuf, vf.header);
		finalSize = dataLen | ((_chipType & 0x80) << 24);
		finalSize += 0x08;
		startOfs |= (dstart_msb << 24);
		
		fwrite(&finalSize, 0x04, 0x01, vf.hFile);	// Data Block Size
		fwrite(&blockSize, 0x04, 0x01, vf.hFile);	// ROM Size
		fwrite(&startOfs, 0x04, 0x01, vf.hFile);	// Data Base Address
		if (data == nullptr && dumpEmptyBlocks)
		{
			DumpEmptyData(vf.hFile, dataLen, 0x00);
		}
		else
		{
			size_t wrtByt = fwrite(data, 0x01, dataLen, vf.hFile);
			if (wrtByt != dataLen)
				print_info("Warning VGM WriteLargeData: wrote only 0x%X bytes instead of 0x%X!\n", (uint32_t)wrtByt, dataLen);
			//else
			//	print_info("Wrote 0x%X bytes, new file ofs: 0x%X\n", (uint32_t)wrtByt, ftell(vf.hFile));
		}
		vf.BytesWrt += 0x07 + (finalSize & 0x7FFFFFFF);
		break;
	case 0xC0:	// RAM Writes
		if (! dataLen)
			dataLen = blockSize - startOfs;
		if (data == nullptr && !dumpEmptyBlocks)
		{
			startOfs = 0x00;
			dataLen = 0x00;
		}
		
		PatchROMBlock(blkType, dataLen, data, startOfs, romPatchBuf, vf.header);
		finalSize = dataLen | ((_chipType & 0x80) << 24);
		if (! (blkType & 0x20))
		{
			finalSize += 0x02;
			fwrite(&finalSize, 0x04, 0x01, vf.hFile);	// Data Block Size
			fwrite(&startOfs, 0x02, 0x01, vf.hFile);	// Data Address
		}
		else
		{
			finalSize += 0x04;
			fwrite(&finalSize, 0x04, 0x01, vf.hFile);	// Data Block Size
			fwrite(&startOfs, 0x04, 0x01, vf.hFile);	// Data Address
		}
		if (data == nullptr && dumpEmptyBlocks)
			DumpEmptyData(vf.hFile, dataLen, 0x00);
		else
			fwrite(data, 0x01, dataLen, vf.hFile);
		vf.BytesWrt += 0x07 + (finalSize & 0x7FFFFFFF);
		break;
	}
	
	return;
}

uint8_t VGMLogger::NES_RAMCheck(VGM_INF& vf, uint32_t blockSize, uint32_t* startOfs, uint32_t* dataLen, const uint8_t* data)
{
	if (*startOfs < 0xC000)
		return 0xFF;	// unable to handle this
	
	uint32_t sOfs = *startOfs - 0xC000;
	uint32_t dLen = *dataLen;
	
	if (vf.NesMemEmpty)
	{
		vf.NesMemEmpty = 0x00;
		memcpy(vf.NesMem, data - sOfs, 0x4000);	// cache memory from 0xC000..0xFFFF
		return 0x02;	// need full dump of the NES ROM section
	}
	
	if (! dLen)
		dLen = 0x4000 - sOfs;
	if (! memcmp(&vf.NesMem[sOfs], data, dLen))	// compare against cache
	{
		// cache match - discard write
		return 0x00;
	}
	else
	{
		// cache mismatch - refresh cache and accept write
		memcpy(&vf.NesMem[sOfs], data, dLen);
		return 0x01;
	}
}

VGMDeviceLog* VGMLogger::GetChip(uint8_t chipType, uint8_t instance)
{
	// This is a small helper-function, that allows drivers/machines to
	// make writes for their chips.
	// e.g. NeoGeo CD rewrites the YM2610's PCM-RAM after changes
	for (VGMDeviceLog& vc : _chips)
	{
		if ((vc._chipType & 0x7F) == chipType &&
			(vc._chipType >> 7) == instance)
			return &vc;
	}
	
	return &dummyDevice;
}

/*static*/ VGMDeviceLog* VGMLogger::GetDummyChip(void)
{
	return &dummyDevice;
}

void VGMDeviceLog::FlushPCMCache(void)
{
	VGMLogger::VGM_INF& vf = _vgmlog->_files[_fileID];
	if (! _vgmlog->_logging || vf.hFile == nullptr)
		return;
	
	if (_pcmCache.CacheData == nullptr)
		return;
	if (_pcmCache.Start == 0xFFFFFFFF || ! _pcmCache.Pos)
		return;
	//print_info("Flushing PCM Data: Chip %02X, Addr %06X, Size %06X\n",
	//			_chipType, _pcmCache.Start, _pcmCache.Pos);
	
	bool singleWrt;
	uint8_t blkType;
	uint8_t cmdType;
	
	cmdType = 0xFF;
	blkType = 0xFF;
	switch(_chipType & 0x7F)
	{
	case VGMC_RF5C68:
		cmdType = 0xC1;
		blkType = 0xC0;
		break;
	case VGMC_RF5C164:
		cmdType = 0xC2;
		blkType = 0xC1;
		break;
	case VGMC_SCSP:
		blkType = 0xE0;
		break;
	case VGMC_ES5503:
		blkType = 0xE1;
		break;
	}
	if (_pcmCache.Pos == 0x01 && cmdType != 0xFF)
		singleWrt = true;
	else
		singleWrt = false;
	
	if (singleWrt)
	{
		// it would be a waste of space to write a data block for 1 byte of data
		fputc(cmdType, vf.hFile);		// Write Memory
		fputc((_pcmCache.Start >> 0) & 0xFF, vf.hFile);	// offset low
		fputc((_pcmCache.Start >> 8) & 0xFF, vf.hFile);	// offset high
		fputc(_pcmCache.CacheData[0x00], vf.hFile);		// Data
		vf.BytesWrt += 0x04;
	}
	else
	{
		uint32_t finalSize;

		// calling WriteLargeData() doesn't work if FlushPCMCache() is called from WriteDelay()
		fputc(0x67, vf.hFile);
		fputc(0x66, vf.hFile);
		fputc(blkType, vf.hFile);
		finalSize = _pcmCache.Pos | ((_chipType & 0x80) << 24);
		if (! (blkType & 0x20))
		{
			finalSize += 0x02;
			fwrite(&finalSize, 0x04, 0x01, vf.hFile);	// Data Block Size
			fwrite(&_pcmCache.Start, 0x02, 0x01, vf.hFile);		// Data Address
		}
		else
		{
			finalSize += 0x04;
			fwrite(&finalSize, 0x04, 0x01, vf.hFile);	// Data Block Size
			fwrite(&_pcmCache.Start, 0x04, 0x01, vf.hFile);		// Data Address
		}
		fwrite(_pcmCache.CacheData, 0x01, _pcmCache.Pos, vf.hFile);
		vf.BytesWrt += 0x07 + (finalSize & 0x7FFFFFFF);
	}
	
	_pcmCache.Start = 0xFFFFFFFF;
	
	return;
}

void VGMLogger::ChangeROMData(uint32_t oldsize, const void* olddata, uint32_t newsize, const void* newdata)
{
	uint8_t curdblk;
	
	print_info("Changing ROM Data (address %p, 0x%X bytes -> address %p, 0x%X bytes) ...\n",
				olddata, oldsize, newdata, newsize);
	for (VGM_INF& vf : _files)
	{
		if (vf.hFile == nullptr)
			continue;
		
		for (curdblk = 0x00; curdblk < vf.DataCount; curdblk ++)
		{
			VGM_ROM_DATA& vrd = vf.DataBlk[curdblk];
			if ((vrd.dataSize & 0x7FFFFFFF) == oldsize && vrd.data == olddata)
			{
				print_info("Match found in VGM %02X, Block Type %02X\n", &vf - &_files[0], vrd.type);
				vrd.dataSize &= 0x80000000;	// keep "chip select" bit
				vrd.dataSize |= newsize;
				vrd.data = newdata;
			}
		}
	}
	
	return;
}

void VGMDeviceLog::DumpSampleROM(uint8_t type, memory_region* region)
{
	if (region == nullptr)
		return;
	
	print_info("VGM - Dumping ROM-Region %s: size 0x%X, ptr %p\n", region->name(), region->bytes(), region->base());
	WriteLargeData(type, region->bytes(), 0x00, 0x00, region->base());
	
	return;
}

void VGMDeviceLog::DumpSampleROM(uint8_t type, address_space& space)
{
	print_info("Space Info: Name %s, Size: 0x%X, Device Name: %s, Device BaseTag: %s\n",
			space.name(), space.addrmask() + 1, space.device().name(), space.device().basetag());
	
	memory_region* rom_region = _machine->root_device().memregion(space.device().basetag());	// get memory region of space's root device
	if (rom_region)
	{
		DumpSampleROM(type, rom_region);
		return;
	}
	
	uint32_t dataSize = space.addrmask() + 1;
	const uint8_t* dataPtrA = static_cast<const uint8_t*>(space.get_read_ptr(0));
	const uint8_t* dataPtrB = static_cast<const uint8_t*>(space.get_read_ptr(space.addrmask()));
	if ((dataPtrB - dataPtrA) == space.addrmask())
	{
		print_info("VGM - Dumping Device-Space %s: size 0x%X, space-ptr %p\n", space.name(), dataSize, dataPtrA);
		WriteLargeData(type, dataSize, 0x00, 0x00, dataPtrA);
	}
	else
	{
		print_info("VGM - Device-Space %s: size 0x%X, dumping empty block due to non-continuous memory\n", space.name(), dataSize);
		WriteLargeData(type, dataSize, 0x00, 0x00, nullptr);
	}
	
	return;
}
