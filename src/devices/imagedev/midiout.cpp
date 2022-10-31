// license:BSD-3-Clause
// copyright-holders:R. Belmont, Valley Bell
/*********************************************************************

    midiout.c

    MIDI Out image device and serial receiver

*********************************************************************/

#include "emu.h"
#include "midiout.h"
#include "osdepend.h"
#include <fstream>

/***************************************************************************
    IMPLEMENTATION
***************************************************************************/

DEFINE_DEVICE_TYPE(MIDIOUT, midiout_device, "midiout", "MIDI Out image device")

const uint32_t midiout_device::MIDI_CMD_SIZE[0x08] =
{	0x02, 0x02, 0x02, 0x02, 0x01, 0x01, 0x02, (uint32_t)-1};	// 80-F0
const uint32_t midiout_device::MIDI_CMD_XSIZE[0x10] =
	//       00    01    02    03    04    05    06    07
{	(uint32_t)-1, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, (uint32_t)-1,	// F0-F7
	        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};	// F8-FF

/*-------------------------------------------------
    ctor
-------------------------------------------------*/

midiout_device::midiout_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, MIDIOUT, tag, owner, clock),
		device_image_interface(mconfig, *this),
		device_serial_interface(mconfig, *this),
		m_midi()
{
}

midiout_device::~midiout_device()
{
	log_end();
}

/*-------------------------------------------------
    device_start
-------------------------------------------------*/

void midiout_device::device_start()
{
	m_midi.reset();
}

void midiout_device::device_reset()
{
	// we don't Tx, we Rx at 31250 8-N-1
	set_data_frame(1, 8, PARITY_NONE, STOP_BITS_1);
	set_rcv_rate(31250);
	set_tra_rate(0);

	m_time_log_start = machine().time();
	m_event_tick = 0;
	m_file_tick = 0;
	m_last_status = 0x00;
}

/*-------------------------------------------------
    call_load
-------------------------------------------------*/

image_init_result midiout_device::call_load()
{
	log_start("out.mid");
	m_midi = machine().osd().create_midi_device();

	if (!m_midi->open_output(filename()))
	{
		m_midi.reset();
		return image_init_result::FAIL;
	}

	return image_init_result::PASS;
}

/*-------------------------------------------------
    call_unload
-------------------------------------------------*/

void midiout_device::call_unload()
{
	if (m_midi)
	{
		m_midi->close();
		m_midi.reset();
	}
	log_end();
}

void midiout_device::rcv_complete()    // Rx completed receiving byte
{
	receive_register_extract();
	uint8_t data = get_received_char();

	if (m_midi)
	{
		log_proc_byte(data);
		m_midi->write(data);
	}
}


void midiout_device::log_write_be16(uint32_t value)
{
	m_log_file->put((value >> 8) & 0xFF);
	m_log_file->put((value >> 0) & 0xFF);
}

void midiout_device::log_write_be32(uint32_t value)
{
	m_log_file->put((value >> 24) & 0xFF);
	m_log_file->put((value >> 16) & 0xFF);
	m_log_file->put((value >>  8) & 0xFF);
	m_log_file->put((value >>  0) & 0xFF);
}

void midiout_device::log_write_varlen(uint32_t value)
{
	constexpr size_t buffer_size = 8; 
	uint8_t buffer[buffer_size];
	size_t buf_pos = sizeof(buffer) - 1;

	buffer[buf_pos] = 0x00 | (value & 0x7F);
	value >>= 7;
	while(value > 0 && buf_pos > 0)
	{
		buf_pos --;
		buffer[buf_pos] = 0x80 | (value & 0x7F);
		value >>= 7;
	}
	m_log_file->write(reinterpret_cast<char*>(&buffer[buf_pos]), buffer_size - buf_pos);
}

void midiout_device::log_start(const std::string& filename)
{
	m_log_file = std::make_unique<std::ofstream>(filename, std::ios::binary);
	if (!m_log_file || !m_log_file->is_open())
		return;

	// write main header
	m_log_file->write("MThd", 4);
	log_write_be32(6);	// header size
	log_write_be16(0);	// Format 0
	log_write_be16(1);	// 1 track
	log_write_be16(LOG_RESOLUTION / 2);	// default tempo is 120 BPM, so we have (n/2 ticks/beat) for (1*n ticks/second)
	// write track header
	m_log_file->write("MTrk", 4);
	log_write_be32(0);	// track size (dummy)
	m_mid_track_start_pos = static_cast<uint32_t>(m_log_file->tellp());
}

