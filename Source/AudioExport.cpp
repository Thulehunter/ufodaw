#include "AudioExport.h"

extern "C"
{
    #include "layer3.h"   // Shine (an MP3 encoder: fixed point, no floating point needed)
}

namespace AudioExport
{
    const std::vector<Choice>& choices()
    {
        static const std::vector<Choice> list = {
            { "WAV 24-bit",                     Format::Wav,  24, 0 },
            { "WAV 16-bit",                     Format::Wav,  16, 0 },
            { "WAV 32-bit float (cannot clip)", Format::Wav,  32, 0 },
            { "FLAC 24-bit (lossless)",         Format::Flac, 24, 0 },
            { "FLAC 16-bit (lossless)",         Format::Flac, 16, 0 },
            { "AIFF 24-bit",                    Format::Aiff, 24, 0 },
            { "AIFF 16-bit",                    Format::Aiff, 16, 0 },
            { "MP3 320 kbps",                   Format::Mp3,  16, 320 },
            { "MP3 256 kbps",                   Format::Mp3,  16, 256 },
            { "MP3 192 kbps",                   Format::Mp3,  16, 192 },
            { "MP3 128 kbps",                   Format::Mp3,  16, 128 },
            { "Ogg Vorbis 320 kbps",            Format::Ogg,  16, 320 },
            { "Ogg Vorbis 192 kbps",            Format::Ogg,  16, 192 },
        };
        return list;
    }

    juce::String extensionOf (Format f)
    {
        switch (f)
        {
            case Format::Wav:  return "wav";
            case Format::Flac: return "flac";
            case Format::Aiff: return "aiff";
            case Format::Ogg:  return "ogg";
            case Format::Mp3:  return "mp3";
        }
        return "wav";
    }

    bool isLossy (Format f) { return f == Format::Ogg || f == Format::Mp3; }

    //==============================================================================
    // MP3 through Shine. The mix goes in as 32-bit integers (JUCE's writer interface), is brought to a sample rate MP3 knows (44.1 /
    // 48 / 32 kHz: a project at 96 kHz is averaged down by 2, other rates are interpolated) and encoded in blocks.
    class Mp3Writer : public juce::AudioFormatWriter
    {
    public:
        Mp3Writer (juce::OutputStream* out, double sourceRate, int channels, int kbps)
            : juce::AudioFormatWriter (out, "MP3", sourceRate, (unsigned int) channels, 16), numChannelsOut (channels)
        {
            const int src = (int) std::lround (sourceRate);
            auto supported = [] (int r) { return r == 44100 || r == 48000 || r == 32000; };
            if (supported (src))
            {
                targetRate = src;
            }
            else if (src % 2 == 0 && supported (src / 2))
            {
                targetRate = src / 2;
                decimate = 2;
            }
            else if (src % 4 == 0 && supported (src / 4))
            {
                targetRate = src / 4;
                decimate = 4;
            }
            else
            {
                targetRate = 48000;   // (an unusual rate: linear interpolation)
                interpolate = true;
                step = sourceRate / 48000.0;
            }

            shine_config_t config {};
            shine_set_config_mpeg_defaults (&config.mpeg);
            config.wave.channels = channels == 1 ? PCM_MONO : PCM_STEREO;
            config.wave.samplerate = targetRate;
            config.mpeg.bitr = kbps;
            config.mpeg.mode = channels == 1 ? MONO : JOINT_STEREO;
            if (shine_check_config (targetRate, kbps) >= 0)
            {
                encoder = shine_initialise (&config);
                if (encoder != nullptr)
                    perPass = shine_samples_per_pass (encoder);
            }
            if (encoder == nullptr)
                output = nullptr;   // (failed: the caller keeps the stream, as with JUCE's own writers)
        }

        ~Mp3Writer() override
        {
            if (encoder == nullptr)
                return;
            if (! pending.empty())   // the last, partial block: padded with silence
            {
                pending.resize ((size_t) (perPass * numChannelsOut), 0);
                encodeBlock();
            }
            int written = 0;
            if (auto* data = shine_flush (encoder, &written); data != nullptr && written > 0 && output != nullptr)
                output->write (data, (size_t) written);
            shine_close (encoder);
        }

        bool isOk() const { return encoder != nullptr; }

