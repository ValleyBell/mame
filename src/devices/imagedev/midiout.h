// license:BSD-3-Clause
// copyright-holders:R. Belmont
/*********************************************************************

    midiout.h

    MIDI Out image device

*********************************************************************/

#ifndef MAME_IMAGEDEV_MIDIOUT_H
#define MAME_IMAGEDEV_MIDIOUT_H

#pragma once

#include "diserial.h"


/***************************************************************************
    TYPE DEFINITIONS
***************************************************************************/

class midiout_device :    public device_t,
						public device_image_interface,
						public device_serial_interface
{
public:
	// construction/destruction
	midiout_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock);
	~midiout_device();

	// image-level overrides
	virtual image_init_result call_load() override;
	virtual void call_unload() override;

	// image device
	virtual bool is_readable()  const noexcept override { return false; }
	virtual bool is_writeable() const noexcept override { return true; }
	virtual bool is_creatable() const noexcept override { return true; }
	virtual bool is_reset_on_load() const noexcept override { return false; }
	virtual const char *file_extensions() const noexcept override { return "mid"; }
	virtual bool core_opens_image_file() const noexcept override { return false; }
	virtual const char *image_type_name() const noexcept override { return "midiout"; }
	virtual const char *image_brief_type_name() const noexcept override { return "mout"; }

	virtual void tx(uint8_t state) { rx_w(state); }

protected:
	// device-level overrides
	virtual void device_start() override;
	virtual void device_reset() override;

	// serial overrides
	virtual void rcv_complete() override;    // Rx completed receiving byte

	void log_write_be16(uint32_t value);
	void log_write_be32(uint32_t value);
	void log_write_varlen(uint32_t value);	// write variable-length value
	void log_start(const std::string& filename);
	void log_proc_byte(uint8_t data);
	void log_finish_comand();
	void log_end();

private:
	std::unique_ptr<osd_midi_device> m_midi;

	std::unique_ptr<std::ofstream> m_log_file;
	attotime m_time_log_start;
	uint32_t m_event_tick;
	uint32_t m_file_tick;
	uint8_t m_last_status;
	uint8_t m_cur_status;
	std::vector<uint8_t> m_cmd_buf;
	uint32_t m_buf_pos;
	uint32_t m_cmd_rem_bytes;
	uint32_t m_mid_track_start_pos;

	static const uint32_t MIDI_CMD_SIZE[0x08];
	static const uint32_t MIDI_CMD_XSIZE[0x10];
	static const uint16_t LOG_RESOLUTION = 31250;	// ticks per second
};

// device type definition
DECLARE_DEVICE_TYPE(MIDIOUT, midiout_device)

// device iterator
typedef device_type_enumerator<midiout_device> midiout_device_enumerator;

#endif // MAME_IMAGEDEV_MIDIOUT_H
