// license:BSD-3-Clause
// copyright-holders:Aaron Giles

#include "emu.h"
#include "vgmwrite.hpp"
#include "ymopm.h"


//*********************************************************
//  YM2151 DEVICE
//*********************************************************

DEFINE_DEVICE_TYPE(YM2151, ym2151_device, "ym2151", "YM2151 OPM")

//-------------------------------------------------
//  ym2151_device - constructor
//-------------------------------------------------

ym2151_device::ym2151_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	ymfm_device_base<ymfm::ym2151>(mconfig, tag, owner, clock, YM2151),
	m_reset_state(1),
	m_vgm_log(VGMLogger::GetDummyChip())
{
}

void ym2151_device::device_start()
{
	parent::device_start();

	m_vgm_log = machine().vgm_logger().OpenDevice(VGMC_YM2151, clock());
}


//-------------------------------------------------
//  write/register_w/data_w - write access
//-------------------------------------------------

void ym2151_device::write(offs_t offset, u8 data)
{
	if (m_reset_state == 0)
		return;
	parent::write(offset, data);
	if (offset & 0x01)
		m_vgm_log->Write(offset >> 1, m_reg, data);
	else
		m_reg = data;
}

void ym2151_device::address_w(u8 data)
{
	if (m_reset_state == 0)
		return;
	parent::address_w(data);
	m_reg = data;
}

void ym2151_device::data_w(u8 data)
{
	if (m_reset_state == 0)
		return;
	parent::data_w(data);
	m_vgm_log->Write(0, m_reg, data);
}


//-------------------------------------------------
//  reset_w - reset line, active LOW
//-------------------------------------------------

void ym2151_device::reset_w(int state)
{
	if (state != m_reset_state)
	{
		m_stream->update();
		m_reset_state = state;
		if (state != 0)
			m_chip.reset();
	}
}



//*********************************************************
//  YM2164 DEVICE
//*********************************************************

DEFINE_DEVICE_TYPE(YM2164, ym2164_device, "ym2164", "YM2164 OPP")

//-------------------------------------------------
//  ym2164_device - constructor
//-------------------------------------------------

ym2164_device::ym2164_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock) :
	ymfm_device_base<ymfm::ym2164>(mconfig, tag, owner, clock, YM2164),
	m_vgm_log(VGMLogger::GetDummyChip())
{
}

void ym2164_device::device_start()
{
	parent::device_start();

	// NOTE: not really working, as YM2164 has a slightly different register layout
	m_vgm_log = machine().vgm_logger().OpenDevice(VGMC_YM2151, clock());
}

void ym2164_device::write(offs_t offset, u8 data)
{
	parent::write(offset, data);
	if (offset & 0x01)
		m_vgm_log->Write(offset >> 1, m_reg, data);
	else
		m_reg = data;
}

void ym2164_device::address_w(u8 data)
{
	parent::address_w(data);
	m_reg = data;
}

void ym2164_device::data_w(u8 data)
{
	parent::data_w(data);
	m_vgm_log->Write(0, m_reg, data);
}
