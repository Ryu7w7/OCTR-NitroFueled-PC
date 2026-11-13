// Real native target resizing and scaled copies, without a game or a window.
namespace vk::detail { class DispatchLoaderDynamic; }
extern vk::detail::DispatchLoaderDynamic live_test_dispatch;
#define VULKAN_HPP_DEFAULT_DISPATCHER live_test_dispatch
#include "../gpu/shim/bbport_native_vk.cpp"
#include <cassert>

vk::detail::DispatchLoaderDynamic live_test_dispatch;

// Compare rendered pixels; changing shader bytes alone does not prove the effect.
void TestBlurPixels(const Vulkan::Instance& gpu, const std::filesystem::path& root) {
    using namespace BbNativeVk;
    const auto device = gpu.GetDevice();
    const auto load = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        assert(file);
        std::vector<u32> code(std::size_t(file.tellg()) / 4);
        file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), code.size() * 4);
        assert(file); return code;
    };
    constexpr u32 w = 64, h = 32;
    const auto texture = [&](u64 address, u32 width, u32 height, auto pixel) {
        auto* target = MakeTarget(address, width, height, vk::Format::eR32G32B32A32Sfloat, false, 0, "blur fixture");
        assert(target);
        const auto upload = Gpu::Upload(width * height * 16, 16);
        auto* data = static_cast<std::array<float, 4>*>(upload.data);
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) data[y * width + x] = pixel(x, y);
        Gpu::Use(target->img, vk::ImageLayout::eTransferDstOptimal); Gpu::Barrier();
        const vk::BufferImageCopy copy{.bufferOffset=upload.offset,
            .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={width,height,1}};
        Gpu::Cmd().copyBufferToImage(upload.buffer,target->img.image,vk::ImageLayout::eTransferDstOptimal,copy);
        Gpu::Use(target->img,vk::ImageLayout::eShaderReadOnlyOptimal);
        return target;
    };
    auto* color = texture(0x800000, w, h, [](u32 x, u32) {
        const float v = (x / 4) % 2 ? 1.f : 0.f;
        return std::array{v, v, v, 1.f};
    });
    auto* velocity = texture(0x900000,w,h,[](u32,u32){return std::array{0.75f,0.5f,1.f,1.f};});
    // The stochastic variant reads its three tap radii from t4; match the fixed-kernel variant.
    auto* taps = texture(0xc00000,w,h,[](u32,u32){return std::array{0.74f,0.42f,0.1f,1.f};});
    auto* lut = texture(0xa00000,1024,32,[](u32 x,u32 y) {
        return std::array{float(x % 32)/31, float(y)/31, float(x / 32)/31, 1.f};
    });
    auto* output = MakeTarget(0xb00000,w,h,vk::Format::eR32G32B32A32Sfloat,false,0,"blur output");
    assert(output);
    const auto constants = Gpu::Upload(65536,256);
    std::memset(constants.data,0,65536);
    auto* cb=static_cast<float*>(constants.data);
    cb[2]=cb[4]=1.f/w; cb[3]=cb[5]=1.f/h; cb[8]=16; cb[9]=32;
    std::array<vk::DescriptorSetLayoutBinding,11> bindings{};
    bindings[0]={0,vk::DescriptorType::eUniformBuffer,1,vk::ShaderStageFlagBits::eFragment};
    const u32 slots[]={0,2,3,4,5};
    for(u32 i=0;i<5;++i) {
        bindings[1+i]={64+slots[i],vk::DescriptorType::eSampledImage,1,vk::ShaderStageFlagBits::eFragment};
        bindings[6+i]={128+slots[i],vk::DescriptorType::eSampler,1,vk::ShaderStageFlagBits::eFragment};
    }
    const auto set_layout=device.createDescriptorSetLayoutUnique({.bindingCount=11,.pBindings=bindings.data()}).value;
    const vk::DescriptorSetLayout raw_layout=*set_layout;
    const auto layout=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&raw_layout}).value;
    const auto sampler=device.createSamplerUnique({.magFilter=vk::Filter::eLinear,.minFilter=vk::Filter::eLinear,
        .addressModeU=vk::SamplerAddressMode::eClampToEdge,.addressModeV=vk::SamplerAddressMode::eClampToEdge,
        .addressModeW=vk::SamplerAddressMode::eClampToEdge,.maxLod=0}).value;
    const vk::DescriptorBufferInfo buffer{constants.buffer,constants.offset,65536};
    const Target* textures[]={color,velocity,velocity,taps,lut};
    std::array<vk::DescriptorImageInfo,10> infos{};
    std::array<vk::WriteDescriptorSet,11> writes{};
    writes[0]={.dstSet={},.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eUniformBuffer,.pBufferInfo=&buffer};
    for(u32 i=0;i<5;++i) {
        infos[i]={{},textures[i]->view,vk::ImageLayout::eShaderReadOnlyOptimal};
        infos[5+i]=vk::DescriptorImageInfo{*sampler,{},vk::ImageLayout::eUndefined};
        writes[1+i]={.dstSet={},.dstBinding=64+slots[i],.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[i]};
        writes[6+i]={.dstSet={},.dstBinding=128+slots[i],.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampler,.pImageInfo=&infos[5+i]};
    }
    const auto vs=load(BLUR_TEST_VERTEX);
    const auto vertex=device.createShaderModuleUnique({.codeSize=vs.size()*4,.pCode=vs.data()}).value;
    VkBuffer readback{}; VmaAllocation allocation{}; VmaAllocationInfo mapped{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=w*h*16,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ai{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,.usage=VMA_MEMORY_USAGE_AUTO};
    assert(vmaCreateBuffer(gpu.GetAllocator(),&bi,&ai,&readback,&allocation,&mapped)==VK_SUCCESS);
    for(const char* name: {"MotionBlur_21a0fd55_ps.spv","MotionBlur_ea382dda_ps.spv"}) {
        const auto original=load(root/"spirv"/name);
        const auto changed=BbBlur::Unblur(original); assert(changed);
        std::array<std::vector<u8>,3> result;
        for(int step=0;step<3;++step) {
            const auto& code=step==1?*changed:original;
            const auto ps=device.createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()}).value;
            const std::array stages{
                vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eVertex,.module=*vertex,.pName="main"},
                vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eFragment,.module=*ps,.pName="main"}};
            const vk::PipelineVertexInputStateCreateInfo input{};
            const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology=vk::PrimitiveTopology::eTriangleList};
            const vk::PipelineViewportStateCreateInfo viewport{};
            const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth=1};
            const vk::PipelineMultisampleStateCreateInfo msaa{.rasterizationSamples=vk::SampleCountFlagBits::e1};
            const vk::PipelineColorBlendAttachmentState color_state{.colorWriteMask=vk::ColorComponentFlags(15)};
            const vk::PipelineColorBlendStateCreateInfo blend{.attachmentCount=1,.pAttachments=&color_state};
            const std::array dynamic{vk::DynamicState::eViewportWithCount,vk::DynamicState::eScissorWithCount};
            const vk::PipelineDynamicStateCreateInfo dynamic_info{.dynamicStateCount=2,.pDynamicStates=dynamic.data()};
            const vk::Format format=vk::Format::eR32G32B32A32Sfloat;
            const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount=1,.pColorAttachmentFormats=&format};
            const auto pipeline=device.createGraphicsPipelineUnique({}, {.pNext=&rendering,.stageCount=2,.pStages=stages.data(),
                .pVertexInputState=&input,.pInputAssemblyState=&assembly,.pViewportState=&viewport,
                .pRasterizationState=&raster,.pMultisampleState=&msaa,.pColorBlendState=&blend,.pDynamicState=&dynamic_info,.layout=*layout}).value;
            const auto set=Gpu::AllocateSet(*set_layout);
            for (auto& write : writes) write.dstSet=set;
            device.updateDescriptorSets(writes,{});
            Gpu::Pass pass{}; pass.color[0]=&output->img; Gpu::Render(pass);
            auto cmd=Gpu::Cmd(); cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,*pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,*layout,0,set,{});
            cmd.setViewportWithCount(vk::Viewport{0,0,float(w),float(h),0,1});
            cmd.setScissorWithCount(vk::Rect2D{{0,0},{w,h}}); cmd.draw(3,1,0,0);
            Gpu::Use(output->img,vk::ImageLayout::eTransferSrcOptimal); Gpu::Barrier();
            const vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={w,h,1}};
            Gpu::Cmd().copyImageToBuffer(output->img.image,vk::ImageLayout::eTransferSrcOptimal,readback,copy);
            Gpu::Submit(); assert(Gpu::WaitDone(Gpu::Submitted(),10'000'000'000ull));
            vmaInvalidateAllocation(gpu.GetAllocator(),allocation,0,VK_WHOLE_SIZE);
            result[step].assign(static_cast<u8*>(mapped.pMappedData),static_cast<u8*>(mapped.pMappedData)+w*h*16);
        }
        u32 different=0;
        for(u32 i=0;i<w*h;++i) different+=std::memcmp(&result[0][i*16],&result[1][i*16],12)!=0;
        std::printf("Motion blur pixel test: %s On/Off differ at %u/%u pixels; On restored=%d\n",name,different,w*h,result[0]==result[2]);
        for (int state=0;state<2;++state) {
            float lo=1e9f,hi=-1e9f;
            for (u32 i=0;i<w*h;++i) {
                float value;std::memcpy(&value,&result[state][i*16],4);
                assert(std::isfinite(value));lo=std::min(lo,value);hi=std::max(hi,value);
            }
            std::printf("  %s red range: %g..%g\n",state==0?"On":"Off",lo,hi);
        }
        std::fflush(stdout);
        assert(different>w*h/8 && result[0]==result[2]);
    }
    vmaDestroyBuffer(gpu.GetAllocator(),readback,allocation);
}

