#include <iostream>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>
#include <mmeapi.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Roapi.h>
#include <endpointvolume.h>
#include <ksmedia.h>
#include "whisper.h"
using namespace std;
#define REFTIMES_PER_SEC 10000000

#define RELEASE(pointer)                \
    if(pointer != NULL)                 \
        pointer->Release();             


class BufferSamples {
    int sampleState;
public:

    BufferSamples() : sampleState{ 0 } {}

    std::vector<float> buf;
    void push_samples(const float* samples, int sampleCount, bool dualChannel = true) {
        for (int i = 0; i < sampleCount; i++) {
            if (!sampleState) buf.push_back(*samples);
            sampleState = (sampleState + 1) % 3;
            if (dualChannel) samples += 2;
            else samples += 1;
        }
    }
}samples;


class WavFile {
    std::filesystem::path filename;
    std::ofstream file;

    UINT32 channels;
    UINT32 sampleSize;

    std::ofstream::pos_type sizePos;
    std::ofstream::pos_type factChunkSampleLengthPos;
    std::ofstream::pos_type dataSizePos;

public:
    WavFile(const char* filename) : filename(filename) {
        file = std::ofstream(this->filename.c_str(), std::ios_base::out | std::ios_base::binary);

        file.write("RIFF", 4);
        sizePos = file.tellp();
        char size[] = { 0x48, 0x00, 0x00, 0x00 };
        file.write(size, 4);

        file.write("WAVE", 4);
        file.write("fmt ", 4);
        char sizeFmtChunk[] = { 0x28, 0x00, 0x00, 0x00 };
        file.write(sizeFmtChunk, 4);

    }

    void setChannels(UINT32 inChannels) {
        this->channels = inChannels;
    }

    void setSampleSize(UINT32 inSampleSize) {
        this->sampleSize = inSampleSize;
    }

    void writeStruct(const WAVEFORMATEXTENSIBLE* waveFormatExtensible) {

        file.write((char*)waveFormatExtensible, sizeof(WAVEFORMATEXTENSIBLE));
        //writing the waveformatextensible file https://www.mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/WAVE.html

        file.write("fact", 4);
        char sizeFactChunk[] = { 0x4, 0, 0, 0 };
        file.write(sizeFactChunk, 4);
        char factChunkSampleLength[] = { 0, 0, 0, 0 };
        factChunkSampleLengthPos = file.tellp();
        file.write(factChunkSampleLength, 4);

        file.write("data", 4);
        char dataSize[] = { 0,0, 0, 0 };
        dataSizePos = file.tellp();
        file.write(dataSize, 4);
    }

    void writeData(BYTE* data, UINT32 dataSize) {
        file.write((const char*)data, dataSize);
    }

    void endData(UINT32 numOfSamples) {

        file.seekp(sizePos);
        UINT32 totalSize = 72 + numOfSamples * channels * sampleSize;
        file.write((const char*)&totalSize, 4);

        file.seekp(factChunkSampleLengthPos);
        UINT32 sampleLength = channels * numOfSamples;
        file.write((const char*)&sampleLength, 4);

        file.seekp(dataSizePos);
        UINT32 sampledDataSize = totalSize - 72;
        file.write((const char*)&sampledDataSize, 4);

    }

    ~WavFile() {
        file.close();
    }
};

#define EXIT_ERROR(hr)                  \
    if(FAILED(hr)){                     \
        std::cout<<std::hex<<hr<<'\n';  \
        goto EXIT;                      \
    }                                   


class AudioCap :public WavFile, BufferSamples {

    const CLSID local_CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
    //interface ID
    const IID local_IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

    //enumerator interface object
    IMMDeviceEnumerator* pEnumerator = NULL;

    //device collection
    IMMDevice* device = NULL;

    //property structures
    IPropertyStore* properties = NULL;
    PROPVARIANT pv;

    IAudioClient* audioClient = NULL;
    WAVEFORMATEX* waveFormat = NULL;

    IAudioCaptureClient* audioCaptureClient = NULL;
    IID  local_IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

    IID local_IID_IAudioClient = __uuidof(IAudioClient);


    UINT32 numOfBufferFrames;
    REFERENCE_TIME actualDuration;
    bool done = FALSE;
    UINT32 framesRead = 0;

    //misc variables
    UINT32 bytesPerSample = 0;
    UINT32 channels = 0;
    UINT32 samplesPerSec = 0;
    IAudioEndpointVolume* endpointvol;
    float vol;
    float newvol = 1.0;
    HRESULT hr;
    vector<float>buffer_txt;

public:

