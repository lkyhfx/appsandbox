/* SPDX-License-Identifier: MIT - GPU-only decode/presentation bridge. */
static bool convert_ayuv_to_rgb(ID3D12Device *device,
                                    ID3D12VideoDevice *video,
                                    ID3D12Resource *decoded_texture, ID3D12Resource **output)
{
    if (!device || !video || !decoded_texture)
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
    const bool supported = SUCCEEDED(hr) &&
        (support.SupportFlags & D3D12_VIDEO_PROCESS_SUPPORT_FLAG_SUPPORTED) != 0;
    if (!supported) {
        if (FAILED(hr)) print_hr("d3d12-ayuv-to-rgb-support", hr);
        else std::fputs("BLOCKED stage=d3d12-ayuv-to-rgb reason=unsupported\n",
                        stderr);
        return false;
    }

    ComPtr<ID3D12VideoProcessor> processor;
    hr = video->CreateVideoProcessor(0, &output_desc, 1, &input_desc,
                                     IID_PPV_ARGS(&processor));
    if (FAILED(hr)) {
        print_hr("d3d12-ayuv-to-rgb-create-processor", hr);
        return false;
    }

    D3D12_RESOURCE_DESC rgb_desc = {};
    rgb_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rgb_desc.Width = 3840u;
    rgb_desc.Height = 2160u;
    rgb_desc.DepthOrArraySize = 1;
    rgb_desc.MipLevels = 1;
    rgb_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rgb_desc.SampleDesc.Count = 1;
    rgb_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rgb_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    default_heap.CreationNodeMask = 1;
    default_heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> rgb_texture;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_SHARED, &rgb_desc,
        D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE, nullptr,
        IID_PPV_ARGS(&rgb_texture));
    if (FAILED(hr)) {
        print_hr("d3d12-ayuv-to-rgb-output", hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC process_queue_desc = {};
    process_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
    ComPtr<ID3D12CommandQueue> process_queue;
    ComPtr<ID3D12CommandAllocator> process_allocator;
    ComPtr<ID3D12VideoProcessCommandList> process_list;
    if (FAILED(device->CreateCommandQueue(&process_queue_desc,
                                          IID_PPV_ARGS(&process_queue))) ||
        FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
            IID_PPV_ARGS(&process_allocator))) ||
        FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, process_allocator.Get(),
            nullptr, IID_PPV_ARGS(&process_list)))) {
        std::fputs("BLOCKED stage=d3d12-ayuv-to-rgb reason=process-queue\n",
                   stderr);
        return false;
    }

    D3D12_RESOURCE_BARRIER input_barrier = {};
    input_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    input_barrier.Transition.pResource = decoded_texture;
    input_barrier.Transition.Subresource = 0;
    input_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    input_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
    process_list->ResourceBarrier(1, &input_barrier);
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS process_input = {};
    process_input.InputStream[0].pTexture2D = decoded_texture;
    process_input.InputStream[0].Subresource = 0;
    process_input.Transform.SourceRectangle = {0, 0,
                                               static_cast<LONG>(3840u),
                                               static_cast<LONG>(2160u)};
    process_input.Transform.DestinationRectangle = process_input.Transform.SourceRectangle;
    process_input.Transform.Orientation = D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT;
    process_input.RateInfo.OutputIndex = 0;
    process_input.RateInfo.InputFrameOrField = 0;
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS process_output = {};
    process_output.OutputStream[0].pTexture2D = rgb_texture.Get();
    process_output.OutputStream[0].Subresource = 0;
    process_output.TargetRectangle = process_input.Transform.DestinationRectangle;
    process_list->ProcessFrames(processor.Get(), &process_output, 1,
                                 &process_input);
    input_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ;
    input_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    process_list->ResourceBarrier(1, &input_barrier);
    D3D12_RESOURCE_BARRIER rgb_barrier = {};
    rgb_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rgb_barrier.Transition.pResource = rgb_texture.Get();
    rgb_barrier.Transition.Subresource = 0;
    rgb_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE;
    rgb_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    process_list->ResourceBarrier(1, &rgb_barrier);
    const HRESULT process_close_hr = process_list->Close();
    if (FAILED(process_close_hr)) {
        print_hr("d3d12-ayuv-to-rgb-process-list", process_close_hr);
        return false;
    }
    ID3D12CommandList *process_lists[] = {process_list.Get()};
    process_queue->ExecuteCommandLists(1, process_lists);
    ComPtr<ID3D12Fence> process_fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                   IID_PPV_ARGS(&process_fence))) ||
        FAILED(process_queue->Signal(process_fence.Get(), 1))) {
        std::fputs("BLOCKED stage=d3d12-ayuv-to-rgb reason=process-fence\n",
                   stderr);
        return false;
    }

    while (process_fence->GetCompletedValue() < 1) {
        if (FAILED(device->GetDeviceRemovedReason())) return false;
        Sleep(1);
    }
    if (FAILED(device->GetDeviceRemovedReason())) return false;
    *output = rgb_texture.Detach();
    return true;
}