int main(int argc, char** argv) {
    using namespace BbNativeVk;
    Vulkan::Instance gpu(0, false);
    static vk::detail::DynamicLoader loader;
    live_test_dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    live_test_dispatch.init(gpu.GetInstance());
    live_test_dispatch.init(gpu.GetDevice());
    instance.store(&gpu);
    if (argc == 2) {
        shader_root = argv[1];
        for (const char* name : {"MotionBlur_21a0fd55_ps", "MotionBlur_ea382dda_ps"}) {
            ShaderInfo blur;
            blur.name = name;
            blur.kind = "ps";
            LoadModule(blur);
            assert(blur.module && blur.no_blur_module && blur.set_layout);
            PipelineState state;
            VertexInput input;
            state.vertices = &input;
            state.ps = &blur;
            const u64 normal = state.Key();
            state.no_blur = true;
            assert(state.Key() != normal);
            gpu.GetDevice().destroyShaderModule(blur.module);
            gpu.GetDevice().destroyShaderModule(blur.no_blur_module);
            gpu.GetDevice().destroyDescriptorSetLayout(blur.set_layout);
        }
    }
    assert(Gpu::Init(gpu, ""));
    Blit::Init(gpu);
    if (argc == 2) TestBlurPixels(gpu, argv[1]);
    drawing = true;
    auto& settings = BbSettings::Get();
    settings.native_live_graphics = true;
    settings.startup_aspect_ratio = BbSettings::Aspect16x9;
    settings.startup_ultrawide = false;
    settings.temporal = BbSettings::TemporalOff;
    Vulkan::FrameCapture::AddDisplayBuffer(0x200000);

    VkBuffer readback{};
    VmaAllocation memory{};
    VmaAllocationInfo mapped{};
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 5120 * 1440 * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo allocation{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage = VMA_MEMORY_USAGE_AUTO};
    assert(vmaCreateBuffer(gpu.GetAllocator(), &buffer, &allocation, &readback, &memory, &mapped) == VK_SUCCESS);
    Batch batch;
    for (const auto [width, height] : {std::pair{1920u, 1080u}, std::pair{1920u, 1200u},
                                      std::pair{3440u, 1440u}, std::pair{5120u, 1440u}}) {
    std::array<u8, CtxBytes> ctx{}, empty{};
    std::array<std::array<u8, ColorTargetBytes>, 4> colors{};
    std::array<u8, DepthTargetBytes> depth{};
    for (unsigned i = 0; i < colors.size(); ++i) {
        const u64 ptr = reinterpret_cast<u64>(colors[i].data());
        std::memcpy(ctx.data() + CtxColorTargets + i * 8, &ptr, 8);
    }
    const u64 ptr = reinterpret_cast<u64>(depth.data());
    std::memcpy(ctx.data() + CtxDepthTarget, &ptr, 8);
    const u32 size = width | (height << 16);
    std::memcpy(depth.data() + 0x80, &size, 4);
    for (int res : {-1, 1, 3, 2, 1}) {
        settings.render_res = res < 0 ? 2 : res;
        settings.native_resolution_requested = res >= 0;
        OpenBatch(&batch);
        ObserveScene(reinterpret_cast<u64>(empty.data()), false);
        ObserveScene(reinterpret_cast<u64>(ctx.data()), true);
        const u32 latched = live_height;
        requested_height = latched == 720 ? 1440 : 720;
        ObserveScene(reinterpret_cast<u64>(ctx.data()), true);
        assert(live_height == latched); // A second attachment clear cannot resize mid-pass.
        requested_height = latched;
        Target* source = MakeTarget(0x100000, width, height, vk::Format::eR8G8B8A8Unorm, false, 0, "test scene");
        Target* display = MakeTarget(0x200000, width, height, vk::Format::eR8G8B8A8Unorm, false, 0, "test display");
        assert(source && display);
        assert(source->img.height == (res < 0 ? height : u32(BbSettings::RenderResHeights[res])));
        assert(source->native_w == width && source->native_h == height);
        assert(display->img.width == width && display->img.height == height && display->scale == 1);
        RunBatch(batch);
        batch.Clear();
        const float blue[] = {0, 0, 1, 1};
        Gpu::ClearColor(source->img, blue);
        Gpu::Pass pass{};
        pass.color[0] = &source->img;
        Gpu::Render(pass);
        const vk::ClearAttachment red{.aspectMask = vk::ImageAspectFlagBits::eColor,
            .colorAttachment = 0, .clearValue = vk::ClearValue{vk::ClearColorValue{std::array<float, 4>{1, 0, 0, 1}}}};
        const vk::ClearRect left{{{0, 0}, {source->img.width / 2, source->img.height}}, 0, 1};
        Gpu::Cmd().clearAttachments(red, left);
        assert(Blit::Copy(source->img, source->view, {0, 0}, display->img,
                         {{0, 0}, {width, height}}, {source->img.width, source->img.height}));
        Gpu::Use(display->img, vk::ImageLayout::eTransferSrcOptimal);
        Gpu::Barrier();
        const vk::BufferImageCopy copy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                       .imageExtent = {width, height, 1}};
        Gpu::Cmd().copyImageToBuffer(display->img.image, vk::ImageLayout::eTransferSrcOptimal, readback, copy);
        Gpu::Submit();
        assert(Gpu::WaitDone(Gpu::Submitted(), 10'000'000'000ull));
        vmaInvalidateAllocation(gpu.GetAllocator(), memory, 0, VK_WHOLE_SIZE);
        const auto* pixels = static_cast<const u8*>(mapped.pMappedData);
        for (u32 pixel : {0u, width * height / 2 + width / 4,
                          width * height / 2 + width * 3 / 4, width * height - 1}) {
            const bool blue_side = pixel % width >= width / 2;
            assert(pixels[pixel * 4] == (blue_side ? 0 : 255) && pixels[pixel * 4 + 1] == 0);
            assert(pixels[pixel * 4 + 2] == (blue_side ? 255 : 0) && pixels[pixel * 4 + 3] == 255);
        }
    }
    }
    std::puts("PASS: live resize for 16:9/16:10/21:9/32:9, fixed display, split-color resampling");
    std::fflush(nullptr);
    std::_Exit(0); // Native recorder resources normally live until process exit.
}
