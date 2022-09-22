#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>

#include <wrl.h>

#include <dxgi1_6.h>
#include <d3d11.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>

#define THROW_IF_FAIL(value) \
{ \
    if(FAILED(value)) \
    { \
        throw std::exception("Operation failed!"); \
    } \
}

template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

using namespace std::chrono_literals;

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<ID3D11Texture2D> colorBuffer;
ComPtr<ID3D11RenderTargetView> colorView;

ComPtr<IMFDXGIDeviceManager> deviceManager;
ComPtr<IMFMediaBuffer> mediaBuffer;
ComPtr<IMFSinkWriter> mp4Writer;
UINT deviceResetToken = 0;
DWORD outputStream = 0xFF;

auto frameStart = std::chrono::high_resolution_clock::now();
LONGLONG frameCounter = 0;

constexpr FLOAT yellow[4] = { 1.f, 1.f, 0.f, 0.f };
constexpr FLOAT cyan[4] = { 0.f, 1.f, 1.f, 0.f };
constexpr FLOAT magenta[4] = { 1.f, 0.f, 1.f, 0.f };
constexpr std::array clearColors = { yellow, cyan, magenta };

constexpr UINT width = 640;
constexpr UINT height = 480;
constexpr UINT fps = 60;
constexpr LONGLONG sampleDuration = 10000000 / fps;
constexpr UINT bitrateInKbps = 4096000; // 4 mbit/s
constexpr UINT framesUntilColorSwitch = fps * 1;
constexpr std::chrono::duration videoLength = 6s;

constexpr const wchar_t* outputFileName = L"D:\\testvideo.mp4";

bool initialized = false;

void initD3D()
{
	THROW_IF_FAIL(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));

	D3D11_TEXTURE2D_DESC colorDesc = {};
	colorDesc.ArraySize = 1;
	colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	colorDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Using BGRA since the encoder seems to expect BGR
	colorDesc.Width = width;
	colorDesc.Height = height;
	colorDesc.MipLevels = 1;
	colorDesc.SampleDesc = { 1, 0 };
	colorDesc.Usage = D3D11_USAGE_DEFAULT;

	THROW_IF_FAIL(device->CreateTexture2D(&colorDesc, nullptr, &colorBuffer));

	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	rtvDesc.Format = colorDesc.Format;

	THROW_IF_FAIL(device->CreateRenderTargetView(colorBuffer.Get(), &rtvDesc, &colorView));
}

void initMediaFoundation()
{
	THROW_IF_FAIL(MFStartup(MF_API_VERSION));

	THROW_IF_FAIL(MFCreateDXGIDeviceManager(&deviceResetToken, &deviceManager));
	THROW_IF_FAIL(deviceManager->ResetDevice(device.Get(), deviceResetToken));

	ComPtr<IMFMediaType> inputType;
	THROW_IF_FAIL(MFCreateMediaType(&inputType));
	THROW_IF_FAIL(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAIL(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)); // looks like this is actually BGR32
	THROW_IF_FAIL(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height));

	ComPtr<IMFMediaType> outputType;
	THROW_IF_FAIL(MFCreateMediaType(&outputType));
	THROW_IF_FAIL(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	THROW_IF_FAIL(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
	THROW_IF_FAIL(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height));
	THROW_IF_FAIL(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1));
	THROW_IF_FAIL(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
	THROW_IF_FAIL(outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrateInKbps));
	THROW_IF_FAIL(outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base));

	THROW_IF_FAIL(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), colorBuffer.Get(), 0, FALSE, &mediaBuffer));

	DWORD bufferLength = 0;
	ComPtr<IMF2DBuffer> mediaBuffer2;
	THROW_IF_FAIL(mediaBuffer.As(&mediaBuffer2));
	THROW_IF_FAIL(mediaBuffer2->GetContiguousLength(&bufferLength));
	THROW_IF_FAIL(mediaBuffer->SetCurrentLength(bufferLength));

	ComPtr<IMFAttributes> attributes;
	THROW_IF_FAIL(MFCreateAttributes(&attributes, 3));
	THROW_IF_FAIL(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
	THROW_IF_FAIL(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, deviceManager.Get()));
	THROW_IF_FAIL(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1));
	THROW_IF_FAIL(MFCreateSinkWriterFromURL(outputFileName, nullptr, attributes.Get(), &mp4Writer));

	THROW_IF_FAIL(mp4Writer->AddStream(outputType.Get(), &outputStream));
	THROW_IF_FAIL(mp4Writer->SetInputMediaType(outputStream, inputType.Get(), nullptr));

	THROW_IF_FAIL(mp4Writer->BeginWriting());
}

void init()
{
	initD3D();
	initMediaFoundation();
	initialized = true;
}

// Manually throttling the render-encoding loop to the video frame rate
void frameBegin()
{
	while (std::chrono::high_resolution_clock::now() - frameStart < std::chrono::nanoseconds(sampleDuration * 100))
	{
		std::this_thread::yield();
	}
	frameStart = std::chrono::high_resolution_clock::now();
}

void renderD3D()
{
	if (!initialized)
		return;

	context->ClearRenderTargetView(colorView.Get(), clearColors[(frameCounter / framesUntilColorSwitch) % clearColors.size()]);
}

void encodeD3D()
{
	if (!initialized)
		return;

	ComPtr<IMFSample> inputSample;
	THROW_IF_FAIL(MFCreateSample(&inputSample));
	THROW_IF_FAIL(inputSample->AddBuffer(mediaBuffer.Get()));
	THROW_IF_FAIL(inputSample->SetSampleDuration(sampleDuration));
	THROW_IF_FAIL(inputSample->SetSampleTime(frameCounter * sampleDuration));
	THROW_IF_FAIL(mp4Writer->WriteSample(outputStream, inputSample.Get()));

	++frameCounter;
}

void shutdown()
{
	THROW_IF_FAIL(mp4Writer->Finalize());

	mp4Writer = nullptr;
	mediaBuffer = nullptr;
	deviceManager = nullptr;

	MFShutdown();
}

int main(int argc, const char** argv)
{
	// encoding a five second sample vid
	const auto programStart = std::chrono::high_resolution_clock::now();

	init();

	// Main message loop:
	while (std::chrono::high_resolution_clock::now() - programStart < videoLength)
	{
		frameBegin();
		renderD3D();
		encodeD3D();
	}

	shutdown();

	return 0;
}