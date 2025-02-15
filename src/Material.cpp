#if !RVE_SERVER
#include "Material.hpp"
#include <sstream>
#include <fstream>
#include <RenderEngine.hpp>
#include "mathtypes.hpp"
#include "glm/gtc/type_ptr.hpp"
#include "Filesystem.hpp"
#include "Common3D.hpp"
#include "App.hpp"
#include <typeindex>
#include "Debug.hpp"
#include "VirtualFileSystem.hpp"
#include <RGL/RGL.hpp>
#include <RGL/Device.hpp>
#include <RGL/Pipeline.hpp>
#include <ravengine_shader_defs.h>

using namespace std;
using namespace RavEngine;

/**
Create a material given a shader. Also registers it in the material manager
@param shader the path to the shader
*/
Material::Material(const std::string_view name, const MaterialConfig& config) : Material(name,name,config) {}

RavEngine::Material::Material(const std::string_view vsh_name, const std::string_view fsh_name, const MaterialConfig& config)
{
    auto device = GetApp()->GetDevice();

    //get all shader files for this programs
    auto vertshaderName = StrFormat("{}.vsh", vsh_name);
    auto fragShaderName = StrFormat("{}.fsh", fsh_name);

    auto vertShader = LoadShaderByFilename(vertshaderName, device);
    auto fragShader = LoadShaderByFilename(fragShaderName, device);

    auto samplerPtr = GetApp()->GetRenderEngine().textureSampler;
    
    auto configBindingsCopy = config.bindings;
    
    if (!config.verbatimConfig){
        // must have the model matrix binding
        configBindingsCopy.push_back(
                                     {
                                         .binding = MODEL_MATRIX_BINDING,
                                         .type = RGL::BindingType::StorageBuffer,
                                         .stageFlags = RGL::BindingVisibility::Vertex
                                     }
                                     );
    }

    pipelineLayout = device->CreatePipelineLayout({
        .bindings = configBindingsCopy,
        .constants = {
            {
               sizeof(RenderEngine::DeferredUBO) + config.pushConstantSize, 0, RGL::StageVisibility(RGL::StageVisibility::Vertex | RGL::StageVisibility::Fragment)
            }
        },
        });
    
    auto vertexConfigCopy = config.vertConfig;
    if (!config.verbatimConfig){
        // must have the entity ID buffer
        vertexConfigCopy.attributeDescs.push_back( {
            .location = ENTITY_INPUT_LOCATION,
            .binding = 1,
            .offset = 0,
            .format = RGL::VertexAttributeFormat::R32_Uint,
        });
        vertexConfigCopy.vertexBindings.push_back({
            .binding = 1,
            .stride = sizeof(uint32_t),
            .inputRate = RGL::InputRate::Instance,
        });
    }

    RGL::RenderPipelineDescriptor rpd{
        .stages = {
            {
                .type = RGL::ShaderStageDesc::Type::Vertex,
                .shaderModule = vertShader,
            },
            {
                .type = RGL::ShaderStageDesc::Type::Fragment,
                .shaderModule = fragShader,
            }
        },
        .vertexConfig = vertexConfigCopy,
        .inputAssembly = {
            .topology = RGL::PrimitiveTopology::TriangleList,
        },
        .rasterizerConfig = {
            .cullMode = config.cullMode,
            .windingOrder = RGL::WindingOrder::Counterclockwise,
        },
        .colorBlendConfig = config.colorBlendConfig,
        .depthStencilConfig = {
            .depthFormat = RGL::TextureFormat::D32SFloat,
            .depthTestEnabled = config.depthTestEnabled,
            .depthWriteEnabled = config.depthWriteEnabled,
            .depthFunction = config.depthCompareFunction,
        },
        .pipelineLayout = pipelineLayout,
    };

    renderPipeline = device->CreateRenderPipeline(rpd);

    // invert some settings for the shadow pipeline
    rpd.stages.pop_back();  // no fragment shader
    rpd.rasterizerConfig.windingOrder = RGL::WindingOrder::Clockwise;   // backface shadows
    rpd.colorBlendConfig.attachments.clear();                           // only write to depth
    shadowRenderPipeline = device->CreateRenderPipeline(rpd);
}

Material::~Material() {
    // enqueue the pipeline and layout for deletion
    if (auto app = GetApp()){
        auto& renderer = app->GetRenderEngine();
        renderer.gcPipelineLayout.enqueue(pipelineLayout);
        renderer.gcRenderPipeline.enqueue(renderPipeline);
        renderer.gcRenderPipeline.enqueue(shadowRenderPipeline);
    }
}
#endif
