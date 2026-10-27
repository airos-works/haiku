/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	SBC encoder, written from the A2DP 1.3 specification, appendix B
	("Technical Specification of SBC"): the analysis filter bank of B.2.2
	(figure 12.4), the scale factors and bit allocation of B.3, and the
	frame syntax of B.4. Section and table numbers below refer to it.
*/


#include <SbcEncoder.h>

#include <math.h>
#include <string.h>


namespace Bluetooth {


// Analysis window C[i] for 4 and 8 subbands (tables 12.23 and 12.24): the
// prototype filter, with every other group of 2M coefficients negated.
static const float kWindow4[40] = {
	0.00000000E+00f, 5.36548976E-04f, 1.49188357E-03f, 2.73370904E-03f,
	3.83720193E-03f, 3.89205149E-03f, 1.86581691E-03f, -3.06012286E-03f,
	1.09137620E-02f, 2.04385087E-02f, 2.88757392E-02f, 3.21939290E-02f,
	2.58767811E-02f, 6.13245186E-03f, -2.88217274E-02f, -7.76463494E-02f,
	1.35593274E-01f, 1.94987841E-01f, 2.46636662E-01f, 2.81828203E-01f,
	2.94315332E-01f, 2.81828203E-01f, 2.46636662E-01f, 1.94987841E-01f,
	-1.35593274E-01f, -7.76463494E-02f, -2.88217274E-02f, 6.13245186E-03f,
	2.58767811E-02f, 3.21939290E-02f, 2.88757392E-02f, 2.04385087E-02f,
	-1.09137620E-02f, -3.06012286E-03f, 1.86581691E-03f, 3.89205149E-03f,
	3.83720193E-03f, 2.73370904E-03f, 1.49188357E-03f, 5.36548976E-04f
};

static const float kWindow8[80] = {
	0.00000000E+00f, 1.56575398E-04f, 3.43256425E-04f, 5.54620202E-04f,
	8.23919506E-04f, 1.13992507E-03f, 1.47640169E-03f, 1.78371725E-03f,
	2.01182542E-03f, 2.10371989E-03f, 1.99454554E-03f, 1.61656283E-03f,
	9.02154502E-04f, -1.78805361E-04f, -1.64973098E-03f, -3.49717454E-03f,
	5.65949473E-03f, 8.02941163E-03f, 1.04584443E-02f, 1.27472335E-02f,
	1.46525263E-02f, 1.59045603E-02f, 1.62208471E-02f, 1.53184106E-02f,
	1.29371806E-02f, 8.85757540E-03f, 2.92408442E-03f, -4.91578024E-03f,
	-1.46404076E-02f, -2.61098752E-02f, -3.90751381E-02f, -5.31873032E-02f,
	6.79989431E-02f, 8.29847578E-02f, 9.75753918E-02f, 1.11196689E-01f,
	1.23264548E-01f, 1.33264415E-01f, 1.40753505E-01f, 1.45389847E-01f,
	1.46955068E-01f, 1.45389847E-01f, 1.40753505E-01f, 1.33264415E-01f,
	1.23264548E-01f, 1.11196689E-01f, 9.75753918E-02f, 8.29847578E-02f,
	-6.79989431E-02f, -5.31873032E-02f, -3.90751381E-02f, -2.61098752E-02f,
	-1.46404076E-02f, -4.91578024E-03f, 2.92408442E-03f, 8.85757540E-03f,
	1.29371806E-02f, 1.53184106E-02f, 1.62208471E-02f, 1.59045603E-02f,
	1.46525263E-02f, 1.27472335E-02f, 1.04584443E-02f, 8.02941163E-03f,
	-5.65949473E-03f, -3.49717454E-03f, -1.64973098E-03f, -1.78805361E-04f,
	9.02154502E-04f, 1.61656283E-03f, 1.99454554E-03f, 2.10371989E-03f,
	2.01182542E-03f, 1.78371725E-03f, 1.47640169E-03f, 1.13992507E-03f,
	8.23919506E-04f, 5.54620202E-04f, 3.43256425E-04f, 1.56575398E-04f
};

// Loudness offsets per sampling frequency (16, 32, 44.1, 48 kHz) and
// subband (tables 12.16 and 12.17).
static const int32 kOffset4[4][4] = {
	{ -1, 0, 0, 0 },
	{ -2, 0, 0, 1 },
	{ -2, 0, 0, 1 },
	{ -2, 0, 0, 1 }
};

static const int32 kOffset8[4][8] = {
	{ -2, 0, 0, 0, 0, 0, 0, 1 },
	{ -3, 0, 0, 0, 0, 0, 1, 2 },
	{ -4, 0, 0, 0, 0, 0, 1, 2 },
	{ -4, 0, 0, 0, 0, 0, 1, 2 }
};


size_t
SbcConfiguration::FrameSize() const
{
	// A2DP 1.3 section 12.9: header, scale factors, then the samples.
	const uint32 channels = Channels();
	size_t size = 4 + (4 * subbands * channels) / 8;
	uint32 bits;
	if (channelMode == SBC_MODE_MONO || channelMode == SBC_MODE_DUAL_CHANNEL)
		bits = blocks * channels * bitpool;
	else if (channelMode == SBC_MODE_STEREO)
		bits = blocks * bitpool;
	else
		bits = subbands + blocks * bitpool;
	return size + (bits + 7) / 8;
}


uint32
SbcConfiguration::BitRate() const
{
	return 8 * FrameSize() * sampleRate / FrameSamples();
}


namespace {

class BitWriter {
public:
	BitWriter(uint8* data)
		:
		fData(data),
		fBits(0)
	{
	}