    AudioCap() : WavFile("recorded.wav") {

        struct whisper_context* ctx;
        WHISPER_API struct whisper_full_params wparams;
        int n_segments;

        hr = Windows::Foundation::Initialize(RO_INIT_MULTITHREADED);
        EXIT_ERROR(hr)

            hr = CoCreateInstance(local_CLSID_MMDeviceEnumerator,
                NULL, CLSCTX_ALL, local_IID_IMMDeviceEnumerator, (void**)&pEnumerator);
        EXIT_ERROR(hr)

            hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
        EXIT_ERROR(hr)

            hr = device->OpenPropertyStore(STGM_READ, &properties);
        EXIT_ERROR(hr)

            hr = properties->GetValue(PKEY_Device_FriendlyName, &pv);
        EXIT_ERROR(hr);

        if (pv.vt == VT_EMPTY)
            return;
        else {
            std::printf("End point device used : %ls\n", pv.pwszVal);
        }

        device->Activate(local_IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&audioClient);

        hr = audioClient->GetMixFormat(&waveFormat);
        EXIT_ERROR(hr)

            if (waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {

                std::cout << "Got Extensible data structure for wave format\n";
                PWAVEFORMATEXTENSIBLE waveFormatExtensible = (PWAVEFORMATEXTENSIBLE)waveFormat;

                if (waveFormatExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {

                    std::cout << "The subtype for Extensible structure is WAVE_FORMAT_FLOAT\n";
                    std::printf("The valid bits per sample are: %d\n", waveFormatExtensible->Samples.wValidBitsPerSample);


                    bytesPerSample = waveFormatExtensible->Format.wBitsPerSample / 8;
                    channels = waveFormatExtensible->Format.nChannels;
                    samplesPerSec = waveFormatExtensible->Format.nSamplesPerSec;
                }

                else {
                    EXIT_ERROR(hr)
                }
            }

            else {
                std::cout << "Default end point shared mode format is unsupported.  wFormatTag = " << waveFormat->wFormatTag << '\n';
                EXIT_ERROR(hr)
            }
        writeStruct((WAVEFORMATEXTENSIBLE*)waveFormat);
        setChannels(channels);
        setSampleSize(bytesPerSample);

        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
            REFTIMES_PER_SEC, 0, waveFormat, NULL);
        EXIT_ERROR(hr)

            hr = audioClient->GetBufferSize(&numOfBufferFrames);
        EXIT_ERROR(hr)

            hr = audioClient->GetService(local_IID_IAudioCaptureClient, (void**)&audioCaptureClient);
        EXIT_ERROR(hr)

            actualDuration = (REFERENCE_TIME)((double)REFTIMES_PER_SEC * numOfBufferFrames / waveFormat->nSamplesPerSec);
        hr = device->Activate(__uuidof(IAudioEndpointVolume),
            CLSCTX_ALL, NULL, (void**)&endpointvol);

        hr = endpointvol->GetMasterVolumeLevelScalar(&vol);
        std::cout << "master volume in float: " << vol;

        hr = endpointvol->SetMasterVolumeLevelScalar(1.0, NULL);
        EXIT_ERROR(hr)

            hr = endpointvol->GetMasterVolumeLevelScalar(&vol);
        std::cout << endl << "master volume in float(after udpate): " << vol;

        hr = audioClient->Start();
        EXIT_ERROR(hr)

            while (done == FALSE) {
                Sleep((DWORD)actualDuration / 100000 / 2);

                UINT32 packetLength;
                PBYTE packet;
                UINT32 numFramesAvailable;
                DWORD flags;

                hr = audioCaptureClient->GetNextPacketSize(&packetLength);
                EXIT_ERROR(hr)

                    while (packetLength != 0) {

                        if (framesRead > 10 * samplesPerSec) {
                            done = TRUE;
                        }

                        hr = audioCaptureClient->GetBuffer(&packet, &numFramesAvailable, &flags, NULL, NULL);
                        EXIT_ERROR(hr)


                            writeData(packet, numFramesAvailable * channels * bytesPerSample);
                        framesRead += numFramesAvailable;
                        samples.push_samples((const float*)packet, numFramesAvailable);

                        hr = audioCaptureClient->ReleaseBuffer(numFramesAvailable);
                        EXIT_ERROR(hr)

                            hr = audioCaptureClient->GetNextPacketSize(&packetLength);
                        EXIT_ERROR(hr)


                    }
            }

        endData(framesRead);

    EXIT:
        RELEASE(properties)
            RELEASE(device)
            RELEASE(pEnumerator)
            RELEASE(audioClient)
            RELEASE(audioCaptureClient)
            CoTaskMemFree(waveFormat);
        Windows::Foundation::Uninitialize();

    }

}audiOBJ;
int main() {


    struct whisper_context* ctx = whisper_init_from_file("whisper_medium/ggml-base.en.bin");
 
    if (whisper_full(ctx, whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH), samples.buf.data(), samples.buf.size()) != 0) {
        fprintf(stderr, "Full Whisper processing failed\n");
        return 7;
    }

    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text(ctx, i);
        printf("%s", text);
    }
    whisper_free(ctx);

    return 0;
}