void midiout_device::log_proc_byte(uint8_t data)
{
	if (!m_log_file || !m_log_file->is_open())
		return;

	if (m_cmd_rem_bytes == 0)
	{
		log_finish_comand();

		// waiting for new command
		if (!(data & 0x80))
		{
			// short event
			if (!m_last_status)
				return;	// unable to determine event type
			m_cur_status = m_last_status;
		}
		else
		{
			m_cur_status = data;
			if (data < 0xF0)
				m_last_status = data;
		}
		if (m_cur_status < 0xF0)
			m_cmd_rem_bytes = MIDI_CMD_SIZE[(m_cur_status >> 4) & 0x07];
		else
			m_cmd_rem_bytes = MIDI_CMD_XSIZE[m_cur_status & 0x0F];
		
		attotime event_time = machine().time() - m_time_log_start;
		m_event_tick = event_time.as_ticks(LOG_RESOLUTION);
		
		if (m_cur_status == 0xFF)	// device reset
		{
			std::string devResetStr = "Device Reset";
			m_cmd_buf.push_back(0xFF);
			m_cmd_buf.push_back(0x06);
			log_finish_comand();
			log_write_varlen(devResetStr.length());
			m_log_file->write(devResetStr.data(), devResetStr.length());
			return;
		}
		else if (m_cur_status >= 0xF1 && m_cur_status != 0xF7)
		{
			return;	// invalid for MIDI files
		}
		
		if (data & 0x80)
		{
			// command byte - put it into the main buffer
			m_cmd_buf.push_back(data);
		}
		else
		{
			// do more processing for a parameter byte (with "remebered" command)
			log_proc_byte(data);
		}
	}
	else
	{
		bool write_data = true;
		
		m_cmd_rem_bytes --;
		if (m_cur_status == 0xF0 || m_cur_status == 0xF7)	// SysEx commands
		{
			if (data & 0x80)	// terminate SysEx command when "command bit" is set
			{
				if (data != 0xF7)
					write_data = false;
				m_cmd_rem_bytes = 0;
			}
		}
		else if (m_cur_status >= 0xF1)
		{
			return;	// invalid for MIDI files
		}
		
		if (write_data)
		{
			m_cmd_buf.push_back(data);
			if (m_cmd_buf.size() >= 0x2000)	// stop command processing after 8 KB
				m_cmd_rem_bytes = 0;
		}
		if (m_cmd_rem_bytes == 0)
			log_finish_comand();
		
		if (!write_data)
		{
			m_cmd_rem_bytes = 0;
			// process command byte again, this time as a "new event"
			log_proc_byte(data);
		}
	}
}

void midiout_device::log_finish_comand()
{
	if (m_cmd_buf.empty())
		return;
	
	//printf("log_finish_comand: FileTick %u, EvtTick %u, Cmd 0x%02X\n", m_file_tick, m_event_tick, m_cmd_buf[0]);
	log_write_varlen(m_event_tick - m_file_tick);
	m_file_tick = m_event_tick;

	if (m_cmd_buf[0] == 0xF0 || m_cmd_buf[0] == 0xF7)
	{
		// special handling for SysEx command to insert length value
		m_log_file->put(m_cmd_buf[0]);
		log_write_varlen(m_cmd_buf.size() - 1);
		m_log_file->write(reinterpret_cast<char*>(&m_cmd_buf[1]), m_cmd_buf.size() - 1);
	}
	else
	{
		m_log_file->write(reinterpret_cast<char*>(m_cmd_buf.data()), m_cmd_buf.size());
	}
	m_cmd_buf.clear();
}

void midiout_device::log_end()
{
	if (!m_log_file || !m_log_file->is_open())
		return;

	log_finish_comand();

	// Accessing machine().time() here seems to cause MAME to crash.
	//attotime event_time = machine().time() - m_time_log_start;
	//m_event_tick = event_time.as_ticks(LOG_RESOLUTION);

	m_cmd_buf.push_back(0xFF);
	m_cmd_buf.push_back(0x2F);
	m_cmd_buf.push_back(0x00);
	log_finish_comand();

	uint32_t trkSize = static_cast<uint32_t>(m_log_file->tellp()) - m_mid_track_start_pos;
	m_log_file->seekp(m_mid_track_start_pos - 4);
	log_write_be32(trkSize);
	m_log_file->close();
}