	void Put(uint32 value, uint32 count)
	{
		for (int32 bit = count - 1; bit >= 0; bit--) {
			if ((fBits & 7) == 0)
				fData[fBits >> 3] = 0;
			if ((value >> bit) & 1)
				fData[fBits >> 3] |= 0x80 >> (fBits & 7);
			fBits++;
		}
	}

	size_t Bits() const { return fBits; }

private:
	uint8*	fData;
	size_t	fBits;
};


/*!	CRC-8 of section B.4.2.3: polynomial x^8 + x^4 + x^3 + x^2 + 1, initial
	value 0x0f, over the bits given, most significant first.
*/
class Crc8 {
public:
	Crc8() : fValue(0x0f) {}

	void Put(uint32 value, uint32 count)
	{
		for (int32 bit = count - 1; bit >= 0; bit--) {
			const uint32 top = ((fValue >> 7) ^ (value >> bit)) & 1;
			fValue = (fValue << 1) & 0xff;
			if (top != 0)
				fValue ^= 0x1d;
		}
	}

	uint8 Value() const { return fValue; }

private:
	uint32	fValue;
};


/*!	Bit allocation of section B.3.3.2 for one set of scale factors, in the
	order in which the remaining bits are handed out: subband by subband,
	and within a subband the channels sharing the bitpool.
*/
void
distribute(int32* bits, const int32* bitneed, int32 count, int32 bitpool)
{
	int32 maxBitneed = 0;
	for (int32 i = 0; i < count; i++) {
		if (bitneed[i] > maxBitneed)
			maxBitneed = bitneed[i];
	}

	int32 bitcount = 0;
	int32 slicecount = 0;
	int32 bitslice = maxBitneed + 1;
	do {
		bitslice--;
		bitcount += slicecount;
		slicecount = 0;
		for (int32 i = 0; i < count; i++) {
			if (bitneed[i] > bitslice + 1 && bitneed[i] < bitslice + 16)
				slicecount++;
			else if (bitneed[i] == bitslice + 1)
				slicecount += 2;
		}
	} while (bitcount + slicecount < bitpool && bitslice > -64);

	if (bitcount + slicecount == bitpool) {
		bitcount += slicecount;
		bitslice--;
	}

	for (int32 i = 0; i < count; i++) {
		if (bitneed[i] < bitslice + 2)
			bits[i] = 0;
		else
			bits[i] = min_c(bitneed[i] - bitslice, (int32)16);
	}

	for (int32 i = 0; bitcount < bitpool && i < count; i++) {
		if (bits[i] >= 2 && bits[i] < 16) {
			bits[i]++;
			bitcount++;
		} else if (bitneed[i] == bitslice + 1 && bitpool > bitcount + 1) {
			bits[i] = 2;
			bitcount += 2;
		}
	}

	for (int32 i = 0; bitcount < bitpool && i < count; i++) {
		if (bits[i] < 16) {
			bits[i]++;
			bitcount++;
		}
	}
}


/*!	Scale factor of section B.3.2: the smallest SF with every sample below
	2^(SF + 1).
*/
int32
scale_factor(float maximum)
{
	int32 factor = 0;
	while (factor < 15 && maximum >= (float)(2 << factor))
		factor++;
	return factor;
}

} // namespace


SbcEncoder::SbcEncoder()
{
	SbcConfiguration config = {};
	config.sampleRate = 44100;
	config.channelMode = SBC_MODE_JOINT_STEREO;
	config.blocks = 16;
	config.subbands = 8;
	config.allocation = SBC_ALLOCATION_LOUDNESS;
	config.bitpool = 53;
	SetTo(config);
}


status_t
SbcEncoder::SetTo(const SbcConfiguration& config)
{
	uint8 frequency;
	switch (config.sampleRate) {
		case 16000:
			frequency = 0;
			break;
		case 32000:
			frequency = 1;
			break;
		case 44100:
			frequency = 2;
			break;
		case 48000:
			frequency = 3;
			break;
		default:
			return B_BAD_VALUE;
	}
	if ((config.blocks != 4 && config.blocks != 8 && config.blocks != 12
			&& config.blocks != 16)
		|| (config.subbands != 4 && config.subbands != 8))
		return B_BAD_VALUE;

	uint8 mode;
	switch (config.channelMode) {
		case SBC_MODE_MONO:
			mode = 0;
			break;
		case SBC_MODE_DUAL_CHANNEL:
			mode = 1;
			break;
		case SBC_MODE_STEREO:
			mode = 2;
			break;
		case SBC_MODE_JOINT_STEREO:
			mode = 3;
			break;
		default:
			return B_BAD_VALUE;
	}

	const uint32 channels = config.Channels();
	const uint32 bitpoolLimit = (channels == 1
		|| config.channelMode == SBC_MODE_DUAL_CHANNEL ? 16 : 32)
			* config.subbands;
	if (config.bitpool < 2 || config.bitpool > bitpoolLimit)
		return B_BAD_VALUE;

	fSampleRate = config.sampleRate;
	fChannelMode = config.channelMode;
	fChannels = channels;
	fBlocks = config.blocks;
	fSubbands = config.subbands;
	fAllocation = config.allocation;
	fBitpool = config.bitpool;
	fFrameSize = config.FrameSize();

	// Section B.4.2: sampling frequency, blocks, channel mode, allocation
	// method, subbands; then the bitpool.
	fHeader[0] = (frequency << 6) | ((fBlocks / 4 - 1) << 4) | (mode << 2)
		| (fAllocation == SBC_ALLOCATION_SNR ? 2 : 0)
		| (fSubbands == 8 ? 1 : 0);
	fHeader[1] = fBitpool;

	// Matrixing of figure 12.4: M[i][k] = cos((i + 0.5) (k - M/2) pi / M).
	for (uint32 i = 0; i < fSubbands; i++) {
		for (uint32 k = 0; k < 2 * fSubbands; k++) {
			fMatrix[i][k] = cos((i + 0.5) * ((int32)k - (int32)fSubbands / 2)
				* M_PI / fSubbands);
		}
	}

	Reset();
	return B_OK;
}


void
SbcEncoder::Reset()
{
	memset(fHistory, 0, sizeof(fHistory));
	fPosition = 10 * fSubbands;
}


/*!	Analysis filter bank of figure 12.4, block by block: shift M new
	samples into X (the newest at X[0]), window it, sum the five partial
	windows and matrix them into M subband samples.
*/
void
SbcEncoder::_Analyse(const int16* samples)
{
	const uint32 M = fSubbands;
	const float* window = M == 8 ? kWindow8 : kWindow4;

	for (uint32 block = 0; block < fBlocks; block++) {
		if (fPosition < M) {
			// Keep the newest 9 M samples, at the top of the buffer.
			for (uint32 channel = 0; channel < fChannels; channel++) {
				memmove(&fHistory[channel][11 * M],
					&fHistory[channel][fPosition], 9 * M * sizeof(float));
			}
			fPosition = 11 * M;
		}
		fPosition -= M;

		for (uint32 channel = 0; channel < fChannels; channel++) {
			float* x = &fHistory[channel][fPosition];
			const int16* input = samples + block * M * fChannels + channel;
			for (uint32 i = 0; i < M; i++)
				x[M - 1 - i] = input[i * fChannels];

			float y[16];
			for (uint32 k = 0; k < 2 * M; k++) {
				float sum = 0;
				for (uint32 j = 0; j < 5; j++)
					sum += window[k + 2 * M * j] * x[k + 2 * M * j];
				y[k] = sum;
			}

			for (uint32 i = 0; i < M; i++) {
				float sum = 0;
				for (uint32 k = 0; k < 2 * M; k++)
					sum += fMatrix[i][k] * y[k];
				fSamples[block][channel][i] = sum;
			}
		}
	}
}


void
SbcEncoder::_AllocateBits(int32 bits[2][8], const int32 scaleFactors[2][8])
{
	const uint32 frequency = fHeader[0] >> 6;
	const int32* offsets = fSubbands == 8 ? kOffset8[frequency]
		: kOffset4[frequency];

	// Section B.3.3.1: what each subband would need.
	int32 bitneed[2][8];
	for (uint32 channel = 0; channel < fChannels; channel++) {
		for (uint32 subband = 0; subband < fSubbands; subband++) {
			const int32 factor = scaleFactors[channel][subband];
			int32& need = bitneed[channel][subband];
			if (fAllocation == SBC_ALLOCATION_SNR)
				need = factor;
			else if (factor == 0)
				need = -5;
			else {
				const int32 loudness = factor - offsets[subband];
				need = loudness > 0 ? loudness / 2 : loudness;
			}
		}
	}

	if (fChannelMode == SBC_MODE_MONO
		|| fChannelMode == SBC_MODE_DUAL_CHANNEL) {
		// Each channel has a bitpool of its own.
		for (uint32 channel = 0; channel < fChannels; channel++)
			distribute(bits[channel], bitneed[channel], fSubbands, fBitpool);
		return;
	}

	// Stereo: both channels share one bitpool, interleaved per subband.
	int32 need[16] = {};
	int32 allocated[16];
	for (uint32 subband = 0; subband < fSubbands; subband++) {
		need[2 * subband] = bitneed[0][subband];
		need[2 * subband + 1] = bitneed[1][subband];
	}
	distribute(allocated, need, 2 * fSubbands, fBitpool);
	for (uint32 subband = 0; subband < fSubbands; subband++) {
		bits[0][subband] = allocated[2 * subband];
		bits[1][subband] = allocated[2 * subband + 1];
	}
}


size_t
SbcEncoder::Encode(const int16* samples, uint8* frame)
{
	_Analyse(samples);

	int32 scaleFactors[2][8];
	for (uint32 channel = 0; channel < fChannels; channel++) {
		for (uint32 subband = 0; subband < fSubbands; subband++) {
			float maximum = 0;
			for (uint32 block = 0; block < fBlocks; block++) {
				const float value = fabsf(fSamples[block][channel][subband]);
				if (value > maximum)
					maximum = value;
			}
			scaleFactors[channel][subband] = scale_factor(maximum);
		}
	}

	// Joint stereo (section B.3.4): code a subband as mid and side when
	// that needs smaller scale factors. The last subband never is.
	uint32 join = 0;
	if (fChannelMode == SBC_MODE_JOINT_STEREO) {
		for (uint32 subband = 0; subband + 1 < fSubbands; subband++) {
			float maxMid = 0;
			float maxSide = 0;
			for (uint32 block = 0; block < fBlocks; block++) {
				const float left = fSamples[block][0][subband];
				const float right = fSamples[block][1][subband];
				maxMid = max_c(maxMid, fabsf((left + right) / 2));
				maxSide = max_c(maxSide, fabsf((left - right) / 2));
			}
			const int32 mid = scale_factor(maxMid);
			const int32 side = scale_factor(maxSide);
			if (mid + side >= scaleFactors[0][subband]
					+ scaleFactors[1][subband])
				continue;

			join |= 1 << (fSubbands - 1 - subband);
			scaleFactors[0][subband] = mid;
			scaleFactors[1][subband] = side;
			for (uint32 block = 0; block < fBlocks; block++) {
				const float left = fSamples[block][0][subband];
				const float right = fSamples[block][1][subband];
				fSamples[block][0][subband] = (left + right) / 2;
				fSamples[block][1][subband] = (left - right) / 2;
			}
		}
	}

	int32 bits[2][8];
	_AllocateBits(bits, scaleFactors);

	// Section B.4: header, CRC, join flags, scale factors, samples.
	BitWriter writer(frame);
	Crc8 crc;
	writer.Put(0x9c, 8);
	writer.Put(fHeader[0], 8);
	writer.Put(fHeader[1], 8);
	writer.Put(0, 8);
		// the CRC, filled in below
	crc.Put(fHeader[0], 8);
	crc.Put(fHeader[1], 8);
	if (fChannelMode == SBC_MODE_JOINT_STEREO) {
		writer.Put(join, fSubbands);
		crc.Put(join, fSubbands);
	}
	for (uint32 channel = 0; channel < fChannels; channel++) {
		for (uint32 subband = 0; subband < fSubbands; subband++) {
			writer.Put(scaleFactors[channel][subband], 4);
			crc.Put(scaleFactors[channel][subband], 4);
		}
	}
	frame[3] = crc.Value();

	// Quantization of section B.3.5.
	for (uint32 block = 0; block < fBlocks; block++) {
		for (uint32 channel = 0; channel < fChannels; channel++) {
			for (uint32 subband = 0; subband < fSubbands; subband++) {
				const int32 count = bits[channel][subband];
				if (count == 0)
					continue;
				const int32 levels = (1 << count) - 1;
				const float factor
					= (float)(2 << scaleFactors[channel][subband]);
				int32 quantized = (int32)((fSamples[block][channel][subband]
					/ factor + 1.0f) * levels / 2.0f);
				if (quantized < 0)
					quantized = 0;
				else if (quantized > levels)
					quantized = levels;
				writer.Put(quantized, count);
			}
		}
	}

	// Padding to the frame size.
	while (writer.Bits() < fFrameSize * 8)
		writer.Put(0, 1);
	return fFrameSize;
}


} // namespace Bluetooth
