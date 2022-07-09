/***********************************************************************************************************************
*                                                                                                                      *
* libscopeprotocols                                                                                                    *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg and contributors                                                         *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

#include "../scopehal/scopehal.h"
#include "EthernetProtocolDecoder.h"
#include "Ethernet100BaseTXDecoder.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

Ethernet100BaseTXDecoder::Ethernet100BaseTXDecoder(const string& color)
	: EthernetProtocolDecoder(color)
{
	m_signalNames.clear();
	m_inputs.clear();

	CreateInput("data");
	CreateInput("clk");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accessors

string Ethernet100BaseTXDecoder::GetProtocolName()
{
	return "Ethernet - 100baseTX";
}

bool Ethernet100BaseTXDecoder::ValidateChannel(size_t i, StreamDescriptor stream)
{
	if(stream.m_channel == NULL)
		return false;

	if( (i == 0) && (stream.GetType() == Stream::STREAM_TYPE_ANALOG) )
		return true;
	if( (i == 1) && (stream.GetType() == Stream::STREAM_TYPE_DIGITAL) )
		return true;

	return false;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual decoder logic

void Ethernet100BaseTXDecoder::Refresh()
{
	ClearPackets();

	if(!VerifyAllInputsOK())
	{
		SetData(NULL, 0);
		return;
	}

	//Get the input data
	auto din = GetAnalogInputWaveform(0);
	auto clk = GetDigitalInputWaveform(1);

	//Sample the input on the edges of the recovered clock
	AnalogWaveform samples;
	SampleOnAnyEdges(din, clk, samples);
	size_t ilen = samples.m_samples.size();

	//MLT-3 decode
	//TODO: some kind of sanity checking that voltage is changing in the right direction
	int oldstate = GetState(samples.m_samples[0]);
	DigitalWaveform bits;
	for(size_t i=1; i<ilen; i++)
	{
		int nstate = GetState(samples.m_samples[i]);

		bits.m_offsets.push_back(samples.m_offsets[i]);
		bits.m_durations.push_back(samples.m_durations[i]);

		//No transition? Add a "0" bit
		if(nstate == oldstate)
			bits.m_samples.push_back(false);

		//Transition? Add a "1" bit
		else
			bits.m_samples.push_back(true);

		oldstate = nstate;
	}

	//RX LFSR sync
	size_t nbits = bits.m_samples.size();
	DigitalWaveform descrambled_bits;
	bool synced = false;
	for(size_t idle_offset = 0; idle_offset<15000; idle_offset++)
	{
		if(TrySync(bits, descrambled_bits, idle_offset, nbits))
		{
			LogTrace("Got good LFSR sync at offset %zu\n", idle_offset);
			synced = true;
			break;
		}
	}
	if(!synced)
	{
		LogTrace("Ethernet100BaseTXDecoder: Unable to sync RX LFSR\n");
		descrambled_bits.clear();
		SetData(nullptr, 0);
		return;
	}

	//Copy our timestamps from the input. Output has femtosecond resolution since we sampled on clock edges
	EthernetWaveform* cap = new EthernetWaveform;
	cap->m_timescale = 1;
	cap->m_startTimestamp = din->m_startTimestamp;
	cap->m_startFemtoseconds = din->m_startFemtoseconds;
	SetData(cap, 0);

	//Search until we find a 1100010001 (J-K, start of stream) sequence
	bool ssd[10] = {1, 1, 0, 0, 0, 1, 0, 0, 0, 1};
	size_t i = 0;
	bool hit = true;
	size_t des10 = descrambled_bits.m_samples.size() - 10;
	for(i=0; i<des10; i++)
	{
		hit = true;
		for(int j=0; j<10; j++)
		{
			bool b = descrambled_bits.m_samples[i+j];
			if(b != ssd[j])
			{
				hit = false;
				break;
			}
		}

		if(hit)
			break;
	}
	if(!hit)
	{
		LogTrace("No SSD found\n");
		return;
	}
	LogTrace("Found SSD at %zu\n", i);

	//Skip the J-K as we already parsed it
	i += 10;

	//4b5b decode table
	static const unsigned int code_5to4[]=
	{
		0, //0x00 unused
		0, //0x01 unused
		0, //0x02 unused
		0, //0x03 unused
		0, //0x04 = /H/, tx error
		0, //0x05 unused
		0, //0x06 unused
		0, //0x07 = /R/, second half of ESD
		0, //0x08 unused
		0x1,
		0x4,
		0x5,
		0, //0x0c unused
		0, //0x0d = /T/, first half of ESD
		0x6,
		0x7,
		0, //0x10 unused
		0, //0x11 = /K/, second half of SSD
		0x8,
		0x9,
		0x2,
		0x3,
		0xa,
		0xb,
		0, //0x18 = /J/, first half of SSD
		0, //0x19 unused
		0xc,
		0xd,
		0xe,
		0xf,
		0x0,
		0, //0x1f = idle
	};

	//Set of recovered bytes and timestamps
	vector<uint8_t> bytes;
	vector<uint64_t> starts;
	vector<uint64_t> ends;

	//Grab 5 bits at a time and decode them
	bool first = true;
	uint8_t current_byte = 0;
	uint64_t current_start = 0;
	size_t deslen = descrambled_bits.m_samples.size()-5;
	for(; i<deslen; i+=5)
	{
		unsigned int code =
			(descrambled_bits.m_samples[i+0] ? 16 : 0) |
			(descrambled_bits.m_samples[i+1] ? 8 : 0) |
			(descrambled_bits.m_samples[i+2] ? 4 : 0) |
			(descrambled_bits.m_samples[i+3] ? 2 : 0) |
			(descrambled_bits.m_samples[i+4] ? 1 : 0);

		//Handle special stuff
		if(code == 0x18)
		{
			//This is a /J/. Next code should be 0x11, /K/ - start of frame.
			//Don't check it for now, just jump ahead 5 bits and get ready to read data
			i += 5;
			continue;
		}
		else if(code == 0x0d)
		{
			//This is a /T/. Next code should be 0x07, /R/ - end of frame.
			//Crunch this frame
			BytesToFrames(bytes, starts, ends, cap);

			//Skip the /R/
			i += 5;

			//and reset for the next one
			starts.clear();
			ends.clear();
			bytes.clear();
			continue;
		}

		//TODO: process /H/ - 0x04 (error in the middle of a packet)

		//Ignore idles
		else if(code == 0x1f)
			continue;

		//Nope, normal nibble.
		unsigned int decoded = code_5to4[code];
		if(first)
		{
			current_start = descrambled_bits.m_offsets[i];
			current_byte = decoded;
		}
		else
		{
			current_byte |= decoded << 4;

			bytes.push_back(current_byte);
			starts.push_back(current_start * cap->m_timescale);
			uint64_t end = descrambled_bits.m_offsets[i+4] + descrambled_bits.m_durations[i+4];
			ends.push_back(end * cap->m_timescale);
		}

		first = !first;
	}
}

bool Ethernet100BaseTXDecoder::TrySync(
	DigitalWaveform& bits,
	DigitalWaveform& descrambled_bits,
	size_t idle_offset,
	size_t stop)
{
	if( (idle_offset + 64) >= bits.m_samples.size())
		return false;
	descrambled_bits.clear();

	//For now, assume the link is idle at the time we triggered
	unsigned int lfsr =
		( (!bits.m_samples[idle_offset + 0]) << 10 ) |
		( (!bits.m_samples[idle_offset + 1]) << 9 ) |
		( (!bits.m_samples[idle_offset + 2]) << 8 ) |
		( (!bits.m_samples[idle_offset + 3]) << 7 ) |
		( (!bits.m_samples[idle_offset + 4]) << 6 ) |
		( (!bits.m_samples[idle_offset + 5]) << 5 ) |
		( (!bits.m_samples[idle_offset + 6]) << 4 ) |
		( (!bits.m_samples[idle_offset + 7]) << 3 ) |
		( (!bits.m_samples[idle_offset + 8]) << 2 ) |
		( (!bits.m_samples[idle_offset + 9]) << 1 ) |
		( (!bits.m_samples[idle_offset + 10]) << 0 );

	//Descramble
	stop = min(stop, bits.m_samples.size());
	size_t start = idle_offset + 11;
	size_t len = stop - start;
	descrambled_bits.m_offsets.reserve(len);
	descrambled_bits.m_durations.reserve(len);
	descrambled_bits.m_samples.reserve(len);
	size_t window = 64 + idle_offset + 11;
	for(size_t i=start; i < stop; i++)
	{
		lfsr = (lfsr << 1) ^ ((lfsr >> 8)&1) ^ ((lfsr >> 10)&1);

		descrambled_bits.m_offsets.push_back(bits.m_offsets[i]);
		descrambled_bits.m_durations.push_back(bits.m_durations[i]);
		bool b = bits.m_samples[i] ^ (lfsr & 1);
		descrambled_bits.m_samples.push_back(b);

		if(descrambled_bits.m_samples.size() == window)
		{
			//We should have at least 64 "1" bits in a row once the descrambling is done.
			//The minimum inter-frame gap is a lot bigger than this.
			for(int j=0; j<64; j++)
			{
				if(descrambled_bits.m_samples[j + idle_offset + 11] != 1)
					return false;
			}
		}
	}

	//Synced, all good
	return true;
}

int Ethernet100BaseTXDecoder::GetState(float voltage)
{
	if(voltage > 0.3)
		return 1;
	else if(voltage < -0.3)
		return -1;
	else
		return 0;
}