        bool write (const int** samples, int numSamples) override
        {
            if (encoder == nullptr || output == nullptr)
                return false;
            constexpr float scale = 1.0f / 2147483648.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                float frame[2] = { 0.0f, 0.0f };
                for (int ch = 0; ch < numChannelsOut; ++ch)
                    frame[ch] = (float) samples[ch][i] * scale;

                if (decimate > 1)   // average groups of `decimate` frames
                {
                    for (int ch = 0; ch < numChannelsOut; ++ch)
                        accum[ch] += frame[ch];
                    if (++accumCount == decimate)
                    {
                        for (int ch = 0; ch < numChannelsOut; ++ch)
                        {
                            push (accum[ch] / (float) decimate);
                            accum[ch] = 0.0f;
                        }
                        accumCount = 0;
                    }
                }
                else if (interpolate)   // linear interpolation between the previous and this frame
                {
                    while (phase < 1.0)
                    {
                        for (int ch = 0; ch < numChannelsOut; ++ch)
                            push (previous[ch] + (float) phase * (frame[ch] - previous[ch]));
                        phase += step;
                    }
                    phase -= 1.0;
                    for (int ch = 0; ch < numChannelsOut; ++ch)
                        previous[ch] = frame[ch];
                }
                else
                {
                    for (int ch = 0; ch < numChannelsOut; ++ch)
                        push (frame[ch]);
                }
            }
            return true;
        }

    private:
        void push (float v)   // one interleaved sample, as 16-bit
        {
            pending.push_back ((int16_t) juce::jlimit (-32768, 32767, (int) std::lround (v * 32767.0f * shineGain)));
            if ((int) pending.size() >= perPass * numChannelsOut)
                encodeBlock();
        }

        void encodeBlock()
        {
            int written = 0;
            if (auto* data = shine_encode_buffer_interleaved (encoder, pending.data(), &written); data != nullptr && written > 0)
                output->write (data, (size_t) written);
            pending.clear();
        }

        // (Shine's output comes out 12.5 % (1 dB) louder than what goes in, measured with a reference decoder; this puts it right)
        static constexpr float shineGain = 0.8889f;

        shine_t encoder = nullptr;
        int numChannelsOut = 2, perPass = 1152, targetRate = 44100, decimate = 1, accumCount = 0;
        bool interpolate = false;
        double step = 1.0, phase = 0.0;
        float accum[2] = { 0.0f, 0.0f }, previous[2] = { 0.0f, 0.0f };
        std::vector<int16_t> pending;
    };

    std::unique_ptr<juce::AudioFormatWriter> createWriter (std::unique_ptr<juce::FileOutputStream>& stream, const Choice& c,
                                                           double sampleRate, int numChannels)
    {
        std::unique_ptr<juce::AudioFormatWriter> writer;
        switch (c.format)
        {
            case Format::Wav:
            {
                juce::WavAudioFormat f;
                writer.reset (f.createWriterFor (stream.get(), sampleRate, (unsigned int) numChannels, c.bits, {}, 0));
                break;
            }
            case Format::Aiff:
            {
                juce::AiffAudioFormat f;
                writer.reset (f.createWriterFor (stream.get(), sampleRate, (unsigned int) numChannels, c.bits, {}, 0));
                break;
            }
            case Format::Flac:
            {
                juce::FlacAudioFormat f;
                writer.reset (f.createWriterFor (stream.get(), sampleRate, (unsigned int) numChannels, c.bits, {}, 0));
                break;
            }
            case Format::Ogg:
            {
                juce::OggVorbisAudioFormat f;
                int option = 0, best = 1 << 30;   // the quality step nearest to the kbps asked for
                const auto options = f.getQualityOptions();
                for (int i = 0; i < options.size(); ++i)
                    if (const int diff = std::abs (options[i].getIntValue() - c.quality); diff < best)
                    {
                        best = diff;
                        option = i;
                    }
                writer.reset (f.createWriterFor (stream.get(), sampleRate, (unsigned int) numChannels, 16, {}, option));
                break;
            }
            case Format::Mp3:
            {
                auto mp3 = std::make_unique<Mp3Writer> (stream.get(), sampleRate, numChannels, c.quality);
                if (mp3->isOk())
                    writer = std::move (mp3);
                break;
            }
        }
        if (writer != nullptr)
            stream.release();   // the writer owns the stream now
        return writer;
    }
}
