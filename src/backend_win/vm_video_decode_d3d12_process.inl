/* SPDX-License-Identifier: MIT - persistent GPU-only decode/presentation bridge. */

/* The process queue is part of VmD3D12Decoder's lifetime. Keeping the
 * processor, queue, allocator, command list and fence alive is important:
 * creating them in the frame path turns hardware decode into a serialized
 * object-creation benchmark. */
struct VmD3D12ProcessPipeline {
    ComPtr<ID3D12VideoProcessor> processor;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12VideoProcessCommandList> list;
    HANDLE completion_event = nullptr;
};

static void destroy_process_pipeline(VmD3D12ProcessPipeline *pipeline)
{
    if (pipeline && pipeline->completion_event) {
        CloseHandle(pipeline->completion_event);
        pipeline->completion_event = nullptr;
    }
}

static bool create_process_pipeline(ID3D12Device *device,
                                    ID3D12VideoDevice *video,
                                    VmD3D12ProcessPipeline *pipeline)
{
    if (!device || !video || !pipeline)
        return false;

    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC input_desc = {};
    input_desc.Format = DXGI_FORMAT_AYUV;
    input_desc.ColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    input_desc.SourceAspectRatio = {1, 1};
    input_desc.DestinationAspectRatio = {1, 1};
    input_desc.FrameRate = {60, 1};
    input_desc.SourceSizeRange = {3840u, 2160u, 3840u, 2160u};
    input_desc.DestinationSizeRange = {3840u, 2160u, 3840u, 2160u};
    input_desc.StereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    input_desc.FieldType = D3D12_VIDEO_FIELD_TYPE_NONE;
    input_desc.DeinterlaceMode = D3D12_VIDEO_PROCESS_DEINTERLACE_FLAG_NONE;

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC output_desc = {};
    output_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    output_desc.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    output_desc.AlphaFillMode = D3D12_VIDEO_PROCESS_ALPHA_FILL_MODE_OPAQUE;
    output_desc.FrameRate = {60, 1};

    D3D12_FEATURE_DATA_VIDEO_PROCESS_SUPPORT support = {};
    support.NodeIndex = 0;
    support.InputSample.Width = 3840u;
    support.InputSample.Height = 2160u;
    support.InputSample.Format.Format = input_desc.Format;
    support.InputSample.Format.ColorSpace = input_desc.ColorSpace;
    support.InputFieldType = input_desc.FieldType;
    support.InputStereoFormat = input_desc.StereoFormat;
    support.InputFrameRate = input_desc.FrameRate;
    support.OutputFormat.Format = output_desc.Format;
    support.OutputFormat.ColorSpace = output_desc.ColorSpace;
    support.OutputStereoFormat = D3D12_VIDEO_FRAME_STEREO_FORMAT_NONE;
    support.OutputFrameRate = output_desc.FrameRate;
    HRESULT hr = video->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_PROCESS_SUPPORT, &support, sizeof(support));
    if (FAILED(hr) ||
        !(support.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED)) {
        if (FAILED(hr)) print_hr("d3d12-ayuv-to-rgb-support", hr);
        else std::fputs("BLOCKED stage=d3d12-ayuv-to-rgb reason=unsupported\n",
                        stderr);
        return false;
    }

    hr = video->CreateVideoProcessor(0, &output_desc, 1, &input_desc,
                                     IID_PPV_ARGS(&pipeline->processor));
    if (FAILED(hr)) {
        print_hr("d3d12-ayuv-to-rgb-create-processor", hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
    bool pipeline_ok = SUCCEEDED(device->CreateCommandQueue(
        &queue_desc, IID_PPV_ARGS(&pipeline->queue)));
    pipeline_ok = pipeline_ok && SUCCEEDED(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
        IID_PPV_ARGS(&pipeline->allocator)));
    pipeline_ok = pipeline_ok && SUCCEEDED(device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, pipeline->allocator.Get(),
        nullptr, IID_PPV_ARGS(&pipeline->list)));
    pipeline_ok = pipeline_ok && SUCCEEDED(pipeline->list->Close());
    if (!pipeline_ok) {
        std::fputs("BLOCKED stage=d3d12-ayuv-to-rgb reason=process-pipeline\n",
                   stderr);
        return false;
    }
    pipeline->completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!pipeline->completion_event) {
        std::fputs("BLOCKED stage=d3d12-ayuv-to-rgb reason=process-event\n",
                   stderr);
        return false;
    }
    return true;
}

static bool record_ayuv_to_rgb(VmD3D12ProcessPipeline *pipeline,
                               ID3D12Resource *decoded_texture,
                               ID3D12Resource *rgb_texture)
{
    if (!pipeline || !decoded_texture || !rgb_texture)
        return false;
    if (FAILED(pipeline->allocator->Reset()) ||
        FAILED(pipeline->list->Reset(pipeline->allocator.Get())))
        return false;

    D3D12_RESOURCE_BARRIER input_barrier = {};
    input_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    input_barrier.Transition.pResource = decoded_texture;
    input_barrier.Transition.Subresource = 0;
    input_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    input_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
    pipeline->list->ResourceBarrier(1, &input_barrier);

    D3D12_RESOURCE_BARRIER output_barrier = {};
    output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    output_barrier.Transition.pResource = rgb_texture;
    output_barrier.Transition.Subresource = 0;
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
    pipeline->list->ResourceBarrier(1, &output_barrier);

    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS process_input = {};
    process_input.InputStream[0].pTexture2D = decoded_texture;
    process_input.InputStream[0].Subresource = 0;
    process_input.Transform.SourceRectangle = {0, 0, 3840, 2160};
    process_input.Transform.DestinationRectangle = process_input.Transform.SourceRectangle;
    process_input.Transform.Orientation = D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;
    process_input.RateInfo.OutputIndex = 0;
    process_input.RateInfo.InputFrameOrField = 0;

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS process_output = {};
    process_output.OutputStream[0].pTexture2D = rgb_texture;
    process_output.OutputStream[0].Subresource = 0;
    process_output.TargetRectangle = process_input.Transform.DestinationRectangle;
    pipeline->list->ProcessFrames(pipeline->processor.Get(), &process_output, 1,
                                  &process_input);

    input_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
    input_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    pipeline->list->ResourceBarrier(1, &input_barrier);
    output_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
    output_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    pipeline->list->ResourceBarrier(1, &output_barrier);
    return SUCCEEDED(pipeline->list->Close());
}

static bool submit_process(VmD3D12ProcessPipeline *pipeline,
                           ID3D12Fence *decode_fence,
                           UINT64 decode_value,
                           ID3D12Fence *process_fence,
                           UINT64 process_value,
                           ID3D12Device *device,
                           ID3D12CommandList *list)
{
    if (!pipeline || !decode_fence || !process_fence || !device || !list)
        return false;
    if (FAILED(pipeline->queue->Wait(decode_fence, decode_value)))
        return false;
    ID3D12CommandList *lists[] = {list};
    pipeline->queue->ExecuteCommandLists(1, lists);
    if (FAILED(pipeline->queue->Signal(process_fence, process_value)))
        return false;
    if (process_fence->GetCompletedValue() < process_value &&
        FAILED(process_fence->SetEventOnCompletion(
            process_value, pipeline->completion_event)))
        return false;
    if (process_fence->GetCompletedValue() < process_value &&
        WaitForSingleObject(pipeline->completion_event, 30000) != WAIT_OBJECT_0)
        return false;
    return SUCCEEDED(device->GetDeviceRemovedReason());
}
