/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Host test of dp_prepare_sink() (headers/private/graphics/common/dp_sink.h)
	against a simulated DPCD:
	g++ -std=c++17 -Wall -Werror -I headers/private/graphics/common \
		src/tests/add-ons/kernel/drivers/graphics/dp_sink/dp_sink_test.cpp \
		-o dp_sink_test && ./dp_sink_test
*/


#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <set>

#include <dp_sink.h>


struct Sink {
	std::map<unsigned, int> dpcd;
	std::set<unsigned> failing;		// addresses whose AUX read fails
	std::map<unsigned, int> written;

	int Read(unsigned address)
	{
		if (failing.count(address) != 0)
			return -1;
		auto found = dpcd.find(address);
		return found != dpcd.end() ? found->second : 0;
	}

	bool Write(unsigned address, int value)
	{
		written[address] = value;
		dpcd[address] = value;
		return true;
	}

	bool Prepare()
	{
		return dp_prepare_sink(
			[this](unsigned address) { return Read(address); },
			[this](unsigned address, int value) { return Write(address, value); });
	}
};


static Sink
hub_converter(int control)
{
	// a USB-C hub's DisplayPort to HDMI converter: DPCD 1.2, a downstream
	// port with detailed capabilities, of type HDMI
	Sink sink;
	sink.dpcd[0x000] = 0x12;
	sink.dpcd[0x005] = 0x1d;
	sink.dpcd[0x080] = 0x0b;
	sink.dpcd[0x600] = 0x01;
	sink.dpcd[0x3050] = control;
	return sink;
}


int
main()
{
	// a converter left in HDMI mode is put back to DVI; its other bits kept
	{
		Sink sink = hub_converter(0x05);
		assert(!sink.Prepare());
		assert(sink.written.size() == 1 && sink.written[0x3050] == 0x04);
	}
	// one already in DVI mode is left alone
	{
		Sink sink = hub_converter(0x00);
		assert(!sink.Prepare());
		assert(sink.written.empty());
	}
	// a sink in D3 is woken (and the caller told to wait); its other power
	// bits kept
	{
		Sink sink = hub_converter(0x00);
		sink.dpcd[0x600] = 0x02 | 0x20;
		assert(sink.Prepare());
		assert(sink.written[0x600] == (0x01 | 0x20));
	}
	// DPCD 1.0: no power state written
	{
		Sink sink = hub_converter(0x00);
		sink.dpcd[0x000] = 0x10;
		sink.dpcd[0x600] = 0x02;
		assert(!sink.Prepare());
		assert(sink.written.count(0x600) == 0);
	}
	// a plain DisplayPort monitor, a DVI or VGA adapter: only power
	{
		Sink sink = hub_converter(0x01);
		sink.dpcd[0x005] = 0x00;
		assert(!sink.Prepare());
		assert(sink.written.empty());
		sink = hub_converter(0x01);
		sink.dpcd[0x080] = 0x0a;	// DVI downstream port
		assert(!sink.Prepare());
		assert(sink.written.empty());
		sink = hub_converter(0x01);
		sink.dpcd[0x005] = 0x09;	// no detailed capability information
		assert(!sink.Prepare());
		assert(sink.written.empty());
	}
	// failed AUX reads write nothing
	{
		Sink sink = hub_converter(0x01);
		sink.failing.insert(0x000);
		assert(!sink.Prepare());
		assert(sink.written.empty());
		sink = hub_converter(0x01);
		sink.failing.insert(0x3050);
		assert(!sink.Prepare());
		assert(sink.written.empty());
	}
	printf("DP_SINK_TEST_PASS\n");
	return 0;
}
