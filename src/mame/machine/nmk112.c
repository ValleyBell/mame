// license:BSD-3-Clause
// copyright-holders:Alex W. Jackson
/*  NMK112 - NMK custom IC for bankswitching the sample ROMs of a pair of
    OKI6295 ADPCM chips

    The address space of each OKI6295 is divided into four banks, each one
    independently controlled. The sample table at the beginning of the
    address space may be divided in four pages as well, banked together
    with the sample data.  This allows each of the four voices on the chip
    to play a sample from a different bank at the same time. */

#include "emu.h"
#include "nmk112.h"
#include "sound/okim6295.h"
#include "sound/vgmwrite.h"

#define TABLESIZE   0x100
#define BANKSIZE    0x10000



const device_type NMK112 = &device_creator<nmk112_device>;

nmk112_device::nmk112_device(const machine_config &mconfig, const char *tag, device_t *owner, UINT32 clock)
	: device_t(mconfig, NMK112, "NMK112", tag, owner, clock, "nmk112", __FILE__),
		m_page_mask(0xff),
		m_tag0(NULL),
		m_tag1(NULL),
		m_rom0(NULL),
		m_rom1(NULL),
		m_size0(0),
		m_size1(0)
{
}

//-------------------------------------------------
//  device_start - device-specific startup
//-------------------------------------------------

void nmk112_device::device_start()
{
	save_item(NAME(m_current_bank));
	machine().save().register_postload(save_prepost_delegate(FUNC(nmk112_device::postload_bankswitch), this));

	m_vgm_idx0 = 0xFFFF;
	if (m_tag0)
	{
		m_rom0 = machine().root_device().memregion(m_tag0)->base();
		m_size0 = machine().root_device().memregion(m_tag0)->bytes() - 0x40000;
		if (m_rom0 != NULL)
		{
			m_vgm_idx0 = machine().device<okim6295_device>(m_tag0)->get_vgm_idx();
			logerror("NMK112 '%s': VGM Idx %u\n", m_tag0, m_vgm_idx0);
			if (m_page_mask & 0x01)
				vgm_write(m_vgm_idx0, 0x00, 0x0E, 0x81);
			else
				vgm_write(m_vgm_idx0, 0x00, 0x0E, 0x01);
			vgm_change_rom_data(m_size0 + 0x40000, m_rom0, m_size0, m_rom0 + 0x40000);
		}
	}
	m_vgm_idx1 = 0xFFFF;
	if (m_tag1)
	{
		m_rom1 = machine().root_device().memregion(m_tag1)->base();
		m_size1 = machine().root_device().memregion(m_tag1)->bytes() - 0x40000;
		if (m_rom1 != NULL)
		{
			m_vgm_idx1 = machine().device<okim6295_device>(m_tag1)->get_vgm_idx();
			logerror("NMK112 '%s': VGM Idx %u\n", m_tag1, m_vgm_idx1);
			if (m_page_mask & 0x02)
				vgm_write(m_vgm_idx1, 0x00, 0x0E, 0x81);
			else
				vgm_write(m_vgm_idx1, 0x00, 0x0E, 0x01);
			vgm_change_rom_data(m_size1 + 0x40000, m_rom1, m_size1, m_rom1 + 0x40000);
		}
	}
}

//-------------------------------------------------
//  device_reset - device-specific reset
//-------------------------------------------------

void nmk112_device::device_reset()
{
	for (int i = 0; i < 8; i++)
	{
		m_current_bank[i] = 0;
		do_bankswitch(i, m_current_bank[i]);
	}
}

void nmk112_device::do_bankswitch( int offset, int data )
{
	int chip = (offset & 4) >> 2;
	int banknum = offset & 3;
	int paged = (m_page_mask & (1 << chip));

	UINT8 *rom = chip ? m_rom1 : m_rom0;
	int size = chip ? m_size1 : m_size0;

	m_current_bank[offset] = data;

	if (size == 0) return;

	int bankaddr = (data * BANKSIZE) % size;

	/* copy the samples */
	if ((paged) && (banknum == 0))
		memcpy(rom + 0x400, rom + 0x40000 + bankaddr + 0x400, BANKSIZE - 0x400);
	else
		memcpy(rom + banknum * BANKSIZE, rom + 0x40000 + bankaddr, BANKSIZE);

	/* also copy the sample address table, if it is paged on this chip */
	if (paged)
	{
		rom += banknum * TABLESIZE;
		memcpy(rom, rom + 0x40000 + bankaddr, TABLESIZE);
	}
}

/*****************************************************************************
    DEVICE HANDLERS
*****************************************************************************/

WRITE8_MEMBER( nmk112_device::okibank_w )
{
	UINT8 chip = (offset & 4) >> 2;
	UINT8 banknum = offset & 3;
	UINT16 vgm_idx = chip ? m_vgm_idx1 : m_vgm_idx0;

	// I want the bank change always to be written to the VGM.
	vgm_write(vgm_idx, 0x00, 0x10 | banknum, data);
	
	if (m_current_bank[offset] != data)
		do_bankswitch(offset, data);
}

void nmk112_device::postload_bankswitch()
{
	for (int i = 0; i < 8; i++)
		do_bankswitch(i, m_current_bank[i]);
}
